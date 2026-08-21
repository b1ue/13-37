#pragma once

#include <stddef.h>

// Small NVS-backed known-network store. Credentials never touch the SD card,
// so mounting it over USB cannot expose WiFi passwords.
constexpr int WIFI_CREDENTIALS_MAX_NETWORKS = 5;

int  wifi_credentials_count();
bool wifi_credentials_get(const char *ssid, char *password_out,
                          size_t password_out_size);
bool wifi_credentials_save(const char *ssid, const char *password);
bool wifi_credentials_forget(const char *ssid);
