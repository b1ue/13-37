#include "stock_screen.h"
#include "usb_sd.h"
#include <Arduino.h>
#include <LilyGoLib.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <SD.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <ctype.h>
#include <string.h>
#include <strings.h>

// Defined in tools_screen.cpp and main.cpp.
void tools_screen_show();
void main_loop_request_lvgl_priority(int cycles);

#define STOCK_MAX_SYMBOLS 5
#define STOCK_SYMBOL_LEN  12
#define STOCK_MAX_SPARK   24
#define STOCK_CONFIG_PATH "/Stocks/config.txt"

struct StockQuote {
    char symbol[STOCK_SYMBOL_LEN];
    char price[20];
    char change[20];
    char pct[20];
    char stamp[24];
    char error[48];
    float spark[STOCK_MAX_SPARK];
    uint8_t spark_count;
    bool ok;
};

struct StockResult {
    StockQuote quotes[STOCK_MAX_SYMBOLS];
    int quote_count;
    char status[80];
    bool request_ok;
};

enum StockOperation : uint8_t { STOCK_REFRESH, STOCK_RUN_PRESET };

struct StockRequest {
    StockOperation operation;
    char server_url[160];
    char auth_token[96];
    char ca_file[64];
    char preset[40];
    char device_name[40];
    char symbols[STOCK_MAX_SYMBOLS][STOCK_SYMBOL_LEN];
    int symbol_count;
    bool allow_http;
    StockResult *result;
};

static lv_obj_t *stock_screen;
static lv_obj_t *status_label;
static lv_obj_t *list_box;
static lv_obj_t *refresh_btn_label;
static lv_obj_t *run_btn_label;

static char s_server_url[160] = "";
static char s_auth_token[96] = "";
static char s_ca_file[64] = "/Stocks/ca.pem";
static char s_preset[40] = "atlas_weekly";
static char s_device_name[40] = "t-watch-1337";
static char s_symbols[STOCK_MAX_SYMBOLS][STOCK_SYMBOL_LEN];
static int  s_symbol_count = 0;
static bool s_allow_http = false;
static StockQuote s_quotes[STOCK_MAX_SYMBOLS];
static char s_last_status[80] = "Configure the Python bridge";
static volatile bool s_fetching = false;
static QueueHandle_t s_result_queue;

