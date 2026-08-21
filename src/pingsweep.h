#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// ICMP ping sweep of the locally-connected subnet. Runs in a background
// task so the UI stays live; discovered hosts are collected into a list and,
// once the sweep finishes, written to /PingSweeps/ on the SD card.

#define PINGSWEEP_MAX_TARGETS 254
#define PINGSWEEP_MAX_DEVICES 64
#define PINGSWEEP_NAME_MAX     64
#define PINGSWEEP_EVIDENCE_MAX 48
#define PINGSWEEP_SERVICES_MAX 80

// Where did the name we display come from? Filled in by hostresolve after
// the sweep completes. NAME_NONE means we never found one and the UI
// should fall back to the raw IP.
enum PingNameSource {
    PNAME_NONE = 0,
    PNAME_MDNS = 1,   // .local name from a multicast-DNS PTR
    PNAME_NBNS = 2,   // NetBIOS Name Service NBSTAT response
    PNAME_PTR  = 3,   // FQDN from a regular DNS PTR via the LAN resolver
    PNAME_OUI  = 4,   // vendor string derived from the MAC OUI (fallback)
};

struct PingDevice {
    uint32_t ip;        // host-order IPv4 address
    uint32_t rtt_ms;    // ICMP echo round-trip time
    uint8_t  mac[6];    // from the ARP cache, best-effort
    bool     has_mac;
    char     name[PINGSWEEP_NAME_MAX];  // empty unless name_source != NONE
    uint8_t  name_source;               // PingNameSource
    // Keep each piece of naming evidence instead of discarding weaker
    // answers when the preferred display name is selected.
    char mdns[PINGSWEEP_EVIDENCE_MAX];
    char nbns[PINGSWEEP_EVIDENCE_MAX];
    char ptr[PINGSWEEP_EVIDENCE_MAX];
    char vendor[PINGSWEEP_EVIDENCE_MAX];
    char services[PINGSWEEP_SERVICES_MAX];
};

void pingsweep_start();          // sweep the connected subnet (bounded)
void pingsweep_stop();           // abort an in-progress sweep
bool pingsweep_is_running();
void pingsweep_poll();           // writes results to SD once done; call from loop()

int  pingsweep_scanned();        // hosts probed so far
int  pingsweep_total();          // total hosts in the sweep
int  pingsweep_device_count();
const PingDevice *pingsweep_device(int idx);
bool pingsweep_just_finished();  // true exactly once after a sweep completes

// Writer used by hostresolve to drop a resolved name onto a discovered
// host. No-op when idx is out of range or `src` is NAME_NONE.
void pingsweep_set_name(int idx, const char *name, uint8_t src);
void pingsweep_add_service_for_ip(uint32_t ip, const char *service);
void pingsweep_network_cidr(char *out, size_t size);
bool pingsweep_is_limited();
