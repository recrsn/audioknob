#pragma once

#include "esp_a2dp_api.h"

void a2dp_sink_init(void);

void a2dp_sink_bt_event(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param);

// Drops the A2DP link to the connected phone, if any. Last resort for
// stopping playback when the phone exposes no AVRCP controller.
void a2dp_sink_disconnect(void);
