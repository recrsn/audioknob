#pragma once

#include "lvgl.h"

// Alpha-only (LV_COLOR_FORMAT_A8) glyphs converted from pi-audio-player's
// lcd/assets/icons -- tint with lv_obj_set_style_image_recolor()/_recolor_opa()
// at draw time rather than baking in a color, so one asset works on every tile.
extern const lv_image_dsc_t icon_airplay;
extern const lv_image_dsc_t icon_bluetooth;
extern const lv_image_dsc_t icon_music_note;
extern const lv_image_dsc_t icon_radio;
