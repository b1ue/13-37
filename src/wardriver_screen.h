#pragma once
#include <lvgl.h>

void wardriver_screen_create();
void wardriver_screen_show();
bool wardriver_screen_is_active();
void wardriver_screen_update();
void wardriver_bg_tick();
bool wardriver_is_running();
void wardriver_prepare_for_sleep();
int  wardriver_get_wifi_count();
int  wardriver_get_bt_count();
