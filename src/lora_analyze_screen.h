#pragma once
#include <lvgl.h>

// LoRa spectrum analyzer — same visual style as the WiFi / BT analyzers, but
// puts the SX1262 in receive mode and sweeps the carrier across a selectable
// sub-GHz band, reading the instantaneous channel RSSI at each step. Reached
// by swiping LEFT on the Bluetooth analyzer; a RIGHT swipe returns there.

void lora_analyze_screen_create();
void lora_analyze_screen_show();
// Open directly from Tools; swipe-right returns to Tools instead of the
// Bluetooth analyzer in the three-screen Analyze chain.
void lora_analyze_screen_show_direct();
bool lora_analyze_screen_is_active();

// True while the analyzer is claiming the SX1262 — main.cpp's
// update_lora_indicator() OR's this with the other LoRa consumers to keep
// the clock-face LoRa icon green during a sweep.
bool lora_analyze_is_running();

// Release the SX1262 and re-arm whichever shared-radio consumer (pager /
// TPMS / APRS) was running before the analyzer started. Safe to call
// when the analyzer isn't running (no-op). The back-button handler in
// main.cpp invokes this so exiting via the boot button doesn't leave the
// radio in standby with pager/tpms/aprs silently dropped.
void lora_analyze_screen_stop();

// Flush an optional completed sweep to /Spectrum without doing SD work from
// the LVGL timer. Call from the normal main loop.
void lora_analyze_bg_tick();
