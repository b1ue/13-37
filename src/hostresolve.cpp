#include "hostresolve.h"
#include "pingsweep.h"
#include "service_discovery.h"
#include <Arduino.h>
#include <WiFi.h>
#include <string.h>
#include <ctype.h>
#include "lwip/sockets.h"
#include "lwip/inet.h"

// Per-protocol pass trace logging. The resolver runs four passes (mDNS,
// NBNS, DNS, OUI), each with sent / replies / named counters; the
// per-MAC "no ARP entry" / "labelled by vendor" lines fire per device.
// Useful while diagnosing why a host stayed nameless, noisy otherwise.
// Errno-bearing socket/bind failures stay unconditional below so a
// hardware-level fault is always visible on the monitor.
#ifndef HR_DEBUG
#define HR_DEBUG 0
#endif
#if HR_DEBUG
  #define HR_LOG(...) Serial.printf(__VA_ARGS__)
#else
  #define HR_LOG(...) ((void)0)
#endif

// ===========================================================================
// Curated consumer-OUI table — Pass 3 fallback.
// Each entry packs the 24-bit OUI into the low bits of a uint32 (b0<<16 |
// b1<<8 | b2) so the lookup is a single equality compare on the MAC's first
// three bytes. ~90 entries covers the bulk of typical residential LANs;
// anything else just stays nameless.
// ===========================================================================

struct OuiEntry { uint32_t oui; const char *vendor; };

static const OuiEntry OUI_TABLE[] = {
    // Apple
    {0x001124,"Apple"},{0x002500,"Apple"},{0x14109F,"Apple"},
    {0x28E14C,"Apple"},{0x34159E,"Apple"},{0x40331A,"Apple"},
    {0x685B35,"Apple"},{0x88E87F,"Apple"},{0x9C04EB,"Apple"},
    {0xA0EDCD,"Apple"},{0xACFDEC,"Apple"},{0xF40F24,"Apple"},
    // Samsung
    {0x0024E9,"Samsung"},{0x002566,"Samsung"},{0x141A00,"Samsung"},
    {0x208425,"Samsung"},{0x382DD1,"Samsung"},{0x40D6BA,"Samsung"},
    {0x88329B,"Samsung"},{0xE8508B,"Samsung"},{0xFC1910,"Samsung"},
    // Google / Nest
    {0x18B430,"Nest"},{0x1C9E46,"Google"},{0x489CCB,"Google"},
    {0x542CA4,"Nest"},{0xF4F5D8,"Google"},{0xF4F5E8,"Google"},
    {0x6466B3,"Google"},
    // Amazon (Lab126, Echo, Fire, Ring)
    {0x186472,"Amazon"},{0x44650D,"Amazon"},{0x68374A,"Amazon"},
    {0x747548,"Amazon"},{0x848BCD,"Amazon"},{0x88717C,"Amazon"},
    {0xB47C9C,"Amazon"},{0xCC9EA2,"Amazon"},{0xFCA667,"Amazon"},
    // Microsoft / Xbox / Surface
    {0x002247,"Microsoft"},{0x10604B,"Microsoft"},{0x586B14,"Microsoft"},
    {0x5C9495,"Microsoft"},{0x945404,"Microsoft"},{0xC83F26,"Microsoft"},
    // Sony / PlayStation
    {0x001D0D,"Sony"},{0x0CFE45,"Sony"},{0x709E29,"Sony"},
    {0x801F02,"Sony"},{0xAC9B0A,"Sony"},
    // Nintendo
    {0x001AE9,"Nintendo"},{0x002709,"Nintendo"},{0x00197D,"Nintendo"},
    {0x18EC0D,"Nintendo"},{0x40D28A,"Nintendo"},
    // LG
    {0x14C913,"LG"},{0x18F0E4,"LG"},{0x24C6CA,"LG"},
    {0x88D1F7,"LG"},{0xA4D1D2,"LG"},
    // Roku
    {0x4019B9,"Roku"},{0xB0A737,"Roku"},{0xB83F4B,"Roku"},
    {0xC8DB26,"Roku"},{0xDC3A5E,"Roku"},
    // Sonos
    {0x000E58,"Sonos"},{0x5CAAFD,"Sonos"},{0x78286F,"Sonos"},
    {0x944443,"Sonos"},{0xB8E937,"Sonos"},
    // Belkin / Wemo
    {0xC4411E,"Belkin"},{0xEC1A59,"Belkin"},{0xEC2280,"Belkin"},
    // TP-Link
    {0x144D67,"TP-Link"},{0x60E327,"TP-Link"},{0xC04A00,"TP-Link"},
    // Netgear
    {0x000FB5,"Netgear"},{0x080091,"Netgear"},{0xA021B7,"Netgear"},
    {0xC03F0E,"Netgear"},
    // Linksys / Cisco
    {0x4C6E6E,"Linksys"},{0x8C0CA3,"Linksys"},{0x000C29,"Cisco"},
    // D-Link
    {0x000F3D,"D-Link"},{0x00FFCB,"D-Link"},{0x281878,"D-Link"},
    {0x90942A,"D-Link"},
    // Espressif (ESP32/ESP8266 — including this watch's MAC)
    {0x18FE34,"Espressif"},{0x247E8B,"Espressif"},{0x441793,"Espressif"},
    {0x600194,"Espressif"},{0x68C631,"Espressif"},{0xA47DD4,"Espressif"},
    {0xEC94CB,"Espressif"},
    // Raspberry Pi
    {0xB827EB,"Raspberry Pi"},{0xDCA632,"Raspberry Pi"},
    {0xE45F01,"Raspberry Pi"},{0x2CCF67,"Raspberry Pi"},
    // Intel (laptop wifi very common)
    {0x186024,"Intel"},{0x1C3947,"Intel"},{0x40A8F0,"Intel"},
    {0x8C04BA,"Intel"},{0xB47DC1,"Intel"},{0xC4E984,"Intel"},
    {0xE45E37,"Intel"},
    // HP
    {0x001B78,"HP"},{0x002219,"HP"},{0x68B599,"HPE"},
    {0xA45D36,"HP"},{0xB05ADA,"HP"},{0xC8CBB8,"HP"},{0xD89D67,"HP"},
    // Lenovo
    {0x14CC20,"Lenovo"},{0x186024,"Lenovo"},{0x88708C,"Lenovo"},
    {0xC85B76,"Lenovo"},
    // Asus
    {0x20CF30,"ASUS"},{0x305A3A,"ASUS"},{0x54B3FA,"ASUS"},
    {0xAC220B,"ASUS"},{0xF02F74,"ASUS"},
    // Tesla
    {0x4CFCAA,"Tesla"},{0xD49E8B,"Tesla"},
    // Ubiquiti
    {0x0418D6,"Ubiquiti"},{0x802AA8,"Ubiquiti"},{0xF09FC2,"Ubiquiti"},
    // Philips Hue (Signify Lighting)
    {0x001788,"Philips Hue"},{0x4CFFC6,"Philips Hue"},
    // ecobee / iRobot
    {0x44617E,"ecobee"},{0x500470,"iRobot"},{0xCC5E18,"iRobot"},
};
static const int OUI_TABLE_COUNT = (int)(sizeof(OUI_TABLE) / sizeof(OUI_TABLE[0]));

