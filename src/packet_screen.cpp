#include "packet_screen.h"
#include "packet_capture.h"
#include "theme.h"
#include <LilyGoLib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

void tools_screen_show();
void main_loop_request_lvgl_priority(int cycles);

namespace {

static lv_obj_t *s_screen;
static lv_obj_t *s_status;
static lv_obj_t *s_start_label;
static lv_obj_t *s_channel_label;
static lv_obj_t *s_stats;
static lv_obj_t *s_bars[13];
static lv_obj_t *s_rows[4];
static lv_obj_t *s_detail;
static int s_selected = 0;

static const char *subtype_name(uint8_t subtype)
{
    switch (subtype) {
    case 0:  return "ASSOC REQ";
    case 1:  return "ASSOC RESP";
    case 2:  return "REASSOC REQ";
    case 3:  return "REASSOC RESP";
    case 4:  return "PROBE REQ";
    case 5:  return "PROBE RESP";
    case 8:  return "BEACON";
    case 9:  return "ATIM";
    case 10: return "DISASSOC";
    case 11: return "AUTH";
    case 12: return "DEAUTH";
    case 13: return "ACTION";
    default: return "MGMT";
    }
}

static void format_mac(char *out, size_t size, const uint8_t mac[6])
{
    snprintf(out, size, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *text, int width)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_size(button, width, 42);
    lv_obj_set_style_bg_color(button, lv_color_make(0x22, 0x22, 0x22), LV_PART_MAIN);
    lv_obj_set_style_border_color(button, theme_accent_dark(), LV_PART_MAIN);
    lv_obj_set_style_border_width(button, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(button, 8, LV_PART_MAIN);
    lv_obj_t *label = lv_label_create(button);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_white(), LV_PART_MAIN);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    lv_obj_add_flag(label, LV_OBJ_FLAG_EVENT_BUBBLE);
    return button;
}

static void update_detail()
{
    PacketCaptureView packet = {};
    if (!packet_capture_get_recent(s_selected, &packet)) {
        lv_label_set_text(s_detail,
            "Tap START to capture nearby WiFi management frames.\n"
            "Receive-only; PCAP files are Wireshark compatible.");
        return;
    }

    char dst[18], src[18], bssid[18];
    format_mac(dst, sizeof(dst), packet.addr1);
    format_mac(src, sizeof(src), packet.addr2);
    format_mac(bssid, sizeof(bssid), packet.addr3);

    char buffer[1100];
    int used = snprintf(buffer, sizeof(buffer),
        "#%lu %s  CH%u  %d dBm  %u/%u bytes\n"
        "DST %s\nSRC %s\nBSS %s\n",
        static_cast<unsigned long>(packet.sequence), subtype_name(packet.subtype),
        packet.channel, packet.rssi, packet.captured_len, packet.original_len,
        dst, src, bssid);

    const uint16_t shown = packet.captured_len > 80 ? 80 : packet.captured_len;
    for (uint16_t i = 0; i < shown && used < static_cast<int>(sizeof(buffer) - 8); ++i) {
        used += snprintf(buffer + used, sizeof(buffer) - used, "%02X%s",
                         packet.bytes[i], (i + 1) % 16 == 0 ? "\n" : " ");
    }
    if (packet.captured_len > shown && used < static_cast<int>(sizeof(buffer) - 5))
        snprintf(buffer + used, sizeof(buffer) - used, "...");
    lv_label_set_text(s_detail, buffer);
}

static void refresh()
{
    if (!s_screen || lv_screen_active() != s_screen) return;

    lv_label_set_text(s_status, packet_capture_status_text());
    lv_label_set_text(s_start_label,
        packet_capture_is_running() || packet_capture_is_starting() ? "STOP" : "START");

    char buffer[128];
    const uint8_t setting = packet_capture_get_channel();
    snprintf(buffer, sizeof(buffer), setting ? "CH %u" : "HOP");
    lv_label_set_text(s_channel_label, buffer);
    snprintf(buffer, sizeof(buffer), "%lu pkt  %lu/s  %lu drop  tuned CH%u",
             static_cast<unsigned long>(packet_capture_total()),
             static_cast<unsigned long>(packet_capture_packets_per_second()),
             static_cast<unsigned long>(packet_capture_dropped()),
             packet_capture_current_channel());
    lv_label_set_text(s_stats, buffer);

    uint32_t max_count = 1;
    for (uint8_t channel = 1; channel <= 13; ++channel) {
        const uint32_t count = packet_capture_channel_count(channel);
        if (count > max_count) max_count = count;
    }
    for (uint8_t i = 0; i < 13; ++i) {
        const uint32_t count = packet_capture_channel_count(i + 1);
        int height = 3 + static_cast<int>(count * 42U / max_count);
        if (height > 45) height = 45;
        lv_obj_set_height(s_bars[i], height);
        lv_obj_set_y(s_bars[i], 48 - height);
        lv_obj_set_style_bg_color(s_bars[i],
            i + 1 == packet_capture_current_channel()
                ? theme_accent_bright() : lv_color_make(0x22, 0x77, 0xAA),
            LV_PART_MAIN);
    }

    const int count = packet_capture_recent_count();
    if (s_selected >= count) s_selected = count > 0 ? count - 1 : 0;
    for (int i = 0; i < 4; ++i) {
        PacketCaptureView packet = {};
        if (!packet_capture_get_recent(i, &packet)) {
            lv_label_set_text(s_rows[i], i == 0 ? "Waiting for frames..." : "");
            continue;
        }
        char source[18];
        format_mac(source, sizeof(source), packet.addr2);
        snprintf(buffer, sizeof(buffer), "CH%02u %4d  %-10s  %s",
                 packet.channel, packet.rssi, subtype_name(packet.subtype), source);
        lv_label_set_text(s_rows[i], buffer);
        lv_obj_set_style_text_color(s_rows[i],
            i == s_selected ? theme_accent_bright() : lv_color_make(0xDD, 0xDD, 0xDD),
            LV_PART_MAIN);
    }
    update_detail();
}

static void on_timer(lv_timer_t *) { refresh(); }

static void on_start(lv_event_t *)
{
    if (packet_capture_is_running() || packet_capture_is_starting())
        packet_capture_stop();
    else
        packet_capture_start();
    refresh();
}

static void on_channel(lv_event_t *)
{
    uint8_t channel = packet_capture_get_channel();
    channel = channel >= 13 ? 0 : channel + 1;
    packet_capture_set_channel(channel);
    refresh();
}

static void on_row(lv_event_t *event)
{
    s_selected = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
    update_detail();
}

static void leave_screen()
{
    packet_capture_stop();
    tools_screen_show();
}

static void on_gesture(lv_event_t *event)
{
    lv_indev_t *input = lv_event_get_indev(event);
    const lv_dir_t direction = lv_indev_get_gesture_dir(input);
    if (direction == LV_DIR_RIGHT || direction == LV_DIR_TOP) leave_screen();
}

} // namespace

