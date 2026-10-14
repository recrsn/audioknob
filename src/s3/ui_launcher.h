#pragma once

// The app launcher: a ring of app icons around the bezel, drawn on
// lv_layer_top() over whatever screen is showing, so opening it reloads
// nothing. The knob walks the ring (the physical ring and the drawn one
// turn together), a press or a tap opens the app.

#include <stdbool.h>

#include "ui_internal.h"

// `open` runs when an app is picked and `dismissed` on a tap off the icons,
// both with the launcher already hidden; `visibility_changed` whenever the
// launcher shows or hides.
void launcher_build(void (*open)(ui_app_t app), void (*dismissed)(void), void (*visibility_changed)(void));
void launcher_show(ui_app_t selected);
void launcher_hide(void);
bool launcher_visible(void);
void launcher_rotate(int steps);
void launcher_activate(void);
