#pragma once

// Shared between ui.c (screens, navigation), the app pages (ui_tools.c and
// the ui_<app>.c files) and ui_wallpaper.c. Not part of the public UI API.

#include <stdbool.h>
#include <stddef.h>

#include "lvgl.h"

// The chip safe area. Chips -- the audio chip and a now-playing screen's
// source pill at the top, the clock's app chips at the bottom -- sit in a
// band UI_CHIP_INSET in from the round edge, where the panel is still ~190 px
// wide; they're capped to UI_CHIP_MAX_W so their corners stay on the glass.
// Page content starts at UI_SAFE_TOP so a chip never lands on it.
#define UI_CHIP_H     28
#define UI_CHIP_INSET 28
#define UI_CHIP_MAX_W 176
#define UI_SAFE_TOP   (UI_CHIP_INSET + UI_CHIP_H + 6)
// A label's LV_ALIGN_CENTER y offset that puts its top at UI_SAFE_TOP.
#define UI_SAFE_TOP_Y(line_h) (UI_SAFE_TOP + (line_h) / 2 - 180)

void ui_label_set_text(lv_obj_t *label, const char *text);
void ui_set_visible(lv_obj_t *obj, bool visible);

// A circular, non-clickable chip with a centered label; returns the label.
// Callers make the chip (the label's parent) clickable if it should be.
lv_obj_t *ui_round_button_create(lv_obj_t *parent, const char *symbol, int32_t size,
                                 const lv_font_t *font, bool dim);

// The idle screen's pre-dithered ambient glow, covering the whole display.
lv_obj_t *ui_wallpaper_create(lv_obj_t *parent);

typedef enum {
    UI_BG_AIRPLAY,
    UI_BG_BLUETOOTH,
    UI_BG_USB,
} ui_bg_source_t;

// A now-playing screen's full-screen background: the source-tinted wash with
// its art tile baked in, and the corners that round cover art over the tile.
typedef struct {
    lv_obj_t *img;
    lv_image_dsc_t dsc;
    lv_obj_t *corners[4];
    ui_bg_source_t source;
} ui_now_playing_bg_t;

void ui_now_playing_bg_create(ui_now_playing_bg_t *bg, lv_obj_t *parent, ui_bg_source_t source);
// An empty, hidden image over the tile (tile_y from screen center) for
// UI_BG_TILE_SIZE square cover art, which the background's corners round.
lv_obj_t *ui_now_playing_art_create(ui_now_playing_bg_t *bg, lv_obj_t *parent, int32_t tile_y);
void ui_now_playing_bg_set(ui_now_playing_bg_t *bg, ui_bg_source_t source);

#include "ui.h"

// The app pages, in launcher order (clockwise from the top).
typedef enum {
    UI_APP_LIGHTS,
    UI_APP_TIMER,
    UI_APP_TOMATO,
    UI_APP_STOPWATCH,
    UI_APP_WEATHER,
    UI_APP_SETTINGS,
    UI_APP_COUNT,
} ui_app_t;

typedef struct {
    const char *name;
    const char *icon; // an icons_48/icons_20 glyph
    lv_color_t tint;
} ui_app_info_t;

// Fills the app pages, which ui.c owns along with their navigation chrome.
// on_alert(screen, true) runs when a countdown on `screen` (timer, tomato)
// reaches zero, so ui.c can bring it forward from wherever the user is;
// on_alert(screen, false) when that alert is dismissed.
void ui_tools_build(lv_obj_t *const screens[UI_APP_COUNT], void (*on_alert)(lv_obj_t *screen, bool ringing));

// The last settings pushed by ui_update_settings() (clock format, units).
const ui_settings_t *ui_settings_current(void);

// Counts as user activity and lights a sleeping display; true if it was
// asleep. For whatever must be seen at once (an alert, a pairing request).
bool ui_wake(void);

ui_app_info_t ui_tools_info(ui_app_t app);
// The app whose page `screen` is, or -1.
int ui_tools_app_of(lv_obj_t *screen);
// What's in progress on `app` -- a countdown's time left, a stopwatch's
// time -- for the clock's chips and the launcher; false when nothing is.
bool ui_tools_status(ui_app_t app, char *buf, size_t len);

// Current conditions for the clock face's weather complication, in the
// configured unit. False until weather has arrived.
typedef struct {
    const char *icon; // an icons_48/icons_20 glyph
    const char *sky;  // "Partly cloudy"
    int temp;
    int high;
    int low;
} ui_weather_glance_t;
bool ui_weather_glance(ui_weather_glance_t *out);

bool ui_tools_alerting(void);
void ui_tools_dismiss(lv_obj_t *screen);

// Knob input for an app page; false when that page (or its current state)
// has no use for it, or `screen` isn't a tool page.
bool ui_tools_knob_rotate(lv_obj_t *screen, int steps);
bool ui_tools_knob_press(lv_obj_t *screen);

// The Bluetooth pairing prompt (ui_pairing.c), over every screen.
bool ui_pairing_visible(void);
// Knob input while the prompt is up; false when it isn't.
bool ui_pairing_knob_press(void);
bool ui_pairing_knob_long_press(void);
// Sends an action to the handler from ui_set_pairing_handler().
void ui_pairing_request(ui_pairing_action_t action);
// The last status pushed, or NULL while Bluetooth isn't answering.
const pairing_status_t *ui_pairing_status(void);
// Re-renders the settings page's Bluetooth rows after ui_pairing_status()
// changes.
void ui_settings_refresh(void);
