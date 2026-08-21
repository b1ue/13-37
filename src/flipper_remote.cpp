#include "flipper_remote.h"
#include "ble_scan_manager.h"

#include <Arduino.h>
#include <esp_gap_ble_api.h>
#include <esp_gattc_api.h>
#include <esp_bt_defs.h>
#include <string.h>

// Official Flipper BLE serial/RPC UUIDs, in the little-endian byte order used
// by ESP-IDF.  Source: targets/f7/ble_glue/services/serial_service_uuid.inc in
// flipperdevices/flipperzero-firmware.
static const uint8_t UUID_SERIAL_SERVICE[16] = {
    0x00, 0x00, 0xFE, 0x60, 0xCC, 0x7A, 0x48, 0x2A,
    0x98, 0x4A, 0x7F, 0x2E, 0xD5, 0xB3, 0xE5, 0x8F,
};
static const uint8_t UUID_SERIAL_RX[16] = {
    0x00, 0x00, 0xFE, 0x62, 0x8E, 0x22, 0x45, 0x41,
    0x9D, 0x4C, 0x21, 0xED, 0xAE, 0x82, 0xED, 0x19,
};
static const uint8_t UUID_RPC_STATUS[16] = {
    0x00, 0x00, 0xFE, 0x64, 0x8E, 0x22, 0x45, 0x41,
    0x9D, 0x4C, 0x21, 0xED, 0xAE, 0x82, 0xED, 0x19,
};

static const uint16_t GATTC_APP_ID = 0x1337;

static volatile FlipperRemoteState s_state = FLIPPER_REMOTE_IDLE;
static char s_status[80] = "Disconnected";
static FlipperDeviceInfo s_target = {};
static esp_gatt_if_t s_gattc_if = ESP_GATT_IF_NONE;
static uint16_t s_conn_id = 0;
static uint16_t s_service_start = 0;
static uint16_t s_service_end = 0;
static uint16_t s_rx_handle = 0;
static uint16_t s_rpc_status_handle = 0;
static volatile bool s_connected = false;
static volatile bool s_pair_pending = false;
static uint32_t s_pair_code = 0;
static uint8_t s_pair_bda[6] = {};
static volatile bool s_write_pending = false;
static bool s_hold_owned = false;
static bool s_app_registered = false;
static volatile bool s_cleanup_pending = false;
static uint32_t s_deadline_ms = 0;
static uint32_t s_command_id = 1;

static void set_status(FlipperRemoteState state, const char *text)
{
    s_state = state;
    strncpy(s_status, text ? text : "", sizeof(s_status) - 1);
    s_status[sizeof(s_status) - 1] = '\0';
}

static esp_bt_uuid_t uuid128(const uint8_t value[16])
{
    esp_bt_uuid_t out = {};
    out.len = ESP_UUID_LEN_128;
    memcpy(out.uuid.uuid128, value, 16);
    return out;
}

static bool uuid_equal(const esp_bt_uuid_t &uuid, const uint8_t value[16])
{
    return uuid.len == ESP_UUID_LEN_128 &&
           memcmp(uuid.uuid.uuid128, value, 16) == 0;
}

static void release_hold()
{
    if (!s_hold_owned) return;
    s_hold_owned = false;
    ble_scan_set_gap_observer(nullptr);
    ble_scan_hold_release();
}

static void fail(const char *reason)
{
    Serial.printf("[Flipper RPC] %s\n", reason ? reason : "failed");
    set_status(FLIPPER_REMOTE_ERROR, reason ? reason : "Connection failed");
    s_pair_pending = false;
    s_write_pending = false;
    if (s_gattc_if != ESP_GATT_IF_NONE && s_connected)
        esp_ble_gattc_close(s_gattc_if, s_conn_id);
    else
        s_cleanup_pending = true;
}

static bool discover_characteristics()
{
    esp_gattc_char_elem_t found = {};
    uint16_t count = 1;
    esp_bt_uuid_t rx_uuid = uuid128(UUID_SERIAL_RX);
    if (esp_ble_gattc_get_char_by_uuid(s_gattc_if, s_conn_id,
                                      s_service_start, s_service_end,
                                      rx_uuid, &found, &count) != ESP_GATT_OK ||
        count == 0) return false;
    s_rx_handle = found.char_handle;

    found = {};
    count = 1;
    esp_bt_uuid_t status_uuid = uuid128(UUID_RPC_STATUS);
    if (esp_ble_gattc_get_char_by_uuid(s_gattc_if, s_conn_id,
                                      s_service_start, s_service_end,
                                      status_uuid, &found, &count) != ESP_GATT_OK ||
        count == 0) return false;
    s_rpc_status_handle = found.char_handle;
    return true;
}

