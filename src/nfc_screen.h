#pragma once
#include <lvgl.h>

void nfc_screen_create();
void nfc_screen_show();
bool nfc_screen_is_active();
bool nfc_screen_is_powered();
void nfc_screen_worker(); // call from loop() — no-op when idle
