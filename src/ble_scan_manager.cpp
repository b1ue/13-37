#include "ble_scan_manager.h"
#include <Arduino.h>
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#define BLE_SCAN_MAX_CONSUMERS 10
#define BLE_RESULT_QUEUE_LEN   24
#define BLE_RESULTS_PER_TICK   2
#define BLE_IDLE_GRACE_MS      750U
#define BLE_RETRY_MS           100U
#define BLE_START_SETTLE_MS     20U
#define BLE_FIRST_START_DELAY_MS 80U

// Never run detector, GPS, SD, or UI-adjacent code on Bluedroid's callback
// task. The GAP callback copies only the fields used by our consumers; the
// normal main loop fans the result out later via ble_scan_tick().
struct QueuedBleResult {
    uint8_t bda[6];
    int     rssi;
    esp_ble_addr_type_t addr_type;
    uint8_t adv_data_len;
    uint8_t scan_rsp_len;
    uint8_t adv[ESP_BLE_ADV_DATA_LEN_MAX + ESP_BLE_SCAN_RSP_DATA_LEN_MAX];
};

static ble_scan_cb_t s_consumers[BLE_SCAN_MAX_CONSUMERS] = {};
static int           s_consumer_count = 0;
static portMUX_TYPE  s_consumer_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_scan_param_pending = false;
static volatile bool s_scan_start_pending = false;
static volatile bool s_scan_stop_pending = false;
static volatile bool s_scanning = false;
static bool          s_shutdown_requested = false;
static uint32_t      s_idle_since_ms = 0;
static uint32_t      s_next_lifecycle_ms = 0;
static QueueHandle_t s_result_queue = nullptr;
static volatile uint32_t s_result_drops = 0;
static uint32_t      s_last_drop_report_ms = 0;
static int           s_hold_count = 0;
static volatile bool s_controller_ready = false;
static ble_gap_observer_t s_gap_observer = nullptr;

enum StartupStepResult {
    STARTUP_READY,
    STARTUP_PROGRESS,
    STARTUP_FAILED,
};

static int consumer_count_snapshot()
{
    portENTER_CRITICAL(&s_consumer_mux);
    int count = s_consumer_count;
    portEXIT_CRITICAL(&s_consumer_mux);
    return count;
}

static void gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    ble_gap_observer_t observer = s_gap_observer;
    if (observer) observer(event, param);

    if (event == ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT) {
        // The last consumer may disappear while parameter setup is in flight.
        // In that case do not begin a scan just so teardown has to stop it.
        if (param->scan_param_cmpl.status == ESP_BT_STATUS_SUCCESS &&
            consumer_count_snapshot() > 0) {
            s_scan_start_pending = true;
            if (esp_ble_gap_start_scanning(0) != ESP_OK)
                s_scan_start_pending = false;
        }
        s_scan_param_pending = false;
        return;
    }
    if (event == ESP_GAP_BLE_SCAN_START_COMPLETE_EVT) {
        s_scanning = param->scan_start_cmpl.status == ESP_BT_STATUS_SUCCESS;
        s_scan_start_pending = false;
        return;
    }
    if (event == ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT) {
        s_scanning = false;
        s_scan_stop_pending = false;
        return;
    }
    if (event != ESP_GAP_BLE_SCAN_RESULT_EVT) return;
    if (param->scan_rst.search_evt != ESP_GAP_SEARCH_INQ_RES_EVT) return;
    if (!s_result_queue || consumer_count_snapshot() == 0) return;

    QueuedBleResult queued = {};
    memcpy(queued.bda, param->scan_rst.bda, sizeof(queued.bda));
    queued.rssi = param->scan_rst.rssi;
    queued.addr_type = param->scan_rst.ble_addr_type;
    queued.adv_data_len = param->scan_rst.adv_data_len;
    queued.scan_rsp_len = param->scan_rst.scan_rsp_len;
    size_t total = (size_t)queued.adv_data_len + queued.scan_rsp_len;
    if (total > sizeof(queued.adv)) {
        // Never let a malformed/vendor event make consumers walk beyond the
        // copied payload. Keep primary data first, then as much response data
        // as fits in the fixed queue item.
        if (queued.adv_data_len > sizeof(queued.adv))
            queued.adv_data_len = sizeof(queued.adv);
        queued.scan_rsp_len = sizeof(queued.adv) - queued.adv_data_len;
        total = sizeof(queued.adv);
    }
    memcpy(queued.adv, param->scan_rst.ble_adv, total);
    if (xQueueSend(s_result_queue, &queued, 0) != pdTRUE)
        ++s_result_drops;
}