static void start_service_search()
{
    s_pair_pending = false;
    set_status(FLIPPER_REMOTE_DISCOVERING, "Paired - opening RPC service...");
    esp_bt_uuid_t service = uuid128(UUID_SERIAL_SERVICE);
    if (esp_ble_gattc_search_service(s_gattc_if, s_conn_id, &service) != ESP_OK)
        fail("RPC service search failed");
    else
        s_deadline_ms = millis() + 10000U;
}

static void on_gap_event(esp_gap_ble_cb_event_t event,
                         esp_ble_gap_cb_param_t *param)
{
    if (!param || s_state == FLIPPER_REMOTE_IDLE) return;

    switch (event) {
    case ESP_GAP_BLE_SEC_REQ_EVT:
        esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
        break;

    case ESP_GAP_BLE_NC_REQ_EVT:
        memcpy(s_pair_bda, param->ble_security.ble_req.bd_addr, 6);
        s_pair_code = param->ble_security.key_notif.passkey;
        s_pair_pending = true;
        set_status(FLIPPER_REMOTE_PAIRING, "Compare the code on both devices");
        s_deadline_ms = millis() + 30000U;
        break;

    case ESP_GAP_BLE_PASSKEY_REQ_EVT:
        // This UI intentionally supports secure numeric comparison rather
        // than accepting an invisible/default PIN.  Reject a legacy passkey
        // request and tell the user how to recover.
        esp_ble_passkey_reply(param->ble_security.ble_req.bd_addr, false, 0);
        fail("Pairing mode unsupported - forget pairings and retry");
        break;

    case ESP_GAP_BLE_AUTH_CMPL_EVT:
        if (param->ble_security.auth_cmpl.success) start_service_search();
        else fail("BLE pairing rejected");
        break;

    default:
        break;
    }
}

static void on_gatt_event(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                          esp_ble_gattc_cb_param_t *param)
{
    if (!param) return;

    switch (event) {
    case ESP_GATTC_REG_EVT:
        if (param->reg.app_id != GATTC_APP_ID) break;
        if (param->reg.status != ESP_GATT_OK) {
            fail("BLE client registration failed");
            break;
        }
        s_app_registered = true;
        s_gattc_if = gattc_if;
        set_status(FLIPPER_REMOTE_CONNECTING, "Connecting to selected Flipper...");
        if (esp_ble_gattc_open(gattc_if, s_target.mac,
                              (esp_ble_addr_type_t)s_target.addr_type,
                              true) != ESP_OK)
            fail("BLE connection request failed");
        else
            s_deadline_ms = millis() + 15000U;
        break;

    case ESP_GATTC_OPEN_EVT:
        if (gattc_if != s_gattc_if) break;
        if (param->open.status != ESP_GATT_OK) {
            fail("Flipper connection failed");
            break;
        }
        s_conn_id = param->open.conn_id;
        s_connected = true;
        set_status(FLIPPER_REMOTE_PAIRING, "Securing BLE connection...");
        if (esp_ble_set_encryption(param->open.remote_bda,
                                   ESP_BLE_SEC_ENCRYPT_MITM) != ESP_OK)
            fail("Could not start secure pairing");
        else
            s_deadline_ms = millis() + 30000U;
        break;

    case ESP_GATTC_SEARCH_RES_EVT:
        if (gattc_if == s_gattc_if &&
            uuid_equal(param->search_res.srvc_id.uuid, UUID_SERIAL_SERVICE)) {
            s_service_start = param->search_res.start_handle;
            s_service_end = param->search_res.end_handle;
        }
        break;

    case ESP_GATTC_SEARCH_CMPL_EVT:
        if (gattc_if != s_gattc_if) break;
        if (param->search_cmpl.status != ESP_GATT_OK || !s_service_start ||
            !discover_characteristics()) {
            fail("Flipper RPC characteristics not found");
            break;
        }
        {
            // The official status characteristic is a four-byte little-endian
            // boolean.  Setting it to 1 enables the protobuf RPC session.
            uint8_t active[4] = {1, 0, 0, 0};
            s_write_pending = true;
            if (esp_ble_gattc_write_char(s_gattc_if, s_conn_id,
                                         s_rpc_status_handle,
                                         sizeof(active), active,
                                         ESP_GATT_WRITE_TYPE_RSP,
                                         ESP_GATT_AUTH_REQ_MITM) != ESP_OK) {
                s_write_pending = false;
                fail("Could not enable Flipper RPC");
            }
        }
        break;

    case ESP_GATTC_WRITE_CHAR_EVT:
        if (gattc_if != s_gattc_if) break;
        s_write_pending = false;
        if (param->write.status != ESP_GATT_OK) {
            fail("Encrypted RPC write failed");
        } else if (param->write.handle == s_rpc_status_handle &&
                   s_state != FLIPPER_REMOTE_DISCONNECTING) {
            set_status(FLIPPER_REMOTE_READY, "Connected - remote input ready");
            s_deadline_ms = 0;
        }
        break;

    case ESP_GATTC_DISCONNECT_EVT:
    case ESP_GATTC_CLOSE_EVT:
        if (gattc_if != s_gattc_if) break;
        s_conn_id = 0;
        s_connected = false;
        s_rx_handle = 0;
        s_rpc_status_handle = 0;
        s_service_start = s_service_end = 0;
        s_pair_pending = false;
        s_write_pending = false;
        s_cleanup_pending = false;
        if (s_app_registered) {
            esp_ble_gattc_app_unregister(s_gattc_if);
            s_app_registered = false;
        }
        s_gattc_if = ESP_GATT_IF_NONE;
        if (s_state == FLIPPER_REMOTE_ERROR)
            release_hold();
        else {
            set_status(FLIPPER_REMOTE_IDLE, "Disconnected");
            release_hold();
        }
        break;

    default:
        break;
    }
}

