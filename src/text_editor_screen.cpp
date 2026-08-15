#include "text_editor_screen.h"
#include "usb_sd.h"
#include "theme.h"
#include <LilyGoLib.h>
#include <SD.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

void tools_screen_show();
void main_loop_request_lvgl_priority(int cycles);

namespace {

constexpr size_t EDITOR_MAX = 4096;
static lv_obj_t *s_screen;
static lv_obj_t *s_filename;
static lv_obj_t *s_body;
static lv_obj_t *s_keyboard;
static lv_obj_t *s_status;
static char s_buffer[EDITOR_MAX + 1];

static void set_status(const char *text, bool error = false)
{
    lv_label_set_text(s_status, text);
    lv_obj_set_style_text_color(s_status,
        error ? lv_color_make(0xFF, 0x66, 0x66)
              : lv_color_make(0x66, 0xCC, 0x99), LV_PART_MAIN);
}

static bool make_path(char *path, size_t path_size)
{
    const char *name = lv_textarea_get_text(s_filename);
    char clean[49] = {};
    size_t used = 0;
    for (const char *p = name; *p && used < sizeof(clean) - 1; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (c < 32 || strchr("/\\:*?\"<>|", c)) clean[used++] = '_';
        else clean[used++] = static_cast<char>(c);
    }
    while (used && (clean[used - 1] == ' ' || clean[used - 1] == '.')) clean[--used] = '\0';
    if (!used || !strcmp(clean, ".") || !strcmp(clean, "..")) {
        set_status("Enter a filename", true);
        return false;
    }
    if (!strchr(clean, '.') && used + 4 < sizeof(clean)) strcat(clean, ".txt");
    snprintf(path, path_size, "/Notes/%s", clean);
    lv_textarea_set_text(s_filename, clean);
    return true;
}

static bool storage_ready()
{
    if (usb_sd_is_running()) {
        set_status("Unmount USB SD first", true);
        return false;
    }
    if (!instance.isCardReady()) {
        set_status("No SD card available", true);
        return false;
    }
    return true;
}

static void on_save(lv_event_t *)
{
    if (!storage_ready()) return;
    char path[64];
    if (!make_path(path, sizeof(path))) return;
    SD.mkdir("/Notes");
    if (SD.exists(path)) SD.remove(path);
    File file = SD.open(path, FILE_WRITE);
    if (!file) {
        set_status("Could not open file", true);
        return;
    }
    const char *text = lv_textarea_get_text(s_body);
    const size_t length = strlen(text);
    const bool okay = file.write(reinterpret_cast<const uint8_t *>(text), length) == length;
    file.close();
    char message[88];
    snprintf(message, sizeof(message), okay ? "Saved %u bytes to %s" : "SD write failed",
             static_cast<unsigned>(length), path);
    set_status(message, !okay);
}

static void on_load(lv_event_t *)
{
    if (!storage_ready()) return;
    char path[64];
    if (!make_path(path, sizeof(path))) return;
    File file = SD.open(path, FILE_READ);
    if (!file) {
        set_status("File not found", true);
        return;
    }
    const size_t available = file.size();
    const size_t wanted = available > EDITOR_MAX ? EDITOR_MAX : available;
    const size_t read = file.readBytes(s_buffer, wanted);
    s_buffer[read] = '\0';
    file.close();
    lv_textarea_set_text(s_body, s_buffer);
    char message[88];
    snprintf(message, sizeof(message), available > EDITOR_MAX
        ? "Loaded first %u bytes (file truncated in editor)"
        : "Loaded %u bytes from %s", static_cast<unsigned>(read), path);
    set_status(message, false);
}

static void on_new(lv_event_t *)
{
    lv_textarea_set_text(s_body, "");
    set_status("New unsaved note");
}

static void on_focus(lv_event_t *event)
{
    lv_obj_t *textarea = static_cast<lv_obj_t *>(lv_event_get_target(event));
    lv_keyboard_set_textarea(s_keyboard, textarea);
    lv_keyboard_set_mode(s_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *text)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_size(button, 104, 40);
    lv_obj_set_style_bg_color(button, lv_color_make(0x22, 0x22, 0x22), LV_PART_MAIN);
    lv_obj_set_style_border_color(button, theme_accent_dark(), LV_PART_MAIN);
    lv_obj_set_style_border_width(button, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(button, 7, LV_PART_MAIN);
    lv_obj_t *label = lv_label_create(button);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_white(), LV_PART_MAIN);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    lv_obj_add_flag(label, LV_OBJ_FLAG_EVENT_BUBBLE);
    return button;
}

static void on_gesture(lv_event_t *event)
{
    lv_indev_t *input = lv_event_get_indev(event);
    const lv_dir_t direction = lv_indev_get_gesture_dir(input);
    if (direction == LV_DIR_RIGHT || direction == LV_DIR_TOP) tools_screen_show();
}

} // namespace

