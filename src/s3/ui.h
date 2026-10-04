#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "protocol.h"

// Builds all screens and shows the idle screen on the active LVGL display.
// Caller must have already registered a display driver.
void ui_init(void);

typedef struct {
    char title[64];
    char artist[64];
    char album[64];
} ui_track_info_t;

// Pushes a fresh wireless_state_t (A2DP state from the ESP32 over UART, or
// AirPlay state from this MCU -- state->source says which) and
// the currently playing track into the wireless now-playing screen. Safe to
// call whether or not that screen is currently visible.
void ui_update_wireless(const wireless_state_t *state, const ui_track_info_t *track);

// Pushes USB UAC2 volume/mute/playback state into the USB now-playing screen.
void ui_update_usb(uint8_t volume, bool muted, bool playing);

// Pushes the current local time into the idle screen. local_time is ignored
// (the screen shows its unsynced placeholder) unless synced is true --
// ntp_is_synced() on the S3 side tracks whether SNTP has ever landed.
void ui_update_clock(bool synced, const struct tm *local_time);

// Shows the WiFi setup screen (a WiFi-join QR for the device's setup
// network plus its name/password for manual entry) and makes it the home
// screen that idle/back navigation returns to. Cheap to call every poll:
// the QR is only regenerated when qr_payload changes.
void ui_show_wifi_setup(const char *qr_payload, const char *ssid, const char *password, const char *status);
