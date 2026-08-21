#include "lora_analyze_screen.h"
#include "bt_analyze_screen.h"
#include "lora_screen.h"
#include "pager.h"
#include "tpms.h"
#include "aprs.h"
#include "usb_sd.h"
#include <LilyGoLib.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include <stdio.h>
#include <string.h>

// Defined in tools_screen.cpp / main.cpp.
void tools_screen_show();
void clock_screen_get_local_time(struct tm *out);

// This is an SDR-style swept-energy display, not an I/Q receiver. The SX1262
// visits one bin at a time and reports instantaneous RSSI. Thirty-two bins are
// dense enough to locate activity while keeping a complete sweep responsive.
#define N_BINS             32
#define BAR_W               9
#define BAR_GAP             2
#define CHART_TOP_Y       270
#define CHART_BOTTOM_Y    350
#define WATERFALL_W       352
#define WATERFALL_H       142
#define WATERFALL_Y        99
#define WATERFALL_ROW_H     3
#define UI_TIMER_MS        20
#define START_PAINT_MS    100
#define RSSI_FLOOR       (-130)
#define RSSI_CEIL         (-30)

struct Band {
    const char *label;
    const char *short_label;
    float start_mhz;
    float stop_mhz;
    float bandwidth_khz;
};

// The chip accepts 150-960 MHz, but the PCB matching network and antenna are
// optimized for the purchased watch variant. Off-band measurements therefore
// remain useful as indications, not calibrated sensitivity measurements.
static const Band s_bands[] = {
    { "US ISM 902-928", "915", 902.0f, 928.0f, 500.0f },
    { "EU ISM 863-870", "868", 863.0f, 870.0f, 250.0f },
    { "ISM 433.0-434.8", "433", 433.0f, 434.8f,  62.5f },
    { "315 MHz (off-band)", "315", 314.5f, 315.5f, 62.5f },
};
static const int N_BANDS = sizeof(s_bands) / sizeof(s_bands[0]);

static const uint16_t SPEED_HOP_MS[] = { 80, 40, 25 };
static const char *SPEED_NAMES[] = { "ECO", "NORM", "FAST" };

// ---- UI -------------------------------------------------------------------

static lv_obj_t *screen;
static lv_obj_t *status_label;
static lv_obj_t *legend_label;
static lv_obj_t *waterfall;
static lv_obj_t *waterfall_error_label;
static lv_obj_t *axis_start_label;
static lv_obj_t *axis_center_label;
static lv_obj_t *axis_stop_label;
static lv_obj_t *band_btn;
static lv_obj_t *band_btn_label;
static lv_obj_t *speed_btn_label;
static lv_obj_t *log_btn;
static lv_obj_t *log_btn_label;
static lv_obj_t *bars[N_BINS];
static lv_obj_t *peak_marks[N_BINS];
static int bar_x[N_BINS];

static void *s_waterfall_buffer = nullptr;
static uint32_t s_waterfall_stride = 0;

// ---- sweep state ----------------------------------------------------------

static int16_t  s_rssi[N_BINS];
static int16_t  s_peak[N_BINS];
static int      s_band = 0;
static uint8_t  s_speed = 1;
static int      s_cur_bin = 0;
static int      s_strongest_bin = 0;
static int8_t   s_strongest_rssi = -127;
static bool     s_running = false;
static bool     s_start_requested = false;
static bool     s_start_error = false;
static bool     s_direct_mode = false;
static int16_t  s_last_radio_err = 0;
static uint32_t s_hop_start_ms = 0;
static uint32_t s_start_not_before_ms = 0;
static uint8_t  s_ui_divider = 0;
static uint32_t s_sweep_count = 0;

// Snapshot of the prior SX1262 consumer. This is also restored when radio
// initialization fails; the old implementation only restored after success.
static bool      s_snapshot_valid = false;
static bool      s_prev_pager_running = false;
static float     s_prev_pager_freq = 0.0f;
static PagerMode s_prev_pager_mode = PAGER_POCSAG_1200;
static bool      s_prev_pager_scan_all = false;
static bool      s_prev_tpms_running = false;
static bool      s_prev_tpms_433 = true;
static bool      s_prev_aprs_running = false;

