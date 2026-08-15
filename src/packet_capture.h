#pragma once

#include <stdint.h>

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

// 0 means hop channels 1-13; 1-13 locks to a channel.
void packet_capture_set_channel(uint8_t channel);
uint8_t packet_capture_get_channel();
uint8_t packet_capture_current_channel();

uint32_t packet_capture_total();
uint32_t packet_capture_dropped();
uint32_t packet_capture_packets_per_second();
uint32_t packet_capture_channel_count(uint8_t channel);

int packet_capture_recent_count();
bool packet_capture_get_recent(int newest_index, PacketCaptureView *out);
const char *packet_capture_log_path();

