#include "theme.h"

static bool s_blue = false;

void theme_set_blue(bool blue) { s_blue = blue; }
bool theme_is_blue() { return s_blue; }

lv_color_t theme_accent_bright()
{
    return s_blue ? lv_color_make(0x44, 0xAA, 0xFF)
                  : lv_color_make(0x00, 0xFF, 0x80);
}

lv_color_t theme_accent_mid()
{
    return s_blue ? lv_color_make(0x33, 0x77, 0xFF)
                  : lv_color_make(0x00, 0xCC, 0x66);
}

lv_color_t theme_accent_dark()
{
    return s_blue ? lv_color_make(0x11, 0x33, 0x66)
                  : lv_color_make(0x00, 0x55, 0x22);
}