// ---- optional CSV logging -------------------------------------------------

static bool     s_logging = false;
static bool     s_log_header_needed = false;
static bool     s_log_pending = false;
static bool     s_log_error = false;
static uint32_t s_log_drops = 0;
static char     s_log_path[72] = {};
static int16_t  s_log_rssi[N_BINS];
static int      s_log_band = 0;
static uint32_t s_log_sweep = 0;

// ---- helpers --------------------------------------------------------------

static float freq_of_bin(int band, int bin)
{
    const Band &b = s_bands[band];
    return b.start_mhz + (b.stop_mhz - b.start_mhz) *
           (float)bin / (float)(N_BINS - 1);
}

static lv_color_t rssi_color(int r)
{
    if (r > -50)  return lv_color_make(0xFF, 0x35, 0x35);
    if (r > -65)  return lv_color_make(0xFF, 0x82, 0x2E);
    if (r > -80)  return lv_color_make(0xFF, 0xD0, 0x18);
    if (r > -95)  return lv_color_make(0x35, 0xD0, 0x58);
    if (r > -110) return lv_color_make(0x18, 0xB8, 0xD8);
    if (r > -122) return lv_color_make(0x24, 0x58, 0xA0);
    return lv_color_make(0x08, 0x10, 0x20);
}

static int value_y(int rssi)
{
    if (rssi < RSSI_FLOOR) rssi = RSSI_FLOOR;
    if (rssi > RSSI_CEIL)  rssi = RSSI_CEIL;
    const int chart_h = CHART_BOTTOM_Y - CHART_TOP_Y;
    return CHART_BOTTOM_Y -
           (rssi - RSSI_FLOOR) * chart_h / (RSSI_CEIL - RSSI_FLOOR);
}

static void reset_peaks()
{
    for (int i = 0; i < N_BINS; ++i) s_peak[i] = RSSI_FLOOR;
    s_strongest_bin = 0;
    s_strongest_rssi = -127;
}

static void reset_data()
{
    for (int i = 0; i < N_BINS; ++i) s_rssi[i] = RSSI_FLOOR;
    reset_peaks();
    s_cur_bin = 0;
    s_sweep_count = 0;
}

static void clear_waterfall()
{
    if (!waterfall || !s_waterfall_buffer) return;
    memset(s_waterfall_buffer, 0, s_waterfall_stride * WATERFALL_H);
    lv_obj_invalidate(waterfall);
}

static void append_waterfall_row()
{
    if (!waterfall || !s_waterfall_buffer) return;
    uint8_t *data = static_cast<uint8_t *>(s_waterfall_buffer);
    const size_t row_bytes = s_waterfall_stride * WATERFALL_ROW_H;
    memmove(data + row_bytes, data,
            s_waterfall_stride * (WATERFALL_H - WATERFALL_ROW_H));

    for (int y = 0; y < WATERFALL_ROW_H; ++y) {
        uint16_t *row = reinterpret_cast<uint16_t *>(data + y * s_waterfall_stride);
        for (int bin = 0; bin < N_BINS; ++bin) {
            const int x0 = bin * WATERFALL_W / N_BINS;
            const int x1 = (bin + 1) * WATERFALL_W / N_BINS;
            const uint16_t color = lv_color_to_u16(rssi_color(s_rssi[bin]));
            for (int x = x0; x < x1; ++x) row[x] = color;
        }
    }
    lv_obj_invalidate(waterfall);
}

static void update_axis_labels()
{
    const Band &b = s_bands[s_band];
    lv_label_set_text_fmt(axis_start_label, "%.3f", (double)b.start_mhz);
    lv_label_set_text_fmt(axis_center_label, "%.3f", (double)((b.start_mhz + b.stop_mhz) * 0.5f));
    lv_label_set_text_fmt(axis_stop_label, "%.3f MHz", (double)b.stop_mhz);
}

