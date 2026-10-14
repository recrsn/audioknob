#pragma once

#include <stdbool.h>

#include "esp_err.h"

// Advertised until the S3 sends the user's name (MSG_DEVICE_NAME), which
// happens as soon as it sees this MCU on the link.
#define AK_BT_DEVICE_NAME "AudioKnob"

// Brings up the BT controller (Classic-only) and Bluedroid host, and sets
// Secure Simple Pairing to Just Works (no display/keyboard on this device).
esp_err_t bt_gap_init(void);

// Makes the device discoverable and connectable so a phone can pair/reconnect.
void bt_gap_set_discoverable(bool discoverable);

// Renames the device for inquiry and remote-name requests. Phones that
// already paired keep their own cached name until they re-query it.
void bt_gap_set_name(const char *name);
