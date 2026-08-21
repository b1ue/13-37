#pragma once
#include <stdint.h>

// Bounded, diagnostic-only multicast discovery. Sends one standards-based
// SSDP M-SEARCH and one DNS-SD service-enumeration query, then listens briefly
// for replies. It never connects to, configures, or probes returned services.
uint16_t service_discovery_run();