static const char *oui_to_vendor(const uint8_t mac[6])
{
    uint32_t oui = ((uint32_t)mac[0] << 16) | ((uint32_t)mac[1] << 8) | mac[2];
    for (int i = 0; i < OUI_TABLE_COUNT; i++)
        if (OUI_TABLE[i].oui == oui) return OUI_TABLE[i].vendor;
    return nullptr;
}

// ===========================================================================
// Shared resolver state. Defined above the pass functions so each pass can
// update the live counters that the UI reads via hostresolve_get_stats().
// ===========================================================================

static volatile bool       s_running = false;
static HostResolveStats    s_stats   = {};

static void stats_reset()
{
    memset(&s_stats, 0, sizeof(s_stats));
    s_stats.pass = HRPASS_IDLE;
}

// ===========================================================================
// DNS / mDNS packet construction + parsing.
// ===========================================================================

// Convert "42.0.0.10.in-addr.arpa" into DNS wire format ([len]label[len]label
// ...[0]) starting at `*out`. Advances *out past the terminator. Returns the
// number of bytes written, or -1 on overflow.
static int dns_write_qname(const char *dotted, uint8_t *out, int out_max)
{
    int oi = 0;
    const char *p = dotted;
    while (*p) {
        const char *dot = strchr(p, '.');
        int label_len = dot ? (int)(dot - p) : (int)strlen(p);
        if (label_len == 0) { p++; continue; }      // leading/double dot
        if (label_len > 63) return -1;
        if (oi + 1 + label_len >= out_max - 1) return -1;
        out[oi++] = (uint8_t)label_len;
        memcpy(out + oi, p, label_len);
        oi += label_len;
        if (!dot) break;
        p = dot + 1;
    }
    if (oi >= out_max) return -1;
    out[oi++] = 0;     // root terminator
    return oi;
}

