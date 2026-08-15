#include "rolling_code.h"
#include "lora_analyze_screen.h"
#include "lora_screen.h"
#include "pager.h"
#include "tpms.h"
#include "aprs.h"
#include "usb_sd.h"
#include <LilyGoLib.h>
#include <SD.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <math.h>
#include <string.h>

// This analyzer never calls radio.transmit(). It records amplitude transitions
// from GetRssiInst and compares normalized timing fingerprints. That can reveal
// repeat-vs-changing behavior, but cannot decrypt a code or prove that a device
// uses a particular rolling-code algorithm.

#define RC_MAX_PULSES       256
#define RC_QUEUE_LEN          2
#define RC_SAMPLE_US         80U
#define RC_END_SILENCE_US 12000U
#define RC_MAX_FRAME_US   180000U
#define RC_MIN_PULSES        12
#define RC_STOP_TIMEOUT_MS  250U

// Private errors live outside RadioLib's normal range so the UI can
// distinguish lifecycle failures from radio-driver failures.
#define RC_ERR_RADIO_BUSY   (-3001)
#define RC_ERR_STOP_TIMEOUT (-3002)

struct RollingCapture {
    uint16_t duration_us[RC_MAX_PULSES];
    uint16_t count;
    uint32_t total_us;
    int8_t   peak_rssi;
};

struct Fingerprint {
    uint8_t  symbol[RC_MAX_PULSES];
    uint16_t count;
    uint16_t base_us;
    uint32_t hash;
};

static const float s_frequencies[] = { 433.92f, 868.35f };
static const char *s_band_names[]  = { "433.92 MHz", "868.35 MHz" };

static volatile bool s_running = false;
static uint8_t        s_band = 0;
static int16_t        s_last_error = 0;
static volatile float s_threshold = -127.0f;
static TaskHandle_t   s_task = nullptr;
static QueueHandle_t  s_queue = nullptr;
static SemaphoreHandle_t s_stopped = nullptr;
static volatile uint32_t s_stack_headroom = 0;

static Fingerprint s_previous = {};
static bool         s_have_previous = false;
static uint32_t     s_capture_count = 0;
static uint32_t     s_unique_count = 0;
static uint32_t     s_repeat_count = 0;
static uint32_t     s_changed_count = 0;
static uint8_t      s_changed_streak = 0;
static uint32_t     s_last_signature = 0;
static uint16_t     s_last_pulse_count = 0;
static uint16_t     s_last_base_us = 0;
static uint8_t      s_last_delta_percent = 0;
static int8_t       s_last_peak_rssi = -127;
static char         s_last_verdict[32] = "No captures yet";
static char         s_log_path[48] = "";

static uint16_t clamp_u16(uint32_t value)
{
    return value > 65535U ? 65535U : (uint16_t)value;
}

static bool another_radio_user_is_active()
{
    return lora_screen_is_powered() || lora_analyze_is_running() ||
           pager_is_running() || tpms_is_running() || aprs_is_running();
}

static uint16_t estimate_base_us(const RollingCapture &capture)
{
    uint16_t values[RC_MAX_PULSES];
    uint16_t n = 0;
    for (uint16_t i = 0; i < capture.count; ++i) {
        const uint16_t d = capture.duration_us[i];
        if (d >= RC_SAMPLE_US && d <= 5000U) values[n++] = d;
    }
    if (n == 0) return RC_SAMPLE_US;

    // Insertion sort is acceptable for <=256 entries and only runs after a
    // completed burst. The middle of the shortest quartile rejects isolated
    // sampling glitches while still estimating the protocol's short pulse.
    for (uint16_t i = 1; i < n; ++i) {
        uint16_t v = values[i];
        int j = (int)i - 1;
        while (j >= 0 && values[j] > v) {
            values[j + 1] = values[j];
            --j;
        }
        values[j + 1] = v;
    }
    uint16_t idx = n / 8U;
    if (idx >= n) idx = n - 1U;
    return values[idx];
}

static Fingerprint make_fingerprint(const RollingCapture &capture)
{
    Fingerprint fp = {};
    fp.count = capture.count;
    fp.base_us = estimate_base_us(capture);
    uint32_t hash = 2166136261UL;
    for (uint16_t i = 0; i < capture.count; ++i) {
        uint32_t ratio = (capture.duration_us[i] + fp.base_us / 2U) / fp.base_us;
        if (ratio < 1U) ratio = 1U;
        if (ratio > 15U) ratio = 15U;
        fp.symbol[i] = (uint8_t)ratio;
        hash ^= (uint8_t)(ratio | ((i & 1U) ? 0x10U : 0x00U));
        hash *= 16777619UL;
    }
    hash ^= capture.count;
    hash *= 16777619UL;
    fp.hash = hash;
    return fp;
}

