#pragma once
#include <stdint.h>
#include <stdbool.h>

// Hostname resolver for ping-sweep results. Runs a three-pass discovery
// against the discovered devices, filling in each PingDevice::name +
// name_source field as answers arrive:
//
//   Pass 1 (mDNS)  — multicast PTR query per IP to 224.0.0.251:5353,
//                    listen 1.5 s for `.local` responses.
//   Pass 2 (PTR)   — unicast PTR query per still-unresolved IP to the
//                    LAN's configured DNS server, 8-way parallel via
//                    select().
//   Pass 3 (OUI)   — vendor lookup from the MAC's first three bytes
//                    against a curated consumer-device OUI table. Always
//                    available as a fallback when both DNS passes miss.
//
// Spawned automatically by pingsweep at the end of a sweep — the UI sees
// names trickle into the device rows as each pass writes them.

// Pass identifiers used in HostResolveStats::pass to tell the UI which
// stage the resolver is currently in.
enum HostResolvePass {
    HRPASS_IDLE = 0,
    HRPASS_MDNS = 1,
    HRPASS_NBNS = 2,   // NetBIOS Name Service (UDP/137 NBSTAT)
    HRPASS_DNS  = 3,
    HRPASS_OUI  = 4,
    HRPASS_SERVICES = 5,
    HRPASS_DONE = 6,
};

// Snapshot of the resolver's progress. Read via hostresolve_get_stats().
// The UI uses it to show what's happening while names trickle in.
struct HostResolveStats {
    uint8_t  pass;             // HostResolvePass
    uint16_t mdns_sent;        // # of mDNS PTR queries emitted
    uint16_t mdns_replies;     // datagrams received during the mDNS window
    uint16_t mdns_named;       // # of devices we wrote a name for
    uint16_t nbns_sent;
    uint16_t nbns_replies;
    uint16_t nbns_named;
    uint16_t dns_sent;
    uint16_t dns_replies;
    uint16_t dns_named;
    uint16_t oui_named;
    uint16_t service_replies;
    uint32_t dns_server_ip;    // host-order; 0 if no LAN DNS configured
    bool     mdns_bound_5353;  // did we manage to bind to port 5353?
};

bool hostresolve_start();    // kicks the resolver task; no-op if running
bool hostresolve_is_running();
void hostresolve_get_stats(HostResolveStats *out);
