#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "protocol.h"

// Builds all screens and shows the idle screen on the active LVGL display.
// Caller must have already registered a display driver.
void ui_init(void);

#define UI_ART_SIZE 132

typedef struct {
    char title[64];
    char artist[64];
    char album[64];
    // Optional cover art, UI_ART_SIZE^2 RGB565; must stay valid until the
    // next call with a different art_version. NULL shows the placeholder.
    const uint16_t *art;
    uint32_t art_version;
} ui_track_info_t;

// Pushes a fresh wireless_state_t (A2DP state from the ESP32 over UART, or
// AirPlay state from this MCU -- state->source says which) and
// the currently playing track into the wireless now-playing screen. Safe to
// call whether or not that screen is currently visible.
void ui_update_wireless(const wireless_state_t *state, const ui_track_info_t *track);

typedef enum {
    UI_TRANSPORT_PREV,
    UI_TRANSPORT_PLAY_PAUSE,
    UI_TRANSPORT_NEXT,
} ui_transport_t;

// Called on the LVGL task when a wireless-screen transport button is
// tapped; must not block.
void ui_set_transport_handler(void (*handler)(ui_transport_t button));

typedef enum {
    UI_SOURCE_AIRPLAY,
    UI_SOURCE_BLUETOOTH,
} ui_source_t;

// Called on the LVGL task when the user picks a source on the switch screen
// (tap the status row of the wireless screen); must not block. The UI then
// shows that source's screen -- its waiting state if nothing is connected --
// until a stream starts or the user swipes home.
void ui_set_source_handler(void (*handler)(ui_source_t source));

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
