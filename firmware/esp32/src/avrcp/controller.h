#pragma once

#include <stdbool.h>

#include "esp_avrc_api.h"

// AVRCP controller role: sends transport passthrough commands (play/pause,
// skip) to the phone, and hands every CT event on to track_meta for
// now-playing metadata and cover art. No browsing.

void avrcp_controller_init(void);

void avrcp_controller_bt_event(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param);

// Toggles play/pause on the connected phone. No-op if nothing is connected.
void avrcp_controller_toggle_play_pause(void);

// Skips to the next/previous track on the connected phone. No-op if nothing is connected.
void avrcp_controller_next(void);
void avrcp_controller_prev(void);

// Next AVRCP transaction label; every CT command needs one. bt_app task only.
uint8_t avrcp_controller_alloc_tl(void);

// Pauses the phone, or drops its A2DP link when there's no AVRCP link to ask
// over.
void avrcp_controller_pause(void);
