#pragma once

// The app launcher: a ring of app icons around the bezel, drawn on
// lv_layer_top() over whatever screen is showing, so opening it reloads
// nothing. The knob walks the ring (the physical ring and the drawn one
// turn together), a press or a tap opens the app.

#include <stdbool.h>
#include <stddef.h>

#include "ui_internal.h"

// The ring's first entry, at the top, is the audio: what's playing, or the
// source picker with nothing connected. The apps follow it, clockwise.
#define LAUNCHER_AUDIO (-1)

typedef struct {
    // An entry was picked (LAUNCHER_AUDIO or a ui_app_t), with the launcher
    // already hidden.
    void (*open)(int entry);
    // A tap off the icons, with the launcher already hidden.
    void (*dismissed)(void);
    // Whenever the launcher shows or hides.
    void (*visibility_changed)(void);
    // The audio entry's line under its name; false for none.
    bool (*audio_status)(char *buf, size_t len);
} launcher_handlers_t;

void launcher_build(const launcher_handlers_t *handlers);
// Opens with `entry` (LAUNCHER_AUDIO or a ui_app_t) selected.
void launcher_show(int entry);
void launcher_hide(void);
bool launcher_visible(void);
void launcher_rotate(int steps);
void launcher_activate(void);
