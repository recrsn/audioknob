#pragma once

#include "lvgl.h"

// Inter (SIL OFL) rasterized with lv_font_conv at the sizes/weights the UI
// uses -- the mock's SF is not redistributable. Latin-1 plus common
// typographic punctuation; anything else falls back to Montserrat 14.
extern const lv_font_t inter_med_12;
extern const lv_font_t inter_med_13;
extern const lv_font_t inter_semi_11;
extern const lv_font_t inter_semi_13;
extern const lv_font_t inter_semi_14;
extern const lv_font_t inter_semi_16;
extern const lv_font_t inter_bold_52;  // digits, ':' and '-' only (clock)
