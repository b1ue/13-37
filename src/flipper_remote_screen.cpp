#include "flipper_remote_screen.h"
#include "flipper_remote.h"
#include "flipper.h"
#include "theme.h"

#include <LilyGoLib.h>
#include <stdio.h>

void tools_screen_show();
void main_loop_request_lvgl_priority(int cycles);

static lv_obj_t *s_screen = nullptr;
static lv_obj_t *s_status = nullptr;
static lv_obj_t *s_device = nullptr;
static lv_obj_t *s_scan_switch = nullptr;
static lv_obj_t *s_connect_btn = nullptr;
static lv_obj_t *s_connect_label = nullptr;
static lv_obj_t *s_remote_buttons[7] = {};
static lv_obj_t *s_pair_panel = nullptr;
static lv_obj_t *s_pair_code = nullptr;
static bool s_switch_refresh = false;

static lv_obj_t *make_button(lv_obj_t *parent, const char *text,
                             int w, int h, lv_color_t color)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_bg_color(btn, color, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(btn, lv_color_make(0x55, 0x55, 0x66), LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(btn, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_center(label);
    lv_obj_add_flag(label, LV_OBJ_FLAG_EVENT_BUBBLE);
    return btn;
}

static void set_remote_enabled(bool enabled)
{
    for (lv_obj_t *button : s_remote_buttons) {
        if (!button) continue;
        if (enabled) lv_obj_clear_state(button, LV_STATE_DISABLED);
        else         lv_obj_add_state(button, LV_STATE_DISABLED);
    }
}

static void refresh_device()
{
    FlipperDeviceInfo info;
    if (!flipper_get_last_device(&info)) {
        lv_label_set_text(s_device,
            "No Flipper seen yet\nEnable detector and keep Flipper Bluetooth on");
        return;
    }
    uint32_t age_s = (millis() - info.seen_ms) / 1000U;
    lv_label_set_text_fmt(s_device,
        "%s  %d dBm  %lus\n%02X:%02X:%02X:%02X:%02X:%02X",
        info.name, (int)info.rssi, (unsigned long)age_s,
        info.mac[0], info.mac[1], info.mac[2],
        info.mac[3], info.mac[4], info.mac[5]);
}

static void refresh_ui()
{
    lv_label_set_text(s_status, flipper_remote_status());
    FlipperRemoteState state = flipper_remote_state();
    bool idle = state == FLIPPER_REMOTE_IDLE || state == FLIPPER_REMOTE_ERROR;
    lv_label_set_text(s_connect_label, idle ? "CONNECT" : "DISCONNECT");
    lv_obj_set_style_bg_color(s_connect_btn,
        idle ? theme_accent_dark() : lv_color_make(0x99, 0x22, 0x22),
        LV_PART_MAIN);
    set_remote_enabled(flipper_remote_is_ready());

    if (flipper_remote_pairing_pending()) {
        lv_label_set_text_fmt(s_pair_code, "%06lu",
                              (unsigned long)flipper_remote_pairing_code());
        lv_obj_clear_flag(s_pair_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_pair_panel);
    } else {
        lv_obj_add_flag(s_pair_panel, LV_OBJ_FLAG_HIDDEN);
    }

    bool scanning = flipper_is_running();
    bool checked = lv_obj_has_state(s_scan_switch, LV_STATE_CHECKED);
    if (scanning != checked) {
        s_switch_refresh = true;
        if (scanning) lv_obj_add_state(s_scan_switch, LV_STATE_CHECKED);
        else          lv_obj_clear_state(s_scan_switch, LV_STATE_CHECKED);
        s_switch_refresh = false;
    }
    refresh_device();
}

static void on_timer(lv_timer_t *)
{
    if (flipper_remote_screen_is_active()) refresh_ui();
}

static void on_scan_toggle(lv_event_t *)
{
    if (s_switch_refresh) return;
    if (lv_obj_has_state(s_scan_switch, LV_STATE_CHECKED)) {
        if (!flipper_start()) lv_obj_clear_state(s_scan_switch, LV_STATE_CHECKED);
    } else {
        flipper_stop();
    }
    main_loop_request_lvgl_priority(8);
}

static void on_connect(lv_event_t *)
{
    FlipperRemoteState state = flipper_remote_state();
    if (state != FLIPPER_REMOTE_IDLE && state != FLIPPER_REMOTE_ERROR) {
        flipper_remote_disconnect();
    } else {
        FlipperDeviceInfo info;
        if (!flipper_get_last_device(&info)) {
            lv_label_set_text(s_status, "No target - enable detector first");
            return;
        }
        flipper_remote_connect(&info);
    }
    refresh_ui();
}

static void on_remote_key(lv_event_t *event)
{
    intptr_t key = (intptr_t)lv_event_get_user_data(event);
    flipper_remote_send_key((FlipperRemoteKey)key);
}

static void on_locate(lv_event_t *) { flipper_remote_locate(); }

static void on_pair_accept(lv_event_t *) { flipper_remote_confirm_pairing(true); }
static void on_pair_cancel(lv_event_t *) { flipper_remote_confirm_pairing(false); }

static void leave_screen()
{
    flipper_remote_screen_stop();
    tools_screen_show();
}

static void on_gesture(lv_event_t *event)
{
    lv_indev_t *indev = lv_event_get_indev(event);
    if (indev && lv_indev_get_gesture_dir(indev) == LV_DIR_TOP) leave_screen();
}

void flipper_remote_screen_create()
{
    if (s_screen) return;

    s_screen = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(s_screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_screen, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_screen);
    lv_label_set_text(title, "FLIPPER REMOTE");
    lv_obj_set_style_text_color(title, lv_color_make(0xFF, 0x88, 0x00), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 7);

    s_status = lv_label_create(s_screen);
    lv_obj_set_width(s_status, 390);
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_status, lv_color_make(0xAA, 0xAA, 0xBB), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_label_set_text(s_status, "Disconnected");
    lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, 44);

    lv_obj_t *device_card = lv_obj_create(s_screen);
    lv_obj_set_size(device_card, 390, 72);
    lv_obj_align(device_card, LV_ALIGN_TOP_MID, 0, 68);
    lv_obj_set_style_bg_color(device_card, lv_color_make(0x12, 0x12, 0x17), LV_PART_MAIN);
    lv_obj_set_style_border_color(device_card, lv_color_make(0x33, 0x33, 0x44), LV_PART_MAIN);
    lv_obj_set_style_border_width(device_card, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(device_card, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_left(device_card, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_top(device_card, 7, LV_PART_MAIN);
    lv_obj_clear_flag(device_card, LV_OBJ_FLAG_SCROLLABLE);

    s_device = lv_label_create(device_card);
    lv_obj_set_width(s_device, 300);
    lv_obj_set_style_text_color(s_device, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_device, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_label_set_text(s_device, "No Flipper seen yet");
    lv_obj_align(s_device, LV_ALIGN_LEFT_MID, 0, 0);

    s_scan_switch = lv_switch_create(device_card);
    lv_obj_set_size(s_scan_switch, 54, 28);
    lv_obj_align(s_scan_switch, LV_ALIGN_RIGHT_MID, -2, 7);
    lv_obj_add_event_cb(s_scan_switch, on_scan_toggle, LV_EVENT_VALUE_CHANGED, nullptr);

    lv_obj_t *scan_label = lv_label_create(device_card);
    lv_label_set_text(scan_label, "SCAN");
    lv_obj_set_style_text_color(scan_label, lv_color_make(0x88, 0x88, 0x99), LV_PART_MAIN);
    lv_obj_set_style_text_font(scan_label, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_align(scan_label, LV_ALIGN_TOP_RIGHT, -8, -2);

    s_connect_btn = make_button(s_screen, "CONNECT", 210, 44, theme_accent_dark());
    lv_obj_align(s_connect_btn, LV_ALIGN_TOP_MID, 0, 151);
    s_connect_label = lv_obj_get_child(s_connect_btn, 0);
    lv_obj_add_event_cb(s_connect_btn, on_connect, LV_EVENT_CLICKED, nullptr);

    const lv_color_t pad = lv_color_make(0x20, 0x20, 0x29);
    s_remote_buttons[0] = make_button(s_screen, "UP", 74, 48, pad);
    lv_obj_align(s_remote_buttons[0], LV_ALIGN_TOP_MID, 0, 211);
    s_remote_buttons[1] = make_button(s_screen, "LEFT", 74, 58, pad);
    lv_obj_align(s_remote_buttons[1], LV_ALIGN_TOP_MID, -84, 267);
    s_remote_buttons[2] = make_button(s_screen, "OK", 74, 58,
                                       lv_color_make(0x44, 0x33, 0x11));
    lv_obj_align(s_remote_buttons[2], LV_ALIGN_TOP_MID, 0, 267);
    s_remote_buttons[3] = make_button(s_screen, "RIGHT", 74, 58, pad);
    lv_obj_align(s_remote_buttons[3], LV_ALIGN_TOP_MID, 84, 267);
    s_remote_buttons[4] = make_button(s_screen, "DOWN", 74, 48, pad);
    lv_obj_align(s_remote_buttons[4], LV_ALIGN_TOP_MID, 0, 333);
    s_remote_buttons[5] = make_button(s_screen, "BACK", 138, 46,
                                       lv_color_make(0x55, 0x22, 0x22));
    lv_obj_align(s_remote_buttons[5], LV_ALIGN_BOTTOM_MID, -78, -14);
    s_remote_buttons[6] = make_button(s_screen, "LOCATE", 138, 46,
                                       theme_accent_dark());
    lv_obj_align(s_remote_buttons[6], LV_ALIGN_BOTTOM_MID, 78, -14);

    const FlipperRemoteKey keys[] = {
        FLIPPER_KEY_UP, FLIPPER_KEY_LEFT, FLIPPER_KEY_OK,
        FLIPPER_KEY_RIGHT, FLIPPER_KEY_DOWN, FLIPPER_KEY_BACK,
    };
    for (int i = 0; i < 6; ++i)
        lv_obj_add_event_cb(s_remote_buttons[i], on_remote_key,
                            LV_EVENT_CLICKED, (void *)(intptr_t)keys[i]);
    lv_obj_add_event_cb(s_remote_buttons[6], on_locate, LV_EVENT_CLICKED, nullptr);

    s_pair_panel = lv_obj_create(s_screen);
    lv_obj_set_size(s_pair_panel, 370, 178);
    lv_obj_align(s_pair_panel, LV_ALIGN_CENTER, 0, 24);
    lv_obj_set_style_bg_color(s_pair_panel, lv_color_make(0x18, 0x18, 0x20), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_pair_panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_pair_panel, theme_accent_bright(), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_pair_panel, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(s_pair_panel, 12, LV_PART_MAIN);
    lv_obj_clear_flag(s_pair_panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *pair_title = lv_label_create(s_pair_panel);
    lv_label_set_text(pair_title, "PAIRING CODE");
    lv_obj_set_style_text_color(pair_title, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(pair_title, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_align(pair_title, LV_ALIGN_TOP_MID, 0, 4);

    s_pair_code = lv_label_create(s_pair_panel);
    lv_label_set_text(s_pair_code, "000000");
    lv_obj_set_style_text_color(s_pair_code, theme_accent_bright(), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_pair_code, &lv_font_montserrat_32, LV_PART_MAIN);
    lv_obj_align(s_pair_code, LV_ALIGN_TOP_MID, 0, 34);

    lv_obj_t *pair_hint = lv_label_create(s_pair_panel);
    lv_label_set_text(pair_hint, "Accept only if Flipper shows the same code");
    lv_obj_set_style_text_color(pair_hint, lv_color_make(0xAA, 0xAA, 0xAA), LV_PART_MAIN);
    lv_obj_set_style_text_font(pair_hint, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_align(pair_hint, LV_ALIGN_TOP_MID, 0, 76);

    lv_obj_t *cancel = make_button(s_pair_panel, "CANCEL", 130, 42,
                                   lv_color_make(0x66, 0x22, 0x22));
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 10, -7);
    lv_obj_add_event_cb(cancel, on_pair_cancel, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *accept = make_button(s_pair_panel, "ACCEPT", 130, 42,
                                   theme_accent_dark());
    lv_obj_align(accept, LV_ALIGN_BOTTOM_RIGHT, -10, -7);
    lv_obj_add_event_cb(accept, on_pair_accept, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_flag(s_pair_panel, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_event_cb(s_screen, on_gesture, LV_EVENT_GESTURE, nullptr);
    lv_timer_create(on_timer, 300, nullptr);
    set_remote_enabled(false);
}

void flipper_remote_screen_show()
{
    flipper_remote_screen_create();
    if (!flipper_is_running()) flipper_start();
    refresh_ui();
    main_loop_request_lvgl_priority(12);
    lv_scr_load(s_screen);
}

void flipper_remote_screen_stop()
{
    flipper_remote_disconnect();
}

bool flipper_remote_screen_is_active()
{
    return s_screen && lv_screen_active() == s_screen;
}
