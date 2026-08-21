#include "theme.h"

static ThemeId s_theme = THEME_MATRIX_GREEN;

static const ThemePalette PALETTES[THEME_COUNT] = {
    { lv_color_hex(0x000000), lv_color_hex(0x06100A), lv_color_hex(0x0A1810),
      lv_color_hex(0x164D2C), lv_color_hex(0xE8FFF0), lv_color_hex(0x83A98F),
      lv_color_hex(0x00FF80), lv_color_hex(0x00CC66), lv_color_hex(0x005522),
      lv_color_hex(0x55FFB0), lv_color_hex(0xFFAA22), 8 },
    { lv_color_hex(0x000000), lv_color_hex(0x06101A), lv_color_hex(0x091827),
      lv_color_hex(0x174B73), lv_color_hex(0xEDF7FF), lv_color_hex(0x839DB2),
      lv_color_hex(0x44AAFF), lv_color_hex(0x3377FF), lv_color_hex(0x113366),
      lv_color_hex(0x66D8FF), lv_color_hex(0xFFAA22), 8 },
    // Late-70s/80s vector-terminal instrumentation: restrained phosphor,
    // true black, squared panels, amber primary data and cyan references.
    { lv_color_hex(0x000000), lv_color_hex(0x070A09), lv_color_hex(0x0B100F),
      lv_color_hex(0x315D5D), lv_color_hex(0xFFE8B0), lv_color_hex(0x8B9A8E),
      lv_color_hex(0xFFC14A), lv_color_hex(0xD88A18), lv_color_hex(0x57370A),
      lv_color_hex(0x55E6E6), lv_color_hex(0xFF6D3A), 1 },
};

void theme_set(ThemeId theme)
{
    s_theme = (theme < THEME_COUNT) ? theme : THEME_MATRIX_GREEN;
}

ThemeId theme_get() { return s_theme; }
const ThemePalette &theme_palette() { return PALETTES[(uint8_t)s_theme]; }

const char *theme_name(ThemeId theme)
{
    switch (theme) {
    case THEME_MATRIX_BLUE: return "Matrix Blue";
    case THEME_RETRO_VECTOR: return "Retro Vector";
    default: return "Matrix Green";
    }
}

bool theme_is_retro() { return s_theme == THEME_RETRO_VECTOR; }
void theme_set_blue(bool blue) { theme_set(blue ? THEME_MATRIX_BLUE : THEME_MATRIX_GREEN); }
bool theme_is_blue() { return s_theme == THEME_MATRIX_BLUE; }
lv_color_t theme_accent_bright() { return theme_palette().accent_bright; }
lv_color_t theme_accent_mid() { return theme_palette().accent_mid; }
lv_color_t theme_accent_dark() { return theme_palette().accent_dark; }
lv_color_t theme_background() { return theme_palette().background; }
lv_color_t theme_surface() { return theme_palette().surface; }
lv_color_t theme_panel() { return theme_palette().panel; }
lv_color_t theme_border() { return theme_palette().border; }
lv_color_t theme_text() { return theme_palette().text; }
lv_color_t theme_muted() { return theme_palette().muted; }
lv_color_t theme_secondary() { return theme_palette().secondary; }
lv_color_t theme_warning() { return theme_palette().warning; }
uint8_t theme_corner_radius() { return theme_palette().corner_radius; }
