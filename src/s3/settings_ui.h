#pragma once

#include <stdbool.h>

// Applies the user's settings (backlight, timezone) and connects them to the
// Settings page: loads them from NVS, handles changes from the UI, persists
// them, follows the IP-detected timezone while that's on "automatic", and
// pushes values (including WiFi/memory info) back to the page.

// Loads settings and applies backlight and timezone. Call after
// lcd_driver_init() and online_start().
void settings_ui_init(void);

// Registers the UI handler and starts the refresh timer. Call under the
// LVGL lock, after ui_init().
void settings_ui_start(void);

bool settings_ui_haptics(void);
