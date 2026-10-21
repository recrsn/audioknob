#pragma once

#include <stdint.h>

// The colour a cover art reads as: its most populous saturated hue, ignoring
// near-black, near-white and grey, with lightness and saturation clamped so
// white text stays readable over a wash of it on UI_COLOR_BG and it holds up
// as an accent. pixels is w x h RGB565 (host byte order). Returns 0xRRGGBB,
// or 0 when the art has too little colour to pick one (the caller falls back
// to the source's tint). Plain C, no LVGL: the device computes it once per art
// version and hands it to the UI as ui_track_info_t.art_tint.
uint32_t art_color_dominant(const uint16_t *pixels, int w, int h);
