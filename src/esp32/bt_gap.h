#pragma once

#include <stdbool.h>

#include "esp_err.h"

#define AK_BT_DEVICE_NAME "AudioKnob"

// Brings up the BT controller (Classic-only) and Bluedroid host, and sets
// Secure Simple Pairing to Just Works (no display/keyboard on this device).
esp_err_t bt_gap_init(void);

// Makes the device discoverable and connectable so a phone can pair/reconnect.
void bt_gap_set_discoverable(bool discoverable);