void packet_screen_create()
{
    s_screen = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(s_screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_screen, 0, LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(s_screen);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
    lv_label_set_text(title, "PACKET LAB");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 7);

    s_status = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_status, lv_color_make(0x99, 0x99, 0x99), LV_PART_MAIN);
    lv_label_set_text(s_status, "Stopped");
    lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, 39);

    lv_obj_t *start = make_button(s_screen, "START", 112);
    lv_obj_align(start, LV_ALIGN_TOP_LEFT, 68, 59);
    s_start_label = lv_obj_get_child(start, 0);
    lv_obj_add_event_cb(start, on_start, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *channel = make_button(s_screen, "HOP", 112);
    lv_obj_align(channel, LV_ALIGN_TOP_RIGHT, -68, 59);
    s_channel_label = lv_obj_get_child(channel, 0);
    lv_obj_add_event_cb(channel, on_channel, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *chart = lv_obj_create(s_screen);
    lv_obj_set_size(chart, 400, 76);
    lv_obj_align(chart, LV_ALIGN_TOP_MID, 0, 106);
    lv_obj_set_style_bg_color(chart, lv_color_make(0x0A, 0x0A, 0x0A), LV_PART_MAIN);
    lv_obj_set_style_border_color(chart, lv_color_make(0x33, 0x33, 0x33), LV_PART_MAIN);
    lv_obj_set_style_border_width(chart, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(chart, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(chart, 0, LV_PART_MAIN);
    lv_obj_clear_flag(chart, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < 13; ++i) {
        s_bars[i] = lv_obj_create(chart);
        lv_obj_set_size(s_bars[i], 19, 3);
        lv_obj_set_pos(s_bars[i], 17 + i * 29, 45);
        lv_obj_set_style_bg_color(s_bars[i], lv_color_make(0x22, 0x77, 0xAA), LV_PART_MAIN);
        lv_obj_set_style_border_width(s_bars[i], 0, LV_PART_MAIN);
        lv_obj_set_style_radius(s_bars[i], 2, LV_PART_MAIN);
        lv_obj_set_style_pad_all(s_bars[i], 0, LV_PART_MAIN);
        lv_obj_clear_flag(s_bars[i], LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *label = lv_label_create(chart);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(label, lv_color_make(0x88, 0x88, 0x88), LV_PART_MAIN);
        char text[3];
        snprintf(text, sizeof(text), "%d", i + 1);
        lv_label_set_text(label, text);
        lv_obj_set_pos(label, 19 + i * 29, 52);
    }

    s_stats = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_stats, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_stats, lv_color_make(0x77, 0xAA, 0xCC), LV_PART_MAIN);
    lv_label_set_text(s_stats, "0 pkt  0/s  0 drop");
    lv_obj_align(s_stats, LV_ALIGN_TOP_MID, 0, 185);

    for (int i = 0; i < 4; ++i) {
        s_rows[i] = lv_label_create(s_screen);
        lv_obj_set_size(s_rows[i], 390, 24);
        lv_obj_set_pos(s_rows[i], 45, 207 + i * 25);
        lv_obj_set_style_text_font(s_rows[i], &lv_font_montserrat_14, LV_PART_MAIN);
        lv_obj_set_style_text_color(s_rows[i], lv_color_make(0xDD, 0xDD, 0xDD), LV_PART_MAIN);
        lv_label_set_text(s_rows[i], "");
        lv_obj_add_flag(s_rows[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_rows[i], on_row, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<intptr_t>(i)));
    }

    lv_obj_t *detail_panel = lv_obj_create(s_screen);
    lv_obj_set_size(detail_panel, 400, 166);
    lv_obj_align(detail_panel, LV_ALIGN_TOP_MID, 0, 308);
    lv_obj_set_style_bg_color(detail_panel, lv_color_make(0x08, 0x08, 0x08), LV_PART_MAIN);
    lv_obj_set_style_border_color(detail_panel, lv_color_make(0x33, 0x33, 0x33), LV_PART_MAIN);
    lv_obj_set_style_border_width(detail_panel, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(detail_panel, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(detail_panel, 8, LV_PART_MAIN);
    lv_obj_set_scroll_dir(detail_panel, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(detail_panel, LV_SCROLLBAR_MODE_AUTO);

    s_detail = lv_label_create(detail_panel);
    lv_obj_set_width(s_detail, 378);
    lv_obj_set_style_text_font(s_detail, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_detail, lv_color_make(0xBB, 0xDD, 0xBB), LV_PART_MAIN);
    lv_label_set_long_mode(s_detail, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_detail,
        "Tap START to capture nearby WiFi management frames.\n"
        "Receive-only; PCAP files are Wireshark compatible.");

    lv_obj_add_event_cb(s_screen, on_gesture, LV_EVENT_GESTURE, nullptr);
    lv_timer_create(on_timer, 300, nullptr);
}

void packet_screen_show()
{
    s_selected = 0;
    main_loop_request_lvgl_priority(12);
    refresh();
    lv_scr_load(s_screen);
}

void packet_screen_stop() { packet_capture_stop(); }
bool packet_screen_is_active() { return lv_screen_active() == s_screen; }

