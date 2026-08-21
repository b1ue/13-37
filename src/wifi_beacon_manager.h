#pragma once
#include <stdint.h>

// Multi-consumer wrapper around the ESP32 WiFi promiscuous API.
//
// The ESP32 only supports one promiscuous callback at a time — the last call
// to esp_wifi_set_promiscuous_rx_cb() wins and silently discards the previous
// one.  This module owns that single slot, parses beacon frames, and fans out
// the result to all registered consumers, so the wardriver, evil-twin
// detector, flock detector, and any future features can share the same scan
// without trampling each other.
//
// WiFi is put in STA+promiscuous mode after the first add() call and torn down
// after the last remove() (reference-counted). Driver transitions are
// asynchronous; channel hopping (200 ms, 1-13) is managed internally.

struct WifiBeacon {
    uint8_t bssid[6];
    char    ssid[33];
    char    auth[48];   // "[WPA2-PSK-CCMP][ESS]" style string
    int8_t  rssi;
    uint8_t channel;
};

typedef void (*wifi_beacon_cb_t)(const WifiBeacon *b);

// Register a consumer.  Idempotent — adding the same cb twice is a no-op.
// Returns false if queue allocation fails or the consumer table is full.
bool wifi_beacon_add(wifi_beacon_cb_t cb);

// Unregister a consumer.  WiFi is torn down when the last consumer leaves.
void wifi_beacon_remove(wifi_beacon_cb_t cb);

// Progress asynchronous radio lifecycle and deliver a bounded number of
// queued beacons to consumers on the main loop. Call frequently.
void wifi_beacon_tick();

// True while a consumer is registered or the radio lifecycle is winding down.
bool wifi_beacon_active();