static uint8_t fingerprint_delta(const Fingerprint &a, const Fingerprint &b)
{
    uint16_t max_count = a.count > b.count ? a.count : b.count;
    if (max_count == 0) return 0;
    uint16_t min_count = a.count < b.count ? a.count : b.count;
    uint16_t different = max_count - min_count;
    for (uint16_t i = 0; i < min_count; ++i) {
        int d = (int)a.symbol[i] - (int)b.symbol[i];
        if (d < 0) d = -d;
        if (d > 0) ++different;
    }
    uint32_t pct = (uint32_t)different * 100U / max_count;
    return pct > 100U ? 100U : (uint8_t)pct;
}

static bool same_timing_family(const Fingerprint &a, const Fingerprint &b)
{
    uint16_t max_count = a.count > b.count ? a.count : b.count;
    uint16_t count_gap = a.count > b.count ? a.count - b.count : b.count - a.count;
    uint16_t base_gap = a.base_us > b.base_us ? a.base_us - b.base_us : b.base_us - a.base_us;
    uint16_t base_tolerance = b.base_us / 3U;
    if (base_tolerance < 100U) base_tolerance = 100U;
    return max_count > 0 && count_gap * 100U <= max_count * 15U &&
           base_gap <= base_tolerance;
}

static void make_log_path()
{
    if (s_log_path[0]) return;
    struct tm now = {};
    instance.rtc.getDateTime(&now);
    snprintf(s_log_path, sizeof(s_log_path), "/Rolling/%04d%02d%02d_%02d%02d%02d.csv",
             now.tm_year + 1900, now.tm_mon + 1, now.tm_mday,
             now.tm_hour, now.tm_min, now.tm_sec);
}

static void log_capture(const RollingCapture &capture, const Fingerprint &fp)
{
    if (!instance.isCardReady() || usb_sd_is_running()) return;
    SD.mkdir("/Rolling");
    make_log_path();
    const bool fresh = !SD.exists(s_log_path);
    File f = SD.open(s_log_path, FILE_APPEND);
    if (!f) return;
    if (fresh) {
        f.println("# Receive-only timing observations; no replay/transmit data path");
        f.println("capture,freq_mhz,rssi_dbm,pulses,total_us,base_us,signature,delta_pct,verdict,durations_us");
    }
    f.printf("%lu,%.2f,%d,%u,%lu,%u,%08lX,%u,%s,\"",
             (unsigned long)s_capture_count, (double)s_frequencies[s_band],
             (int)capture.peak_rssi, (unsigned)capture.count,
             (unsigned long)capture.total_us, (unsigned)fp.base_us,
             (unsigned long)fp.hash, (unsigned)s_last_delta_percent,
             s_last_verdict);
    for (uint16_t i = 0; i < capture.count; ++i) {
        if (i) f.print(' ');
        f.print(capture.duration_us[i]);
    }
    f.println("\"");
    f.close();
}

static void process_capture(const RollingCapture &capture)
{
    Fingerprint fp = make_fingerprint(capture);
    ++s_capture_count;
    s_last_signature = fp.hash;
    s_last_pulse_count = fp.count;
    s_last_base_us = fp.base_us;
    s_last_peak_rssi = capture.peak_rssi;
    s_last_delta_percent = s_have_previous ? fingerprint_delta(fp, s_previous) : 0;

    if (!s_have_previous) {
        ++s_unique_count;
        s_changed_streak = 0;
        snprintf(s_last_verdict, sizeof(s_last_verdict), "Baseline captured");
    } else if (fp.hash == s_previous.hash || s_last_delta_percent <= 3U) {
        ++s_repeat_count;
        s_changed_streak = 0;
        snprintf(s_last_verdict, sizeof(s_last_verdict), "Repeated/static frame");
    } else if (same_timing_family(fp, s_previous) && s_last_delta_percent <= 60U) {
        ++s_unique_count;
        ++s_changed_count;
        if (s_changed_streak < 255U) ++s_changed_streak;
        snprintf(s_last_verdict, sizeof(s_last_verdict), "%s",
                 s_changed_streak >= 2U ? "Rolling-code candidate" : "Changed frame");
    } else {
        ++s_unique_count;
        s_changed_streak = 0;
        snprintf(s_last_verdict, sizeof(s_last_verdict), "Different signal family");
    }

    s_previous = fp;
    s_have_previous = true;
    log_capture(capture, fp);
}