// Decode a possibly-compressed DNS name starting at `off` within the packet.
// Writes the dotted name into `out`. Returns the number of bytes the name
// occupies AT THE GIVEN OFFSET (not counting compressed targets) so the
// caller can advance past the question/answer record. Returns -1 on
// malformed input.
static int dns_decode_name(const uint8_t *pkt, int pkt_len, int off,
                            char *out, int out_max)
{
    int oi = 0;
    int orig_off = off;
    int hops = 0;
    bool followed_pointer = false;
    int  bytes_at_first_pointer = 0;

    while (off < pkt_len) {
        uint8_t b = pkt[off];
        if (b == 0) {
            off++;
            break;
        }
        if ((b & 0xC0) == 0xC0) {
            // Pointer — top 2 bits set, next 14 bits are the target offset.
            if (off + 1 >= pkt_len) return -1;
            if (!followed_pointer) {
                bytes_at_first_pointer = (off - orig_off) + 2;
                followed_pointer = true;
            }
            int target = ((b & 0x3F) << 8) | pkt[off + 1];
            if (target >= pkt_len) return -1;
            off = target;
            if (++hops > 16) return -1;             // bogus loop
            continue;
        }
        if ((b & 0xC0) != 0) return -1;             // reserved bits set
        int label_len = b;
        if (off + 1 + label_len > pkt_len) return -1;
        if (oi > 0 && oi < out_max - 1) out[oi++] = '.';
        for (int i = 0; i < label_len && oi < out_max - 1; i++)
            out[oi++] = (char)pkt[off + 1 + i];
        off += 1 + label_len;
    }
    out[oi < out_max ? oi : out_max - 1] = '\0';
    return followed_pointer ? bytes_at_first_pointer : (off - orig_off);
}

// Build a DNS / mDNS PTR query for an IPv4 in-addr.arpa name. Writes the
// packet into `buf` and returns its length. The same builder works for both
// — mDNS just has txid=0 and flags=0.
static int build_ptr_query(uint8_t *buf, int buflen, uint16_t txid,
                            uint16_t flags, uint32_t ip_host_order)
{
    if (buflen < 64) return -1;
    int o = 0;
    buf[o++] = txid >> 8;  buf[o++] = txid & 0xFF;
    buf[o++] = flags >> 8; buf[o++] = flags & 0xFF;
    buf[o++] = 0; buf[o++] = 1;   // QDCOUNT = 1
    buf[o++] = 0; buf[o++] = 0;   // ANCOUNT
    buf[o++] = 0; buf[o++] = 0;   // NSCOUNT
    buf[o++] = 0; buf[o++] = 0;   // ARCOUNT

    char qname[40];
    snprintf(qname, sizeof(qname), "%u.%u.%u.%u.in-addr.arpa",
        (unsigned)(ip_host_order & 0xFF),
        (unsigned)((ip_host_order >>  8) & 0xFF),
        (unsigned)((ip_host_order >> 16) & 0xFF),
        (unsigned)((ip_host_order >> 24) & 0xFF));
    int n = dns_write_qname(qname, buf + o, buflen - o);
    if (n < 0) return -1;
    o += n;

    if (o + 4 > buflen) return -1;
    buf[o++] = 0; buf[o++] = 12;  // QTYPE = PTR
    buf[o++] = 0; buf[o++] = 1;   // QCLASS = IN
    return o;
}

