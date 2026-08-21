#include "wifi_credentials.h"

#include <Arduino.h>
#include <Preferences.h>
#include <string.h>

namespace {

constexpr const char *NVS_NAMESPACE = "wifi-known";

struct KnownNetwork {
    char ssid[33];
    char password[64];
};

KnownNetwork s_networks[WIFI_CREDENTIALS_MAX_NETWORKS] = {};
uint8_t s_count = 0;
bool s_loaded = false;

void copy_text(char *destination, size_t size, const char *source)
{
    if (!destination || size == 0) return;
    strlcpy(destination, source ? source : "", size);
}

void key_for(char *out, size_t size, char prefix, int index)
{
    snprintf(out, size, "%c%d", prefix, index);
}

void ensure_loaded()
{
    if (s_loaded) return;
    s_loaded = true;

    Preferences preferences;
    if (!preferences.begin(NVS_NAMESPACE, true)) return;
    s_count = preferences.getUChar("count", 0);
    if (s_count > WIFI_CREDENTIALS_MAX_NETWORKS)
        s_count = WIFI_CREDENTIALS_MAX_NETWORKS;

    uint8_t valid = 0;
    for (uint8_t i = 0; i < s_count; ++i) {
        char ssid_key[4];
        char pass_key[4];
        key_for(ssid_key, sizeof(ssid_key), 's', i);
        key_for(pass_key, sizeof(pass_key), 'p', i);
        String ssid = preferences.getString(ssid_key, "");
        if (ssid.isEmpty() || ssid.length() > 32) continue;
        String password = preferences.getString(pass_key, "");
        copy_text(s_networks[valid].ssid, sizeof(s_networks[valid].ssid),
                  ssid.c_str());
        copy_text(s_networks[valid].password,
                  sizeof(s_networks[valid].password), password.c_str());
        ++valid;
    }
    s_count = valid;
    preferences.end();
}

bool persist()
{
    Preferences preferences;
    if (!preferences.begin(NVS_NAMESPACE, false)) return false;
    bool ok = preferences.putUChar("count", s_count) == sizeof(uint8_t);
    for (int i = 0; i < WIFI_CREDENTIALS_MAX_NETWORKS; ++i) {
        char ssid_key[4];
        char pass_key[4];
        key_for(ssid_key, sizeof(ssid_key), 's', i);
        key_for(pass_key, sizeof(pass_key), 'p', i);
        if (i < s_count) {
            ok = preferences.putString(ssid_key, s_networks[i].ssid) > 0 && ok;
            // An empty password is valid for an open network. putString()
            // returns zero for it, so do not interpret that as a failure.
            preferences.putString(pass_key, s_networks[i].password);
        } else {
            preferences.remove(ssid_key);
            preferences.remove(pass_key);
        }
    }
    preferences.end();
    return ok;
}

int find_network(const char *ssid)
{
    if (!ssid || !ssid[0]) return -1;
    for (uint8_t i = 0; i < s_count; ++i)
        if (strcmp(s_networks[i].ssid, ssid) == 0) return i;
    return -1;
}

} // namespace

int wifi_credentials_count()
{
    ensure_loaded();
    return s_count;
}

bool wifi_credentials_get(const char *ssid, char *password_out,
                          size_t password_out_size)
{
    ensure_loaded();
    int index = find_network(ssid);
    if (index < 0) return false;
    if (password_out && password_out_size)
        copy_text(password_out, password_out_size, s_networks[index].password);
    return true;
}

bool wifi_credentials_save(const char *ssid, const char *password)
{
    ensure_loaded();
    if (!ssid || !ssid[0] || strlen(ssid) > 32 ||
        (password && strlen(password) > 63)) return false;

    KnownNetwork updated = {};
    copy_text(updated.ssid, sizeof(updated.ssid), ssid);
    copy_text(updated.password, sizeof(updated.password), password);

    int existing = find_network(ssid);
    if (existing >= 0) {
        for (int i = existing; i > 0; --i)
            s_networks[i] = s_networks[i - 1];
    } else {
        int last = s_count < WIFI_CREDENTIALS_MAX_NETWORKS
                 ? s_count : WIFI_CREDENTIALS_MAX_NETWORKS - 1;
        for (int i = last; i > 0; --i)
            s_networks[i] = s_networks[i - 1];
        if (s_count < WIFI_CREDENTIALS_MAX_NETWORKS) ++s_count;
    }
    s_networks[0] = updated;
    return persist();
}

bool wifi_credentials_forget(const char *ssid)
{
    ensure_loaded();
    int index = find_network(ssid);
    if (index < 0) return false;
    memset(&s_networks[index], 0, sizeof(s_networks[index]));
    for (int i = index; i + 1 < s_count; ++i)
        s_networks[i] = s_networks[i + 1];
    if (s_count) {
        --s_count;
        memset(&s_networks[s_count], 0, sizeof(s_networks[s_count]));
    }
    return persist();
}
