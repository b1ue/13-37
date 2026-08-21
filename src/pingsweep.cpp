#include "pingsweep.h"

void clock_screen_get_local_time(struct tm *out);
#include "hostresolve.h"
#include "usb_sd.h"
#include <Arduino.h>
#include <LilyGoLib.h>
#include <SD.h>
#include <WiFi.h>
#include <string.h>
#include "ping/ping_sock.h"
#include "lwip/ip_addr.h"
#include "lwip/etharp.h"

// ---- state -----------------------------------------------------------------

static PingDevice        s_devices[PINGSWEEP_MAX_DEVICES];
static volatile int      s_device_count = 0;
static volatile int      s_scanned      = 0;
static int               s_total        = 0;
static volatile bool     s_running      = false;
static volatile bool     s_done         = false;
static volatile bool     s_abort        = false;
static bool              s_logged       = false;
static volatile bool     s_just_finished = false;

static uint32_t s_network = 0;               // host-order IPv4
static uint32_t s_broadcast = 0;
static uint8_t  s_prefix_len = 24;
static bool     s_limited = false;
static uint32_t s_targets[PINGSWEEP_MAX_TARGETS];
static char    s_ssid[33];

// Per-host ping result, shared between the esp_ping callbacks and sweep_task.
static SemaphoreHandle_t   s_sem = nullptr;
static volatile bool       s_cur_alive = false;
static volatile uint32_t   s_cur_rtt   = 0;
static esp_ping_callbacks_t s_cbs;

// ---- esp_ping callbacks (run on the ping session's internal task) ----------

static void on_ping_success(esp_ping_handle_t hdl, void *args)
{
    uint32_t t = 0;
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &t, sizeof(t));
    s_cur_rtt   = t;
    s_cur_alive = true;
}
static void on_ping_timeout(esp_ping_handle_t hdl, void *args) { (void)hdl; (void)args; }
static void on_ping_end(esp_ping_handle_t hdl, void *args)
{
    (void)hdl; (void)args;
    if (s_sem) xSemaphoreGive(s_sem);
}

// Best-effort MAC lookup: the host was just pinged, so it is still fresh in
// the lwIP ARP cache.
static uint32_t lwip_order(uint32_t ip)
{
    return ((ip >> 24) & 0xFFU) | ((ip >> 8) & 0xFF00U) |
           ((ip << 8) & 0xFF0000U) | ((ip << 24) & 0xFF000000U);
}

static void lookup_mac(uint32_t host_ip, PingDevice *dev)
{
    dev->has_mac = false;
    uint32_t want = lwip_order(host_ip);
    for (int i = 0; i < 32; i++) {
        ip4_addr_t      *ip  = nullptr;
        struct netif    *nif = nullptr;
        struct eth_addr *eth = nullptr;
        if (!etharp_get_entry(i, &ip, &nif, &eth)) continue;
        if (ip && eth && ip->addr == want) {
            memcpy(dev->mac, eth->addr, 6);
            dev->has_mac = true;
            return;
        }
    }
}

// ---- sweep task ------------------------------------------------------------