// Parse a DNS response and walk its answer section for the first PTR record.
// On success returns 1 and writes the decoded hostname; also writes the IP
// (host-order) that the PTR resolved by re-parsing the question section.
static int parse_ptr_response(const uint8_t *pkt, int pkt_len,
                              uint32_t *ip_out, char *name_out, int name_max)
{
    if (pkt_len < 12) return 0;
    uint16_t qd = ((uint16_t)pkt[4] << 8) | pkt[5];
    uint16_t an = ((uint16_t)pkt[6] << 8) | pkt[7];
    if (an == 0) return 0;

    int off = 12;

    // Pull the IP back out of the question's qname — labels are reversed
    // back to dotted form via dns_decode_name, then we parse "d.c.b.a".
    char qname[80];
    uint32_t ip = 0;
    bool got_ip = false;
    for (uint16_t i = 0; i < qd; i++) {
        int n = dns_decode_name(pkt, pkt_len, off, qname, sizeof(qname));
        if (n < 0) return 0;
        off += n;
        if (off + 4 > pkt_len) return 0;
        off += 4;   // skip QTYPE + QCLASS
        if (!got_ip) {
            unsigned a, b, c, d;
            if (sscanf(qname, "%u.%u.%u.%u.in-addr.arpa", &d, &c, &b, &a) == 4) {
                ip = ((a & 0xFF) << 24) | ((b & 0xFF) << 16)
                   | ((c & 0xFF) <<  8) |  (d & 0xFF);
                got_ip = true;
            }
        }
    }

    // Walk answers; first PTR wins.
    for (uint16_t i = 0; i < an; i++) {
        char rname[80];
        int n = dns_decode_name(pkt, pkt_len, off, rname, sizeof(rname));
        if (n < 0) return 0;
        off += n;
        if (off + 10 > pkt_len) return 0;
        uint16_t type   = ((uint16_t)pkt[off + 0] << 8) | pkt[off + 1];
        // skip class(2) + ttl(4) — we don't use them
        uint16_t rdlen  = ((uint16_t)pkt[off + 8] << 8) | pkt[off + 9];
        off += 10;
        if (off + rdlen > pkt_len) return 0;
        if (type == 12) {   // PTR
            char ptr[80];
            int nn = dns_decode_name(pkt, pkt_len, off, ptr, sizeof(ptr));
            if (nn < 0) return 0;
            // Strip trailing dot if present.
            int L = (int)strlen(ptr);
            if (L > 0 && ptr[L - 1] == '.') ptr[--L] = '\0';
            if (L > 0) {
                strncpy(name_out, ptr, name_max - 1);
                name_out[name_max - 1] = '\0';
                if (ip_out) *ip_out = ip;
                return 1;
            }
        }
        off += rdlen;
    }
    return 0;
}

// Find the device-list index for an IPv4 address (host order). Returns -1
// when no match — mDNS / DNS responses can come back for IPs we didn't
// query when a stale cache entry replies, so we skip those.
static int find_device_idx(uint32_t ip)
{
    int n = pingsweep_device_count();
    for (int i = 0; i < n; i++) {
        const PingDevice *d = pingsweep_device(i);
        if (d && d->ip == ip) return i;
    }
    return -1;
}

// ===========================================================================
// Pass 1 — mDNS multicast PTR
// ===========================================================================
//
// Open one UDP socket bound to *:5353, join 224.0.0.251, fire one PTR query
// per device, then poll recvfrom() with a 1.5-second cumulative window.
// Each response can name one device — write it via pingsweep_set_name().

#define MDNS_ADDR  "224.0.0.251"
#define MDNS_PORT  5353
#define MDNS_WINDOW_MS  1500

