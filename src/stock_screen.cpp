#include "stock_screen.h"
#include "usb_sd.h"
#include <Arduino.h>
#include <LilyGoLib.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <SD.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

// Defined in tools_screen.cpp
void tools_screen_show();
// Defined in main.cpp
void main_loop_request_lvgl_priority(int cycles);

#define STOCK_MAX_SYMBOLS 5
#define STOCK_SYMBOL_LEN  12
#define STOCK_CONFIG_PATH "/Stocks/config.txt"

struct StockQuote {
    char symbol[STOCK_SYMBOL_LEN];
    char price[20];
    char change[20];
    char pct[20];
    char stamp[24];
    char error[40];
    bool ok;
};

static lv_obj_t *stock_screen;
static lv_obj_t *status_label;
static lv_obj_t *list_box;
static lv_obj_t *refresh_btn;
static lv_obj_t *refresh_btn_label;

static char       s_api_key[48] = "";
static char       s_symbols[STOCK_MAX_SYMBOLS][STOCK_SYMBOL_LEN];
static int        s_symbol_count = 0;
static StockQuote s_quotes[STOCK_MAX_SYMBOLS];
static volatile bool s_fetching = false;
static volatile bool s_dirty    = false;
static TaskHandle_t  s_task     = nullptr;

static void set_default_symbols()
{
    static const char *defs[] = { "AAPL", "MSFT", "SPY", "QQQ" };
    s_symbol_count = 4;
    for (int i = 0; i < s_symbol_count; i++) {
        strncpy(s_symbols[i], defs[i], STOCK_SYMBOL_LEN - 1);
        s_symbols[i][STOCK_SYMBOL_LEN - 1] = '\0';
    }
}

static void trim(char *s)
{
    if (!s) return;
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    int n = (int)strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
}

static void uppercase_symbol(char *s)
{
    for (char *p = s; *p; ++p)
        *p = (char)toupper((unsigned char)*p);
}

static void parse_symbols(char *csv)
{
    s_symbol_count = 0;
    char *tok = strtok(csv, ",");
    while (tok && s_symbol_count < STOCK_MAX_SYMBOLS) {
        trim(tok);
        uppercase_symbol(tok);
        if (tok[0]) {
            strncpy(s_symbols[s_symbol_count], tok, STOCK_SYMBOL_LEN - 1);
            s_symbols[s_symbol_count][STOCK_SYMBOL_LEN - 1] = '\0';
            s_symbol_count++;
        }
        tok = strtok(nullptr, ",");
    }
    if (s_symbol_count == 0) set_default_symbols();
}

static void load_config()
{
    s_api_key[0] = '\0';
    set_default_symbols();

    if (!instance.isCardReady() || usb_sd_is_running()) return;
    if (!SD.exists(STOCK_CONFIG_PATH)) return;

    File f = SD.open(STOCK_CONFIG_PATH, FILE_READ);
    if (!f) return;

    while (f.available()) {
        char line[96] = {};
        size_t n = f.readBytesUntil('\n', line, sizeof(line) - 1);
        line[n] = '\0';
        trim(line);
        if (!line[0] || line[0] == '#') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = line;
        char *val = eq + 1;
        trim(key);
        trim(val);
        if (strcasecmp(key, "apikey") == 0) {
            strncpy(s_api_key, val, sizeof(s_api_key) - 1);
            s_api_key[sizeof(s_api_key) - 1] = '\0';
        } else if (strcasecmp(key, "symbols") == 0) {
            parse_symbols(val);
        }
    }
    f.close();
}

static bool json_value(const String &body, const char *key, char *out, size_t out_sz)
{
    if (!out || out_sz == 0) return false;
    out[0] = '\0';
    String needle = String("\"") + key + "\": \"";
    int i = body.indexOf(needle);
    if (i < 0) return false;
    i += needle.length();
    int j = body.indexOf('"', i);
    if (j < 0 || j <= i) return false;
    size_t n = (size_t)(j - i);
    if (n >= out_sz) n = out_sz - 1;
    memcpy(out, body.c_str() + i, n);
    out[n] = '\0';
    return true;
}

static void mark_error(int idx, const char *msg)
{
    if (idx < 0 || idx >= STOCK_MAX_SYMBOLS) return;
    StockQuote &q = s_quotes[idx];
    strncpy(q.symbol, s_symbols[idx], sizeof(q.symbol) - 1);
    q.symbol[sizeof(q.symbol) - 1] = '\0';
    q.ok = false;
    strncpy(q.error, msg, sizeof(q.error) - 1);
    q.error[sizeof(q.error) - 1] = '\0';
    s_dirty = true;
}

