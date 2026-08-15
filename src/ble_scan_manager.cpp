#include "ble_scan_manager.h"
#include <Arduino.h>
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "freertos/FreeRTOS.h"

#define BLE_SCAN_MAX_CONSUMERS 6
#define BLE_IDLE_GRACE_MS      750U
#define BLE_RETRY_MS           100U

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

static int consumer_count_snapshot()
{
    portENTER_CRITICAL(&s_consumer_mux);
    int count = s_consumer_count;
    portEXIT_CRITICAL(&s_consumer_mux);
    return count;
}

static void gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
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

    // Snapshot under a short critical section so add/remove cannot shift the
    // table while the Bluetooth host task is walking it. Consumer callbacks
    // run only after the lock is released.
    ble_scan_cb_t snapshot[BLE_SCAN_MAX_CONSUMERS] = {};
    int count = 0;
    portENTER_CRITICAL(&s_consumer_mux);
    count = s_consumer_count;
    for (int i = 0; i < count; ++i) snapshot[i] = s_consumers[i];
    portEXIT_CRITICAL(&s_consumer_mux);
    for (int i = 0; i < count; ++i)
        if (snapshot[i]) snapshot[i](param);
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

static bool ensure_controller_ready()
{
    bool ok = true;
    if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE) {
        esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
        ok = esp_bt_controller_init(&bt_cfg) == ESP_OK;
    }
    if (ok && esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_INITED)
        ok = esp_bt_controller_enable(ESP_BT_MODE_BLE) == ESP_OK;
    if (ok && esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_UNINITIALIZED)
        ok = esp_bluedroid_init() == ESP_OK;
    if (ok && esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_INITIALIZED)
        ok = esp_bluedroid_enable() == ESP_OK;
    if (!ok) return false;
    if (esp_ble_gap_register_callback(gap_cb) != ESP_OK) return false;
    return request_scan_parameters();
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

    s_shutdown_requested = false;
    s_idle_since_ms = 0;
    if (!first || ensure_controller_ready()) return true;

    // Roll back only this callback if controller startup failed.
    portENTER_CRITICAL(&s_consumer_mux);
    for (int i = 0; i < s_consumer_count; ++i) {
        if (s_consumers[i] != cb) continue;
        for (int j = i; j < s_consumer_count - 1; ++j)
            s_consumers[j] = s_consumers[j + 1];
        s_consumers[--s_consumer_count] = nullptr;
        break;
    }
    portEXIT_CRITICAL(&s_consumer_mux);
    schedule_shutdown();
    return false;
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
    if (last) schedule_shutdown();
}

void ble_scan_tick()
{
    const int consumers = consumer_count_snapshot();
    const uint32_t now = millis();
    if (consumers > 0) {
        s_shutdown_requested = false;
        s_idle_since_ms = 0;
        if (!s_scanning && !s_scan_param_pending && !s_scan_start_pending &&
            !s_scan_stop_pending &&
            (int32_t)(now - s_next_lifecycle_ms) >= 0) {
            if (!ensure_controller_ready()) {
                Serial.println("[BLE scan] restart failed; will retry");
                s_next_lifecycle_ms = now + 500U;
            }
        }
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
    return consumer_count_snapshot() > 0 || s_shutdown_requested;
}
int ble_scan_consumer_count() { return consumer_count_snapshot(); }
