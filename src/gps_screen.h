#pragma once
#include <lvgl.h>

void gps_screen_create();
void gps_screen_show();
bool gps_screen_is_active();
bool gps_screen_is_powered();
bool gps_screen_has_lock();
void gps_screen_poll();