static void fetch_one(int idx)
{
    StockQuote &q = s_quotes[idx];
    memset(&q, 0, sizeof(q));
    strncpy(q.symbol, s_symbols[idx], sizeof(q.symbol) - 1);
    q.symbol[sizeof(q.symbol) - 1] = '\0';

    WiFiClientSecure client;
    client.setInsecure();

    String url = "https://www.alphavantage.co/query?function=GLOBAL_QUOTE&symbol=";
    url += q.symbol;
    url += "&apikey=";
    url += s_api_key;

    HTTPClient http;
    http.setConnectTimeout(7000);
    http.setTimeout(7000);
    if (!http.begin(client, url)) {
        mark_error(idx, "request setup failed");
        return;
    }

    int code = http.GET();
    if (code != 200) {
        http.end();
        char err[32];
        snprintf(err, sizeof(err), "HTTP %d", code);
        mark_error(idx, err);
        return;
    }

    String body = http.getString();
    http.end();

    if (body.indexOf("Note") >= 0 || body.indexOf("rate limit") >= 0) {
        mark_error(idx, "rate limited");
        return;
    }
    if (body.indexOf("Information") >= 0) {
        mark_error(idx, "bad API key/symbol");
        return;
    }

    bool ok = json_value(body, "05. price", q.price, sizeof(q.price));
    json_value(body, "09. change", q.change, sizeof(q.change));
    json_value(body, "10. change percent", q.pct, sizeof(q.pct));
    json_value(body, "07. latest trading day", q.stamp, sizeof(q.stamp));

    if (!ok || !q.price[0]) {
        mark_error(idx, "no quote returned");
        return;
    }
    q.ok = true;
    s_dirty = true;
}

static void fetch_task(void *)
{
    for (int i = 0; i < s_symbol_count; i++) {
        if (WiFi.status() != WL_CONNECTED) {
            mark_error(i, "WiFi disconnected");
            break;
        }
        fetch_one(i);
        vTaskDelay(pdMS_TO_TICKS(14000)); // Alpha Vantage free tier is tight.
    }
    s_fetching = false;
    s_task = nullptr;
    s_dirty = true;
    vTaskDelete(NULL);
}