static int mdns_pass_run()
{
    int n = pingsweep_device_count();
    if (n == 0) return 0;

    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) {
        Serial.printf("[HR] mdns: socket() failed errno=%d\n", errno);
        return 0;
    }

    int reuse = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_port   = htons(MDNS_PORT);
    local.sin_addr.s_addr = INADDR_ANY;

    bool bound_5353 = (bind(s, (struct sockaddr *)&local, sizeof(local)) == 0);
    if (!bound_5353) {
        // Already bound — fall back to an ephemeral port. We can still
        // SEND multicast queries from any port; the multicast-group join
        // below makes incoming responses reach us regardless.
        Serial.printf("[HR] mdns: 5353 bind failed (errno=%d) - ephemeral\n", errno);
        close(s);
        s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s < 0) { Serial.printf("[HR] mdns: ephemeral socket failed\n"); return 0; }
        local.sin_port = 0;
        if (bind(s, (struct sockaddr *)&local, sizeof(local)) < 0) {
            Serial.printf("[HR] mdns: ephemeral bind failed errno=%d\n", errno);
            close(s); return 0;
        }
    }
    s_stats.mdns_bound_5353 = bound_5353;

    // Multicast group join — REQUIRED to receive the multicast responses
    // mDNS responders typically emit, whether we bound to 5353 or to an
    // ephemeral port. We pin to the WiFi station's address as the egress
    // interface so the join lands on the right netif (otherwise lwIP
    // defaults to the wrong one when multiple are up).
    IPAddress sta_ip = WiFi.localIP();
    struct ip_mreq mreq;
    mreq.imr_multiaddr.s_addr = inet_addr(MDNS_ADDR);
    mreq.imr_interface.s_addr = (uint32_t)sta_ip;
    if (setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0)
        Serial.printf("[HR] mdns: IP_ADD_MEMBERSHIP failed errno=%d\n", errno);

    int ttl = 1;
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    int loop = 0;
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
    // Use the station interface for outbound multicast too, otherwise
    // the kernel may pick the wrong netif.
    struct in_addr iface_addr;
    iface_addr.s_addr = (uint32_t)sta_ip;
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, &iface_addr, sizeof(iface_addr));

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(MDNS_PORT);
    dst.sin_addr.s_addr = inet_addr(MDNS_ADDR);

    // Burst-send all queries. mDNS responders rate-limit themselves so a
    // tight burst is fine — they reply when they're ready.
    uint8_t buf[80];
    for (int i = 0; i < n; i++) {
        const PingDevice *d = pingsweep_device(i);
        if (!d) continue;
        int len = build_ptr_query(buf, sizeof(buf), 0, 0, d->ip);
        if (len > 0 &&
            sendto(s, buf, len, 0, (struct sockaddr *)&dst, sizeof(dst)) > 0)
            s_stats.mdns_sent++;
    }
    HR_LOG("[HR] mdns: sent %u queries (bound5353=%d)\n",
          (unsigned)s_stats.mdns_sent, bound_5353);

    // Listen for responses for MDNS_WINDOW_MS. Each iteration handles one
    // datagram; we keep going until the window expires.
    int resolved = 0;
    uint32_t deadline = millis() + MDNS_WINDOW_MS;
    while ((int32_t)(millis() - deadline) < 0) {
        fd_set rfds; FD_ZERO(&rfds); FD_SET(s, &rfds);
        uint32_t remaining = deadline - millis();
        struct timeval tv = { (long)(remaining / 1000),
                              (long)((remaining % 1000) * 1000) };
        int rc = select(s + 1, &rfds, NULL, NULL, &tv);
        if (rc <= 0) break;

        uint8_t pkt[512];
        struct sockaddr_in from; socklen_t fl = sizeof(from);
        int got = recvfrom(s, pkt, sizeof(pkt), 0,
                           (struct sockaddr *)&from, &fl);
        if (got <= 0) continue;
        s_stats.mdns_replies++;

        uint32_t ip;
        char name[PINGSWEEP_NAME_MAX];
        if (parse_ptr_response(pkt, got, &ip, name, sizeof(name))) {
            int idx = find_device_idx(ip);
            if (idx >= 0) {
                pingsweep_set_name(idx, name, PNAME_MDNS);
                s_stats.mdns_named++;
                resolved++;
            }
        }
    }
    HR_LOG("[HR] mdns: %u replies / %d named\n",
          (unsigned)s_stats.mdns_replies, resolved);

    close(s);
    return resolved;
}

// ===========================================================================
// Pass 1.5 — NetBIOS Name Service (NBSTAT) on UDP/137
// ===========================================================================
//
// Windows PCs, NAS appliances (Synology, QNAP, FreeNAS, …), most network
// printers, and a surprising amount of consumer IoT gear still respond to
// NetBIOS name queries — usually more reliably than mDNS on networks where
// the AP blocks multicast forwarding. We send an NBSTAT (Node Status)
// query to UDP/137 of each host that didn't get an mDNS name; the response
// includes a small "names table" from which we pick the first unique
// workstation- or file-server-suffix entry as the displayable name.

#define NBNS_PORT       137
#define NBNS_WINDOW_MS  1500

// Build a NBSTAT query for the wildcard name "*". The wildcard query has
// the original NetBIOS name "*<15 null bytes>"; per RFC 1002 we encode
// each byte to two ASCII chars in 0x41 ('A')-relative half-nibble form.
// "*" = 0x2A = (0x2 + 'A')<<8 | (0xA + 'A') = 'C' 'K'; every 0x00 → 'A' 'A'.
// Final encoded form: "CKAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA" (32 chars).
static int build_nbns_query(uint8_t *buf, int buflen, uint16_t txid)
{
    if (buflen < 50) return -1;
    int o = 0;
    buf[o++] = txid >> 8;  buf[o++] = txid & 0xFF;
    buf[o++] = 0x00;       buf[o++] = 0x00;   // flags: standard query
    buf[o++] = 0x00;       buf[o++] = 0x01;   // QDCOUNT=1
    buf[o++] = 0x00;       buf[o++] = 0x00;   // ANCOUNT
    buf[o++] = 0x00;       buf[o++] = 0x00;   // NSCOUNT
    buf[o++] = 0x00;       buf[o++] = 0x00;   // ARCOUNT
    buf[o++] = 0x20;                           // length byte (32)
    static const char wildcard[33] = "CKAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
    memcpy(buf + o, wildcard, 32); o += 32;
    buf[o++] = 0x00;                           // root terminator
    buf[o++] = 0x00; buf[o++] = 0x21;          // QTYPE = NBSTAT (33)
    buf[o++] = 0x00; buf[o++] = 0x01;          // QCLASS = IN
    return o;
}

