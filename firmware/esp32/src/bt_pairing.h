#pragma once

#include <stdbool.h>

#include "esp_gap_bt_api.h"
#include "protocol.h"

// Who may pair. The device stays connectable but hidden, so bonded phones
// reconnect while new ones can pair only during a window the user opens
// on the S3's screen, and only once the user accepts the numeric
// comparison code shown there. Bluedroid accepts Just Works pairing
// without asking the app, so any bond made outside the window is removed
// again as soon as it completes.
//
// Everything here runs on the bt_app task; the entry points below only
// dispatch to it, so they are safe from any task.

// Call once Bluedroid is up (after bt_gap_init()).
void bt_pairing_init(void);

// Bluedroid GAP security events, from bt_gap's callback.
void bt_pairing_gap_event(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param);

// CMD_PAIRING_* and CMD_FORGET_DEVICES from the S3.
void bt_pairing_command(protocol_cmd_t cmd);

// A copy of the current status for MSG_PAIRING. All zeros (PAIRING_IDLE,
// nothing bonded) until bt_pairing_init().
pairing_status_t bt_pairing_status(void);