void text_editor_screen_create()
{
    s_screen = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(s_screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_screen, 0, LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(s_screen);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
    lv_label_set_text(title, "TEXT EDITOR");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 5);

    s_filename = lv_textarea_create(s_screen);
    lv_textarea_set_one_line(s_filename, true);
    lv_textarea_set_max_length(s_filename, 48);
    lv_textarea_set_placeholder_text(s_filename, "filename.txt");
    lv_textarea_set_text(s_filename, "note.txt");
    lv_obj_set_size(s_filename, 310, 42);
    lv_obj_align(s_filename, LV_ALIGN_TOP_MID, 0, 39);
    lv_obj_set_style_text_font(s_filename, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_filename, lv_color_make(0x11, 0x11, 0x11), LV_PART_MAIN);
    lv_obj_set_style_text_color(s_filename, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_border_color(s_filename, lv_color_make(0x44, 0x44, 0x44), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_filename, 1, LV_PART_MAIN);
    lv_obj_add_event_cb(s_filename, on_focus, LV_EVENT_FOCUSED, nullptr);

    lv_obj_t *load = make_button(s_screen, "LOAD");
    lv_obj_align(load, LV_ALIGN_TOP_LEFT, 65, 86);
    lv_obj_add_event_cb(load, on_load, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *save = make_button(s_screen, "SAVE");
    lv_obj_align(save, LV_ALIGN_TOP_MID, 0, 86);
    lv_obj_add_event_cb(save, on_save, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *fresh = make_button(s_screen, "NEW");
    lv_obj_align(fresh, LV_ALIGN_TOP_RIGHT, -65, 86);
    lv_obj_add_event_cb(fresh, on_new, LV_EVENT_CLICKED, nullptr);

    s_status = lv_label_create(s_screen);
    lv_obj_set_width(s_status, 390);
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_status, lv_color_make(0x66, 0xCC, 0x99), LV_PART_MAIN);
    lv_label_set_text(s_status, "Files are stored in /Notes on SD");
    lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, 130);

    s_body = lv_textarea_create(s_screen);
    lv_textarea_set_one_line(s_body, false);
    lv_textarea_set_max_length(s_body, EDITOR_MAX);
    lv_textarea_set_placeholder_text(s_body, "Tap here and type...");
    lv_obj_set_size(s_body, 400, 112);
    lv_obj_align(s_body, LV_ALIGN_TOP_MID, 0, 151);
    lv_obj_set_style_text_font(s_body, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_body, lv_color_make(0x08, 0x08, 0x08), LV_PART_MAIN);
    lv_obj_set_style_text_color(s_body, lv_color_make(0xDD, 0xEE, 0xDD), LV_PART_MAIN);
    lv_obj_set_style_border_color(s_body, lv_color_make(0x44, 0x44, 0x44), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_body, 1, LV_PART_MAIN);
    lv_obj_add_event_cb(s_body, on_focus, LV_EVENT_FOCUSED, nullptr);

    s_keyboard = lv_keyboard_create(s_screen);
    lv_obj_set_size(s_keyboard, 410, 208);
    lv_obj_align(s_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_mode(s_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(s_keyboard, s_body);

    lv_obj_add_event_cb(s_screen, on_gesture, LV_EVENT_GESTURE, nullptr);
}

void text_editor_screen_show()
{
    main_loop_request_lvgl_priority(12);
    lv_keyboard_set_textarea(s_keyboard, s_body);
    lv_scr_load(s_screen);
}

bool text_editor_screen_is_active() { return lv_screen_active() == s_screen; }