// Parse a NBNS NBSTAT response and pick the most descriptive name from the
// embedded names table. Returns 1 on success.
//
// Layout we walk: hdr(12) + question(34+4) + answer{ name + type(2) +
// class(2) + ttl(4) + rdlen(2) + rdata }. The answer name often uses DNS
// compression (0xC0XX pointer) back into the question. RDATA = 1-byte count
// followed by N * 18-byte records (16-byte padded name + 2-byte flags).
// We prefer the first non-group name with type-suffix 0x00 (Workstation) or
// 0x20 (File Server) — both identify the device itself rather than a domain
// or browser role.
static int parse_nbns_response(const uint8_t *pkt, int pkt_len,
                               char *name_out, int name_max)
{
    if (pkt_len < 12 + 34 + 4 + 12 + 1) return 0;
    int off = 12;
    // Skip question: 1-byte length + 32-byte name + 1-byte terminator + 4
    off += 34 + 4;

    // Answer name — pointer (2 bytes) or full (34 bytes).
    if (off + 2 > pkt_len) return 0;
    if ((pkt[off] & 0xC0) == 0xC0) off += 2;
    else                            off += 34;

    if (off + 10 > pkt_len) return 0;
    uint16_t type = ((uint16_t)pkt[off] << 8) | pkt[off + 1];
    if (type != 0x21) return 0;             // must be NBSTAT response
    off += 8;                                // skip type + class + ttl
    uint16_t rdlen = ((uint16_t)pkt[off] << 8) | pkt[off + 1];
    off += 2;
    if (off + rdlen > pkt_len) return 0;

    if (rdlen < 1) return 0;
    uint8_t num = pkt[off++];

    // Two-pass: first look for suffix 0x00 (workstation = device's hostname).
    // If none, accept 0x20 (file server). NetBIOS names are 15 ASCII chars +
    // a 1-byte type suffix, space-padded.
    for (int prefer_workstation = 1; prefer_workstation >= 0; prefer_workstation--) {
        uint8_t want_suffix = prefer_workstation ? 0x00 : 0x20;
        int p = off;
        for (int i = 0; i < num; i++) {
            if (p + 18 > pkt_len) return 0;
            uint8_t  suffix = pkt[p + 15];
            uint16_t flags  = ((uint16_t)pkt[p + 16] << 8) | pkt[p + 17];
            bool     is_group = (flags & 0x8000) != 0;
            if (!is_group && suffix == want_suffix) {
                char nm[16];
                memcpy(nm, pkt + p, 15);
                nm[15] = '\0';
                // Trim trailing spaces / nulls.
                int L = 15;
                while (L > 0 && (nm[L - 1] == ' ' || nm[L - 1] == '\0'))
                    nm[--L] = '\0';
                if (L > 0) {
                    strncpy(name_out, nm, name_max - 1);
                    name_out[name_max - 1] = '\0';
                    return 1;
                }
            }
            p += 18;
        }
    }
    return 0;
}

static int nbns_pass_run()
{
    int n = pingsweep_device_count();
    if (n == 0) return 0;

    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) { Serial.printf("[HR] nbns: socket failed errno=%d\n", errno); return 0; }

    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_port   = 0;
    local.sin_addr.s_addr = INADDR_ANY;
    bind(s, (struct sockaddr *)&local, sizeof(local));

    // Unicast a query to each host that's still nameless.
    uint8_t buf[80];
    for (int i = 0; i < n; i++) {
        const PingDevice *d = pingsweep_device(i);
        if (!d || d->name_source != PNAME_NONE) continue;
        int len = build_nbns_query(buf, sizeof(buf), (uint16_t)(i + 1));
        if (len <= 0) continue;

        struct sockaddr_in dst;
        memset(&dst, 0, sizeof(dst));
        dst.sin_family = AF_INET;
        dst.sin_port   = htons(NBNS_PORT);
        dst.sin_addr.s_addr = htonl(d->ip);
        if (sendto(s, buf, len, 0, (struct sockaddr *)&dst, sizeof(dst)) > 0)
            s_stats.nbns_sent++;
    }
    HR_LOG("[HR] nbns: sent %u queries\n", (unsigned)s_stats.nbns_sent);

    int resolved = 0;
    uint32_t deadline = millis() + NBNS_WINDOW_MS;
    while ((int32_t)(millis() - deadline) < 0) {
        fd_set rfds; FD_ZERO(&rfds); FD_SET(s, &rfds);
        uint32_t remaining = deadline - millis();
        struct timeval tv = { (long)(remaining / 1000),
                              (long)((remaining % 1000) * 1000) };
        int rc = select(s + 1, &rfds, NULL, NULL, &tv);
        if (rc <= 0) break;

        uint8_t pkt[512];
        struct sockaddr_in from; socklen_t fl = sizeof(from);
        int got = recvfrom(s, pkt, sizeof(pkt), 0,
                           (struct sockaddr *)&from, &fl);
        if (got <= 0) continue;
        s_stats.nbns_replies++;

        // NBNS responses don't carry the queried IP — use the sender's.
        uint32_t ip = ntohl(from.sin_addr.s_addr);
        char name[PINGSWEEP_NAME_MAX];
        if (parse_nbns_response(pkt, got, name, sizeof(name))) {
            int idx = find_device_idx(ip);
            if (idx >= 0) {
                pingsweep_set_name(idx, name, PNAME_NBNS);
                s_stats.nbns_named++;
                resolved++;
            }
        }
    }
    HR_LOG("[HR] nbns: %u replies / %d named\n",
                  (unsigned)s_stats.nbns_replies, resolved);

    close(s);
    return resolved;
}

