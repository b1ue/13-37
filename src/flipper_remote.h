#pragma once

#include "flipper.h"
#include <stdint.h>

enum FlipperRemoteState {
    FLIPPER_REMOTE_IDLE,
    FLIPPER_REMOTE_STARTING,
    FLIPPER_REMOTE_CONNECTING,
    FLIPPER_REMOTE_PAIRING,
    FLIPPER_REMOTE_DISCOVERING,
    FLIPPER_REMOTE_READY,
    FLIPPER_REMOTE_DISCONNECTING,
    FLIPPER_REMOTE_ERROR,
};

enum FlipperRemoteKey {
    FLIPPER_KEY_UP = 0,
    FLIPPER_KEY_DOWN = 1,
    FLIPPER_KEY_RIGHT = 2,
    FLIPPER_KEY_LEFT = 3,
    FLIPPER_KEY_OK = 4,
    FLIPPER_KEY_BACK = 5,
};

// Connects only to the explicitly selected advertisement.  The Flipper's BLE
// security policy still controls pairing; no address spoofing or auto-connect
// to arbitrary nearby devices is performed.
bool flipper_remote_connect(const FlipperDeviceInfo *device);
void flipper_remote_disconnect();
void flipper_remote_bg_tick();

FlipperRemoteState flipper_remote_state();
const char *flipper_remote_status();
bool flipper_remote_is_ready();

// Numeric comparison pairing.  The same code must be accepted on the watch
// and on the physical Flipper.
bool flipper_remote_pairing_pending();
uint32_t flipper_remote_pairing_code();
void flipper_remote_confirm_pairing(bool accept);

// Allow-listed official RPC operations used by the controller screen.
bool flipper_remote_send_key(FlipperRemoteKey key);
bool flipper_remote_locate();
bool flipper_remote_ping();
