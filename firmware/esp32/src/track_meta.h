#pragma once

#include "esp_avrc_api.h"

// AVRCP now-playing metadata and cover art, pushed to the S3. On connect it
// asks the phone which notifications it supports, registers for track
// changes, and fetches title/artist/album on each one. If the phone offers
// AVRCP 1.6 cover art, it also opens the OBEX cover-art channel and streams
// each track's linked thumbnail (a 200x200 JPEG) to the S3 as it arrives.

// Feeds an AVRCP controller event. Must run on the bt_app task (via
// avrcp_controller's dispatch), with metadata text and cover-art data
// already deep-copied out of the Bluedroid callback.
void track_meta_handle_ct_event(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param);