static void sweep_task(void *)
{
    for (int target = 0; target < s_total && !s_abort; target++) {
        s_cur_alive = false;
        s_cur_rtt   = 0;

        ip_addr_t addr;
        memset(&addr, 0, sizeof(addr));
        addr.type = IPADDR_TYPE_V4;
        addr.u_addr.ip4.addr = lwip_order(s_targets[target]);

        esp_ping_config_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.count           = 1;
        cfg.interval_ms     = 10;
        cfg.timeout_ms      = 150;     // LAN round-trips are well under this
        cfg.data_size       = 32;
        cfg.ttl             = 64;
        cfg.target_addr     = addr;
        cfg.task_stack_size = 3072;
        cfg.task_prio       = 2;
        cfg.interface       = 0;

        esp_ping_handle_t hdl = nullptr;
        if (esp_ping_new_session(&cfg, &s_cbs, &hdl) == ESP_OK && hdl) {
            xSemaphoreTake(s_sem, 0);          // drop any stale signal
            esp_ping_start(hdl);
            xSemaphoreTake(s_sem, pdMS_TO_TICKS(2000));
            esp_ping_delete_session(hdl);
        }

        if (s_cur_alive && s_device_count < PINGSWEEP_MAX_DEVICES) {
            PingDevice dev;
            memset(&dev, 0, sizeof(dev));
            dev.ip = s_targets[target];
            dev.rtt_ms = s_cur_rtt;
            lookup_mac(dev.ip, &dev);
            s_devices[s_device_count] = dev;
            s_device_count = s_device_count + 1;   // publish last (UI reads count)
        }
        s_scanned = s_scanned + 1;
    }

    s_done    = true;
    s_running = false;

    // Kick the name resolver — it walks the device list and fills in
    // d.name / d.name_source via pingsweep_set_name(). Runs on its own
    // task so the sweep task can exit immediately.
    hostresolve_start();

    vTaskDelete(NULL);
}

// ---- SD log ----------------------------------------------------------------

