#pragma once

#include <stdbool.h>

#include "ui.h"

// Applies the user's settings (backlight, timezone, Bluetooth, AirPlay
// versions) and connects them to the Settings page: loads them from NVS,
// handles changes from the UI, persists them, follows the IP-detected
// timezone while that's on "automatic", and pushes values (including
// WiFi/memory info) back to the page.

// Loads settings and applies them. Call after uart_link_start(),
// airplay_init(), lcd_driver_init() and online_start().
void settings_ui_init(void);

// Registers the UI handler and starts the refresh timer. Call under the
// LVGL lock, after ui_init().
void settings_ui_start(void);

// For the web page: what the Settings page shows, and a change applied as
// if made there. Both take the LVGL lock, so never call them from the LVGL
// task.
void settings_ui_snapshot(ui_settings_t *out);
void settings_ui_change(ui_setting_t setting, int value, const char *zone);

bool settings_ui_haptics(void);