static void update_bars()
{
    for (int i = 0; i < N_BINS; ++i) {
        int y = value_y(s_rssi[i]);
        int h = CHART_BOTTOM_Y - y;
        if (h < 2) h = 2;
        lv_obj_set_pos(bars[i], bar_x[i], CHART_BOTTOM_Y - h);
        lv_obj_set_height(bars[i], h);
        lv_obj_set_style_bg_color(bars[i], rssi_color(s_rssi[i]), LV_PART_MAIN);

        int py = value_y(s_peak[i]);
        lv_obj_set_pos(peak_marks[i], bar_x[i], py);
        if (s_peak[i] <= RSSI_FLOOR)
            lv_obj_add_flag(peak_marks[i], LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_clear_flag(peak_marks[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void refresh_log_button()
{
    if (!log_btn) return;
    lv_label_set_text(log_btn_label, s_logging ? "LOG ON" : "LOG");
    lv_obj_set_style_bg_color(log_btn,
        s_logging ? lv_color_make(0x00, 0x70, 0x38)
                  : lv_color_make(0x22, 0x2A, 0x34), LV_PART_MAIN);
}

static void update_status()
{
    if (s_log_error) {
        lv_label_set_text(status_label, "Logging unavailable - check SD / USB SD");
        lv_obj_set_style_text_color(status_label,
            lv_color_make(0xFF, 0xAA, 0x33), LV_PART_MAIN);
        return;
    }
    if (s_start_requested) {
        lv_label_set_text(status_label, "Starting receiver...");
        lv_obj_set_style_text_color(status_label,
            lv_color_make(0xCC, 0xCC, 0xCC), LV_PART_MAIN);
        return;
    }
    if (s_start_error || !s_running) {
        if (lora_screen_is_powered())
            lv_label_set_text(status_label, "Radio in use - stop Meshtastic first");
        else if (s_last_radio_err)
            lv_label_set_text_fmt(status_label, "Radio error %d", (int)s_last_radio_err);
        else
            lv_label_set_text(status_label, "Stopped");
        lv_obj_set_style_text_color(status_label,
            s_start_error ? lv_color_make(0xFF, 0x77, 0x44)
                          : lv_color_make(0x88, 0x88, 0x88), LV_PART_MAIN);
        return;
    }

    if (s_strongest_rssi > -127) {
        lv_label_set_text_fmt(status_label, "PEAK %d dBm @ %.3f MHz",
            (int)s_strongest_rssi,
            (double)freq_of_bin(s_band, s_strongest_bin));
        lv_obj_set_style_text_color(status_label,
            lv_color_make(0x44, 0xDD, 0x77), LV_PART_MAIN);
    } else {
        lv_label_set_text(status_label, s_bands[s_band].label);
    }
}

static void update_legend()
{
    if (!s_running) {
        lv_label_set_text(legend_label, "receive-only swept RSSI");
        return;
    }
    lv_label_set_text_fmt(legend_label,
        "NOW %.3f MHz  %s %u ms/bin  BW %.1f kHz",
        (double)freq_of_bin(s_band, s_cur_bin), SPEED_NAMES[s_speed],
        (unsigned)SPEED_HOP_MS[s_speed],
        (double)s_bands[s_band].bandwidth_khz);
}

static void queue_log_sweep()
{
    if (!s_logging) return;
    if (s_log_pending) ++s_log_drops;
    memcpy(s_log_rssi, s_rssi, sizeof(s_log_rssi));
    s_log_band = s_band;
    s_log_sweep = s_sweep_count;
    s_log_pending = true;
}

// ---- radio lifecycle ------------------------------------------------------

static void restore_previous_owner()
{
    if (!s_snapshot_valid) return;
    if (s_prev_pager_running) {
        if (s_prev_pager_scan_all) pager_start_scanner(s_prev_pager_mode);
        else                       pager_start(s_prev_pager_freq, s_prev_pager_mode);
    }
    if (s_prev_tpms_running) tpms_start(s_prev_tpms_433);
    if (s_prev_aprs_running) aprs_start();

    s_prev_pager_running = false;
    s_prev_tpms_running = false;
    s_prev_aprs_running = false;
    s_snapshot_valid = false;
}

static bool start_analysis()
{
    if (s_running) return true;
    s_start_error = false;
    s_last_radio_err = 0;
    if (lora_screen_is_powered()) {
        s_start_error = true;
        return false;
    }

    s_prev_pager_running = pager_is_running();
    s_prev_pager_freq = pager_get_freq();
    s_prev_pager_mode = pager_get_mode();
    s_prev_pager_scan_all = pager_is_scanning_all();
    s_prev_tpms_running = tpms_is_running();
    s_prev_tpms_433 = tpms_is_freq_433();
    s_prev_aprs_running = aprs_is_running();
    s_snapshot_valid = true;

    pager_stop();
    tpms_stop();
    aprs_stop();
    instance.powerControl(POWER_RADIO, true);

    const Band &b = s_bands[s_band];
    int16_t rc = radio.begin(freq_of_bin(s_band, 0), b.bandwidth_khz,
                             7, 5, 0x12, 14, 8, 1.6f);
    s_last_radio_err = rc;
    if (rc == RADIOLIB_ERR_NONE) rc = radio.startReceive();
    s_last_radio_err = rc;
    if (rc != RADIOLIB_ERR_NONE) {
        s_start_error = true;
        restore_previous_owner();
        return false;
    }

    reset_data();
    clear_waterfall();
    s_hop_start_ms = millis();
    s_running = true;
    return true;
}

static void stop_analysis()
{
    s_start_requested = false;
    s_logging = false;
    refresh_log_button();
    if (s_running) {
        s_running = false;
        radio.standby();
    }
    restore_previous_owner();
}

static int8_t read_rssi()
{
    float r = radio.getRSSI(false);
    if (r > 0) r = 0;
    if (r < -127) r = -127;
    return (int8_t)r;
}

static bool hop_one()
{
    int8_t r = read_rssi();
    s_rssi[s_cur_bin] = r;
    if (r > s_peak[s_cur_bin]) s_peak[s_cur_bin] = r;
    if (r > s_strongest_rssi) {
        s_strongest_rssi = r;
        s_strongest_bin = s_cur_bin;
    }

    ++s_cur_bin;
    const bool sweep_complete = s_cur_bin >= N_BINS;
    if (sweep_complete) {
        s_cur_bin = 0;
        ++s_sweep_count;
        append_waterfall_row();
        queue_log_sweep();
        update_bars();
        update_status();
    }

    int16_t rc = radio.setFrequency(freq_of_bin(s_band, s_cur_bin), true);
    if (rc == RADIOLIB_ERR_NONE) rc = radio.startReceive();
    if (rc != RADIOLIB_ERR_NONE) {
        s_last_radio_err = rc;
        s_start_error = true;
        stop_analysis();
    }
    return sweep_complete;
}

// ---- events ---------------------------------------------------------------

static void on_timer(lv_timer_t *)
{
    if (!lora_analyze_screen_is_active()) return;

    uint32_t now = millis();
    if (s_start_requested && (int32_t)(now - s_start_not_before_ms) >= 0) {
        s_start_requested = false;
        start_analysis();
        update_status();
        update_legend();
    }
    if (s_running && now - s_hop_start_ms >= SPEED_HOP_MS[s_speed]) {
        s_hop_start_ms = now;
        hop_one();
    }
    if (++s_ui_divider >= 5) {
        s_ui_divider = 0;
        update_legend();
    }
}

static void on_band(lv_event_t *)
{
    s_band = (s_band + 1) % N_BANDS;
    lv_label_set_text(band_btn_label, s_bands[s_band].short_label);
    reset_data();
    clear_waterfall();
    update_axis_labels();
    update_bars();

    if (s_running) {
        radio.standby();
        int16_t rc = radio.setBandwidth(s_bands[s_band].bandwidth_khz);
        if (rc == RADIOLIB_ERR_NONE)
            rc = radio.setFrequency(freq_of_bin(s_band, 0));
        if (rc == RADIOLIB_ERR_NONE) rc = radio.startReceive();
        if (rc != RADIOLIB_ERR_NONE) {
            s_last_radio_err = rc;
            s_start_error = true;
            stop_analysis();
        }
        s_hop_start_ms = millis();
    }
    update_status();
    update_legend();
}

static void on_speed(lv_event_t *)
{
    s_speed = (uint8_t)((s_speed + 1U) % 3U);
    lv_label_set_text(speed_btn_label, SPEED_NAMES[s_speed]);
    update_legend();
}

static void on_peak_reset(lv_event_t *)
{
    reset_peaks();
    update_bars();
    update_status();
}

static void on_log(lv_event_t *)
{
    s_log_error = false;
    if (s_logging) {
        s_logging = false;
        refresh_log_button();
        update_status();
        return;
    }
    if (!instance.isCardReady() || usb_sd_is_running()) {
        s_log_error = true;
        refresh_log_button();
        update_status();
        return;
    }

    struct tm t;
    clock_screen_get_local_time(&t);
    snprintf(s_log_path, sizeof(s_log_path),
        "/Spectrum/%04d%02d%02d_%02d%02d%02d.csv",
        t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
        t.tm_hour, t.tm_min, t.tm_sec);
    s_log_header_needed = true;
    s_log_pending = false;
    s_log_drops = 0;
    s_logging = true;
    refresh_log_button();
    update_status();
}

static void on_gesture(lv_event_t *e)
{
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_event_get_indev(e));
    if (dir != LV_DIR_RIGHT) return;
    stop_analysis();
    if (s_direct_mode) tools_screen_show();
    else               bt_analyze_screen_show();
}

// ---- layout ---------------------------------------------------------------

static lv_obj_t *make_control(const char *text, int x, lv_event_cb_t cb,
                              lv_obj_t **label_out = nullptr)
{
    lv_obj_t *button = lv_obj_create(screen);
    lv_obj_set_size(button, 82, 42);
    lv_obj_set_pos(button, x, 411);
    lv_obj_set_style_radius(button, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, lv_color_make(0x22, 0x2A, 0x34), LV_PART_MAIN);
    lv_obj_set_style_border_color(button, lv_color_make(0x44, 0x66, 0x88), LV_PART_MAIN);
    lv_obj_set_style_border_width(button, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_all(button, 0, LV_PART_MAIN);
    lv_obj_clear_flag(button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *label = lv_label_create(button);
    lv_obj_set_style_text_color(label, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    lv_obj_add_flag(label, LV_OBJ_FLAG_EVENT_BUBBLE);
    if (label_out) *label_out = label;
    return button;
}

void lora_analyze_screen_create()
{
    screen = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(screen, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(screen, 0, LV_PART_MAIN);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(screen);
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_32, LV_PART_MAIN);
    lv_label_set_text(title, "SPECTRUM");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 6);

    status_label = lv_label_create(screen);
    lv_obj_set_width(status_label, 390);
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_label_set_text(status_label, "Starting receiver...");
    lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 46);

    legend_label = lv_label_create(screen);
    lv_obj_set_width(legend_label, 390);
    lv_obj_set_style_text_align(legend_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(legend_label, lv_color_make(0x88, 0x99, 0xAA), LV_PART_MAIN);
    lv_obj_set_style_text_font(legend_label, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_label_set_text(legend_label, "receive-only swept RSSI");
    lv_obj_align(legend_label, LV_ALIGN_TOP_MID, 0, 70);

    waterfall = lv_canvas_create(screen);
    s_waterfall_stride = lv_draw_buf_width_to_stride(WATERFALL_W, LV_COLOR_FORMAT_RGB565);
    const size_t waterfall_bytes = s_waterfall_stride * WATERFALL_H;
    s_waterfall_buffer = heap_caps_aligned_calloc(
        LV_DRAW_BUF_ALIGN, 1, waterfall_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_waterfall_buffer)
        s_waterfall_buffer = heap_caps_aligned_calloc(
            LV_DRAW_BUF_ALIGN, 1, waterfall_bytes, MALLOC_CAP_8BIT);
    if (s_waterfall_buffer) {
        lv_canvas_set_buffer(waterfall, s_waterfall_buffer,
                             WATERFALL_W, WATERFALL_H, LV_COLOR_FORMAT_RGB565);
        lv_obj_set_style_border_color(waterfall, lv_color_make(0x33, 0x55, 0x77), LV_PART_MAIN);
        lv_obj_set_style_border_width(waterfall, 1, LV_PART_MAIN);
        lv_obj_set_pos(waterfall, (410 - WATERFALL_W) / 2, WATERFALL_Y);
    } else {
        lv_obj_set_size(waterfall, WATERFALL_W, WATERFALL_H);
        lv_obj_set_pos(waterfall, (410 - WATERFALL_W) / 2, WATERFALL_Y);
        waterfall_error_label = lv_label_create(screen);
        lv_obj_set_style_text_color(waterfall_error_label,
            lv_color_make(0xFF, 0x77, 0x44), LV_PART_MAIN);
        lv_label_set_text(waterfall_error_label, "Waterfall memory unavailable");
        lv_obj_align(waterfall_error_label, LV_ALIGN_TOP_MID, 0, WATERFALL_Y + 58);
    }

    const int total_w = N_BINS * BAR_W + (N_BINS - 1) * BAR_GAP;
    const int start_x = (410 - total_w) / 2;
    for (int i = 0; i < N_BINS; ++i) {
        bar_x[i] = start_x + i * (BAR_W + BAR_GAP);
        bars[i] = lv_obj_create(screen);
        lv_obj_set_size(bars[i], BAR_W, 2);
        lv_obj_set_style_bg_color(bars[i], rssi_color(RSSI_FLOOR), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(bars[i], LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(bars[i], 0, LV_PART_MAIN);
        lv_obj_set_style_radius(bars[i], 1, LV_PART_MAIN);
        lv_obj_set_style_pad_all(bars[i], 0, LV_PART_MAIN);
        lv_obj_clear_flag(bars[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_pos(bars[i], bar_x[i], CHART_BOTTOM_Y - 2);

        peak_marks[i] = lv_obj_create(screen);
        lv_obj_set_size(peak_marks[i], BAR_W, 2);
        lv_obj_set_style_bg_color(peak_marks[i], lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_border_width(peak_marks[i], 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(peak_marks[i], 0, LV_PART_MAIN);
        lv_obj_clear_flag(peak_marks[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(peak_marks[i], LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_t *baseline = lv_obj_create(screen);
    lv_obj_set_size(baseline, total_w, 1);
    lv_obj_set_pos(baseline, start_x, CHART_BOTTOM_Y);
    lv_obj_set_style_bg_color(baseline, lv_color_make(0x55, 0x66, 0x77), LV_PART_MAIN);
    lv_obj_set_style_border_width(baseline, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(baseline, 0, LV_PART_MAIN);
    lv_obj_clear_flag(baseline, LV_OBJ_FLAG_SCROLLABLE);

    axis_start_label = lv_label_create(screen);
    axis_center_label = lv_label_create(screen);
    axis_stop_label = lv_label_create(screen);
    lv_obj_t *axis_labels[] = { axis_start_label, axis_center_label, axis_stop_label };
    for (lv_obj_t *label : axis_labels) {
        lv_obj_set_style_text_color(label, lv_color_make(0x88, 0x99, 0xAA), LV_PART_MAIN);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_12, LV_PART_MAIN);
    }
    lv_obj_align(axis_start_label, LV_ALIGN_TOP_LEFT, start_x, 356);
    lv_obj_align(axis_center_label, LV_ALIGN_TOP_MID, 0, 356);
    lv_obj_align(axis_stop_label, LV_ALIGN_TOP_RIGHT, -start_x, 356);
    update_axis_labels();

    lv_obj_t *hint = lv_label_create(screen);
    lv_obj_set_style_text_color(hint, lv_color_make(0x66, 0x77, 0x88), LV_PART_MAIN);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_label_set_text(hint, "receive-only  |  white ticks = peak hold");
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 384);

    band_btn = make_control(s_bands[s_band].short_label, 25, on_band, &band_btn_label);
    make_control(SPEED_NAMES[s_speed], 118, on_speed, &speed_btn_label);
    make_control("PEAK CLR", 211, on_peak_reset);
    log_btn = make_control("LOG", 304, on_log, &log_btn_label);

    lv_obj_t *back_hint = lv_label_create(screen);
    lv_obj_set_style_text_color(back_hint, lv_color_make(0x55, 0x66, 0x77), LV_PART_MAIN);
    lv_obj_set_style_text_font(back_hint, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_label_set_text(back_hint, "swipe right / BOOT to exit");
    lv_obj_align(back_hint, LV_ALIGN_BOTTOM_MID, 0, -14);

    lv_obj_add_event_cb(screen, on_gesture, LV_EVENT_GESTURE, nullptr);
    lv_timer_create(on_timer, UI_TIMER_MS, nullptr);
    reset_data();
    update_bars();
}

static void show_common(bool direct)
{
    s_direct_mode = direct;
    s_log_error = false;
    s_start_error = false;
    s_start_requested = true;
    s_start_not_before_ms = millis() + START_PAINT_MS;
    update_status();
    update_legend();
    refresh_log_button();
    lv_scr_load(screen);
}

void lora_analyze_screen_show() { show_common(false); }
void lora_analyze_screen_show_direct() { show_common(true); }

bool lora_analyze_screen_is_active() { return lv_screen_active() == screen; }
bool lora_analyze_is_running() { return s_running; }

void lora_analyze_screen_stop() { stop_analysis(); }

void lora_analyze_bg_tick()
{
    if (!s_log_pending) return;
    if (!instance.isCardReady() || usb_sd_is_running()) {
        s_log_pending = false;
        s_logging = false;
        s_log_error = true;
        refresh_log_button();
        return;
    }

    if (!SD.exists("/Spectrum")) SD.mkdir("/Spectrum");
    if (s_log_header_needed) {
        File header = SD.open(s_log_path, FILE_WRITE);
        if (!header) {
            s_log_pending = false;
            s_logging = false;
            s_log_error = true;
            refresh_log_button();
            return;
        }
        header.print("timestamp,sweep,band,start_mhz,stop_mhz,step_mhz");
        for (int i = 0; i < N_BINS; ++i) header.printf(",rssi_%02d", i);
        header.print("\n");
        header.close();
        s_log_header_needed = false;
    }

    File f = SD.open(s_log_path, FILE_APPEND);
    if (!f) {
        s_log_pending = false;
        s_logging = false;
        s_log_error = true;
        refresh_log_button();
        return;
    }
    struct tm t;
    clock_screen_get_local_time(&t);
    const Band &b = s_bands[s_log_band];
    f.printf("%04d-%02d-%02dT%02d:%02d:%02d,%lu,%s,%.4f,%.4f,%.6f",
        t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
        t.tm_hour, t.tm_min, t.tm_sec,
        (unsigned long)s_log_sweep, b.short_label,
        (double)b.start_mhz, (double)b.stop_mhz,
        (double)((b.stop_mhz - b.start_mhz) / (N_BINS - 1)));
    for (int i = 0; i < N_BINS; ++i) f.printf(",%d", (int)s_log_rssi[i]);
    f.print("\n");
    f.close();
    s_log_pending = false;
}
