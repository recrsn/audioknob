#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "protocol.h"

// The user-facing device name: the AirPlay speaker, the Bluetooth device
// (via the ESP32), the setup hotspot's SSID and, slugified, the mDNS
// hostname. Persisted in NVS namespace "device"; first boot picks
// "AudioKnob-" plus 8 random characters so several units on one network
// can be told apart.

typedef void (*device_name_cb_t)(const char *name);

// Loads (or creates) the name. on_change runs on the caller of
// device_name_set() after every successful change. NVS must be up
// (settings_ui_init() does that).
void device_name_init(device_name_cb_t on_change);

// Thread-safe copy; len >= DEVICE_NAME_MAX + 1 never truncates.
void device_name_get(char *out, size_t len);

// The name as a DNS label ("Amitosh's Desk" -> "amitoshs-desk"), for mDNS.
void device_name_hostname(char *out, size_t len);

// Trims surrounding whitespace, then rejects empty names, control
// characters and names over DEVICE_NAME_MAX bytes. Saves and notifies on
// success. Safe from any task.
bool device_name_set(const char *name);
