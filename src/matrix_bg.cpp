#include "matrix_bg.h"
#include "stock_screen.h"
#include "theme.h"
#include "ble_scan_manager.h"
#include <LilyGoLib.h>
#include <WiFi.h>
#include <esp_system.h>   // esp_random
#include <stdio.h>

#define MX_COLS    22
#define MX_ROWS    26
#define MX_COL_W   18     // 22 * 18 = 396 px, centred on the 410 px panel
#define MX_DEFAULT_POWER 1

static lv_obj_t  *mx_cont = nullptr;
static lv_obj_t  *mx_col[MX_COLS];
static lv_timer_t *mx_timer = nullptr;
static bool       mx_enabled = false;
static bool       mx_stock_data = false;
static bool       mx_system_data = false;
static uint8_t    mx_power_mode = MX_DEFAULT_POWER;
static MatrixRainPalette mx_palette = MATRIX_RAIN_GREEN;
static char       mx_stock_feed[192];
static size_t     mx_stock_feed_len = 0;
static char       mx_system_feed[192];
static size_t     mx_system_feed_len = 0;
static uint32_t   mx_last_data_refresh_ms = 0;
static uint8_t    mx_next_col = 0;

// Eco and Balanced update half the columns per callback, cutting expensive
// label invalidations to roughly 25% / 50% of the original animation load.
// Smooth preserves the original 22-column, 120 ms behavior.
static const uint16_t MX_POWER_PERIOD_MS[] = { 240, 120, 120 };
static const uint8_t  MX_POWER_COLS[]      = {  11,  11,  22 };

static int  head[MX_COLS];          // current head row; negative = still entering
static int  tlen[MX_COLS];          // trail length
static char cell[MX_COLS][MX_ROWS]; // stable glyphs so the trail doesn't fully reshuffle
// Easter-egg state per column. 0 = no egg (regular rain). >0 = chars
// [0..egg_len-1] of cell[c][] hold a fixed string; mx_tick skips its
// head-randomize and trail-flicker for those cells so the word stays
// readable as the bright head passes through it.
static int  egg_len[MX_COLS];

