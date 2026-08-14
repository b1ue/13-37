#pragma once

#include <stdint.h>

// Receive-only pulse-train analyzer for nearby sub-GHz remotes. The module
// deliberately exposes no transmit or replay API.
bool rolling_code_start();
void rolling_code_stop();
void rolling_code_prepare_for_sleep();
bool rolling_code_is_running();
int16_t rolling_code_last_error();

void rolling_code_set_band(uint8_t band);
uint8_t rolling_code_get_band();
float rolling_code_get_frequency();

void rolling_code_reset_session();
void rolling_code_worker();

uint32_t rolling_code_capture_count();
uint32_t rolling_code_unique_count();
uint32_t rolling_code_repeat_count();
uint32_t rolling_code_changed_count();
uint32_t rolling_code_last_signature();
uint16_t rolling_code_last_pulse_count();
uint16_t rolling_code_last_base_us();
uint8_t rolling_code_last_delta_percent();
int8_t rolling_code_last_peak_rssi();
float rolling_code_threshold();
const char *rolling_code_last_verdict();
const char *rolling_code_last_log_path();
