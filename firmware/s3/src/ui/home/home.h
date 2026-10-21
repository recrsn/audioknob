#pragma once

// The clock face, home unless WiFi setup is pending: date, time, weather,
// the now-playing card, the attention band and a chip per app with
// something in progress. A tap off them opens the launcher.

#include "lvgl.h"

// Space between the clock (date + time) and the complications above and
// below it, on top of the column's own spacing.
#define HOME_COMP_GAP UI_SPACE_M

void home_build(void);
lv_obj_t *home_screen(void);
// Polled: what's in progress changes every second, the weather every 15 min.
void home_refresh(void);
// The time as the clock shows it, for the now-playing pill; empty unsynced.
const char *home_time_text(void);
