#pragma once
#include "esp_gap_ble_api.h"

// Multi-consumer wrapper around the ESP-IDF BLE scan API. The controller has
// one GAP callback slot, so this manager owns it and fans inquiry results out
// to registered scanners without callbacks replacing each other.

typedef void (*ble_scan_cb_t)(esp_ble_gap_cb_param_t *param);
typedef void (*ble_gap_observer_t)(esp_gap_ble_cb_event_t event,
                                   esp_ble_gap_cb_param_t *param);

// Register a consumer. Idempotent for an existing callback. Hardware startup
// is deferred to ble_scan_tick(); false means queue allocation failed or the
// ten-entry consumer table is full.
bool ble_scan_add(ble_scan_cb_t cb);

// Unregister a consumer. Final controller shutdown is deliberately deferred
// so this is always safe to call from an LVGL event callback.
void ble_scan_remove(ble_scan_cb_t cb);

// Progress asynchronous scan stop and controller teardown. Call frequently
// from the main loop; each call performs at most one lifecycle operation.
void ble_scan_tick();

// True while consumers exist or final controller teardown is still pending.
// This keeps exclusive BLE users (notably HID) out of the handoff window.
bool ble_scan_active();
int  ble_scan_consumer_count();
uint32_t ble_scan_result_drop_count();

// Non-scanning BLE clients (currently the paired Flipper RPC controller) can
// hold the controller/host awake without registering a fake scan consumer.
// Startup and shutdown remain staged by ble_scan_tick().
void ble_scan_hold_acquire();
void ble_scan_hold_release();
bool ble_scan_ready();

// One lightweight observer for pairing/security GAP events.  It is invoked on
// Bluedroid's callback task, so the observer must only copy state or send the
// immediate security acknowledgement required by ESP-IDF.
void ble_scan_set_gap_observer(ble_gap_observer_t observer);