// No '#' — it is the LVGL recolor escape character and would corrupt parsing.
static const char MX_CHARSET[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789@$%&*+=<>?";
static const int MX_CHARSET_LEN = sizeof(MX_CHARSET) - 1;

// Easter-egg strings. All chars must live in MX_CHARSET (no '#' or
// lowercase). Roughly 1-in-30 column resets seeds the column's top
// cells with one of these.
static const char *MX_EGGS[] = {
    "HACKEDEXISTENCE",
    "R3DFISH",
    "DZAZ ",
    "1337",
    "HACKTHEPLANET",
};
static const int MX_EGG_COUNT = sizeof(MX_EGGS) / sizeof(MX_EGGS[0]);

static char rnd_char() { return MX_CHARSET[esp_random() % MX_CHARSET_LEN]; }

static void refresh_stock_feed()
{
    mx_stock_feed_len = mx_stock_data
        ? stock_screen_build_rain_feed(mx_stock_feed, sizeof(mx_stock_feed)) : 0;
}

static void refresh_system_feed()
{
    if (!mx_system_data) {
        mx_system_feed_len = 0;
        return;
    }

    int battery = instance.pmu.getBatteryPercent();
    if (battery < 0) battery = 0;
    if (battery > 100) battery = 100;
    const uint32_t uptime_min = millis() / 60000UL;
    const uint32_t heap_kb = ESP.getFreeHeap() / 1024UL;
    const uint32_t psram_kb = ESP.getFreePsram() / 1024UL;
    const bool wifi_up = WiFi.status() == WL_CONNECTED;
    const int ble_users = ble_scan_consumer_count();
    const uint32_t ble_drops = ble_scan_result_drop_count();

    const int n = snprintf(mx_system_feed, sizeof(mx_system_feed),
        "BAT%03d%s>UP%02luH%02luM>HEAP%luK>PSRAM%luK>WIFI%d>BLE%d>DROP%lu>",
        battery, instance.pmu.isCharging() ? "CHG" : "",
        (unsigned long)(uptime_min / 60UL),
        (unsigned long)(uptime_min % 60UL),
        (unsigned long)heap_kb, (unsigned long)psram_kb,
        wifi_up ? 1 : 0, ble_users, (unsigned long)ble_drops);
    mx_system_feed_len = n > 0
        ? (size_t)((n < (int)sizeof(mx_system_feed)) ? n : sizeof(mx_system_feed) - 1)
        : 0;
}

static void refresh_data_feeds()
{
    refresh_stock_feed();
    refresh_system_feed();
    mx_last_data_refresh_ms = millis();
}

static char displayed_glyph(int c, int r)
{
    const char *feed = nullptr;
    size_t feed_len = 0;
    if (mx_stock_feed_len && mx_system_feed_len) {
        // Mixed mode deliberately alternates sources by column. It is easier
        // to read than concatenating feeds and costs no extra rendering work.
        if ((c & 1) == 0) {
            feed = mx_system_feed;
            feed_len = mx_system_feed_len;
        } else {
            feed = mx_stock_feed;
            feed_len = mx_stock_feed_len;
        }
    } else if (mx_system_feed_len) {
        feed = mx_system_feed;
        feed_len = mx_system_feed_len;
    } else if (mx_stock_feed_len) {
        feed = mx_stock_feed;
        feed_len = mx_stock_feed_len;
    }
    if (!feed || feed_len == 0) return cell[c][r];
    // A relatively-prime column stride prevents adjacent trails from showing
    // the same slice. The normal falling head/trail controls movement/shading.
    return feed[((size_t)c * 17U + (size_t)r) % feed_len];
}

static void col_reset(int c)
{
    tlen[c] = 6 + (int)(esp_random() % 13);          // 6..18
    head[c] = -(int)(esp_random() % MX_ROWS);        // stagger entry from above
    for (int r = 0; r < MX_ROWS; r++)
        cell[c][r] = MX_CHARSET[esp_random() % MX_CHARSET_LEN];
    egg_len[c] = 0;
    // ~1 in 30 reset chances, repaint the whole column with one of the
    // easter-egg strings tiled top-to-bottom. egg_len = MX_ROWS so
    // mx_tick's randomize + flicker skip every cell, leaving the
    // repeated text frozen while the head's bright gradient slides
    // down through it. Bump tlen to MX_ROWS so the trail covers the
    // full column when the head reaches the bottom.
    if ((esp_random() % 30) == 0) {
        const char *s = MX_EGGS[esp_random() % MX_EGG_COUNT];
        int slen = 0;
        while (s[slen]) slen++;
        if (slen > 0) {
            for (int r = 0; r < MX_ROWS; r++)
                cell[c][r] = s[r % slen];
            egg_len[c] = MX_ROWS;
            tlen[c]    = MX_ROWS;
        }
    }
}

// Distance 0 = bright head, increasing distance = dimmer trail.
static const char *shade(int dist, int len)
{
    if (mx_palette == MATRIX_RAIN_BLUE) {
        if (dist == 0)        return "D6EEFF";
        if (dist <= len / 4)  return "66BBFF";
        if (dist <= len / 2)  return "2277DD";
        return "103B77";
    }
    if (dist == 0)        return "CCFFCC";
    if (dist <= len / 4)  return "5BFF8C";
    if (dist <= len / 2)  return "22BB44";
    return "0E6622";
}

static void render_col(int c)
{
    char buf[MX_ROWS * 12];
    size_t n = 0;
    for (int r = 0; r < MX_ROWS; r++) {
        int dist = head[c] - r;   // 0 at head, grows up the trail
        if (head[c] >= 0 && r <= head[c] && dist < tlen[c]) {
            const size_t remaining = sizeof(buf) - n;
            const int written = snprintf(buf + n, remaining,
                                         "#%s %c#\n", shade(dist, tlen[c]), displayed_glyph(c, r));
            if (written < 0) break;
            if (static_cast<size_t>(written) >= remaining) {
                n = sizeof(buf) - 1;
                break;
            }
            n += static_cast<size_t>(written);
        } else {
            if (n == sizeof(buf) - 1) break;
            buf[n++] = '\n';      // empty row keeps vertical alignment
        }
    }
    if (n > 0 && buf[n - 1] == '\n') n--;   // trim trailing newline
    buf[n] = '\0';
    lv_label_set_text(mx_col[c], buf);
}

static void mx_tick(lv_timer_t *)
{
    // Refresh cached text every three seconds. This reads only in-memory
    // state; stock/network fetching and all filesystem I/O remain elsewhere.
    const uint32_t now = millis();
    if ((mx_stock_data || mx_system_data) &&
        now - mx_last_data_refresh_ms >= 3000U)
        refresh_data_feeds();

    const uint8_t cols = MX_POWER_COLS[mx_power_mode];
    for (uint8_t i = 0; i < cols; ++i) {
        const int c = mx_next_col;
        mx_next_col = (uint8_t)((mx_next_col + 1U) % MX_COLS);
        head[c]++;
        int el = egg_len[c];
        // Skip randomization for cells that hold easter-egg chars
        // (rows [0..el-1]); randomize freely outside that range.
        if (head[c] >= el && head[c] < MX_ROWS)
            cell[c][head[c]] = rnd_char();          // fresh glyph at the head
        if ((esp_random() & 0x0F) == 0) {           // occasional trail flicker
            int r = (int)(esp_random() % MX_ROWS);
            if (r >= el) cell[c][r] = rnd_char();
        }
        if (head[c] - tlen[c] > MX_ROWS)
            col_reset(c);
        render_col(c);
    }
}

lv_obj_t *matrix_bg_create(lv_obj_t *parent)
{
    mx_cont = lv_obj_create(parent);
    lv_obj_remove_style_all(mx_cont);
    lv_obj_set_size(mx_cont, MX_COL_W * MX_COLS, 502);
    lv_obj_align(mx_cont, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_scrollbar_mode(mx_cont, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(mx_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(mx_cont, LV_OBJ_FLAG_CLICKABLE);

    for (int c = 0; c < MX_COLS; c++) {
        lv_obj_t *l = lv_label_create(mx_cont);
        lv_label_set_recolor(l, true);
        lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
        lv_obj_set_width(l, MX_COL_W);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_16, LV_PART_MAIN);
        // Non-recoloured glyphs (blank rows) render invisibly on the black screen
        lv_obj_set_style_text_color(l, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_pos(l, c * MX_COL_W, 0);
        mx_col[c] = l;
        col_reset(c);
    }

    lv_obj_add_flag(mx_cont, LV_OBJ_FLAG_HIDDEN);
    return mx_cont;
}

void matrix_bg_set_enabled(bool en)
{
    mx_enabled = en;
    if (!mx_cont) return;
    if (en) {
        lv_obj_clear_flag(mx_cont, LV_OBJ_FLAG_HIDDEN);
        if (!mx_timer)
            mx_timer = lv_timer_create(mx_tick, MX_POWER_PERIOD_MS[mx_power_mode], nullptr);
    } else {
        lv_obj_add_flag(mx_cont, LV_OBJ_FLAG_HIDDEN);
        if (mx_timer) { lv_timer_del(mx_timer); mx_timer = nullptr; }
    }
}

bool matrix_bg_is_enabled() { return mx_enabled; }

void matrix_bg_set_palette(MatrixRainPalette palette)
{
    mx_palette = palette == MATRIX_RAIN_BLUE ? MATRIX_RAIN_BLUE : MATRIX_RAIN_GREEN;
    matrix_bg_refresh_theme();
}

MatrixRainPalette matrix_bg_palette() { return mx_palette; }

void matrix_bg_set_stock_data(bool enabled)
{
    mx_stock_data = enabled;
    refresh_data_feeds();
    if (!mx_cont || !mx_enabled) return;
    for (int c = 0; c < MX_COLS; ++c) render_col(c);
}

bool matrix_bg_stock_data_enabled() { return mx_stock_data; }

void matrix_bg_set_system_data(bool enabled)
{
    mx_system_data = enabled;
    refresh_data_feeds();
    if (!mx_cont || !mx_enabled) return;
    for (int c = 0; c < MX_COLS; ++c) render_col(c);
}

bool matrix_bg_system_data_enabled() { return mx_system_data; }

void matrix_bg_set_power_mode(uint8_t mode)
{
    if (mode > 2) mode = MX_DEFAULT_POWER;
    mx_power_mode = mode;
    mx_next_col = 0;
    if (mx_timer) lv_timer_set_period(mx_timer, MX_POWER_PERIOD_MS[mx_power_mode]);
}

uint8_t matrix_bg_power_mode() { return mx_power_mode; }

void matrix_bg_set_paused(bool paused)
{
    if (!mx_timer) return;
    if (paused) lv_timer_pause(mx_timer);
    else        lv_timer_resume(mx_timer);
}

void matrix_bg_refresh_theme()
{
    if (!mx_cont || !mx_enabled) return;
    for (int c = 0; c < MX_COLS; c++) render_col(c);
}
