#include "nfc_screen.h"
#include "nfc_write_screen.h"
#include <LilyGoLib.h>
#include <SD.h>
#include <stdarg.h>

static lv_obj_t *nfc_screen;
static lv_obj_t *toggle_sw;
static lv_obj_t *status_label;
static lv_obj_t *read_btn;
static lv_obj_t *read_btn_label;
static lv_obj_t *write_btn;
static lv_obj_t *write_btn_label;
static lv_obj_t *save_btn;
static lv_obj_t *save_btn_label;
static lv_obj_t *data_panel;
static lv_obj_t *data_label;

static bool nfc_powered = false;

enum NfcReadState { NFC_IDLE, NFC_DISCOVERING };
static NfcReadState s_read_state  = NFC_IDLE;
static bool         s_card_ready  = false;
static uint8_t      s_raw_buf[1024];
static uint32_t     s_raw_len = 0;
static bool         s_have_scan = false;
static char         s_last_report[1800];
static char         s_last_uid[32];

static void appendf(char *buf, int &n, int bufsize, const char *fmt, ...)
{
    if (n >= bufsize - 1) return;
    va_list args;
    va_start(args, fmt);
    int wrote = vsnprintf(buf + n, bufsize - n, fmt, args);
    va_end(args);
    if (wrote < 0) return;
    n += wrote;
    if (n >= bufsize) n = bufsize - 1;
}

static const char *tech_name(const rfalNfcDevice *dev)
{
    switch (dev->type) {
    case RFAL_NFC_POLL_TYPE_NFCA: return "NFC-A / ISO 14443A";
    case RFAL_NFC_POLL_TYPE_NFCB: return "NFC-B / ISO 14443B";
    case RFAL_NFC_POLL_TYPE_NFCF: return "NFC-F / FeliCa";
    case RFAL_NFC_POLL_TYPE_NFCV: return "NFC-V / ISO 15693";
    case RFAL_NFC_LISTEN_TYPE_ST25TB: return "ST25TB";
    case RFAL_NFC_POLL_TYPE_AP2P: return "NFC peer-to-peer";
    default: return "Unknown NFC technology";
    }
}

static const char *interface_name(rfalNfcRfInterface intf)
{
    switch (intf) {
    case RFAL_NFC_INTERFACE_RF: return "RF";
    case RFAL_NFC_INTERFACE_ISODEP: return "ISO-DEP";
    case RFAL_NFC_INTERFACE_NFCDEP: return "NFC-DEP";
    default: return "Unknown";
    }
}

static const char *ndef_device_name(ndefDeviceType type)
{
    switch (type) {
    case NDEF_DEV_T1T: return "NFC Forum Type 1";
    case NDEF_DEV_T2T: return "NFC Forum Type 2";
    case NDEF_DEV_T3T: return "NFC Forum Type 3";
    case NDEF_DEV_T4T: return "NFC Forum Type 4";
    case NDEF_DEV_T5T: return "NFC Forum Type 5";
    default: return "NDEF-capable tag";
    }
}

static ndefDeviceType ndef_device_type(const rfalNfcDevice *dev)
{
    if (dev->type == RFAL_NFC_POLL_TYPE_NFCA) {
        if (dev->dev.nfca.type == RFAL_NFCA_T1T) return NDEF_DEV_T1T;
        if (dev->dev.nfca.type == RFAL_NFCA_T4T ||
            dev->dev.nfca.type == RFAL_NFCA_T4T_NFCDEP) return NDEF_DEV_T4T;
        return NDEF_DEV_T2T;
    }
    if (dev->type == RFAL_NFC_POLL_TYPE_NFCB) return NDEF_DEV_T4T;
    if (dev->type == RFAL_NFC_POLL_TYPE_NFCF) return NDEF_DEV_T3T;
    if (dev->type == RFAL_NFC_POLL_TYPE_NFCV) return NDEF_DEV_T5T;
    return NDEF_DEV_NONE;
}

static const char *ndef_state_name(ndefState state)
{
    switch (state) {
    case NDEF_STATE_INITIALIZED: return "initialized / empty";
    case NDEF_STATE_READWRITE: return "read/write";
    case NDEF_STATE_READONLY: return "read-only";
    default: return "invalid";
    }
}

