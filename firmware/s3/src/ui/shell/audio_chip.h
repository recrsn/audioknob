#pragma once

// The audio chip: a persistent "back to what's playing" pill at the top of
// every app page, on lv_layer_top() so one widget serves them all. Its
// refresh also redraws the clock's now-playing card.

#include "lvgl.h"

void audio_chip_build(void);
// Runs on every state push; each setter skips no-op changes.
void audio_chip_refresh(void);
// The screen a tap goes to, as of the last refresh; NULL for none.
lv_obj_t *audio_chip_target(void);