// ===========================================================================
// Pass 2 — unicast reverse DNS via the LAN's resolver
// ===========================================================================
//
// One UDP socket, all queries to dnsIP():53, await answers for 1.5 s. The
// LAN resolver doesn't typically multiplex multiple queries on a single
// socket the way mDNS does, but since this is a local server we can fire
// the burst and listen — replies come back per-query identified by their
// transaction id.

#define DNS_PORT        53
#define DNS_WINDOW_MS   1500

static int dns_ptr_pass_run()
{
    int n = pingsweep_device_count();
    if (n == 0) return 0;

    IPAddress dns_addr = WiFi.dnsIP(0);
    // Cache the DNS server IP in stats (host-byte order for display).
    // 0.0.0.0 means the LAN didn't hand us a resolver via DHCP — the
    // pass exits early in that case since there's nothing to query.
    uint32_t na = (uint32_t)dns_addr;   // network-order
    s_stats.dns_server_ip = ((na & 0xFF) << 24) | (((na >> 8) & 0xFF) << 16)
                          | (((na >> 16) & 0xFF) << 8) | ((na >> 24) & 0xFF);
    if (dns_addr == IPAddress(0, 0, 0, 0)) {
        HR_LOG("[HR] dns: no LAN resolver configured, skipping\n");
        return 0;
    }

    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) { Serial.printf("[HR] dns: socket() failed errno=%d\n", errno); return 0; }

    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_port   = 0;
    local.sin_addr.s_addr = INADDR_ANY;
    bind(s, (struct sockaddr *)&local, sizeof(local));

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(DNS_PORT);
    dst.sin_addr.s_addr = (uint32_t)dns_addr;   // already network order

    // Tag the unresolved devices and fire one query each. Skip anything
    // mDNS already gave us a name for.
    uint8_t buf[80];
    for (int i = 0; i < n; i++) {
        const PingDevice *d = pingsweep_device(i);
        if (!d || d->name_source != PNAME_NONE) continue;
        int len = build_ptr_query(buf, sizeof(buf), (uint16_t)(i + 1),
                                  0x0100 /* RD=1 */, d->ip);
        if (len > 0 &&
            sendto(s, buf, len, 0, (struct sockaddr *)&dst, sizeof(dst)) > 0)
            s_stats.dns_sent++;
    }
    HR_LOG("[HR] dns: sent %u queries to %u.%u.%u.%u\n",
          (unsigned)s_stats.dns_sent,
          (unsigned)((s_stats.dns_server_ip >> 24) & 0xFF),
          (unsigned)((s_stats.dns_server_ip >> 16) & 0xFF),
          (unsigned)((s_stats.dns_server_ip >>  8) & 0xFF),
          (unsigned)( s_stats.dns_server_ip        & 0xFF));

    int resolved = 0;
    uint32_t deadline = millis() + DNS_WINDOW_MS;
    while ((int32_t)(millis() - deadline) < 0) {
        fd_set rfds; FD_ZERO(&rfds); FD_SET(s, &rfds);
        uint32_t remaining = deadline - millis();
        struct timeval tv = { (long)(remaining / 1000),
                              (long)((remaining % 1000) * 1000) };
        int rc = select(s + 1, &rfds, NULL, NULL, &tv);
        if (rc <= 0) break;

        uint8_t pkt[512];
        struct sockaddr_in from; socklen_t fl = sizeof(from);
        int got = recvfrom(s, pkt, sizeof(pkt), 0,
                           (struct sockaddr *)&from, &fl);
        if (got <= 0) continue;
        s_stats.dns_replies++;

        uint32_t ip;
        char name[PINGSWEEP_NAME_MAX];
        if (parse_ptr_response(pkt, got, &ip, name, sizeof(name))) {
            int idx = find_device_idx(ip);
            if (idx >= 0) {
                pingsweep_set_name(idx, name, PNAME_PTR);
                s_stats.dns_named++;
                resolved++;
            }
        }
    }
    HR_LOG("[HR] dns: %u replies / %d named\n",
          (unsigned)s_stats.dns_replies, resolved);

    close(s);
    return resolved;
}

