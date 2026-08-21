#pragma once

#include <stdint.h>
#include <stddef.h>

// Receive-only 802.11 management-frame capture. There is intentionally no
// packet injection, replay, deauthentication, or transmit API in this module.

static constexpr uint16_t PACKET_CAPTURE_SNAPLEN = 192;

struct PacketCaptureView {
    uint32_t sequence;
    uint32_t timestamp_ms;
    int8_t   rssi;
    uint8_t  channel;
    uint16_t original_len;
    uint16_t captured_len;
    uint8_t  type;
    uint8_t  subtype;
    uint8_t  addr1[6];
    uint8_t  addr2[6];
    uint8_t  addr3[6];
    uint8_t  bytes[PACKET_CAPTURE_SNAPLEN];
};

bool packet_capture_start();
void packet_capture_stop();
void packet_capture_prepare_for_sleep();
void packet_capture_tick();

bool packet_capture_is_running();
bool packet_capture_is_starting();
const char *packet_capture_status_text();

// Receive-side filters. subtype_mask uses one bit per 802.11 management
// subtype (0xFFFF = any). The optional MAC matches any of addr1/2/3.
void packet_capture_set_subtype_mask(uint16_t subtype_mask);
uint16_t packet_capture_subtype_mask();
void packet_capture_set_min_rssi(int8_t rssi);
int8_t packet_capture_min_rssi();
void packet_capture_set_mac_filter(const uint8_t mac[6]);
void packet_capture_clear_mac_filter();
bool packet_capture_mac_filter(uint8_t out[6]);

// 0 means hop channels 1-13; 1-13 locks to a channel.
void packet_capture_set_channel(uint8_t channel);
uint8_t packet_capture_get_channel();
uint8_t packet_capture_current_channel();

uint32_t packet_capture_total();
uint32_t packet_capture_dropped();
uint32_t packet_capture_packets_per_second();
uint32_t packet_capture_rejected();
uint32_t packet_capture_channel_count(uint8_t channel);

int packet_capture_recent_count();
bool packet_capture_get_recent(int newest_index, PacketCaptureView *out);
const char *packet_capture_log_path();
void packet_capture_summary_text(char *out, size_t size);