static void append_printable_preview(char *buf, int &n, int bufsize,
                                     const uint8_t *data, uint32_t len)
{
    uint32_t shown = len > 96U ? 96U : len;
    appendf(buf, n, bufsize, "Payload: ");
    for (uint32_t i = 0; i < shown && n < bufsize - 2; ++i) {
        char c = (char)data[i];
        buf[n++] = (c >= 32 && c <= 126) ? c : '.';
    }
    if (shown < len) appendf(buf, n, bufsize, "...");
    appendf(buf, n, bufsize, "\n");
}

// Called by the RFAL state machine (from within rfalNfcWorker)
static void on_rfal_notify(rfalNfcState st)
{
    if (st == RFAL_NFC_STATE_ACTIVATED)
        s_card_ready = true;
}

static void start_discovery()
{
    rfalNfcDiscoverParam p;
    memset(&p, 0, sizeof(p));
    p.devLimit        = 1;
    p.techs2Find      = RFAL_NFC_POLL_TECH_A |
                        RFAL_NFC_POLL_TECH_B |
                        RFAL_NFC_POLL_TECH_F |
                        RFAL_NFC_POLL_TECH_V |
                        RFAL_NFC_POLL_TECH_ST25TB;
    p.nfcfBR          = RFAL_BR_212;
    p.GBLen           = RFAL_NFCDEP_GB_MAX_LEN;
    p.notifyCb        = on_rfal_notify;
    p.totalDuration   = 1000U;
    p.wakeupEnabled   = false;
    NFCReader.rfalNfcDiscover(&p);
}

