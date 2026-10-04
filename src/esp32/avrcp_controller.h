#pragma once

#include <stdbool.h>

#include "esp_avrc_api.h"

// AVRCP controller role: lets us send transport passthrough commands
// (play/pause) to the phone from the local push button. This is the only
// AVRCP CT usage in this codebase — no browsing, no metadata requests.

void avrcp_controller_init(void);

void avrcp_controller_bt_event(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param);

// Toggles play/pause on the connected phone. No-op if nothing is connected.
void avrcp_controller_toggle_play_pause(void);

// Sends an explicit pause. Returns false if no AVRCP controller link is up.
bool avrcp_controller_pause(void);
