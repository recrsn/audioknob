#pragma once

// The clock face's bottom complication, closing its column: what's playing
// or paused, or with nothing connected, that the device is ready to. Its
// button plays/pauses without leaving the clock.

#include "lvgl.h"

void np_card_build(lv_obj_t *col);
// `target` is where the audio chip leads (np_audio_screen()).
void np_card_refresh(lv_obj_t *target);
