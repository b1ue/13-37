#pragma once
#include <stdint.h>
#include <stdbool.h>

// Single daily alarm: rings once per day at the configured local time when
// enabled. Persisted to /Settings/alarm.txt. The alarm_screen.cpp settings UI
// drives this module; alarm_tick() — called from the main loop — fires the
// haptic + the ringing screen when the time matches.

void alarm_init();    // load /Settings/alarm.txt; call in setup()
void alarm_tick();    // check time + drive haptic while ringing; call in loop()

int  alarm_get_hour();
int  alarm_get_minute();
bool alarm_is_enabled();
void alarm_set(int hour, int minute, bool enabled);   // updates + persists

bool alarm_is_ringing();
// Snooze uses a millis()-based deadline and is intentionally kept awake until
// it fires; unlike the daily alarm, that deadline is not persisted over reset.
bool alarm_is_snoozed();
void alarm_dismiss();              // stop ringing; no further fire today
void alarm_snooze(int minutes);    // stop now, re-ring after `minutes`

// Wall-clock time of the next ring. Usually the saved alarm time; while a
// snooze is pending, the snooze deadline projected onto the local clock.
void alarm_get_next_fire_time(int *h, int *m);

// True once the alarm has been loaded from SD or explicitly set via
// alarm_set(); false on a fresh boot with no /Settings/alarm.txt. The alarm
// screen uses this to decide whether to seed its rollers with the saved time
// or with a "now + 1 hour" suggestion.
bool alarm_is_configured();

// ---- alert behaviour --------------------------------------------------------
// Persisted to /Settings/alarm.txt alongside the alarm time. Defaults on a
// fresh device: vibrate on, audio on, volume 100%, snooze 5 minutes.

bool     alarm_get_vibrate();
void     alarm_set_vibrate(bool on);

bool     alarm_get_audio();
void     alarm_set_audio(bool on);

uint8_t  alarm_get_volume();           // 0..100
void     alarm_set_volume(uint8_t pct);

int      alarm_get_snooze_minutes();   // currently 5, 10, or 20
void     alarm_set_snooze_minutes(int minutes);

// ---- shared chime ----------------------------------------------------------
// Lets other modules (timer screen, future notifiers) play the same
// doorbell-loop chime the alarm uses, without re-implementing the I2S
// task. `volume_override` (1..100) wins over the saved alarm volume for
// the duration of this chime; pass 0 to use the saved volume as-is.
// Calling start while the chime is already running is a no-op.
void     alarm_play_chime_loop(uint8_t volume_override);
void     alarm_stop_chime_loop();