static void dispatch_scan_results()
{
    if (!s_result_queue) return;

    for (int n = 0; n < BLE_RESULTS_PER_TICK; ++n) {
        QueuedBleResult queued;
        if (xQueueReceive(s_result_queue, &queued, 0) != pdTRUE) break;

        // Recreate the subset of the IDF event consumed by scanner modules.
        // It remains valid until every callback returns synchronously.
        esp_ble_gap_cb_param_t param = {};
        param.scan_rst.search_evt = ESP_GAP_SEARCH_INQ_RES_EVT;
        memcpy(param.scan_rst.bda, queued.bda, sizeof(queued.bda));
        param.scan_rst.rssi = queued.rssi;
        param.scan_rst.ble_addr_type = queued.addr_type;
        param.scan_rst.adv_data_len = queued.adv_data_len;
        param.scan_rst.scan_rsp_len = queued.scan_rsp_len;
        size_t total = (size_t)queued.adv_data_len + queued.scan_rsp_len;
        if (total > sizeof(param.scan_rst.ble_adv))
            total = sizeof(param.scan_rst.ble_adv);
        memcpy(param.scan_rst.ble_adv, queued.adv, total);

        ble_scan_cb_t snapshot[BLE_SCAN_MAX_CONSUMERS] = {};
        int count = 0;
        portENTER_CRITICAL(&s_consumer_mux);
        count = s_consumer_count;
        for (int i = 0; i < count; ++i) snapshot[i] = s_consumers[i];
        portEXIT_CRITICAL(&s_consumer_mux);
        for (int i = 0; i < count; ++i)
            if (snapshot[i]) snapshot[i](&param);
    }

    uint32_t drops = s_result_drops;
    uint32_t now = millis();
    if (drops > 0 && now - s_last_drop_report_ms >= 30000U) {
        s_last_drop_report_ms = now;
        Serial.printf("[BLE scan] queued-result drops=%lu\n",
                      (unsigned long)drops);
    }
}

static bool request_scan_parameters()
{
    if (s_scanning || s_scan_param_pending || s_scan_start_pending ||
        s_scan_stop_pending) return true;

    // Passive scan with a 100 ms interval and 30 ms window. The previous
    // 50/30 ms settings kept the receiver on about 60% of the time; this is
    // about 30% while still seeing normally repeated advertisements quickly.
    esp_ble_scan_params_t scan_params = {
        BLE_SCAN_TYPE_PASSIVE,
        BLE_ADDR_TYPE_PUBLIC,
        BLE_SCAN_FILTER_ALLOW_ALL,
        0x00A0,
        0x0030,
        BLE_SCAN_DUPLICATE_DISABLE
    };
    s_scan_param_pending = true;
    esp_err_t rc = esp_ble_gap_set_scan_params(&scan_params);
    if (rc != ESP_OK) s_scan_param_pending = false;
    return rc == ESP_OK;
}

// Progress exactly one potentially expensive controller/host operation. Cold
// BLE startup used to run init -> enable -> Bluedroid init -> enable in one
// main-loop pass, which could starve LVGL long enough to look like a frozen
// Skimmer/Flipper tile. Teardown was already staged; startup now matches it.
static StartupStepResult progress_controller_startup(uint32_t now)
{
    if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE) {
        esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
        if (esp_bt_controller_init(&bt_cfg) != ESP_OK) return STARTUP_FAILED;
        s_next_lifecycle_ms = now + BLE_START_SETTLE_MS;
        return STARTUP_PROGRESS;
    }
    if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_INITED) {
        if (esp_bt_controller_enable(ESP_BT_MODE_BLE) != ESP_OK)
            return STARTUP_FAILED;
        s_next_lifecycle_ms = now + BLE_START_SETTLE_MS;
        return STARTUP_PROGRESS;
    }
    if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_UNINITIALIZED) {
        if (esp_bluedroid_init() != ESP_OK) return STARTUP_FAILED;
        s_next_lifecycle_ms = now + BLE_START_SETTLE_MS;
        return STARTUP_PROGRESS;
    }
    if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_INITIALIZED) {
        if (esp_bluedroid_enable() != ESP_OK) return STARTUP_FAILED;
        s_next_lifecycle_ms = now + BLE_START_SETTLE_MS;
        return STARTUP_PROGRESS;
    }
    if (esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_ENABLED ||
        esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_ENABLED)
        return STARTUP_FAILED;
    if (esp_ble_gap_register_callback(gap_cb) != ESP_OK)
        return STARTUP_FAILED;
    s_controller_ready = true;
    return STARTUP_READY;
}

