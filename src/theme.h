#pragma once
#include <lvgl.h>
#include <stdint.h>

// UI palettes are deliberately independent from background effects. Matrix
// rain has its own green/blue selector in matrix_bg.h.
enum ThemeId : uint8_t {
    THEME_MATRIX_GREEN = 0,
    THEME_MATRIX_BLUE  = 1,
    THEME_RETRO_VECTOR = 2,
    THEME_COUNT
};

struct ThemePalette {
    lv_color_t background;
    lv_color_t surface;
    lv_color_t panel;
    lv_color_t border;
    lv_color_t text;
    lv_color_t muted;
    lv_color_t accent_bright;
    lv_color_t accent_mid;
    lv_color_t accent_dark;
    lv_color_t secondary;
    lv_color_t warning;
    uint8_t corner_radius;
};

void theme_set(ThemeId theme);
ThemeId theme_get();
const ThemePalette &theme_palette();
const char *theme_name(ThemeId theme);
bool theme_is_retro();

// Compatibility wrappers for screens that only need accent colors and for
// the legacy blue_theme setting.
void theme_set_blue(bool blue);
bool theme_is_blue();
lv_color_t theme_accent_bright();
lv_color_t theme_accent_mid();
lv_color_t theme_accent_dark();
lv_color_t theme_background();
lv_color_t theme_surface();
lv_color_t theme_panel();
lv_color_t theme_border();
lv_color_t theme_text();
lv_color_t theme_muted();
lv_color_t theme_secondary();
lv_color_t theme_warning();
uint8_t theme_corner_radius();
