#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "protocol.h"

// AirPlay mode, owned by this MCU: the RAOP receiver, its playback/volume
// state, and DACP control back to the sender. Mirrors what the ESP32 does
// for A2DP/AVRCP -- the knob is authoritative locally and pushed to the
// sender, the sender's slider is applied locally.

// Starts the player and control task. The receiver itself only comes up
// on the first airplay_network_up().
void airplay_init(void);

// Safe from the WiFi event task: hands off to the control task.
void airplay_network_up(const uint8_t mac[6]);

bool airplay_is_connected(void);

// Snapshot for display, shaped like the ESP32's A2DP state so the wireless
// screen can render either (source is always SOURCE_AIRPLAY).
void airplay_get_state(wireless_state_t *state, track_info_t *track);

// Local controls. Non-blocking: DACP requests go out on the control task.
void airplay_adjust_volume(int steps);
void airplay_toggle_play_pause(void);

// A2DP took the DAC: stop local output and pause the sender (or drop the
// session if it has no DACP remote). Non-blocking.
void airplay_preempt(void);
