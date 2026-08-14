#include "rolling_code_screen.h"
#include "rolling_code.h"
#include <LilyGoLib.h>
#include <lvgl.h>

static lv_obj_t *s_screen;
static lv_obj_t *s_status;
static lv_obj_t *s_details;
static lv_obj_t *s_start_btn;
static lv_obj_t *s_start_label;
static lv_obj_t *s_band_label;

static void refresh_ui()
{
    bool running = rolling_code_is_running();
    lv_label_set_text(s_start_label, running ? "STOP" : "START");
    lv_obj_set_style_bg_color(s_start_btn,
        running ? lv_color_make(0xAA, 0x22, 0x22) : lv_color_make(0x00, 0x88, 0x55),
        LV_PART_MAIN);

    if (!running && rolling_code_last_error() != 0) {
        lv_label_set_text_fmt(s_status, "Radio unavailable (%d)",
                              (int)rolling_code_last_error());
        lv_obj_set_style_text_color(s_status, lv_color_make(0xFF, 0x88, 0x44), LV_PART_MAIN);
    } else {
        lv_label_set_text_fmt(s_status, "%s | threshold %.0f dBm",
                              running ? "RECEIVE ONLY" : "Stopped",
                              (double)rolling_code_threshold());
        lv_obj_set_style_text_color(s_status,
            running ? lv_color_make(0x00, 0xCC, 0x66) : lv_color_make(0x88, 0x88, 0x88),
            LV_PART_MAIN);
    }

    lv_label_set_text_fmt(s_details,
        "%s\n\nCaptures: %lu   Unique: %lu\nRepeats: %lu   Changed: %lu\n\n"
        "Last signature: %08lX\nPulses: %u   Base: %u us\nDelta: %u%%   Peak: %d dBm\n\n"
        "A candidate means similarly timed frames changed across button presses. "
        "It does not decrypt or authenticate the protocol.\n\nLog: %s",
        rolling_code_last_verdict(),
        (unsigned long)rolling_code_capture_count(),
        (unsigned long)rolling_code_unique_count(),
        (unsigned long)rolling_code_repeat_count(),
        (unsigned long)rolling_code_changed_count(),
        (unsigned long)rolling_code_last_signature(),
        (unsigned)rolling_code_last_pulse_count(),
        (unsigned)rolling_code_last_base_us(),
        (unsigned)rolling_code_last_delta_percent(),
        (int)rolling_code_last_peak_rssi(),
        rolling_code_last_log_path()[0] ? rolling_code_last_log_path() : "SD log starts after capture");
}

static void on_start(lv_event_t *)
{
    if (rolling_code_is_running()) rolling_code_stop();
    else rolling_code_start();
    refresh_ui();
}

static void on_band(lv_event_t *)
{
    rolling_code_set_band((rolling_code_get_band() + 1U) % 2U);
    lv_label_set_text_fmt(s_band_label, "%.2f MHz", (double)rolling_code_get_frequency());
    refresh_ui();
}

static void on_reset(lv_event_t *)
{
    rolling_code_reset_session();
    refresh_ui();
}

static lv_obj_t *make_button(const char *text, int x, lv_event_cb_t cb,
                             lv_obj_t **label_out = nullptr)
{
    lv_obj_t *btn = lv_obj_create(s_screen);
    lv_obj_set_size(btn, 116, 50);
    lv_obj_align(btn, LV_ALIGN_TOP_MID, x, 92);
    lv_obj_set_style_radius(btn, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_make(0x22, 0x44, 0x66), LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *label = lv_label_create(btn);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_white(), LV_PART_MAIN);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    lv_obj_add_flag(label, LV_OBJ_FLAG_EVENT_BUBBLE);
    if (label_out) *label_out = label;
    return btn;
}

static void on_timer(lv_timer_t *)
{
    if (rolling_code_screen_is_active()) refresh_ui();
}

void rolling_code_screen_create()
{
    s_screen = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(s_screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_screen, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_screen);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_36, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
    lv_label_set_text(title, "ROLLING RX");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    lv_obj_t *subtitle = lv_label_create(s_screen);
    lv_obj_set_style_text_font(subtitle, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(subtitle, lv_color_make(0x88, 0x88, 0x88), LV_PART_MAIN);
    lv_label_set_text(subtitle, "Passive pulse timing - no transmit path");
    lv_obj_align(subtitle, LV_ALIGN_TOP_MID, 0, 57);

    s_start_btn = make_button("START", -126, on_start, &s_start_label);
    make_button("433.92 MHz", 0, on_band, &s_band_label);
    make_button("RESET", 126, on_reset);

    s_status = lv_label_create(s_screen);
    lv_obj_set_width(s_status, 380);
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, 153);

    lv_obj_t *panel = lv_obj_create(s_screen);
    lv_obj_set_size(panel, 390, 270);
    lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, 181);
    lv_obj_set_style_bg_color(panel, lv_color_make(0x08, 0x08, 0x08), LV_PART_MAIN);
    lv_obj_set_style_border_color(panel, lv_color_make(0x33, 0x33, 0x33), LV_PART_MAIN);
    lv_obj_set_style_border_width(panel, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(panel, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(panel, 10, LV_PART_MAIN);
    lv_obj_set_scroll_dir(panel, LV_DIR_VER);

    s_details = lv_label_create(panel);
    lv_obj_set_width(s_details, lv_pct(100));
    lv_label_set_long_mode(s_details, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_details, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_details, lv_color_make(0xCC, 0xCC, 0xCC), LV_PART_MAIN);

    lv_obj_t *hint = lv_label_create(s_screen);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(hint, lv_color_make(0x55, 0x55, 0x55), LV_PART_MAIN);
    lv_label_set_text(hint, "Boot button stops receiver and returns");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -8);

    lv_timer_create(on_timer, 300, nullptr);
    refresh_ui();
}

void rolling_code_screen_show()
{
    refresh_ui();
    lv_scr_load(s_screen);
}

bool rolling_code_screen_is_active() { return lv_screen_active() == s_screen; }

void rolling_code_screen_stop() { rolling_code_stop(); }
