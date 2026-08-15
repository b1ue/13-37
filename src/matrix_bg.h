#pragma once
#include <lvgl.h>

// Creates the Matrix "digital rain" background as the first child of `parent`
// (so it renders behind all other widgets). Starts hidden and paused.
lv_obj_t *matrix_bg_create(lv_obj_t *parent);

void matrix_bg_set_enabled(bool en);
bool matrix_bg_is_enabled();

// Replace ordinary random glyphs with a passive stream made from configured
// stock symbols and cached quote data. This option never fetches data itself.
void matrix_bg_set_stock_data(bool enabled);
bool matrix_bg_stock_data_enabled();

// Temporarily pause/resume the rain animation timer without changing the
// enabled state. Used during screen transitions so LVGL isn't being
// forced to re-render 22 recolored labels every 120 ms while it's trying
// to redraw a new screen on top of a busy main loop.
void matrix_bg_set_paused(bool paused);
// Re-render visible columns after the global accent palette changes.
void matrix_bg_refresh_theme();