// Appends NDEF record content to buf[n..bufsize]. Updates n.
static void append_ndef(NdefClass &ndef, rfalNfcDevice *dev, char *buf, int &n, int bufsize)
{
    ReturnCode err = ndef.ndefPollerContextInitialization(dev);
    if (err != ST_ERR_NONE) {
        appendf(buf, n, bufsize, "NDEF: not supported\n");
        return;
    }

    ndefInfo info;
    err = ndef.ndefPollerNdefDetect(&info);
    if (err != ST_ERR_NONE) {
        appendf(buf, n, bufsize, "NDEF: no formatted message (%d)\n", (int)err);
        return;
    }

    appendf(buf, n, bufsize,
            "Tag: %s\nNDEF: v%u.%u, %s\nCapacity: %lu bytes, message %lu, free %lu\n",
            ndef_device_name(ndef_device_type(dev)),
            (unsigned)info.majorVersion, (unsigned)info.minorVersion,
            ndef_state_name(info.state), (unsigned long)info.areaLen,
            (unsigned long)info.messageLen,
            (unsigned long)info.areaAvalableSpaceLen);

    uint32_t actual = 0;
    memset(s_raw_buf, 0, sizeof(s_raw_buf));
    err = ndef.ndefPollerReadRawMessage(s_raw_buf, sizeof(s_raw_buf), &actual);
    if (err != ST_ERR_NONE) {
        appendf(buf, n, bufsize, "Read error: %d\n", (int)err);
        return;
    }
    s_raw_len = actual;
    if (actual == 0) {
        appendf(buf, n, bufsize, "Records: 0\n");
        return;
    }

    ndefMessage    msg;
    ndefConstBuffer ndefBuf = { s_raw_buf, actual };
    err = ndef.ndefMessageDecode(&ndefBuf, &msg);
    if (err != ST_ERR_NONE) {
        appendf(buf, n, bufsize, "Decode error: %d\n", (int)err);
        return;
    }

    ndefRecord *rec = ndefMessageGetFirstRecord(&msg);
    int record_no = 0;
    while (rec && n < bufsize - 180) {
        ++record_no;
        appendf(buf, n, bufsize, "\nRecord %d: TNF %u, type %.*s, %lu bytes\n",
                record_no, (unsigned)ndefHeaderTNF(rec),
                (int)rec->typeLength, rec->type ? (const char *)rec->type : "",
                (unsigned long)rec->bufPayload.length);
        ndefType type;
        if (ndef.ndefRecordToType(rec, &type) == ST_ERR_NONE) {
            switch (type.id) {
            case NDEF_TYPE_RTD_TEXT: {
                uint8_t enc;
                ndefConstBuffer8 lang;
                ndefConstBuffer  sentence;
                ndef.ndefGetRtdText(&type, &enc, &lang, &sentence);
                appendf(buf, n, bufsize, "Text [%.*s]: %.*s\n",
                    (int)lang.length, (const char *)lang.buffer,
                    (int)sentence.length, (const char *)sentence.buffer);
                break;
            }
            case NDEF_TYPE_RTD_URI: {
                ndefConstBuffer proto, url;
                ndef.ndefGetRtdUri(&type, &proto, &url);
                appendf(buf, n, bufsize, "URI: %.*s%.*s\n",
                    (int)proto.length,  (const char *)proto.buffer,
                    (int)url.length,    (const char *)url.buffer);
                break;
            }
            case NDEF_TYPE_MEDIA_WIFI: {
                ndefTypeWifi wifi;
                ndef.ndefGetWifi(&type, &wifi);
                appendf(buf, n, bufsize, "WiFi SSID: %.*s\nKey: [%lu bytes hidden]\n",
                    (int)wifi.bufNetworkSSID.length, (const char *)wifi.bufNetworkSSID.buffer,
                    (unsigned long)wifi.bufNetworkKey.length);
                break;
            }
            case NDEF_TYPE_RTD_AAR: {
                ndefConstBuffer aar;
                ndef.ndefGetRtdAar(&type, &aar);
                appendf(buf, n, bufsize, "Android app: %.*s\n",
                    (int)aar.length, (const char *)aar.buffer);
                break;
            }
            case NDEF_TYPE_MEDIA:
            case NDEF_TYPE_MEDIA_VCARD: {
                ndefConstBuffer8 media_type;
                ndefConstBuffer payload;
                if (ndef.ndefGetMedia(&type, &media_type, &payload) == ST_ERR_NONE) {
                    appendf(buf, n, bufsize, "MIME: %.*s\n",
                            (int)media_type.length, (const char *)media_type.buffer);
                    append_printable_preview(buf, n, bufsize, payload.buffer, payload.length);
                }
                break;
            }
            default:
                append_printable_preview(buf, n, bufsize,
                                         rec->bufPayload.buffer,
                                         rec->bufPayload.length);
                break;
            }
        } else {
            append_printable_preview(buf, n, bufsize,
                                     rec->bufPayload.buffer,
                                     rec->bufPayload.length);
        }
        rec = ndefMessageGetNextRecord(rec);
    }

    appendf(buf, n, bufsize, "\nRaw NDEF (%lu bytes): ", (unsigned long)s_raw_len);
    uint32_t preview = s_raw_len > 48U ? 48U : s_raw_len;
    for (uint32_t i = 0; i < preview; ++i) appendf(buf, n, bufsize, "%02X", s_raw_buf[i]);
    if (preview < s_raw_len) appendf(buf, n, bufsize, "...");
    appendf(buf, n, bufsize, "\n");
}

