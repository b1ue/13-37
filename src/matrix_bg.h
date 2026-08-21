#pragma once
#include <lvgl.h>

// Creates the Matrix "digital rain" background as the first child of `parent`
// (so it renders behind all other widgets). Starts hidden and paused.
lv_obj_t *matrix_bg_create(lv_obj_t *parent);

void matrix_bg_set_enabled(bool en);
bool matrix_bg_is_enabled();

enum MatrixRainPalette : uint8_t {
    MATRIX_RAIN_GREEN = 0,
    MATRIX_RAIN_BLUE  = 1,
};
void matrix_bg_set_palette(MatrixRainPalette palette);
MatrixRainPalette matrix_bg_palette();

// Replace ordinary random glyphs with a passive stream made from configured
// stock symbols and cached quote data. This option never fetches data itself.
void matrix_bg_set_stock_data(bool enabled);
bool matrix_bg_stock_data_enabled();

// Mix a privacy-safe system telemetry stream into the rain (battery, uptime,
// free heap, WiFi state, and active BLE scanner count). Values are sampled at
// a low rate; the animation timer never performs radio or filesystem I/O.
void matrix_bg_set_system_data(bool enabled);
bool matrix_bg_system_data_enabled();

// Animation/redraw budget: 0 = Eco, 1 = Balanced, 2 = Smooth.
void matrix_bg_set_power_mode(uint8_t mode);
uint8_t matrix_bg_power_mode();

// Temporarily pause/resume the rain animation timer without changing the
// enabled state. Used during screen transitions so the selected redraw
// budget does not compete with a new screen on top of a busy main loop.
void matrix_bg_set_paused(bool paused);
// Re-render visible columns after its independent rain palette changes.
void matrix_bg_refresh_theme();