static void configure_security()
{
    esp_ble_auth_req_t auth = ESP_LE_AUTH_REQ_SC_MITM_BOND;
    esp_ble_io_cap_t io = ESP_IO_CAP_IO;
    uint8_t key_size = 16;
    uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth, sizeof(auth));
    esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &io, sizeof(io));
    esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(key_size));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, sizeof(init_key));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key, sizeof(rsp_key));
}

bool flipper_remote_connect(const FlipperDeviceInfo *device)
{
    if (!device || (s_state != FLIPPER_REMOTE_IDLE &&
                    s_state != FLIPPER_REMOTE_ERROR)) return false;
    if (s_cleanup_pending || s_app_registered || s_hold_owned) return false;
    s_target = *device;
    s_service_start = s_service_end = 0;
    s_rx_handle = s_rpc_status_handle = 0;
    s_pair_pending = false;
    s_write_pending = false;
    s_hold_owned = true;
    ble_scan_hold_acquire();
    ble_scan_set_gap_observer(on_gap_event);
    set_status(FLIPPER_REMOTE_STARTING, "Starting Bluetooth client...");
    s_deadline_ms = millis() + 10000U;
    return true;
}

void flipper_remote_disconnect()
{
    if (s_state == FLIPPER_REMOTE_IDLE) return;
    s_pair_pending = false;
    if (s_gattc_if != ESP_GATT_IF_NONE && s_connected) {
        set_status(FLIPPER_REMOTE_DISCONNECTING, "Disconnecting...");
        s_deadline_ms = millis() + 5000U;
        esp_ble_gattc_close(s_gattc_if, s_conn_id);
    } else {
        if (s_app_registered && s_gattc_if != ESP_GATT_IF_NONE) {
            esp_ble_gattc_app_unregister(s_gattc_if);
            s_app_registered = false;
        }
        s_gattc_if = ESP_GATT_IF_NONE;
        set_status(FLIPPER_REMOTE_IDLE, "Disconnected");
        release_hold();
    }
}