static void nfc_process_card()
{
    rfalNfcDevice *dev = nullptr;
    if (NFCReader.rfalNfcGetActiveDevice(&dev) != ST_ERR_NONE || !dev) {
        lv_label_set_text(data_label, "Activation completed, but device details were unavailable");
        return;
    }

    char *buf = s_last_report;
    const int bufsize = (int)sizeof(s_last_report);
    int  n = 0;
    s_raw_len = 0;
    s_have_scan = false;
    s_last_uid[0] = '\0';

    appendf(buf, n, bufsize, "Technology: %s\nInterface: %s\n",
            tech_name(dev), interface_name(dev->rfInterface));

    // Card UID
    appendf(buf, n, bufsize, "UID:");
    int uid_n = 0;
    for (int i = 0; i < dev->nfcidLen; i++) {
        appendf(buf, n, bufsize, " %02X", dev->nfcid[i]);
        if (uid_n < (int)sizeof(s_last_uid) - 3)
            uid_n += snprintf(s_last_uid + uid_n, sizeof(s_last_uid) - uid_n,
                              "%02X", dev->nfcid[i]);
    }
    appendf(buf, n, bufsize, "\n");

    if (dev->type == RFAL_NFC_POLL_TYPE_NFCA) {
        const rfalNfcaListenDevice &a = dev->dev.nfca;
        const char *kind = "Type 2 / MIFARE-family";
        if (a.type == RFAL_NFCA_T1T) kind = "NFC Forum Type 1";
        else if (a.type == RFAL_NFCA_T4T) kind = "NFC Forum Type 4A";
        else if (a.type == RFAL_NFCA_NFCDEP) kind = "NFC-DEP";
        else if (a.type == RFAL_NFCA_T4T_NFCDEP) kind = "Type 4A + NFC-DEP";
        appendf(buf, n, bufsize, "Card family: %s\nATQA: %02X %02X   SAK: %02X\n",
                kind, a.sensRes.anticollisionInfo, a.sensRes.platformInfo,
                a.selRes.sak);
    } else if (dev->type == RFAL_NFC_POLL_TYPE_NFCB) {
        const rfalNfcbListenDevice &b = dev->dev.nfcb;
        appendf(buf, n, bufsize, "ATQB length: %u   AFI: %02X\nProtocol: %s\n",
                (unsigned)b.sensbResLen, b.sensbRes.appData.AFI,
                rfalNfcbIsIsoDepSupported(&b) ? "ISO-DEP supported" : "raw RF");
    } else if (dev->type == RFAL_NFC_POLL_TYPE_NFCF) {
        appendf(buf, n, bufsize, "SENSF response: %u bytes\n",
                (unsigned)dev->dev.nfcf.sensfResLen);
    } else if (dev->type == RFAL_NFC_POLL_TYPE_NFCV) {
        appendf(buf, n, bufsize, "DSFID: %02X   Flags: %02X\n",
                dev->dev.nfcv.InvRes.DSFID, dev->dev.nfcv.InvRes.RES_FLAG);
    }

    appendf(buf, n, bufsize, "\n");

    // NDEF records
    NdefClass ndef(&NFCReader);
    append_ndef(ndef, dev, buf, n, bufsize);

    if (n == 0) appendf(buf, n, bufsize, "(Empty)");
    buf[n] = '\0';
    s_have_scan = true;

    lv_label_set_text(data_label, buf);
    lv_obj_scroll_to_y(data_panel, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(save_btn, lv_color_make(0x22, 0x66, 0x99), LV_PART_MAIN);
    lv_obj_set_style_text_color(save_btn_label, lv_color_white(), LV_PART_MAIN);
}

static void on_save_btn(lv_event_t *)
{
    if (!s_have_scan) {
        lv_label_set_text(status_label, "Read a tag before saving");
        return;
    }
    if (!instance.isCardReady()) {
        lv_label_set_text(status_label, "SD card unavailable");
        return;
    }

    SD.mkdir("/NFC");
    struct tm now = {};
    instance.rtc.getDateTime(&now);
    char path[96];
    snprintf(path, sizeof(path), "/NFC/%04d%02d%02d_%02d%02d%02d_%s.txt",
             now.tm_year + 1900, now.tm_mon + 1, now.tm_mday,
             now.tm_hour, now.tm_min, now.tm_sec,
             s_last_uid[0] ? s_last_uid : "NOUID");
    File f = SD.open(path, FILE_WRITE);
    if (!f) {
        lv_label_set_text(status_label, "Could not create NFC log");
        return;
    }
    f.println(s_last_report);
    if (s_raw_len) {
        f.printf("\nFull raw NDEF (%lu bytes):\n", (unsigned long)s_raw_len);
        for (uint32_t i = 0; i < s_raw_len; ++i) {
            f.printf("%02X", s_raw_buf[i]);
            if ((i & 31U) == 31U) f.println();
        }
        f.println();
    }
    f.close();
    lv_label_set_text_fmt(status_label, "Saved %s", path);
}

static void set_read_btn_scanning(bool scanning)
{
    if (scanning) {
        lv_obj_set_style_bg_color(read_btn, lv_color_make(0xCC, 0x22, 0x22), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(read_btn_label, lv_color_white(), LV_PART_MAIN);
        lv_label_set_text(read_btn_label, "Stop");
    } else {
        lv_color_t bg  = nfc_powered ? lv_color_make(0x00, 0xCC, 0x66) : lv_color_make(0x33, 0x33, 0x33);
        lv_color_t txt = nfc_powered ? lv_color_white()                 : lv_color_make(0x77, 0x77, 0x77);
        lv_obj_set_style_bg_color(read_btn, bg, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(read_btn_label, txt, LV_PART_MAIN);
        lv_label_set_text(read_btn_label, "Read");
    }
}

static void update_ui()
{
    lv_label_set_text(status_label, nfc_powered ? "NFC: ON" : "NFC: OFF");

    lv_color_t bg  = nfc_powered ? lv_color_make(0x00, 0xCC, 0x66) : lv_color_make(0x33, 0x33, 0x33);
    lv_color_t txt = nfc_powered ? lv_color_white()                 : lv_color_make(0x77, 0x77, 0x77);

    lv_obj_set_style_bg_color(write_btn, bg,  LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(write_btn_label, txt, LV_PART_MAIN);

    lv_color_t save_bg = s_have_scan ? lv_color_make(0x22, 0x66, 0x99)
                                     : lv_color_make(0x33, 0x33, 0x33);
    lv_obj_set_style_bg_color(save_btn, save_bg, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(save_btn_label,
        s_have_scan ? lv_color_white() : lv_color_make(0x77, 0x77, 0x77),
        LV_PART_MAIN);

    // Read button always reflects the live scan state so it stays correct
    // when re-entering the screen mid-scan
    set_read_btn_scanning(s_read_state == NFC_DISCOVERING);
}

static void on_toggle(lv_event_t *e)
{
    nfc_powered = lv_obj_has_state(toggle_sw, LV_STATE_CHECKED);
    instance.powerControl(POWER_NFC, nfc_powered);
    if (nfc_powered) {
        instance.initNFC();
    } else {
        if (s_read_state == NFC_DISCOVERING) {
            NFCReader.rfalNfcDeactivate(false);
        }
        s_read_state = NFC_IDLE;
        set_read_btn_scanning(false);
        lv_label_set_text(data_label, "");
        s_have_scan = false;
    }
    update_ui();
}

static void on_read_btn(lv_event_t *e)
{
    if (!nfc_powered) return;

    if (s_read_state == NFC_DISCOVERING) {
        // Stop scanning
        s_read_state = NFC_IDLE;
        NFCReader.rfalNfcDeactivate(false);
        set_read_btn_scanning(false);
        lv_label_set_text(data_label, "");
    } else {
        // Start scanning
        s_card_ready = false;
        s_read_state = NFC_DISCOVERING;
        set_read_btn_scanning(true);
        lv_label_set_text(data_label, "Scanning...\nHold card near watch");
        start_discovery();
    }
}

static void on_write_btn(lv_event_t *e)
{
    if (!nfc_powered) return;
    // Stop any in-progress read scan before handing the reader to the writer
    if (s_read_state == NFC_DISCOVERING) {
        NFCReader.rfalNfcDeactivate(false);
        s_read_state = NFC_IDLE;
    }
    nfc_write_screen_show();
}

static lv_obj_t *make_btn(lv_obj_t *parent, const char *text, int x_ofs, int y_ofs,
                           lv_obj_t **out_label)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_set_size(btn, 160, 60);
    lv_obj_set_style_radius(btn, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_make(0x33, 0x33, 0x33), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(btn, LV_ALIGN_TOP_MID, x_ofs, y_ofs);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl, lv_color_make(0x77, 0x77, 0x77), LV_PART_MAIN);
    lv_label_set_text(lbl, text);
    lv_obj_center(lbl);

    *out_label = lbl;
    return btn;
}

void nfc_screen_create()
{
    nfc_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(nfc_screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(nfc_screen, 0, LV_PART_MAIN);

    // Title
    lv_obj_t *title = lv_label_create(nfc_screen);
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_label_set_text(title, "NFC");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 5);

    // Power toggle
    toggle_sw = lv_switch_create(nfc_screen);
    lv_obj_set_size(toggle_sw, 100, 50);
    lv_obj_set_style_bg_color(toggle_sw, lv_color_make(0x44, 0x44, 0x44), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(toggle_sw, lv_color_make(0x00, 0xCC, 0x66), LV_PART_MAIN | LV_STATE_CHECKED);
    lv_obj_add_event_cb(toggle_sw, on_toggle, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_align(toggle_sw, LV_ALIGN_TOP_MID, -90, 72);

    // Status label (right of toggle)
    status_label = lv_label_create(nfc_screen);
    lv_obj_set_style_text_color(status_label, lv_color_make(0xAA, 0xAA, 0xAA), LV_PART_MAIN);
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_obj_align(status_label, LV_ALIGN_TOP_MID, 60, 87);

    // Read / Write buttons — gray until NFC is on
    read_btn  = make_btn(nfc_screen, "Read",  -130, 160, &read_btn_label);
    save_btn  = make_btn(nfc_screen, "Save",     0, 160, &save_btn_label);
    write_btn = make_btn(nfc_screen, "Write", +130, 160, &write_btn_label);
    lv_obj_set_width(read_btn, 120);
    lv_obj_set_width(save_btn, 120);
    lv_obj_set_width(write_btn, 120);
    lv_obj_add_event_cb(read_btn,  on_read_btn,  LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(save_btn,  on_save_btn,  LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(write_btn, on_write_btn, LV_EVENT_CLICKED, NULL);

    // Scrollable data result panel
    data_panel = lv_obj_create(nfc_screen);
    lv_obj_set_size(data_panel, 390, 210);
    lv_obj_align(data_panel, LV_ALIGN_TOP_MID, 0, 232);
    lv_obj_set_style_bg_color(data_panel, lv_color_make(0x0A, 0x0A, 0x0A), LV_PART_MAIN);
    lv_obj_set_style_border_color(data_panel, lv_color_make(0x33, 0x33, 0x33), LV_PART_MAIN);
    lv_obj_set_style_border_width(data_panel, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(data_panel, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(data_panel, 8, LV_PART_MAIN);
    lv_obj_set_scroll_dir(data_panel, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(data_panel, LV_SCROLLBAR_MODE_AUTO);

    data_label = lv_label_create(data_panel);
    lv_obj_set_width(data_label, lv_pct(100));
    lv_obj_set_style_text_color(data_label, lv_color_make(0xCC, 0xCC, 0xCC), LV_PART_MAIN);
    lv_obj_set_style_text_font(data_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_label_set_long_mode(data_label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(data_label, "");

    // Navigation hint
    lv_obj_t *hint = lv_label_create(nfc_screen);
    lv_obj_set_style_text_color(hint, lv_color_make(0x44, 0x44, 0x44), LV_PART_MAIN);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_text(hint, "Boot button to return");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -8);

    update_ui();
}

void nfc_screen_show()
{
    bool hw_on = instance.pmu.isEnableDLDO1();
    nfc_powered = hw_on;
    if (hw_on)
        lv_obj_add_state(toggle_sw, LV_STATE_CHECKED);
    else
        lv_obj_clear_state(toggle_sw, LV_STATE_CHECKED);
    update_ui();
    lv_scr_load(nfc_screen);
}

bool nfc_screen_is_active()
{
    return lv_screen_active() == nfc_screen;
}

bool nfc_screen_is_powered()
{
    return nfc_powered;
}

void nfc_screen_worker()
{
    if (s_read_state == NFC_IDLE) return;

    // Pause while the user is on another screen, but keep the read state
    // so returning to the NFC screen resumes the scan with the "Stop" button
    if (!nfc_screen_is_active()) return;

    NFCReader.rfalNfcWorker();

    if (s_card_ready) {
        s_card_ready = false;
        nfc_process_card();
        // Discovery now covers A/B/F/V/ST25TB, so do not issue the old
        // NFC-A-specific sleep command after every activation.
        NFCReader.rfalNfcDeactivate(false);
        s_read_state = NFC_IDLE;
        set_read_btn_scanning(false);
    }
}
