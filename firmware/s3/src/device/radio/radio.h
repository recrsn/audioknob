#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "protocol.h"
#include "ui.h"

// Internet radio mode, owned by this MCU like AirPlay: the station being
// played from the presets, its volume, and its claim on the DAC. Starting a
// station takes the DAC from whatever has it (AirPlay is preempted, A2DP
// paused); anything else starting to play preempts the radio. All
// functions are non-blocking and safe from any task.

void radio_init(void);

// Plays preset `index`, or stops when it's -1.
void radio_play(int index);
void radio_stop(void);
void radio_toggle_play_pause(void);
void radio_next(void);
void radio_prev(void);
void radio_adjust_volume(int steps);

// Another source took the DAC: stop, and drop the station so the radio no
// longer shows as connected.
void radio_preempt(void);

// A station is loaded: playing, or stopped by the user and ready to resume.
bool radio_is_loaded(void);

// Shaped like the other sources' state (source is SOURCE_RADIO): connected
// while a station is loaded, playing while it's meant to play; the track's
// title is the stream's own (or the station's name), its artist the station
// name or what's happening ("Buffering…").
void radio_get_state(wireless_state_t *state, track_info_t *track);
void radio_get_station_name(char *out, size_t len);

// The Radio app's data. Returns a version that changes whenever it does.
uint32_t radio_ui_snapshot(ui_radio_t *out);
