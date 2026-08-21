#include "wifi_beacon_manager.h"
#include <WiFi.h>
#include "esp_wifi.h"
#include <lvgl.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define WBM_MAX_CONSUMERS       6
#define WBM_QUEUE_LEN          24
#define WBM_RESULTS_PER_TICK    2
#define WBM_RETRY_MS         1000U

static wifi_beacon_cb_t s_consumers[WBM_MAX_CONSUMERS] = {};
static int              s_count     = 0;
static lv_timer_t      *s_hop_timer = nullptr;
static uint8_t          s_hop_ch    = 1;
static QueueHandle_t    s_beacon_queue = nullptr;
static volatile bool    s_hw_active = false;
static volatile bool    s_lifecycle_busy = false;
static volatile bool    s_lifecycle_starting = false;
static volatile uint32_t s_queue_drops = 0;
static uint32_t         s_last_drop_report_ms = 0;
static volatile uint32_t s_next_start_ms = 0;

static void parse_and_queue(const uint8_t *frame, int len,
                            int8_t rssi, uint8_t ch)
{
    if (len < 38) return;
    if ((frame[0] & 0xFC) != 0x80) return;   // not a beacon

    uint16_t cap = frame[34] | ((uint16_t)frame[35] << 8);
    if (!(cap & 0x0001)) return;              // not an infrastructure AP

    bool has_privacy = (cap & 0x0010) != 0;
    bool has_rsn     = false;
    bool has_wpa     = false;

    WifiBeacon b = {};
    memcpy(b.bssid, frame + 16, 6);
    b.rssi    = rssi;
    b.channel = ch;

    const uint8_t *tags     = frame + 36;
    const int      tags_len = len - 36;

    for (int pos = 0; pos + 2 <= tags_len; ) {
        uint8_t id = tags[pos], tl = tags[pos + 1];
        if (pos + 2 + tl > tags_len) break;
        const uint8_t *td = tags + pos + 2;

        if (id == 0 && tl <= 32) {
            memcpy(b.ssid, td, tl);
            b.ssid[tl] = '\0';
        } else if (id == 48) {
            has_rsn = true;
        } else if (id == 221 && tl >= 4 &&
                   td[0] == 0x00 && td[1] == 0x50 && td[2] == 0xF2 && td[3] == 0x01) {
            has_wpa = true;
        }
        pos += 2 + tl;
    }

    if (has_rsn)
        snprintf(b.auth, sizeof(b.auth), "[WPA2-PSK-CCMP][ESS]");
    else if (has_wpa)
        snprintf(b.auth, sizeof(b.auth), "[WPA-PSK-CCMP+TKIP][ESS]");
    else if (has_privacy)
        snprintf(b.auth, sizeof(b.auth), "[WEP][ESS]");
    else
        snprintf(b.auth, sizeof(b.auth), "[ESS]");

    if (s_beacon_queue && xQueueSend(s_beacon_queue, &b, 0) != pdTRUE)
        ++s_queue_drops;
}

static void promisc_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (type != WIFI_PKT_MGMT) return;
    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
    parse_and_queue(pkt->payload, (int)pkt->rx_ctrl.sig_len,
                    (int8_t)pkt->rx_ctrl.rssi, (uint8_t)pkt->rx_ctrl.channel);
}

static void on_channel_hop(lv_timer_t *)
{
    s_hop_ch = (s_hop_ch % 13) + 1;
    esp_wifi_set_channel(s_hop_ch, WIFI_SECOND_CHAN_NONE);
}

static bool start_wifi_radio()
{
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    if (esp_wifi_set_promiscuous(true) != ESP_OK) {
        WiFi.mode(WIFI_OFF);
        return false;
    }
    wifi_promiscuous_filter_t filter = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT
    };
    esp_wifi_set_promiscuous_filter(&filter);
    esp_wifi_set_promiscuous_rx_cb(promisc_cb);
    s_hop_ch   = 1;
    esp_wifi_set_channel(s_hop_ch, WIFI_SECOND_CHAN_NONE);
    return true;
}

static void stop_wifi_radio()
{
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(nullptr);
    WiFi.mode(WIFI_OFF);
}