// ===========================================================================
// Pass 3 — OUI fallback
// ===========================================================================

static int oui_pass_run()
{
    int n = pingsweep_device_count();
    int resolved = 0;
    for (int i = 0; i < n; i++) {
        const PingDevice *d = pingsweep_device(i);
        if (!d) continue;
        if (d->name_source != PNAME_NONE) continue;     // earlier pass won

        if (!d->has_mac) {
            // No ARP entry for the host — happens occasionally when the
            // cache evicts before we look. Log it so the user can tell
            // OUI from random-MAC from no-MAC-at-all cases.
            HR_LOG("[HR] oui: %u.%u.%u.%u no MAC in ARP cache\n",
                (d->ip >> 24) & 0xFF, (d->ip >> 16) & 0xFF,
                (d->ip >>  8) & 0xFF,  d->ip        & 0xFF);
            continue;
        }

        // Locally-administered bit (mac[0] & 0x02) is set on every modern
        // phone using MAC randomization for WiFi privacy. There's no
        // useful OUI for these — they're synthetic — so we label them
        // as "(random MAC)" so the user at least gets feedback that the
        // device is real, just anonymous.
        bool locally_admin = (d->mac[0] & 0x02) != 0;
        const char *v = oui_to_vendor(d->mac);

        HR_LOG("[HR] oui: %u.%u.%u.%u %02X:%02X:%02X:%02X:%02X:%02X %s\n",
            (d->ip >> 24) & 0xFF, (d->ip >> 16) & 0xFF,
            (d->ip >>  8) & 0xFF,  d->ip        & 0xFF,
            d->mac[0], d->mac[1], d->mac[2],
            d->mac[3], d->mac[4], d->mac[5],
            v ? v : (locally_admin ? "(random MAC)" : "(unknown OUI)"));

        if (v) {
            pingsweep_set_name(i, v, PNAME_OUI);
            s_stats.oui_named++;
            resolved++;
        } else if (locally_admin) {
            pingsweep_set_name(i, "(random MAC)", PNAME_OUI);
            s_stats.oui_named++;
            resolved++;
        }
    }
    HR_LOG("[HR] oui: %d devices labelled by vendor\n", resolved);
    return resolved;
}

// ===========================================================================
// Task plumbing
// ===========================================================================

static void resolve_task(void *)
{
    HR_LOG("[HR] hostresolve: starting (%d devices)\n", pingsweep_device_count());

    s_stats.pass = HRPASS_MDNS;
    mdns_pass_run();

    s_stats.pass = HRPASS_NBNS;
    nbns_pass_run();

    s_stats.pass = HRPASS_DNS;
    dns_ptr_pass_run();

    s_stats.pass = HRPASS_OUI;
    oui_pass_run();

    s_stats.pass = HRPASS_SERVICES;
    s_stats.service_replies = service_discovery_run();

    s_stats.pass = HRPASS_DONE;
    HR_LOG("[HR] hostresolve: done (mdns=%u nbns=%u dns=%u oui=%u services=%u)\n",
          (unsigned)s_stats.mdns_named, (unsigned)s_stats.nbns_named,
          (unsigned)s_stats.dns_named, (unsigned)s_stats.oui_named,
          (unsigned)s_stats.service_replies);

    s_running = false;
    vTaskDelete(NULL);
}

bool hostresolve_start()
{
    if (s_running) return false;
    if (WiFi.status() != WL_CONNECTED) return false;
    if (pingsweep_device_count() == 0) return false;

    stats_reset();
    s_running = true;

    // 5 KB stack — sockets + select() + small packet buffers fit easily.
    BaseType_t ok = xTaskCreatePinnedToCore(
        resolve_task, "hostresolve", 5120, NULL, 1, NULL, 0);
    if (ok != pdPASS) {
        s_running = false;
        s_stats.pass = HRPASS_IDLE;
        return false;
    }
    return true;
}

bool hostresolve_is_running() { return s_running; }

void hostresolve_get_stats(HostResolveStats *out)
{
    if (!out) return;
    *out = s_stats;
}