static void schedule_shutdown()
{
    s_shutdown_requested = true;
    s_idle_since_ms = millis();
    s_next_lifecycle_ms = s_idle_since_ms;
}

bool ble_scan_add(ble_scan_cb_t cb)
{
    if (!cb) return false;
    if (!s_result_queue) {
        s_result_queue = xQueueCreate(BLE_RESULT_QUEUE_LEN,
                                      sizeof(QueuedBleResult));
        if (!s_result_queue) return false;
    }

    portENTER_CRITICAL(&s_consumer_mux);
    for (int i = 0; i < s_consumer_count; ++i) {
        if (s_consumers[i] == cb) {
            portEXIT_CRITICAL(&s_consumer_mux);
            return true;
        }
    }
    if (s_consumer_count >= BLE_SCAN_MAX_CONSUMERS) {
        portEXIT_CRITICAL(&s_consumer_mux);
        return false;
    }
    const bool first = s_consumer_count == 0;
    s_consumers[s_consumer_count++] = cb;
    portEXIT_CRITICAL(&s_consumer_mux);

    if (first) {
        xQueueReset(s_result_queue);
        s_result_drops = 0;
        s_last_drop_report_ms = millis();
    }

    // Consumer registration is deliberately cheap: controller/host startup
    // can take long enough to starve LVGL (and used to happen directly in a
    // switch callback). ble_scan_tick() owns startup and retry on later loop
    // passes, just as it already owns final shutdown.
    s_shutdown_requested = false;
    s_idle_since_ms = 0;
    // Let LVGL paint the tile's enabled state before the first cold-start
    // operation. Additional consumers join immediately without restarting it.
    s_next_lifecycle_ms = millis() + (first ? BLE_FIRST_START_DELAY_MS : 0U);
    return true;
}

void ble_scan_remove(ble_scan_cb_t cb)
{
    if (!cb) return;
    bool last = false;
    portENTER_CRITICAL(&s_consumer_mux);
    for (int i = 0; i < s_consumer_count; ++i) {
        if (s_consumers[i] != cb) continue;
        for (int j = i; j < s_consumer_count - 1; ++j)
            s_consumers[j] = s_consumers[j + 1];
        s_consumers[--s_consumer_count] = nullptr;
        last = s_consumer_count == 0;
        break;
    }
    portEXIT_CRITICAL(&s_consumer_mux);

    // Crucially, no controller calls or acknowledgement waits occur in the
    // UI event that removed the final consumer. ble_scan_tick() performs the
    // stop and one teardown step per later main-loop pass.
    if (last) {
        if (s_result_queue) xQueueReset(s_result_queue);
        if (s_hold_count == 0) schedule_shutdown();
        else                  s_next_lifecycle_ms = millis();
    }
}

