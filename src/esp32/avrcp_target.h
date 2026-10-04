#pragma once

#include <stdint.h>

#include "esp_avrc_api.h"

// AVRCP target role: we're the "absolute volume" target the phone (the
// AVRCP controller) pushes volume changes to, and we push local knob changes
// back the same way — keeps phone slider and knob position in sync rather
// than fighting each other.

void avrcp_target_init(void);

void avrcp_target_bt_event(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param);

// Called by the encoder when the knob changes volume locally (0-100).
// Updates app_state and notifies the connected controller, if any.
void avrcp_target_set_local_volume(uint8_t volume_pct);
