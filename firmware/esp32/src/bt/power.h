#pragma once

#include <stdbool.h>

// The user's Bluetooth on/off setting. The S3 owns it (it's on the Settings
// page) but it's kept here too, in NVS, so this MCU boots straight into the
// right mode. Switching restarts this MCU: Bluedroid and its profiles come
// up only at boot.

// Brings up NVS. Defaults to on.
bool bt_power_enabled(void);

// Saves the setting and restarts into it if it changed. Safe from any task.
void bt_power_set_enabled(bool enabled);

// Bluetooth off: light-sleeps between visits from the S3, for good.
void bt_power_sleep_forever(void) __attribute__((noreturn));
