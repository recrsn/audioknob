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
    // The connected sender's name ("Amitosh's iPhone"); "" when unknown.
    char peer[64];
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

// Pushes the ESP32's Bluetooth pairing status, or NULL while there is none
// (Bluetooth off, ESP32 not answering). Cheap to call every poll. Anything
// past PAIRING_IDLE shows a prompt over whatever screen is up.
void ui_update_pairing(const pairing_status_t *status);

typedef enum {
    UI_PAIRING_OPEN,   // start the pairing window
    UI_PAIRING_CLOSE,  // end it early
    UI_PAIRING_ACCEPT, // the code matches the phone's
    UI_PAIRING_REJECT,
    UI_PAIRING_FORGET, // remove every paired device
} ui_pairing_action_t;

// Called on the LVGL task for the pairing prompt's buttons and the
// Bluetooth settings rows; must not block. The result shows in a later
// ui_update_pairing().
void ui_set_pairing_handler(void (*handler)(ui_pairing_action_t action));

// Pushes the version of a firmware update that's been received and checked
// and waits for the user, or NULL while there is none. Cheap to call every
// poll; a pending update asks once, over whatever screen is up.
void ui_update_firmware(const char *pending_version);

// Called on the LVGL task with the user's answer to that prompt; install
// may restart the device.
void ui_set_firmware_handler(void (*handler)(bool install));

// Called on the LVGL task when the user picks a source on the switch screen
// (tap the status row of the wireless screen); must not block. The UI then
// shows that source's screen -- its waiting state if nothing is connected --
// until a stream starts or the user swipes home.
void ui_set_source_handler(void (*handler)(ui_source_t source));

// Knob input for what the UI owns outright: the launcher and the app pages
// (lights, timer, tomato, stopwatch, settings).
// Returns false when the active screen has no use for it, leaving the caller
// to apply it elsewhere (volume, play/pause). Call with the LVGL lock held.
bool ui_knob_rotate(int steps);
bool ui_knob_press(void);
// Opens the app launcher, or closes it to the clock; never falls through.
void ui_knob_long_press(void);

// Called on the LVGL task when a countdown (timer, tomato) starts ringing
// (true) and once nothing rings any more (false), so hardware can add a
// buzz; must not block.
void ui_set_alert_handler(void (*handler)(bool ringing));

// Called on the LVGL task when the display goes to sleep (false) after the
// Settings page's idle timeout, and when input, a ringing countdown, a
// pairing request or a new stream wakes it (true), so hardware can switch
// the backlight; must not block. The UI blacks the screen out by itself.
void ui_set_display_handler(void (*handler)(bool on));

#ifdef AK_USB_AUDIO
// Pushes USB UAC2 volume/mute/playback state into the USB now-playing screen.
// USB audio isn't implemented, so the screen is compiled out without
// AK_USB_AUDIO.
void ui_update_usb(uint8_t volume, bool muted, bool playing);
#endif

// Pushes the current local time into the idle screen. local_time is ignored
// (the screen shows its unsynced placeholder) unless synced is true --
// ntp_is_synced() on the S3 side tracks whether SNTP has ever landed.
void ui_update_clock(bool synced, const struct tm *local_time);

// Shows the WiFi setup screen (a WiFi-join QR for the device's setup
// network plus its name/password for manual entry) and makes it the home
// screen that idle/back navigation returns to. Cheap to call every poll:
// the QR is only regenerated when qr_payload changes.
void ui_show_wifi_setup(const char *qr_payload, const char *ssid, const char *password, const char *status);

#define UI_WEATHER_HOURS 4

// Temperatures are Celsius; the UI converts per the units setting. Codes are
// WMO weather interpretation codes, as Open-Meteo reports them.
typedef struct {
    bool valid; // false: nothing to show yet, `status` says why
    char status[48];
    char place[32];
    float temp_c;
    float high_c;
    float low_c;
    uint8_t code;
    bool is_day;
    struct {
        uint8_t hour; // local, 0-23
        float temp_c;
        uint8_t code;
        bool is_day;
    } hours[UI_WEATHER_HOURS];
} ui_weather_t;

// Pushes the weather page's data; call when it changes, not every poll.
void ui_update_weather(const ui_weather_t *weather);

#define UI_LIGHTS_MAX 6

typedef struct {
    char name[24];
    bool reachable;
    bool on;
    uint8_t brightness; // 1-100 %
} ui_light_t;

// Pushes every light's state; cheap to call every poll (ignored unless
// something changed).
void ui_update_lights(const ui_light_t *lights, int count);

typedef struct {
    void (*set_power)(int index, bool on);
    void (*set_brightness)(int index, uint8_t pct);
} ui_lights_handler_t;

// Called on the LVGL task when the user changes a light; must not block.
// The change shows once a ui_update_lights() reflects it.
void ui_set_lights_handler(const ui_lights_handler_t *handler);

typedef struct {
    uint8_t brightness; // backlight, 5-100 %
    uint16_t sleep_s;   // idle time before the display sleeps; 0 = never
    bool clock_24h;
    bool fahrenheit;
    bool haptics;
    bool bluetooth;
    uint8_t airplay;   // UI_AIRPLAY_* mask; 0 = off
    bool airplay2_available; // else only UI_AIRPLAY_1 means anything
    bool tz_auto;      // following the location's zone
    bool location_auto; // following the IP-detected location
    char location[32]; // city in effect; "" while still unknown
    char tz_name[40];  // zone in effect; "" while still unknown
    char device_name[33];
    char hostname[64]; // mDNS, without ".local"
    char wifi_ssid[33]; // "" when not connected
    char ip[16];
    char version[32];
    uint32_t free_internal_kb;
    uint32_t free_psram_kb;
} ui_settings_t;

// Pushes the settings page's values; cheap to call every poll.
void ui_update_settings(const ui_settings_t *settings);

typedef enum {
    UI_SETTING_BRIGHTNESS, // value: 5-100
    UI_SETTING_CLOCK_24H,  // value: 0/1
    UI_SETTING_FAHRENHEIT, // value: 0/1
    UI_SETTING_HAPTICS,    // value: 0/1
    UI_SETTING_TIMEZONE,   // zone: a tz_db name, or NULL for automatic
    UI_SETTING_FORGET_WIFI,
    UI_SETTING_BLUETOOTH,  // value: 0/1
    UI_SETTING_AIRPLAY,    // value: UI_AIRPLAY_* mask
    UI_SETTING_SLEEP,      // value: seconds, 0-UI_SLEEP_MAX_S; 0 = never
    UI_SETTING_LOCATION,   // back to the IP-detected one; picking one is the web page's
} ui_setting_t;

#define UI_AIRPLAY_1 (1u << 0)
#define UI_AIRPLAY_2 (1u << 1)

#define UI_SLEEP_MAX_S 3600

// Called on the LVGL task when the user changes a setting; must not block.
// The handler applies and persists it, then pushes ui_update_settings().
void ui_set_settings_handler(void (*handler)(ui_setting_t setting, int value, const char *zone));