static void capture_task(void *)
{
    float noise_sum = 0.0f;
    uint8_t noise_samples = 0;
    for (; noise_samples < 64 && s_running; ++noise_samples) {
        noise_sum += radio.getRSSI(false);
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    if (s_running && noise_samples)
        s_threshold = noise_sum / noise_samples + 10.0f;

    while (s_running) {
        vTaskDelay(pdMS_TO_TICKS(1));
        if (!s_running) break;
        s_stack_headroom = (uint32_t)uxTaskGetStackHighWaterMark(nullptr);
        float rssi = radio.getRSSI(false);
        if (rssi <= s_threshold) continue;

        RollingCapture capture = {};
        capture.peak_rssi = (int8_t)rssi;
        bool level = true;
        uint32_t frame_start = micros();
        uint32_t run_start = frame_start;
        uint32_t low_start = 0;

        while (s_running) {
            uint32_t sample_start = micros();
            rssi = radio.getRSSI(false);
            if (rssi > capture.peak_rssi) capture.peak_rssi = (int8_t)rssi;
            bool next_level = rssi > s_threshold;
            uint32_t now = micros();

            if (next_level != level) {
                uint32_t run_us = now - run_start;
                if (capture.count < RC_MAX_PULSES)
                    capture.duration_us[capture.count++] = clamp_u16(run_us);
                level = next_level;
                run_start = now;
                low_start = level ? 0U : now;
            } else if (!level && low_start == 0U) {
                low_start = now;
            }

            uint32_t elapsed = now - frame_start;
            bool ended = (!level && low_start && (uint32_t)(now - low_start) >= RC_END_SILENCE_US);
            if (ended || capture.count >= RC_MAX_PULSES || elapsed >= RC_MAX_FRAME_US) {
                capture.total_us = elapsed;
                if (capture.count >= RC_MIN_PULSES && s_queue)
                    xQueueSend(s_queue, &capture, 0);
                break;
            }

            uint32_t spent = micros() - sample_start;
            if (spent < RC_SAMPLE_US) delayMicroseconds(RC_SAMPLE_US - spent);
        }
    }

    // Signal completion before deleting ourselves. The controller waits for
    // this acknowledgement before touching the shared radio again, avoiding
    // a force-delete while RadioLib or SPI is mid-call.
    s_stack_headroom = (uint32_t)uxTaskGetStackHighWaterMark(nullptr);
    Serial.printf("[RollingRX] capture task stopped; stack headroom=%lu bytes\n",
                  (unsigned long)s_stack_headroom);
    s_task = nullptr;
    if (s_stopped) xSemaphoreGive(s_stopped);
    vTaskDelete(nullptr);
}

bool rolling_code_start()
{
    if (s_running) return true;
    s_last_error = 0;
    if (s_task || another_radio_user_is_active()) {
        s_last_error = RC_ERR_RADIO_BUSY;
        Serial.println("[RollingRX] start refused: shared radio is busy");
        return false;
    }

    instance.powerControl(POWER_RADIO, true);
    int16_t rc = radio.beginFSK(s_frequencies[s_band], 4.8, 5.0, 234.3,
                                10, 16, 1.6);
    if (rc == RADIOLIB_ERR_NONE) rc = radio.setEncoding(RADIOLIB_ENCODING_NRZ);
    uint8_t sync[] = { 0xD3, 0x91, 0xD3, 0x91 };
    if (rc == RADIOLIB_ERR_NONE) rc = radio.setSyncWord(sync, sizeof(sync));
    if (rc == RADIOLIB_ERR_NONE) rc = radio.setCRC(0);
    if (rc == RADIOLIB_ERR_NONE) rc = radio.fixedPacketLengthMode(64);
    if (rc == RADIOLIB_ERR_NONE) {
        // A previously stopped packet decoder may have left an ISR callback
        // installed. Rolling RX polls RSSI and must not dispatch that owner.
        radio.clearPacketReceivedAction();
        rc = radio.startReceive();
    }
    s_last_error = rc;
    if (rc != RADIOLIB_ERR_NONE) {
        Serial.printf("[RollingRX] radio initialization failed: %d\n", (int)rc);
        radio.standby();
        return false;
    }

    if (!s_queue) s_queue = xQueueCreate(RC_QUEUE_LEN, sizeof(RollingCapture));
    if (!s_stopped) s_stopped = xSemaphoreCreateBinary();
    if (!s_queue || !s_stopped) {
        s_last_error = RADIOLIB_ERR_MEMORY_ALLOCATION_FAILED;
        Serial.println("[RollingRX] start failed: queue/semaphore allocation");
        radio.standby();
        return false;
    }
    xQueueReset(s_queue);
    xSemaphoreTake(s_stopped, 0);
    s_stack_headroom = 0;
    s_running = true;
    BaseType_t created = xTaskCreatePinnedToCore(capture_task, "rolling_rx", 4096,
                                                 nullptr, 5, &s_task, 0);
    if (created != pdPASS) {
        s_running = false;
        s_task = nullptr;
        s_last_error = RADIOLIB_ERR_MEMORY_ALLOCATION_FAILED;
        Serial.println("[RollingRX] start failed: task allocation");
        radio.standby();
        return false;
    }
    Serial.printf("[RollingRX] receive-only analyzer started at %.2f MHz\n",
                  (double)s_frequencies[s_band]);
    return true;
}

static void stop_receiver()
{
    if (!s_running && !s_task) return;
    s_running = false;

    // In normal operation the task observes s_running within at most one RSSI
    // sample and acknowledges via s_stopped. If the driver itself wedges, do
    // not force-delete a task that may hold SPI state; report the timeout and
    // leave the watchdog/reboot path available instead of corrupting the bus.
    TaskHandle_t task = s_task;
    if (task && task != xTaskGetCurrentTaskHandle() && s_stopped &&
        xSemaphoreTake(s_stopped, pdMS_TO_TICKS(RC_STOP_TIMEOUT_MS)) != pdTRUE) {
        s_last_error = RC_ERR_STOP_TIMEOUT;
        Serial.println("[RollingRX] cooperative stop timed out; reboot recommended");
        return;
    }

    radio.standby();
    Serial.printf("[RollingRX] receiver stopped; error=%d\n", (int)s_last_error);
}

void rolling_code_stop() { stop_receiver(); }

void rolling_code_prepare_for_sleep() { stop_receiver(); }

bool rolling_code_is_running() { return s_running; }
int16_t rolling_code_last_error() { return s_last_error; }
const char *rolling_code_last_error_text()
{
    if (s_last_error == 0) return "";
    if (s_last_error == RC_ERR_RADIO_BUSY)
        return "Stop the other sub-GHz tool first";
    if (s_last_error == RC_ERR_STOP_TIMEOUT)
        return "Receiver stop timed out; reboot recommended";
    if (s_last_error == RADIOLIB_ERR_MEMORY_ALLOCATION_FAILED)
        return "Not enough memory to start receiver";
    return "Radio initialization failed";
}
uint32_t rolling_code_stack_headroom() { return s_stack_headroom; }

void rolling_code_set_band(uint8_t band)
{
    if (band >= sizeof(s_frequencies) / sizeof(s_frequencies[0])) return;
    if (band == s_band) return;
    bool restart = s_running;
    if (restart) rolling_code_stop();
    s_band = band;
    s_log_path[0] = '\0';
    rolling_code_reset_session();
    if (restart) rolling_code_start();
}

uint8_t rolling_code_get_band() { return s_band; }
float rolling_code_get_frequency() { return s_frequencies[s_band]; }

void rolling_code_reset_session()
{
    s_previous = {};
    s_have_previous = false;
    s_capture_count = 0;
    s_unique_count = 0;
    s_repeat_count = 0;
    s_changed_count = 0;
    s_changed_streak = 0;
    s_last_signature = 0;
    s_last_pulse_count = 0;
    s_last_base_us = 0;
    s_last_delta_percent = 0;
    s_last_peak_rssi = -127;
    snprintf(s_last_verdict, sizeof(s_last_verdict), "No captures yet");
    s_log_path[0] = '\0';
    if (s_queue) xQueueReset(s_queue);
}

void rolling_code_worker()
{
    if (!s_queue) return;
    RollingCapture capture;
    if (xQueueReceive(s_queue, &capture, 0) == pdTRUE)
        process_capture(capture);
}

uint32_t rolling_code_capture_count() { return s_capture_count; }
uint32_t rolling_code_unique_count() { return s_unique_count; }
uint32_t rolling_code_repeat_count() { return s_repeat_count; }
uint32_t rolling_code_changed_count() { return s_changed_count; }
uint32_t rolling_code_last_signature() { return s_last_signature; }
uint16_t rolling_code_last_pulse_count() { return s_last_pulse_count; }
uint16_t rolling_code_last_base_us() { return s_last_base_us; }
uint8_t rolling_code_last_delta_percent() { return s_last_delta_percent; }
int8_t rolling_code_last_peak_rssi() { return s_last_peak_rssi; }
float rolling_code_threshold() { return s_threshold; }
const char *rolling_code_last_verdict() { return s_last_verdict; }
const char *rolling_code_last_log_path() { return s_log_path; }
