#include "packet_capture.h"
#include "usb_sd.h"
#include "wifi_beacon_manager.h"
#include "wifi_radio_screen.h"
#include <Arduino.h>
#include <LilyGoLib.h>
#include <WiFi.h>
#include <SD.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

void clock_screen_get_local_time(struct tm *out);

namespace {

constexpr uint8_t  QUEUE_LEN       = 16;
constexpr uint8_t  RECENT_LEN      = 16;
constexpr uint32_t HOP_INTERVAL_MS = 250;
constexpr uint32_t FLUSH_INTERVAL_MS = 2000;

struct RawPacket {
    uint64_t timestamp_us;
    int8_t   rssi;
    uint8_t  channel;
    uint16_t original_len;
    uint16_t captured_len;
    uint8_t  bytes[PACKET_CAPTURE_SNAPLEN];
};

struct __attribute__((packed)) PcapGlobalHeader {
    uint32_t magic;
    uint16_t major;
    uint16_t minor;
    int32_t  timezone;
    uint32_t sigfigs;
    uint32_t snaplen;
    uint32_t network;
};

struct __attribute__((packed)) PcapRecordHeader {
    uint32_t ts_sec;
    uint32_t ts_usec;
    uint32_t included_len;
    uint32_t original_len;
};

static QueueHandle_t s_queue = nullptr;
static volatile bool s_running = false;
static volatile bool s_start_requested = false;
static volatile uint32_t s_callback_dropped = 0;
static volatile uint32_t s_callback_rejected = 0;
static wifi_mode_t s_previous_mode = WIFI_MODE_NULL;
static bool s_owns_wifi = false;

static uint8_t s_channel_setting = 0;
static uint8_t s_current_channel = 1;
static uint32_t s_last_hop_ms = 0;
static uint32_t s_channel_counts[13] = {};
static uint32_t s_total = 0;
static uint32_t s_processed_dropped = 0;
static uint32_t s_pps = 0;
static uint32_t s_pps_window_count = 0;
static uint32_t s_pps_window_ms = 0;
static uint16_t s_subtype_mask = 0xFFFFU;
static int8_t s_min_rssi = -127;
static bool s_mac_filter_enabled = false;
static uint8_t s_mac_filter[6] = {};
static uint32_t s_subtype_counts[16] = {};
static uint64_t s_total_bytes = 0;
static int32_t s_rssi_sum = 0;
static int8_t s_peak_rssi = -127;
static uint32_t s_session_start_ms = 0;
static uint32_t s_session_stop_ms = 0;

static PacketCaptureView s_recent[RECENT_LEN] = {};
static uint8_t s_recent_head = 0;
static uint8_t s_recent_count = 0;

static File s_pcap;
static char s_log_path[64] = "";
static uint32_t s_last_flush_ms = 0;
static uint8_t s_records_since_flush = 0;
static char s_status[64] = "Stopped";

static bool mac_equal(const uint8_t *a, const uint8_t *b)
{
    return memcmp(a, b, 6) == 0;
}

static void set_status(const char *text)
{
    strncpy(s_status, text, sizeof(s_status) - 1);
    s_status[sizeof(s_status) - 1] = '\0';
}

static void promiscuous_callback(void *buffer, wifi_promiscuous_pkt_type_t type)
{
    if (!s_running || type != WIFI_PKT_MGMT || !buffer || !s_queue) return;

    const wifi_promiscuous_pkt_t *packet =
        static_cast<const wifi_promiscuous_pkt_t *>(buffer);
    const uint16_t len = packet->rx_ctrl.sig_len;
    if (packet->rx_ctrl.rssi < s_min_rssi || len < 2) {
        ++s_callback_rejected;
        return;
    }
    const uint8_t subtype = (packet->payload[0] >> 4) & 0x0F;
    if ((s_subtype_mask & (1U << subtype)) == 0) {
        ++s_callback_rejected;
        return;
    }
    if (s_mac_filter_enabled) {
        if (len < 24 ||
            (!mac_equal(&packet->payload[4], s_mac_filter) &&
             !mac_equal(&packet->payload[10], s_mac_filter) &&
             !mac_equal(&packet->payload[16], s_mac_filter))) {
            ++s_callback_rejected;
            return;
        }
    }
    RawPacket raw = {};
    raw.timestamp_us = static_cast<uint64_t>(esp_timer_get_time());
    raw.rssi = packet->rx_ctrl.rssi;
    raw.channel = packet->rx_ctrl.channel;
    raw.original_len = packet->rx_ctrl.sig_len;
    raw.captured_len = raw.original_len > PACKET_CAPTURE_SNAPLEN
        ? PACKET_CAPTURE_SNAPLEN : raw.original_len;
    memcpy(raw.bytes, packet->payload, raw.captured_len);

    if (xQueueSend(s_queue, &raw, 0) != pdTRUE) ++s_callback_dropped;
}

static uint16_t channel_frequency(uint8_t channel)
{
    return static_cast<uint16_t>(2407U + 5U * channel);
}

static bool write_all(const void *data, size_t size)
{
    return s_pcap && s_pcap.write(static_cast<const uint8_t *>(data), size) == size;
}

static void open_pcap()
{
    s_log_path[0] = '\0';
    if (!instance.isCardReady() || usb_sd_is_running()) return;

    SD.mkdir("/Captures");
    struct tm now = {};
    clock_screen_get_local_time(&now);
    snprintf(s_log_path, sizeof(s_log_path),
             "/Captures/wifi_%04d%02d%02d_%02d%02d%02d.pcap",
             now.tm_year + 1900, now.tm_mon + 1, now.tm_mday,
             now.tm_hour, now.tm_min, now.tm_sec);

    // A rapid stop/start can land in the same RTC second. Never append a
    // second PCAP global header to an existing file.
    if (SD.exists(s_log_path)) SD.remove(s_log_path);
    s_pcap = SD.open(s_log_path, FILE_WRITE);
    if (!s_pcap) {
        s_log_path[0] = '\0';
        return;
    }

    const PcapGlobalHeader header = {
        0xa1b2c3d4U, 2, 4, 0, 0,
        static_cast<uint32_t>(PACKET_CAPTURE_SNAPLEN + 13U),
        127U // LINKTYPE_IEEE802_11_RADIOTAP
    };
    if (!write_all(&header, sizeof(header))) {
        s_pcap.close();
        s_log_path[0] = '\0';
    }
}

static void write_pcap_record(const RawPacket &raw)
{
    if (!s_pcap) return;

    // Minimal radiotap header: channel frequency/flags and antenna RSSI.
    uint8_t radiotap[13] = {};
    radiotap[2] = sizeof(radiotap);
    const uint32_t present = (1U << 3) | (1U << 5);
    memcpy(&radiotap[4], &present, sizeof(present));
    const uint16_t frequency = channel_frequency(raw.channel);
    const uint16_t flags = 0x0080U; // 2 GHz; modulation is intentionally unspecified
    memcpy(&radiotap[8], &frequency, sizeof(frequency));
    memcpy(&radiotap[10], &flags, sizeof(flags));
    radiotap[12] = static_cast<uint8_t>(raw.rssi);

    timeval tv = {};
    gettimeofday(&tv, nullptr);
    const PcapRecordHeader record = {
        static_cast<uint32_t>(tv.tv_sec),
        static_cast<uint32_t>(tv.tv_usec),
        static_cast<uint32_t>(sizeof(radiotap) + raw.captured_len),
        static_cast<uint32_t>(sizeof(radiotap) + raw.original_len)
    };
    if (!write_all(&record, sizeof(record)) ||
        !write_all(radiotap, sizeof(radiotap)) ||
        !write_all(raw.bytes, raw.captured_len)) {
        s_pcap.close();
        set_status("Running - SD write failed");
        return;
    }
    ++s_records_since_flush;
}

static void close_pcap()
{
    if (s_pcap) {
        s_pcap.flush();
        s_pcap.close();
    }
    s_records_since_flush = 0;
}

static void write_summary()
{
    if (!s_log_path[0] || usb_sd_is_running() || !instance.isCardReady()) return;
    char path[72];
    strncpy(path, s_log_path, sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';
    char *ext = strrchr(path, '.');
    if (ext) strcpy(ext, ".summary.txt");
    else strncat(path, ".summary.txt", sizeof(path) - strlen(path) - 1);
    File f = SD.open(path, FILE_WRITE);
    if (!f) return;
    char summary[512];
    packet_capture_summary_text(summary, sizeof(summary));
    f.print(summary);
    f.printf("\nPCAP: %s\nChannel setting: %s\nSubtype mask: 0x%04X\nMinimum RSSI: %d dBm\n",
             s_log_path, s_channel_setting ? String(s_channel_setting).c_str() : "hop",
             s_subtype_mask, (int)s_min_rssi);
    if (s_mac_filter_enabled)
        f.printf("MAC filter: %02X:%02X:%02X:%02X:%02X:%02X\n",
                 s_mac_filter[0], s_mac_filter[1], s_mac_filter[2],
                 s_mac_filter[3], s_mac_filter[4], s_mac_filter[5]);
    f.close();
}

static void reset_session()
{
    memset(s_channel_counts, 0, sizeof(s_channel_counts));
    memset(s_recent, 0, sizeof(s_recent));
    s_recent_head = 0;
    s_recent_count = 0;
    s_total = 0;
    s_processed_dropped = 0;
    s_callback_dropped = 0;
    s_callback_rejected = 0;
    s_pps = 0;
    s_pps_window_count = 0;
    s_pps_window_ms = millis();
    s_session_start_ms = s_pps_window_ms;
    s_session_stop_ms = 0;
    s_total_bytes = 0;
    s_rssi_sum = 0;
    s_peak_rssi = -127;
    memset(s_subtype_counts, 0, sizeof(s_subtype_counts));
    if (s_queue) xQueueReset(s_queue);
}

static void process_packet(const RawPacket &raw)
{
    PacketCaptureView view = {};
    view.sequence = ++s_total;
    view.timestamp_ms = static_cast<uint32_t>(raw.timestamp_us / 1000ULL);
    view.rssi = raw.rssi;
    view.channel = raw.channel;
    view.original_len = raw.original_len;
    view.captured_len = raw.captured_len;
    memcpy(view.bytes, raw.bytes, raw.captured_len);

    if (raw.captured_len >= 2) {
        const uint16_t frame_control =
            static_cast<uint16_t>(raw.bytes[0]) |
            (static_cast<uint16_t>(raw.bytes[1]) << 8);
        view.type = (frame_control >> 2) & 0x03;
        view.subtype = (frame_control >> 4) & 0x0F;
        ++s_subtype_counts[view.subtype];
    }
    if (raw.captured_len >= 24) {
        memcpy(view.addr1, &raw.bytes[4], 6);
        memcpy(view.addr2, &raw.bytes[10], 6);
        memcpy(view.addr3, &raw.bytes[16], 6);
    }

    s_recent[s_recent_head] = view;
    s_recent_head = (s_recent_head + 1U) % RECENT_LEN;
    if (s_recent_count < RECENT_LEN) ++s_recent_count;
    if (raw.channel >= 1 && raw.channel <= 13) ++s_channel_counts[raw.channel - 1];
    ++s_pps_window_count;
    s_total_bytes += raw.original_len;
    s_rssi_sum += raw.rssi;
    if (raw.rssi > s_peak_rssi) s_peak_rssi = raw.rssi;
    write_pcap_record(raw);
}

static bool begin_capture()
{
    s_previous_mode = WiFi.getMode();
    if (!WiFi.mode(WIFI_STA)) {
        set_status("WiFi initialization failed");
        return false;
    }
    WiFi.disconnect(false, false);

    wifi_promiscuous_filter_t filter = {};
    filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
    if (esp_wifi_set_promiscuous_filter(&filter) != ESP_OK ||
        esp_wifi_set_promiscuous_rx_cb(promiscuous_callback) != ESP_OK ||
        esp_wifi_set_channel(s_channel_setting ? s_channel_setting : 1,
                             WIFI_SECOND_CHAN_NONE) != ESP_OK ||
        esp_wifi_set_promiscuous(true) != ESP_OK) {
        esp_wifi_set_promiscuous_rx_cb(nullptr);
        esp_wifi_set_promiscuous(false);
        if (s_previous_mode == WIFI_MODE_NULL) WiFi.mode(WIFI_OFF);
        else WiFi.mode(s_previous_mode);
        set_status("Promiscuous mode failed");
        return false;
    }

    s_current_channel = s_channel_setting ? s_channel_setting : 1;
    s_last_hop_ms = millis();
    s_last_flush_ms = s_last_hop_ms;
    s_owns_wifi = true;
    s_running = true;
    open_pcap();
    set_status(s_pcap ? "Running - PCAP to SD" : "Running - RAM only");
    return true;
}

} // namespace

bool packet_capture_start()
{
    if (s_running || s_start_requested) return true;
    if (usb_sd_is_running()) {
        set_status("Unmount USB SD first");
        return false;
    }
    if (wifi_beacon_active() || WiFi.status() == WL_CONNECTED ||
        wifi_radio_screen_is_powered()) {
        set_status("WiFi radio busy");
        return false;
    }
    if (!s_queue) s_queue = xQueueCreate(QUEUE_LEN, sizeof(RawPacket));
    if (!s_queue) {
        set_status("Capture queue unavailable");
        return false;
    }
    reset_session();
    set_status("Starting...");
    s_start_requested = true;
    return true;
}

void packet_capture_stop()
{
    const bool had_active_session = s_running || s_start_requested;
    s_start_requested = false;
    if (s_running) {
        s_running = false;
        esp_wifi_set_promiscuous_rx_cb(nullptr);
        esp_wifi_set_promiscuous(false);
    }
    if (had_active_session) s_session_stop_ms = millis();
    close_pcap();
    if (had_active_session) write_summary();
    if (s_owns_wifi) {
        if (s_previous_mode == WIFI_MODE_NULL) {
            WiFi.mode(WIFI_OFF);
            esp_wifi_stop();
            esp_wifi_deinit();
        } else {
            WiFi.mode(s_previous_mode);
        }
        s_owns_wifi = false;
    }
    set_status("Stopped");
}

void packet_capture_prepare_for_sleep()
{
    packet_capture_stop();
}

void packet_capture_tick()
{
    if (s_start_requested) {
        s_start_requested = false;
        begin_capture();
    }
    if (!s_running) return;
    if (usb_sd_is_running()) {
        packet_capture_stop();
        set_status("Stopped - USB SD active");
        return;
    }

    const uint32_t now = millis();
    if (s_channel_setting == 0 && now - s_last_hop_ms >= HOP_INTERVAL_MS) {
        s_current_channel = (s_current_channel % 13U) + 1U;
        esp_wifi_set_channel(s_current_channel, WIFI_SECOND_CHAN_NONE);
        s_last_hop_ms = now;
    }

    RawPacket raw = {};
    // A bounded batch keeps SD traffic from monopolizing a UI iteration.
    for (uint8_t i = 0; i < 4 && xQueueReceive(s_queue, &raw, 0) == pdTRUE; ++i)
        process_packet(raw);

    s_processed_dropped = s_callback_dropped;
    if (now - s_pps_window_ms >= 1000U) {
        const uint32_t elapsed = now - s_pps_window_ms;
        s_pps = elapsed ? s_pps_window_count * 1000U / elapsed : 0;
        s_pps_window_count = 0;
        s_pps_window_ms = now;
    }
    if (s_pcap && (s_records_since_flush >= 16 ||
                   now - s_last_flush_ms >= FLUSH_INTERVAL_MS)) {
        s_pcap.flush();
        s_records_since_flush = 0;
        s_last_flush_ms = now;
    }
}

bool packet_capture_is_running() { return s_running; }
bool packet_capture_is_starting() { return s_start_requested; }
const char *packet_capture_status_text() { return s_status; }

void packet_capture_set_subtype_mask(uint16_t mask) { s_subtype_mask = mask ? mask : 0xFFFFU; }
uint16_t packet_capture_subtype_mask() { return s_subtype_mask; }
void packet_capture_set_min_rssi(int8_t rssi) { s_min_rssi = rssi < -127 ? -127 : rssi; }
int8_t packet_capture_min_rssi() { return s_min_rssi; }
void packet_capture_set_mac_filter(const uint8_t mac[6])
{
    if (!mac) return;
    memcpy(s_mac_filter, mac, 6);
    s_mac_filter_enabled = true;
}
void packet_capture_clear_mac_filter() { s_mac_filter_enabled = false; }
bool packet_capture_mac_filter(uint8_t out[6])
{
    if (!s_mac_filter_enabled) return false;
    if (out) memcpy(out, s_mac_filter, 6);
    return true;
}

void packet_capture_set_channel(uint8_t channel)
{
    if (channel > 13) channel = 0;
    s_channel_setting = channel;
    if (!s_running) return;
    s_current_channel = channel ? channel : 1;
    esp_wifi_set_channel(s_current_channel, WIFI_SECOND_CHAN_NONE);
    s_last_hop_ms = millis();
}

uint8_t packet_capture_get_channel() { return s_channel_setting; }
uint8_t packet_capture_current_channel() { return s_current_channel; }
uint32_t packet_capture_total() { return s_total; }
uint32_t packet_capture_dropped() { return s_processed_dropped; }
uint32_t packet_capture_packets_per_second() { return s_pps; }
uint32_t packet_capture_rejected() { return s_callback_rejected; }

uint32_t packet_capture_channel_count(uint8_t channel)
{
    return channel >= 1 && channel <= 13 ? s_channel_counts[channel - 1] : 0;
}

int packet_capture_recent_count() { return s_recent_count; }

bool packet_capture_get_recent(int newest_index, PacketCaptureView *out)
{
    if (!out || newest_index < 0 || newest_index >= s_recent_count) return false;
    int index = static_cast<int>(s_recent_head) - 1 - newest_index;
    while (index < 0) index += RECENT_LEN;
    *out = s_recent[index];
    return true;
}

const char *packet_capture_log_path() { return s_log_path; }

void packet_capture_summary_text(char *out, size_t size)
{
    if (!out || size == 0) return;
    const uint32_t end = s_session_stop_ms ? s_session_stop_ms : millis();
    const uint32_t duration = s_session_start_ms ? (end - s_session_start_ms) / 1000U : 0;
    const int avg = s_total ? s_rssi_sum / (int32_t)s_total : 0;
    snprintf(out, size,
        "Capture summary\nDuration: %lu s\nAccepted: %lu  filtered: %lu  dropped: %lu\n"
        "Bytes: %llu  RSSI avg/peak: %d/%d dBm\n"
        "Frames: beacon %lu probe-req %lu probe-resp %lu auth %lu deauth %lu",
        (unsigned long)duration, (unsigned long)s_total,
        (unsigned long)s_callback_rejected, (unsigned long)s_processed_dropped,
        (unsigned long long)s_total_bytes, avg, (int)s_peak_rssi,
        (unsigned long)s_subtype_counts[8], (unsigned long)s_subtype_counts[4],
        (unsigned long)s_subtype_counts[5], (unsigned long)s_subtype_counts[11],
        (unsigned long)s_subtype_counts[12]);
}
