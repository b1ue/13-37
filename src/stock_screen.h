#pragma once
#include <lvgl.h>
#include <stddef.h>

void stock_screen_create();
void stock_screen_show();
bool stock_screen_is_active();
bool stock_screen_is_fetching();

// Build a compact, display-only stream from configured symbols and quotes
// already cached by the Stocks screen. This never performs I/O or starts a
// network request, so Matrix rain cannot become a sleep or battery blocker.
size_t stock_screen_build_rain_feed(char *out, size_t out_size);
