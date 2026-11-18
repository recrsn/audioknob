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
// Inter Bold with its tabular figures (tnum) baked into the cmap by
// tools/freeze_tnum.py, for running times and readings. Digits, ':', '.',
// '-' (both), plus '%' and degree sign (52 only).
extern const lv_font_t inter_tnum_52;
extern const lv_font_t inter_tnum_26;

// Google Material Symbols Rounded (Apache-2.0), filled, weight 500, at the
// matching optical size; only the glyphs below. Regenerate by instancing
// the variable font with fontTools.varLib.instancer, then lv_font_conv.
extern const lv_font_t icons_48;
extern const lv_font_t icons_20;

#define ICON_CLEAR_DAY    "\xEF\x85\x97"
#define ICON_CLEAR_NIGHT  "\xEE\x87\xB9"
#define ICON_PARTLY_DAY   "\xEF\x85\xB2"
#define ICON_PARTLY_NIGHT "\xEE\xA9\x86"
#define ICON_CLOUD        "\xEE\x8A\xBD"
#define ICON_FOG          "\xEE\xA0\x98"
#define ICON_DRIZZLE      "\xEF\x98\x9E"
#define ICON_RAIN         "\xEF\x85\xB6"
#define ICON_SNOW         "\xEE\x8B\x8D"
#define ICON_STORM        "\xEE\xAF\x9B"
#define ICON_BULB         "\xEE\x83\xB0"
#define ICON_HOURGLASS    "\xEE\xA9\x9B"
#define ICON_NUTRITION    "\xEE\x84\x90"
#define ICON_STOPWATCH    "\xEE\x90\xA5"
#define ICON_SETTINGS     "\xEE\xA2\xB8"
// icons_20 only: the weather complication and page's air quality and UV.
#define ICON_AIR          "\xEE\xBF\x98"
#define ICON_UV           "\xEE\x94\x98"
#define ICON_RADIO        "\xEE\x80\xBE"

// The Talk glyph lives in icons_talk_48/20, fallbacks of icons_48/20.
// Talk is a walkie-talkie; regenerate all three sizes with tools/gen_talk_icons.py.
#define ICON_TALK "\xEE\x80\xA9"
// Voice notes. icons_20 (through icons_talk_20): every one; icons_talk_24,
// for the thread's round buttons: mic, send and delete.
extern const lv_font_t icons_talk_24;
#define ICON_NOTE_MIC      "\xEE\x8C\x9D"
#define ICON_NOTE_SEND     "\xEE\x85\xA3"
#define ICON_NOTE_DELETE   "\xEE\xA1\xB2"
#define ICON_NOTE_SENT     "\xEE\xA1\xB6"
#define ICON_NOTE_PLAYED   "\xEE\xA1\xB7"
#define ICON_NOTE_SENDING  "\xEE\xA2\xB5"
#define ICON_NOTE_FAILED   "\xEE\x80\x80"
#define ICON_CALL_OUT      "\xEE\x82\xB2"
#define ICON_CALL_IN       "\xEE\x82\xB5"
#define ICON_CALL_MISSED   "\xEE\x82\xB4"
#define ICON_NOTE_PLAY     "\xEE\x80\xB7"
#define ICON_NOTE_STOP     "\xEE\x81\x87"

