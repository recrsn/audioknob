#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "protocol.h"
#include "ui/ui.h"

// Which source is "in front" -- shown on the now-playing screen and driven
// by the knob, the touch transport and the web page -- and the controls
// that act on it. Whatever holds the DAC is in front; with nothing playing,
// an open AirPlay session, then a stopped radio station, then Bluetooth.
// Only for Bluetooth does the ESP32 own the controls, this MCU only
// displaying. Safe from any task.

typedef enum {
    NOW_PLAYING_BLUETOOTH,
    NOW_PLAYING_AIRPLAY,
    NOW_PLAYING_RADIO,
} now_playing_front_t;

now_playing_front_t now_playing_front(void);
bool now_playing_airplay_in_front(void);

// Snapshot of the source in front. False while that's Bluetooth and the
// ESP32 hasn't reported yet; track is zeroed when there's no metadata.
bool now_playing_get(wireless_state_t *state, track_info_t *track);

// The name of the sender in front ("Alex's iPhone"), or "" when it gave
// none.
void now_playing_peer_name(char *out, size_t len);

// The front source's cover art (ARTWORK_SIZE^2 RGB565), or NULL.
const uint16_t *now_playing_artwork(uint32_t *version);

// Volume of the source in front, a knob detent (1 %) per step. False when
// nothing is connected to take it.
bool now_playing_adjust_volume(int steps);
bool now_playing_set_volume(uint8_t pct);

void now_playing_transport(ui_transport_t button);

// A manual pick from the switch screen. It holds only while the DAC stays
// where the pick put it: if another source then starts playing, "last to
// start wins" takes over and the pick is dropped. Picking either one stops
// a playing radio.
void now_playing_pick(ui_source_t source);