// WiFi.mode()/driver transitions can take long enough to starve LVGL on this
// board. Run those operations on a small worker task; wifi_beacon_tick() owns
// LVGL timer creation/deletion and result delivery on the normal main loop.
static void lifecycle_task(void *)
{
    const bool starting = s_lifecycle_starting;
    if (starting) {
        bool ok = start_wifi_radio();
        s_hw_active = ok;
        if (!ok) s_next_start_ms = millis() + WBM_RETRY_MS;
    } else {
        stop_wifi_radio();
        s_hw_active = false;
    }
    s_lifecycle_busy = false;
    vTaskDelete(nullptr);
}

static bool launch_lifecycle(bool starting)
{
    if (s_lifecycle_busy) return true;
    s_lifecycle_starting = starting;
    s_lifecycle_busy = true;
    BaseType_t created = xTaskCreate(lifecycle_task,
                                    starting ? "wifi_scan_on" : "wifi_scan_off",
                                    4096, nullptr, 1, nullptr);
    if (created != pdPASS) {
        s_lifecycle_busy = false;
        if (starting) s_next_start_ms = millis() + WBM_RETRY_MS;
        return false;
    }
    return true;
}

bool wifi_beacon_add(wifi_beacon_cb_t cb)
{
    if (!cb) return false;
    if (!s_beacon_queue) {
        s_beacon_queue = xQueueCreate(WBM_QUEUE_LEN, sizeof(WifiBeacon));
        if (!s_beacon_queue) return false;
    }
    for (int i = 0; i < WBM_MAX_CONSUMERS; i++)
        if (s_consumers[i] == cb) return true;   // idempotent

    for (int i = 0; i < WBM_MAX_CONSUMERS; i++) {
        if (!s_consumers[i]) {
            const bool first = s_count == 0;
            s_consumers[i] = cb;
            s_count++;
            if (first) {
                xQueueReset(s_beacon_queue);
                s_queue_drops = 0;
                s_last_drop_report_ms = millis();
                s_next_start_ms = millis();
            }
            return true;
        }
    }
    return false;  // table full
}

void wifi_beacon_remove(wifi_beacon_cb_t cb)
{
    if (!cb) return;
    for (int i = 0; i < WBM_MAX_CONSUMERS; i++) {
        if (s_consumers[i] == cb) {
            s_consumers[i] = nullptr;
            if (--s_count == 0 && s_beacon_queue)
                xQueueReset(s_beacon_queue);
            return;
        }
    }
}

void wifi_beacon_tick()
{
    const uint32_t now = millis();
    if (s_count > 0) {
        if (s_hw_active && !s_hop_timer)
            s_hop_timer = lv_timer_create(on_channel_hop, 200, nullptr);

        if (!s_hw_active && !s_lifecycle_busy &&
            (int32_t)(now - s_next_start_ms) >= 0) {
            if (!launch_lifecycle(true))
                Serial.println("[WiFi scan] could not launch startup task");
        }

        if (s_hw_active && s_beacon_queue) {
            for (int n = 0; n < WBM_RESULTS_PER_TICK; ++n) {
                WifiBeacon beacon;
                if (xQueueReceive(s_beacon_queue, &beacon, 0) != pdTRUE) break;
                // All consumer code now runs on the main loop, never in the
                // promiscuous WiFi callback.
                for (int i = 0; i < WBM_MAX_CONSUMERS; ++i)
                    if (s_consumers[i]) s_consumers[i](&beacon);
            }
        }

        uint32_t drops = s_queue_drops;
        if (drops > 0 && now - s_last_drop_report_ms >= 30000U) {
            s_last_drop_report_ms = now;
            Serial.printf("[WiFi scan] queued-beacon drops=%lu\n",
                          (unsigned long)drops);
        }
        return;
    }

    if (s_hop_timer) {
        lv_timer_del(s_hop_timer);
        s_hop_timer = nullptr;
    }
    if (s_hw_active && !s_lifecycle_busy)
        launch_lifecycle(false);
}

bool wifi_beacon_active()
{
    return s_count > 0 || s_hw_active || s_lifecycle_busy;
}
