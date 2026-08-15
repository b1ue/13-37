#pragma once
#include <stdint.h>

// BLE HID mouse — advertises the watch as a Bluetooth mouse so a computer can
// pair with it and receive pointer input driven from the touchscreen trackpad.

// HID button bitmask values.
enum {
    MOUSE_BTN_LEFT   = 0x01,
    MOUSE_BTN_RIGHT  = 0x02,
    MOUSE_BTN_MIDDLE = 0x04,
};

// Bring up BLE and start advertising as a HID mouse. Returns false if BLE is
// already held by the scanners (AirTag / wardriver) — they share a single GAP
// callback slot and must be stopped first.
bool mouse_hid_start();

// Stop advertising, disconnect peers, remove GATT services, and tear the BLE
// stack back down (controller memory is kept so scanners can reclaim it).
void mouse_hid_stop();

bool mouse_hid_is_running();      // BLE up (advertising or connected)
bool mouse_hid_is_connected();    // a host is currently connected

// Send a relative pointer movement (clamped to the HID ±127 range).
void mouse_hid_move(int dx, int dy);
// Press then release the given button mask (a full click).
void mouse_hid_click(uint8_t buttons);
// Relative scroll-wheel step (clamped to ±127).
void mouse_hid_scroll(int wheel);