static void write_log()
{
    if (usb_sd_is_running() || !instance.isCardReady()) return;
    if (!SD.exists("/PingSweeps")) SD.mkdir("/PingSweeps");

    struct tm t;
    clock_screen_get_local_time(&t);
    char path[64];
    snprintf(path, sizeof(path), "/PingSweeps/sweep_%04d%02d%02d_%02d%02d%02d.txt",
        t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);

    File f = SD.open(path, FILE_WRITE);
    if (!f) return;

    f.printf("Ping sweep  %04d-%02d-%02d %02d:%02d:%02d\n",
        t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
    f.printf("Network: %u.%u.%u.%u/%u   SSID \"%s\"%s\n",
        (s_network >> 24) & 0xFF, (s_network >> 16) & 0xFF,
        (s_network >> 8) & 0xFF, s_network & 0xFF, s_prefix_len, s_ssid,
        s_limited ? "   (bounded sample)" : "");
    f.printf("Devices found: %d\n\n", s_device_count);

    for (int i = 0; i < s_device_count; i++) {
        const PingDevice &d = s_devices[i];
        f.printf("%u.%u.%u.%u\t%u ms\t",
            (d.ip >> 24) & 0xFF, (d.ip >> 16) & 0xFF,
            (d.ip >> 8) & 0xFF, d.ip & 0xFF, d.rtt_ms);
        if (d.has_mac)
            f.printf("%02X:%02X:%02X:%02X:%02X:%02X",
                d.mac[0], d.mac[1], d.mac[2], d.mac[3], d.mac[4], d.mac[5]);
        else
            f.print("(MAC unknown)");
        // Trailing tab + name source tag + name (only when we resolved one)
        // so the log mirrors what the user saw on screen.
        if (d.name_source != PNAME_NONE && d.name[0]) {
            const char *src =
                (d.name_source == PNAME_MDNS) ? "mdns" :
                (d.name_source == PNAME_NBNS) ? "nbns" :
                (d.name_source == PNAME_PTR)  ? "ptr"  :
                (d.name_source == PNAME_OUI)  ? "oui"  : "?";
            f.printf("\t%s\t%s", src, d.name);
        }
        if (d.services[0]) f.printf("\tservices\t%s", d.services);
        f.print("\n");
    }
    f.close();
}

static void csv_field(File &f, const char *text)
{
    f.print('"');
    for (const char *p = text ? text : ""; *p; ++p) {
        if (*p == '"') f.print("\"\"");
        else if (*p != '\r' && *p != '\n') f.print(*p);
    }
    f.print('"');
}

// Append a resolved snapshot to a durable evidence table. Keeping separate
// mDNS/NBNS/PTR/OUI fields lets later desktop analysis correlate a device
// even when its IP or preferred display name changes between sweeps.
static void write_profiles()
{
    if (usb_sd_is_running() || !instance.isCardReady()) return;
    if (!SD.exists("/Network")) SD.mkdir("/Network");
    const char *path = "/Network/host_profiles.csv";
    const bool add_header = !SD.exists(path);
    File f = SD.open(path, FILE_APPEND);
    if (!f) return;
    if (add_header)
        f.println("timestamp,ssid,ip,mac,rtt_ms,preferred,mdns,nbns,ptr,vendor,services");

    struct tm t = {};
    clock_screen_get_local_time(&t);
    char stamp[24];
    snprintf(stamp, sizeof(stamp), "%04d-%02d-%02dT%02d:%02d:%02d",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec);
    for (int i = 0; i < s_device_count; ++i) {
        const PingDevice &d = s_devices[i];
        char ipbuf[16], macbuf[18] = "";
        snprintf(ipbuf, sizeof(ipbuf), "%u.%u.%u.%u",
            (d.ip >> 24) & 0xFF, (d.ip >> 16) & 0xFF,
            (d.ip >> 8) & 0xFF, d.ip & 0xFF);
        if (d.has_mac)
            snprintf(macbuf, sizeof(macbuf), "%02X:%02X:%02X:%02X:%02X:%02X",
                d.mac[0], d.mac[1], d.mac[2], d.mac[3], d.mac[4], d.mac[5]);
        csv_field(f, stamp); f.print(','); csv_field(f, s_ssid); f.print(',');
        csv_field(f, ipbuf); f.print(','); csv_field(f, macbuf);
        f.printf(",%lu,", (unsigned long)d.rtt_ms);
        csv_field(f, d.name); f.print(','); csv_field(f, d.mdns); f.print(',');
        csv_field(f, d.nbns); f.print(','); csv_field(f, d.ptr); f.print(',');
        csv_field(f, d.vendor); f.print(','); csv_field(f, d.services); f.println();
    }
    f.close();
}

// ---- public API ------------------------------------------------------------

void pingsweep_start()
{
    if (s_running) return;
    if (WiFi.status() != WL_CONNECTED) return;

    IPAddress ip = WiFi.localIP();
    IPAddress mask = WiFi.subnetMask();
    const uint32_t local = ((uint32_t)ip[0] << 24) | ((uint32_t)ip[1] << 16) |
                           ((uint32_t)ip[2] << 8) | ip[3];
    const uint32_t netmask = ((uint32_t)mask[0] << 24) | ((uint32_t)mask[1] << 16) |
                             ((uint32_t)mask[2] << 8) | mask[3];
    s_network = local & netmask;
    s_broadcast = s_network | ~netmask;
    s_prefix_len = 0;
    for (uint32_t bit = 0x80000000U; bit && (netmask & bit); bit >>= 1)
        ++s_prefix_len;

    const uint64_t usable = s_broadcast > s_network + 1U
        ? (uint64_t)s_broadcast - s_network - 1ULL : 0ULL;
    s_limited = usable > PINGSWEEP_MAX_TARGETS;
    uint32_t first = s_network + 1U;
    uint32_t last = s_broadcast - 1U;
    if (s_limited) {
        const uint32_t half = PINGSWEEP_MAX_TARGETS / 2U;
        first = local > half ? local - half : s_network + 1U;
        if (first < s_network + 1U) first = s_network + 1U;
        last = first + PINGSWEEP_MAX_TARGETS;
        if (last > s_broadcast - 1U) {
            last = s_broadcast - 1U;
            first = last - PINGSWEEP_MAX_TARGETS;
        }
    }
    s_total = 0;
    if (usable > 0) {
        for (uint32_t candidate = first;
             candidate <= last && s_total < PINGSWEEP_MAX_TARGETS;
             ++candidate) {
            if (candidate != local) s_targets[s_total++] = candidate;
            if (candidate == 0xFFFFFFFFU) break;
        }
    }
    String ss = WiFi.SSID();
    strncpy(s_ssid, ss.c_str(), sizeof(s_ssid) - 1);
    s_ssid[sizeof(s_ssid) - 1] = '\0';

    s_device_count  = 0;
    s_scanned       = 0;
    s_done          = false;
    s_abort         = false;
    s_logged        = false;
    s_just_finished = false;

    if (!s_sem) s_sem = xSemaphoreCreateBinary();

    s_cbs.cb_args         = nullptr;
    s_cbs.on_ping_success = on_ping_success;
    s_cbs.on_ping_timeout = on_ping_timeout;
    s_cbs.on_ping_end     = on_ping_end;

    s_running = true;
    xTaskCreatePinnedToCore(sweep_task, "pingsweep", 4096, NULL, 1, NULL, 0);
}

void pingsweep_stop() { s_abort = true; }

bool pingsweep_is_running() { return s_running; }

void pingsweep_poll()
{
    if (!s_done || s_logged || hostresolve_is_running()) return;
    s_logged        = true;
    s_just_finished = true;
    write_log();
    write_profiles();
}

int pingsweep_scanned()      { return s_scanned; }
int pingsweep_total()        { return s_total; }
int pingsweep_device_count() { return s_device_count; }

const PingDevice *pingsweep_device(int idx)
{
    if (idx < 0 || idx >= s_device_count) return nullptr;
    return &s_devices[idx];
}

void pingsweep_set_name(int idx, const char *name, uint8_t src)
{
    if (idx < 0 || idx >= s_device_count) return;
    if (src == PNAME_NONE || !name || !name[0]) return;
    char *evidence = nullptr;
    if (src == PNAME_MDNS) evidence = s_devices[idx].mdns;
    else if (src == PNAME_NBNS) evidence = s_devices[idx].nbns;
    else if (src == PNAME_PTR) evidence = s_devices[idx].ptr;
    else if (src == PNAME_OUI) evidence = s_devices[idx].vendor;
    if (evidence) {
        strncpy(evidence, name, PINGSWEEP_EVIDENCE_MAX - 1);
        evidence[PINGSWEEP_EVIDENCE_MAX - 1] = '\0';
    }
    // Don't overwrite a more-authoritative display name with a weaker one.
    // Ranking (high → low): MDNS=4, NBNS=3, PTR=2, OUI=1, NONE=0.
    static const uint8_t rank_of[] = { 0, 4, 3, 2, 1 }; // index = PingNameSource
    uint8_t cur = s_devices[idx].name_source;
    if (cur < (uint8_t)(sizeof(rank_of) / sizeof(rank_of[0])) &&
        src < (uint8_t)(sizeof(rank_of) / sizeof(rank_of[0]))) {
        if (rank_of[cur] >= rank_of[src]) return;
    }
    strncpy(s_devices[idx].name, name, PINGSWEEP_NAME_MAX - 1);
    s_devices[idx].name[PINGSWEEP_NAME_MAX - 1] = '\0';
    s_devices[idx].name_source = src;
}

void pingsweep_add_service_for_ip(uint32_t ip, const char *service)
{
    if (!service || !service[0]) return;
    for (int i = 0; i < s_device_count; ++i) {
        PingDevice &d = s_devices[i];
        if (d.ip != ip || strstr(d.services, service)) continue;
        const size_t used = strlen(d.services);
        if (used && used + 2 < sizeof(d.services)) strcat(d.services, ", ");
        strncat(d.services, service, sizeof(d.services) - strlen(d.services) - 1);
        return;
    }
}

void pingsweep_network_cidr(char *out, size_t size)
{
    if (!out || size == 0) return;
    snprintf(out, size, "%u.%u.%u.%u/%u%s",
        (s_network >> 24) & 0xFF, (s_network >> 16) & 0xFF,
        (s_network >> 8) & 0xFF, s_network & 0xFF, s_prefix_len,
        s_limited ? " sample" : "");
}

bool pingsweep_is_limited() { return s_limited; }

bool pingsweep_just_finished()
{
    bool f = s_just_finished;
    s_just_finished = false;
    return f;
}