static void copy_text(char *dest, size_t size, const char *src)
{
    if (!dest || size == 0) return;
    strncpy(dest, src ? src : "", size - 1);
    dest[size - 1] = '\0';
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

static void set_default_symbols()
{
    static const char *defaults[] = { "AAPL", "MSFT", "SPY", "QQQ" };
    s_symbol_count = 4;
    for (int i = 0; i < s_symbol_count; ++i)
        copy_text(s_symbols[i], sizeof(s_symbols[i]), defaults[i]);
}

static void parse_symbols(char *csv)
{
    s_symbol_count = 0;
    char *save = nullptr;
    for (char *tok = strtok_r(csv, ",", &save);
         tok && s_symbol_count < STOCK_MAX_SYMBOLS;
         tok = strtok_r(nullptr, ",", &save)) {
        trim(tok);
        for (char *p = tok; *p; ++p) *p = (char)toupper((unsigned char)*p);
        if (*tok) copy_text(s_symbols[s_symbol_count++], STOCK_SYMBOL_LEN, tok);
    }
    if (!s_symbol_count) set_default_symbols();
}

static void load_config()
{
    s_server_url[0] = '\0';
    s_auth_token[0] = '\0';
    copy_text(s_ca_file, sizeof(s_ca_file), "/Stocks/ca.pem");
    copy_text(s_preset, sizeof(s_preset), "atlas_weekly");
    copy_text(s_device_name, sizeof(s_device_name), "t-watch-1337");
    s_allow_http = false;
    set_default_symbols();

    if (!instance.isCardReady() || usb_sd_is_running() || !SD.exists(STOCK_CONFIG_PATH)) return;
    File file = SD.open(STOCK_CONFIG_PATH, FILE_READ);
    if (!file) return;

    while (file.available()) {
        char line[256] = {};
        size_t count = file.readBytesUntil('\n', line, sizeof(line) - 1);
        line[count] = '\0';
        trim(line);
        if (!line[0] || line[0] == '#') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = line;
        char *value = eq + 1;
        trim(key);
        trim(value);

        if (!strcasecmp(key, "server_url")) copy_text(s_server_url, sizeof(s_server_url), value);
        else if (!strcasecmp(key, "auth_token")) copy_text(s_auth_token, sizeof(s_auth_token), value);
        else if (!strcasecmp(key, "ca_file")) copy_text(s_ca_file, sizeof(s_ca_file), value);
        else if (!strcasecmp(key, "preset")) copy_text(s_preset, sizeof(s_preset), value);
        else if (!strcasecmp(key, "device_name")) copy_text(s_device_name, sizeof(s_device_name), value);
        else if (!strcasecmp(key, "allow_http")) s_allow_http = atoi(value) != 0;
        else if (!strcasecmp(key, "symbols")) parse_symbols(value);
    }
    file.close();
    size_t len = strlen(s_server_url);
    while (len && s_server_url[len - 1] == '/') s_server_url[--len] = '\0';
}

static bool read_ca_file(const char *path, char **pem_out)
{
    *pem_out = nullptr;
    if (!path[0] || !instance.isCardReady() || usb_sd_is_running() || !SD.exists(path)) return false;
    File file = SD.open(path, FILE_READ);
    if (!file) return false;
    size_t length = file.size();
    if (!length || length > 8192) { file.close(); return false; }
    char *pem = (char *)malloc(length + 1);
    if (!pem) { file.close(); return false; }
    size_t read = file.readBytes(pem, length);
    file.close();
    pem[read] = '\0';
    if (read != length) { free(pem); return false; }
    *pem_out = pem;
    return true;
}

static void add_auth_header(HTTPClient &http, const StockRequest &request)
{
    if (!request.auth_token[0]) return;
    String value = "Bearer ";
    value += request.auth_token;
    http.addHeader("Authorization", value);
}

static int perform_http(StockRequest &request, const String &url,
                        const char *post_body, String &response, char *error, size_t error_size)
{
    HTTPClient http;
    http.setConnectTimeout(7000);
    http.setTimeout(12000);
    int code = -1;

    if (url.startsWith("https://")) {
        char *pem = nullptr;
        if (!read_ca_file(request.ca_file, &pem)) {
            copy_text(error, error_size, "HTTPS requires ca_file on SD");
            return -1;
        }
        WiFiClientSecure client;
        client.setCACert(pem);
        if (!http.begin(client, url)) {
            free(pem);
            copy_text(error, error_size, "request setup failed");
            return -1;
        }
        add_auth_header(http, request);
        if (post_body) {
            http.addHeader("Content-Type", "application/json");
            code = http.POST((uint8_t *)post_body, strlen(post_body));
        } else {
            code = http.GET();
        }
        if (code > 0) response = http.getString();
        http.end();
        free(pem);
    } else if (url.startsWith("http://") && request.allow_http) {
        WiFiClient client;
        if (!http.begin(client, url)) {
            copy_text(error, error_size, "request setup failed");
            return -1;
        }
        add_auth_header(http, request);
        if (post_body) {
            http.addHeader("Content-Type", "application/json");
            code = http.POST((uint8_t *)post_body, strlen(post_body));
        } else {
            code = http.GET();
        }
        if (code > 0) response = http.getString();
        http.end();
    } else {
        copy_text(error, error_size, "Use HTTPS, or set allow_http=1");
        return -1;
    }

    if (code <= 0) copy_text(error, error_size, "bridge connection failed");
    else if (code < 200 || code >= 300) snprintf(error, error_size, "Bridge HTTP %d", code);
    return code;
}

static StockQuote *find_quote(StockResult &result, const char *symbol)
{
    for (int i = 0; i < result.quote_count; ++i)
        if (!strcasecmp(result.quotes[i].symbol, symbol)) return &result.quotes[i];
    if (result.quote_count >= STOCK_MAX_SYMBOLS) return nullptr;
    StockQuote *quote = &result.quotes[result.quote_count++];
    memset(quote, 0, sizeof(*quote));
    copy_text(quote->symbol, sizeof(quote->symbol), symbol);
    return quote;
}

static void parse_snapshot(String &body, StockResult &result)
{
    int start = 0;
    while (start < body.length()) {
        int end = body.indexOf('\n', start);
        if (end < 0) end = body.length();
        String row = body.substring(start, end);
        row.trim();
        start = end + 1;
        if (!row.length() || row == "VERSION=1") continue;

        if (row.startsWith("STATUS=")) {
            copy_text(result.status, sizeof(result.status), row.c_str() + 7);
        } else if (row.startsWith("ERROR=")) {
            copy_text(result.status, sizeof(result.status), row.c_str() + 6);
        } else if (row.startsWith("QUOTE=")) {
            char value[160];
            copy_text(value, sizeof(value), row.c_str() + 6);
            char *parts[5] = {};
            char *save = nullptr;
            int count = 0;
            for (char *tok = strtok_r(value, "|", &save); tok && count < 5;
                 tok = strtok_r(nullptr, "|", &save)) parts[count++] = tok;
            if (count >= 2) {
                StockQuote *quote = find_quote(result, parts[0]);
                if (quote) {
                    copy_text(quote->price, sizeof(quote->price), parts[1]);
                    if (count > 2) copy_text(quote->change, sizeof(quote->change), parts[2]);
                    if (count > 3) copy_text(quote->pct, sizeof(quote->pct), parts[3]);
                    if (count > 4) copy_text(quote->stamp, sizeof(quote->stamp), parts[4]);
                    quote->ok = quote->price[0] != '\0';
                }
            }
        } else if (row.startsWith("SPARK=")) {
            char value[320];
            copy_text(value, sizeof(value), row.c_str() + 6);
            char *bar = strchr(value, '|');
            if (!bar) continue;
            *bar = '\0';
            StockQuote *quote = find_quote(result, value);
            if (!quote) continue;
            char *save = nullptr;
            for (char *tok = strtok_r(bar + 1, ",", &save);
                 tok && quote->spark_count < STOCK_MAX_SPARK;
                 tok = strtok_r(nullptr, ",", &save)) {
                quote->spark[quote->spark_count++] = strtof(tok, nullptr);
            }
        }
    }
    if (!result.status[0]) copy_text(result.status, sizeof(result.status), "Bridge snapshot ready");
}

static void stock_task(void *argument)
{
    StockRequest *request = (StockRequest *)argument;
    StockResult *result = request->result;
    memset(result, 0, sizeof(*result));

    if (WiFi.status() != WL_CONNECTED) {
        copy_text(result->status, sizeof(result->status), "WiFi disconnected");
    } else if (request->operation == STOCK_REFRESH) {
        String url = request->server_url;
        url += "/api/watch/snapshot?symbols=";
        for (int i = 0; i < request->symbol_count; ++i) {
            if (i) url += ',';
            url += request->symbols[i];
        }
        String body;
        char error[80] = {};
        int code = perform_http(*request, url, nullptr, body, error, sizeof(error));
        if (code >= 200 && code < 300) {
            result->request_ok = true;
            parse_snapshot(body, *result);
        } else {
            copy_text(result->status, sizeof(result->status), error);
        }
    } else {
        String url = request->server_url;
        url += "/api/jobs";
        char body[192];
        snprintf(body, sizeof(body),
                 "{\"preset\":\"%s\",\"device_name\":\"%s\"}",
                 request->preset, request->device_name);
        String response;
        char error[80] = {};
        int code = perform_http(*request, url, body, response, error, sizeof(error));
        if (code >= 200 && code < 300) {
            result->request_ok = true;
            snprintf(result->status, sizeof(result->status), "Started preset: %.40s", request->preset);
        } else {
            copy_text(result->status, sizeof(result->status), error);
        }
    }

    if (xQueueSend(s_result_queue, &result, pdMS_TO_TICKS(500)) != pdTRUE)
        delete result;
    delete request;
    vTaskDelete(nullptr);
}

static void start_request(StockOperation operation)
{
    if (s_fetching) return;
    load_config();
    if (WiFi.status() != WL_CONNECTED) {
        copy_text(s_last_status, sizeof(s_last_status), "Connect WiFi first");
        return;
    }
    if (!s_server_url[0]) {
        copy_text(s_last_status, sizeof(s_last_status), "Add server_url to /Stocks/config.txt");
        return;
    }
    if (!s_result_queue) {
        copy_text(s_last_status, sizeof(s_last_status), "Stock result queue unavailable");
        return;
    }

    StockRequest *request = new StockRequest();
    if (!request) {
        copy_text(s_last_status, sizeof(s_last_status), "Out of memory");
        return;
    }
    memset(request, 0, sizeof(*request));
    request->result = new StockResult();
    if (!request->result) {
        delete request;
        copy_text(s_last_status, sizeof(s_last_status), "Out of memory");
        return;
    }
    memset(request->result, 0, sizeof(*request->result));
    request->operation = operation;
    copy_text(request->server_url, sizeof(request->server_url), s_server_url);
    copy_text(request->auth_token, sizeof(request->auth_token), s_auth_token);
    copy_text(request->ca_file, sizeof(request->ca_file), s_ca_file);
    copy_text(request->preset, sizeof(request->preset), s_preset);
    copy_text(request->device_name, sizeof(request->device_name), s_device_name);
    request->allow_http = s_allow_http;
    request->symbol_count = s_symbol_count;
    for (int i = 0; i < s_symbol_count; ++i)
        copy_text(request->symbols[i], STOCK_SYMBOL_LEN, s_symbols[i]);

    s_fetching = true;
    copy_text(s_last_status, sizeof(s_last_status),
              operation == STOCK_REFRESH ? "Loading bridge snapshot..." : "Starting Python preset...");
    TaskHandle_t handle = nullptr;
    BaseType_t created = xTaskCreate(stock_task, "stock_bridge", 10240, request, 1, &handle);
    if (created != pdPASS) {
        s_fetching = false;
        delete request->result;
        delete request;
        copy_text(s_last_status, sizeof(s_last_status), "Could not start stock task");
    }
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *text, lv_obj_t **label_out)
{
    lv_obj_t *button = lv_obj_create(parent);
    lv_obj_set_size(button, 172, 44);
    lv_obj_set_style_radius(button, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, lv_color_make(0x00, 0x88, 0xCC), LV_PART_MAIN);
    lv_obj_set_style_border_width(button, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(button, 0, LV_PART_MAIN);
    lv_obj_clear_flag(button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    *label_out = lv_label_create(button);
    lv_obj_set_style_text_color(*label_out, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(*label_out, &lv_font_montserrat_20, LV_PART_MAIN);
    lv_label_set_text(*label_out, text);
    lv_obj_center(*label_out);
    return button;
}

static void add_text(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, color, LV_PART_MAIN);
    lv_label_set_text(label, text);
}

static void add_sparkline(lv_obj_t *card, const StockQuote &quote)
{
    if (quote.spark_count < 2) return;
    float low = quote.spark[0], high = quote.spark[0];
    for (int i = 1; i < quote.spark_count; ++i) {
        if (quote.spark[i] < low) low = quote.spark[i];
        if (quote.spark[i] > high) high = quote.spark[i];
    }
    float range = high - low;
    if (range < 0.0001f) range = 1.0f;

    lv_obj_t *chart = lv_chart_create(card);
    lv_obj_set_size(chart, lv_pct(100), 58);
    lv_obj_set_style_bg_opa(chart, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(chart, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(chart, 0, LV_PART_MAIN);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(chart, quote.spark_count);
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, 1000);
    lv_obj_set_style_size(chart, 0, 0, LV_PART_INDICATOR);
    lv_chart_series_t *series = lv_chart_add_series(
        chart,
        quote.spark[quote.spark_count - 1] < quote.spark[0]
            ? lv_color_make(0xFF, 0x66, 0x66) : lv_color_make(0x00, 0xCC, 0x66),
        LV_CHART_AXIS_PRIMARY_Y);
    for (int i = 0; i < quote.spark_count; ++i)
        lv_chart_set_next_value(chart, series, (int32_t)((quote.spark[i] - low) * 1000.0f / range));
}

static void redraw()
{
    if (!stock_screen) return;
    lv_obj_clean(list_box);
    lv_label_set_text(status_label, s_last_status);
    lv_obj_set_style_text_color(status_label,
        s_fetching ? lv_color_make(0xFF, 0xCC, 0x00) : lv_color_make(0x88, 0xCC, 0xAA),
        LV_PART_MAIN);
    lv_label_set_text(refresh_btn_label, s_fetching ? "WAIT" : "REFRESH");
    lv_label_set_text(run_btn_label, s_fetching ? "WAIT" : "RUN PRESET");

    for (int i = 0; i < s_symbol_count; ++i) {
        const StockQuote &quote = s_quotes[i];
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

        const char *symbol = quote.symbol[0] ? quote.symbol : s_symbols[i];
        if (quote.ok) {
            char first[48];
            snprintf(first, sizeof(first), "%s  %s", symbol, quote.price);
            add_text(card, first, &lv_font_montserrat_24, lv_color_white());
            bool down = quote.change[0] == '-';
            char second[72];
            snprintf(second, sizeof(second), "%s  %s  %s", quote.change, quote.pct, quote.stamp);
            add_text(card, second, &lv_font_montserrat_16,
                     down ? lv_color_make(0xFF, 0x66, 0x66) : lv_color_make(0x00, 0xCC, 0x66));
            add_sparkline(card, quote);
        } else {
            add_text(card, symbol, &lv_font_montserrat_24, lv_color_white());
            add_text(card, quote.error[0] ? quote.error : "not fetched",
                     &lv_font_montserrat_16, lv_color_make(0x99, 0x99, 0x99));
        }
    }
}

static void on_refresh(lv_event_t *) { start_request(STOCK_REFRESH); redraw(); }
static void on_run(lv_event_t *) { start_request(STOCK_RUN_PRESET); redraw(); }

static void on_gesture(lv_event_t *event)
{
    lv_dir_t direction = lv_indev_get_gesture_dir(lv_event_get_indev(event));
    if (direction == LV_DIR_RIGHT || direction == LV_DIR_TOP) tools_screen_show();
}

static void on_timer(lv_timer_t *)
{
    if (!s_result_queue) return;
    StockResult *result = nullptr;
    bool changed = false;
    while (xQueueReceive(s_result_queue, &result, 0) == pdTRUE) {
        s_fetching = false;
        copy_text(s_last_status, sizeof(s_last_status), result->status);
        if (result->request_ok && result->quote_count) {
            memset(s_quotes, 0, sizeof(s_quotes));
            for (int i = 0; i < result->quote_count; ++i) {
                for (int j = 0; j < s_symbol_count; ++j) {
                    if (!strcasecmp(result->quotes[i].symbol, s_symbols[j])) {
                        s_quotes[j] = result->quotes[i];
                        break;
                    }
                }
            }
        }
        delete result;
        changed = true;
    }
    if (changed && stock_screen && lv_screen_active() == stock_screen) redraw();
}

void stock_screen_create()
{
    if (stock_screen) return;
    s_result_queue = xQueueCreate(2, sizeof(StockResult *));
    stock_screen = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(stock_screen, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(stock_screen, 0, LV_PART_MAIN);
    lv_obj_clear_flag(stock_screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(stock_screen);
    lv_obj_set_style_text_color(title, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_label_set_text(title, "STOCKS");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    status_label = lv_label_create(stock_screen);
    lv_obj_set_width(status_label, 390);
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(status_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 58);

    lv_obj_t *refresh = make_button(stock_screen, "REFRESH", &refresh_btn_label);
    lv_obj_align(refresh, LV_ALIGN_TOP_MID, -91, 84);
    lv_obj_add_event_cb(refresh, on_refresh, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *run = make_button(stock_screen, "RUN PRESET", &run_btn_label);
    lv_obj_align(run, LV_ALIGN_TOP_MID, 91, 84);
    lv_obj_add_event_cb(run, on_run, LV_EVENT_CLICKED, nullptr);

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
    lv_obj_add_event_cb(stock_screen, on_gesture, LV_EVENT_GESTURE, nullptr);
    lv_timer_create(on_timer, 500, nullptr);
    load_config();
    redraw();
}

void stock_screen_show()
{
    if (!stock_screen) stock_screen_create(); // avoid permanent LVGL heap use until opened
    main_loop_request_lvgl_priority(12);
    if (!s_fetching) load_config();
    bool have_quote = false;
    for (int i = 0; i < s_symbol_count; ++i) if (s_quotes[i].ok) have_quote = true;
    if (!have_quote && !s_fetching && WiFi.status() == WL_CONNECTED && s_server_url[0])
        start_request(STOCK_REFRESH);
    redraw();
    lv_scr_load(stock_screen);
}

bool stock_screen_is_active()
{
    return stock_screen && lv_screen_active() == stock_screen;
}

bool stock_screen_is_fetching()
{
    return s_fetching;
}