void ble_scan_tick()
{
    const int consumers = consumer_count_snapshot();
    int holds;
    portENTER_CRITICAL(&s_consumer_mux);
    holds = s_hold_count;
    portEXIT_CRITICAL(&s_consumer_mux);
    const uint32_t now = millis();
    if (consumers > 0 || holds > 0) {
        s_shutdown_requested = false;
        s_idle_since_ms = 0;
        // A hold can arrive between two staged shutdown operations.  Never
        // trust the cached ready flag if either real stack component has
        // already moved out of ENABLED; progress startup resumes from that
        // exact stage on this or a later tick.
        if (esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_ENABLED ||
            esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_ENABLED)
            s_controller_ready = false;
        if (!s_controller_ready && !s_scan_param_pending &&
            !s_scan_start_pending && !s_scan_stop_pending &&
            (int32_t)(now - s_next_lifecycle_ms) >= 0) {
            StartupStepResult result = progress_controller_startup(now);
            if (result == STARTUP_FAILED) {
                Serial.println("[BLE scan] restart failed; will retry");
                s_next_lifecycle_ms = now + 500U;
            }
        }

        if (s_controller_ready && consumers > 0 && !s_scanning &&
            !s_scan_param_pending && !s_scan_start_pending &&
            !s_scan_stop_pending &&
            (int32_t)(now - s_next_lifecycle_ms) >= 0) {
            if (!request_scan_parameters()) s_next_lifecycle_ms = now + 500U;
        } else if (consumers == 0 && s_scanning && !s_scan_stop_pending) {
            s_scan_stop_pending = true;
            if (esp_ble_gap_stop_scanning() != ESP_OK) {
                s_scan_stop_pending = false;
                s_scanning = false;
            }
        }

        if (consumers > 0) dispatch_scan_results();
        return;
    }
    if (!s_shutdown_requested) return;

    if (now - s_idle_since_ms < BLE_IDLE_GRACE_MS ||
        (int32_t)(now - s_next_lifecycle_ms) < 0) return;
    if (s_scan_param_pending || s_scan_start_pending || s_scan_stop_pending) return;

    if (s_scanning) {
        s_scan_stop_pending = true;
        esp_err_t rc = esp_ble_gap_stop_scanning();
        if (rc != ESP_OK) {
            s_scan_stop_pending = false;
            // If the host already considers the scan stopped, continue with
            // teardown instead of trapping the lifecycle in a retry loop.
            s_scanning = false;
            s_next_lifecycle_ms = now + BLE_RETRY_MS;
        }
        return;
    }

    // One potentially expensive controller operation per pass. This keeps a
    // button/touch event and LVGL refresh between phases and avoids the old
    // stop->wait->disable->deinit chain that could freeze the next input.
    esp_err_t rc = ESP_OK;
    if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_ENABLED)
        rc = esp_bluedroid_disable();
    else if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_INITIALIZED)
        rc = esp_bluedroid_deinit();
    else if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_ENABLED)
        rc = esp_bt_controller_disable();
    else if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_INITED)
        rc = esp_bt_controller_deinit();
    else {
        s_shutdown_requested = false;
        s_controller_ready = false;
        s_scan_param_pending = false;
        s_scan_start_pending = false;
        s_scan_stop_pending = false;
        s_scanning = false;
        Serial.println("[BLE scan] controller asleep");
        return;
    }
    if (rc != ESP_OK) {
        Serial.printf("[BLE scan] deferred teardown step failed: %d\n", (int)rc);
        s_next_lifecycle_ms = now + BLE_RETRY_MS;
    }
}

// Treat deferred shutdown as active too. This prevents the BLE HID mouse from
// initializing its own stack in the short interval after the last scanner
// leaves but before Bluedroid/controller teardown has completed.
bool ble_scan_active()
{
    int holds;
    portENTER_CRITICAL(&s_consumer_mux);
    holds = s_hold_count;
    portEXIT_CRITICAL(&s_consumer_mux);
    return consumer_count_snapshot() > 0 || holds > 0 || s_shutdown_requested;
}
int ble_scan_consumer_count() { return consumer_count_snapshot(); }
uint32_t ble_scan_result_drop_count() { return s_result_drops; }

void ble_scan_hold_acquire()
{
    portENTER_CRITICAL(&s_consumer_mux);
    ++s_hold_count;
    portEXIT_CRITICAL(&s_consumer_mux);
    s_shutdown_requested = false;
    s_idle_since_ms = 0;
    s_next_lifecycle_ms = millis();
}

void ble_scan_hold_release()
{
    bool idle = false;
    portENTER_CRITICAL(&s_consumer_mux);
    if (s_hold_count > 0) --s_hold_count;
    idle = s_hold_count == 0 && s_consumer_count == 0;
    portEXIT_CRITICAL(&s_consumer_mux);
    if (idle) schedule_shutdown();
}

bool ble_scan_ready() { return s_controller_ready; }

void ble_scan_set_gap_observer(ble_gap_observer_t observer)
{
    s_gap_observer = observer;
}
