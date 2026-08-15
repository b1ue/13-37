#pragma once
#include <stdint.h>

enum FlockConfidence : uint8_t {
    FLOCK_CONFIDENCE_LOW = 1,
    FLOCK_CONFIDENCE_MEDIUM = 2,
    FLOCK_CONFIDENCE_HIGH = 3,
};

// Checks a detected device against the surveillance-vendor OUI table and the
// device-name pattern list. `name` may be NULL or empty. Returns true if the
// device matched (and was newly logged after dedup).
bool flock_check(const uint8_t *mac6, int8_t rssi, const char *name, char source);

int  flock_get_count();
void flock_bg_tick();
void flock_reset_count();

// Standalone tile API -- starts/stops dedicated WiFi and BLE scans so the
// detector runs without the wardriver.
bool flock_start();
void flock_stop();
void flock_prepare_for_sleep();
bool flock_is_running();
bool flock_is_starting();
bool flock_is_stopping();
const char *flock_status_text();

// Passive, local-only alert policy. These options do not start either radio
// and therefore do not change the detector or deep-sleep lifecycle.
void flock_set_alerts_enabled(bool enabled);
bool flock_alerts_enabled();
void flock_set_strong_only(bool enabled);
bool flock_strong_only();
