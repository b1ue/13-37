#include "about_screen.h"
#include "theme.h"

void tools_screen_show();
void main_loop_request_lvgl_priority(int cycles);

static lv_obj_t *s_screen = nullptr;
static lv_obj_t *s_accent_line = nullptr;

static void on_gesture(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    if (indev && lv_indev_get_gesture_dir(indev) == LV_DIR_TOP)
        tools_screen_show();
}

static lv_obj_t *make_credit(lv_obj_t *parent, const char *role,
                             const char *username, int y)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, 360, 74);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, y);
    lv_obj_set_style_bg_color(card, lv_color_make(0x12, 0x12, 0x16), LV_PART_MAIN);
    lv_obj_set_style_border_color(card, lv_color_make(0x33, 0x33, 0x44), LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 10, LV_PART_MAIN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *role_label = lv_label_create(card);
    lv_label_set_text(role_label, role);
    lv_obj_set_style_text_color(role_label, lv_color_make(0x88, 0x88, 0x99), LV_PART_MAIN);
    lv_obj_set_style_text_font(role_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(role_label, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *user_label = lv_label_create(card);
    lv_label_set_text(user_label, username);
    lv_obj_set_style_text_color(user_label, theme_accent_bright(), LV_PART_MAIN);
    lv_obj_set_style_text_font(user_label, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_align(user_label, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    return user_label;
}

static void create_screen()
{
    if (s_screen) return;
    s_screen = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(s_screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_screen, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_screen);
    lv_label_set_text(title, "ABOUT");
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    s_accent_line = lv_obj_create(s_screen);
    lv_obj_set_size(s_accent_line, 210, 3);
    lv_obj_set_style_bg_color(s_accent_line, theme_accent_mid(), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_accent_line, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_accent_line, 0, LV_PART_MAIN);
    lv_obj_align(s_accent_line, LV_ALIGN_TOP_MID, 0, 66);

    make_credit(s_screen, "UPSTREAM AUTHOR", "@r3dfish", 94);
    make_credit(s_screen, "OPTIMIZED FORK", "@b1ue", 180);

    lv_obj_t *repo = lv_label_create(s_screen);
    lv_obj_set_width(repo, 360);
    lv_obj_set_style_text_align(repo, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_color(repo, lv_color_make(0xAA, 0xAA, 0xAA), LV_PART_MAIN);
    lv_obj_set_style_text_font(repo, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_label_set_text(repo,
        "13:37 for LILYGO T-Watch Ultra\n"
        "github.com/b1ue/13-37\n"
        "MIT License");
    lv_obj_align(repo, LV_ALIGN_TOP_MID, 0, 278);

    lv_obj_t *hint = lv_label_create(s_screen);
    lv_label_set_text(hint, "BOOT or swipe up to return to Tools");
    lv_obj_set_style_text_color(hint, lv_color_make(0x66, 0x66, 0x66), LV_PART_MAIN);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -46);

    lv_obj_add_event_cb(s_screen, on_gesture, LV_EVENT_GESTURE, nullptr);
}

void about_screen_show()
{
    create_screen();
    lv_obj_set_style_bg_color(s_accent_line, theme_accent_mid(), LV_PART_MAIN);
    // Refresh the two username labels (second child in each credit card).
    for (uint32_t i = 0; i < lv_obj_get_child_count(s_screen); i++) {
        lv_obj_t *child = lv_obj_get_child(s_screen, i);
        if (lv_obj_get_child_count(child) == 2)
            lv_obj_set_style_text_color(lv_obj_get_child(child, 1),
                                        theme_accent_bright(), LV_PART_MAIN);
    }
    main_loop_request_lvgl_priority(12);
    lv_scr_load(s_screen);
}

bool about_screen_is_active()
{
    return s_screen && lv_screen_active() == s_screen;
}
