#pragma once

// Which screen shows: home (the clock, or the WiFi setup QR while setup is
// pending), the app pages, and the source screens that surface on their
// own while something plays. Any real navigation closes the launcher.

#include "ui/internal.h"

// Creates the app pages and their chrome.
void nav_build(void);
// Shows `home` and starts the timer that sends an idle page home and
// refreshes whatever polls.
void nav_start(lv_obj_t *home);

lv_obj_t *nav_app_screen(ui_app_t app);
lv_obj_t *nav_home(void);
// Becomes home; shown at once if the old home was showing.
void nav_set_home(lv_obj_t *screen);

// Loads `target` unless it already shows.
void nav_show(lv_obj_t *target);
// Sends a playing source to the background.
void nav_go_home(void);
// For a home zone: a swipe up or a tap goes home.
void nav_swipe_home_cb(lv_event_t *e);
// Sends a playing source to the background, as a swipe home would.
void nav_open_app(ui_app_t app);
// The user asked for `screen`: it stays up even paused or waiting, until
// a swipe home or the next stream start.
void nav_open_pinned(lv_obj_t *screen);
// What's playing, or with nothing connected, the source picker.
void nav_open_audio(void);
void nav_open_launcher(void);
// Opens a launcher entry (LAUNCHER_AUDIO or a ui_app_t).
void nav_launcher_open(int entry);

typedef struct {
    bool was_playing;
    uint32_t stopped_at; // lv_tick when playback last stopped
} nav_surface_t;

// Shared by every source: surfaces `screen` while `playing`, falls back to
// home once it stops (a while later while still connected), and respects an
// in-progress swipe home. A fresh not-playing -> playing edge always
// un-homes, so a new stream is never hidden behind the clock.
void nav_surface(lv_obj_t *screen, bool playing, bool connected, nav_surface_t *st);