static void start_fetch()
{
    load_config();
    memset(s_quotes, 0, sizeof(s_quotes));
    s_dirty = true;

    if (WiFi.status() != WL_CONNECTED) return;
    if (!s_api_key[0]) return;
    if (s_fetching) return;

    s_fetching = true;
    xTaskCreate(fetch_task, "stocks", 8192, nullptr, 1, &s_task);
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *text)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_set_size(b, 152, 44);
    lv_obj_set_style_radius(b, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(b, lv_color_make(0x00, 0x88, 0xCC), LV_PART_MAIN);
    lv_obj_set_style_border_width(b, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(b, 0, LV_PART_MAIN);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_color(l, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    refresh_btn_label = l;
    return b;
}

static lv_obj_t *make_card()
{
    lv_obj_t *card = lv_obj_create(list_box);
    lv_obj_set_width(card, lv_pct(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, lv_color_make(0x16, 0x16, 0x16), LV_PART_MAIN);
    lv_obj_set_style_border_color(card, lv_color_make(0x33, 0x33, 0x33), LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_row(card, 3, LV_PART_MAIN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_layout(card, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    return card;
}

static void add_text(lv_obj_t *parent, const char *txt, const lv_font_t *font,
                     lv_color_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, color, LV_PART_MAIN);
    lv_label_set_text(l, txt);
}

static void redraw()
{
    lv_obj_clean(list_box);

    if (WiFi.status() != WL_CONNECTED) {
        lv_label_set_text(status_label, "Connect WiFi first");
        lv_obj_set_style_text_color(status_label, lv_color_make(0xFF, 0xCC, 0x00), LV_PART_MAIN);
    } else if (!s_api_key[0]) {
        lv_label_set_text(status_label, "Add /Stocks/config.txt with apikey=");
        lv_obj_set_style_text_color(status_label, lv_color_make(0xFF, 0xCC, 0x00), LV_PART_MAIN);
    } else if (s_fetching) {
        lv_label_set_text(status_label, "Fetching quotes...");
        lv_obj_set_style_text_color(status_label, lv_color_make(0xFF, 0xCC, 0x00), LV_PART_MAIN);
    } else {
        lv_label_set_text(status_label, "Quotes ready");
        lv_obj_set_style_text_color(status_label, lv_color_make(0x00, 0xCC, 0x66), LV_PART_MAIN);
    }
    lv_label_set_text(refresh_btn_label, s_fetching ? "WAIT" : "REFRESH");

    for (int i = 0; i < s_symbol_count; i++) {
        lv_obj_t *card = make_card();
        const StockQuote &q = s_quotes[i];
        char sym[STOCK_SYMBOL_LEN];
        strncpy(sym, s_symbols[i], sizeof(sym) - 1);
        sym[sizeof(sym) - 1] = '\0';
        if (q.symbol[0]) strncpy(sym, q.symbol, sizeof(sym) - 1);

        if (q.ok) {
            char line1[48];
            snprintf(line1, sizeof(line1), "%s  %s", sym, q.price);
            add_text(card, line1, &lv_font_montserrat_24, lv_color_white());

            bool down = q.change[0] == '-';
            char line2[64];
            snprintf(line2, sizeof(line2), "%s  %s  %s",
                     q.change[0] ? q.change : "--",
                     q.pct[0] ? q.pct : "",
                     q.stamp[0] ? q.stamp : "");
            add_text(card, line2, &lv_font_montserrat_16,
                     down ? lv_color_make(0xFF, 0x66, 0x66)
                          : lv_color_make(0x00, 0xCC, 0x66));
        } else {
            add_text(card, sym, &lv_font_montserrat_24, lv_color_white());
            const char *err = q.error[0] ? q.error : "not fetched";
            add_text(card, err, &lv_font_montserrat_16, lv_color_make(0x99, 0x99, 0x99));
        }
    }
}

static void on_refresh(lv_event_t *)
{
    if (!s_fetching) start_fetch();
    redraw();
}

static void on_gesture(lv_event_t *e)
{
    lv_indev_t *indev = lv_event_get_indev(e);
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir == LV_DIR_RIGHT || dir == LV_DIR_TOP)
        tools_screen_show();
}

static void on_timer(lv_timer_t *)
{
    if (lv_screen_active() != stock_screen) return;
    if (!s_dirty) return;
    s_dirty = false;
    redraw();
}

void stock_screen_create()
{
    stock_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(stock_screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(stock_screen, 0, LV_PART_MAIN);
    lv_obj_clear_flag(stock_screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(stock_screen);
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_label_set_text(title, "STOCKS");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    status_label = lv_label_create(stock_screen);
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(status_label, lv_color_make(0x88, 0x88, 0x88), LV_PART_MAIN);
    lv_label_set_text(status_label, "Quotes load over WiFi");
    lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 58);

    refresh_btn = make_button(stock_screen, "REFRESH");
    lv_obj_align(refresh_btn, LV_ALIGN_TOP_MID, 0, 84);
    lv_obj_add_event_cb(refresh_btn, on_refresh, LV_EVENT_CLICKED, NULL);

    list_box = lv_obj_create(stock_screen);
    lv_obj_set_size(list_box, 404, 350);
    lv_obj_align(list_box, LV_ALIGN_TOP_MID, 0, 138);
    lv_obj_set_style_bg_color(list_box, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_color(list_box, lv_color_make(0x33, 0x33, 0x33), LV_PART_MAIN);
    lv_obj_set_style_border_width(list_box, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(list_box, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(list_box, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_row(list_box, 6, LV_PART_MAIN);
    lv_obj_set_scroll_dir(list_box, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list_box, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_layout(list_box, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(list_box, LV_FLEX_FLOW_COLUMN);

    lv_obj_add_event_cb(stock_screen, on_gesture, LV_EVENT_GESTURE, NULL);
    lv_timer_create(on_timer, 1000, NULL);
    load_config();
    redraw();
}

void stock_screen_show()
{
    main_loop_request_lvgl_priority(12);
    load_config();
    if (WiFi.status() == WL_CONNECTED && s_api_key[0] && !s_fetching) {
        bool have_quote = false;
        for (int i = 0; i < s_symbol_count; i++)
            if (s_quotes[i].ok || s_quotes[i].error[0]) have_quote = true;
        if (!have_quote) start_fetch();
    }
    redraw();
    lv_scr_load(stock_screen);
}

bool stock_screen_is_active()
{
    return lv_screen_active() == stock_screen;
}