void flipper_remote_bg_tick()
{
    uint32_t now = millis();
    if (s_cleanup_pending && !s_connected) {
        s_cleanup_pending = false;
        if (s_app_registered && s_gattc_if != ESP_GATT_IF_NONE) {
            esp_ble_gattc_app_unregister(s_gattc_if);
            s_app_registered = false;
        }
        s_gattc_if = ESP_GATT_IF_NONE;
        release_hold();
    }

    if (s_state == FLIPPER_REMOTE_STARTING && ble_scan_ready()) {
        configure_security();
        // Bluedroid deinit discards callback registration, so renew it for
        // every staged startup even though the C++ function pointer is stable.
        if (esp_ble_gattc_register_callback(on_gatt_event) != ESP_OK) {
            fail("Could not register BLE RPC client");
            return;
        }
        if (esp_ble_gattc_app_register(GATTC_APP_ID) != ESP_OK) {
            fail("Could not start BLE RPC client");
            return;
        }
        set_status(FLIPPER_REMOTE_CONNECTING, "Preparing connection...");
        s_deadline_ms = now + 10000U;
    }

    if (s_deadline_ms && (int32_t)(now - s_deadline_ms) >= 0 &&
        s_state != FLIPPER_REMOTE_IDLE && s_state != FLIPPER_REMOTE_READY &&
        s_state != FLIPPER_REMOTE_ERROR) {
        fail("Flipper RPC timed out");
        s_deadline_ms = 0;
    }
}

FlipperRemoteState flipper_remote_state() { return s_state; }
const char *flipper_remote_status() { return s_status; }
bool flipper_remote_is_ready() { return s_state == FLIPPER_REMOTE_READY; }
bool flipper_remote_pairing_pending() { return s_pair_pending; }
uint32_t flipper_remote_pairing_code() { return s_pair_code; }

void flipper_remote_confirm_pairing(bool accept)
{
    if (!s_pair_pending) return;
    s_pair_pending = false;
    esp_ble_confirm_reply(s_pair_bda, accept);
    if (accept) {
        set_status(FLIPPER_REMOTE_PAIRING, "Confirm the same code on Flipper...");
        s_deadline_ms = millis() + 30000U;
    } else {
        fail("Pairing cancelled");
    }
}

static size_t put_varint(uint8_t *out, uint32_t value)
{
    size_t n = 0;
    do {
        uint8_t byte = value & 0x7FU;
        value >>= 7;
        if (value) byte |= 0x80U;
        out[n++] = byte;
    } while (value && n < 5);
    return n;
}

static bool send_main(uint32_t content_field, const uint8_t *sub, size_t sub_len)
{
    if (!flipper_remote_is_ready() || !s_rx_handle || s_write_pending ||
        sub_len > 24) return false;

    uint8_t body[48];
    size_t b = 0;
    body[b++] = 0x08;  // PB.Main.command_id, varint
    b += put_varint(body + b, s_command_id++);
    b += put_varint(body + b, (content_field << 3) | 2U);
    b += put_varint(body + b, (uint32_t)sub_len);
    if (sub_len) {
        memcpy(body + b, sub, sub_len);
        b += sub_len;
    }

    // Flipper RPC uses protobuf-delimited PB.Main frames.
    uint8_t frame[52];
    size_t f = put_varint(frame, (uint32_t)b);
    memcpy(frame + f, body, b);
    f += b;

    s_write_pending = true;
    esp_err_t rc = esp_ble_gattc_write_char(
        s_gattc_if, s_conn_id, s_rx_handle, f, frame,
        ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_MITM);
    if (rc != ESP_OK) s_write_pending = false;
    return rc == ESP_OK;
}

bool flipper_remote_send_key(FlipperRemoteKey key)
{
    // PB_Gui.SendInputEventRequest: key=field 1, type=field 2 (SHORT=2).
    uint8_t sub[4];
    size_t n = 0;
    if (key != FLIPPER_KEY_UP) {
        sub[n++] = 0x08;
        sub[n++] = (uint8_t)key;
    }
    sub[n++] = 0x10;
    sub[n++] = 0x02;
    return send_main(23, sub, n);
}

bool flipper_remote_locate()
{
    // PB_System.PlayAudiovisualAlertRequest is an empty message, field 38.
    return send_main(38, nullptr, 0);
}

bool flipper_remote_ping()
{
    // PB_System.PingRequest { data: "TW" }, field 5.
    const uint8_t sub[] = {0x0A, 0x02, 'T', 'W'};
    return send_main(5, sub, sizeof(sub));
}
