#pragma once

// The UI's design tokens: every screen takes its colors, type, spacing,
// radii and control sizes from here rather than from literals, so a role
// (dim text, a button's fill, a source's tint) looks the same everywhere.
// LVGL-only, like the rest of the UI, so env:native builds it unchanged.
//
// Quiet by default: one dark background, white text in three strengths,
// color only where it means something (a source, an app, the accent for
// "on"/"editing", the danger red for what can't be undone).

#include "fonts.h"
#include "lvgl.h"

// -- color (RGB hex, for lv_color_hex() and the uint32_t tint fields) --

#define UI_COLOR_BG         0x0b0b10 // every screen and full-screen overlay
#define UI_COLOR_SURFACE    0x1c1c24 // button and icon-chip fills
#define UI_COLOR_TRACK      0x2a2a30 // a dial's unfilled track
#define UI_COLOR_CONTROL    0x3a3a46 // a switch's off track, a level bar's track
#define UI_COLOR_TEXT       0xffffff
#define UI_COLOR_TEXT_DIM   0x9a9aa4 // secondary text, values, overlines, quarter ticks
#define UI_COLOR_TEXT_FAINT 0x5a5a64 // what's off or inert, minor ticks
#define UI_COLOR_ACCENT     0x4ea1ff // on, selected for editing
#define UI_COLOR_DANGER     0xff7a7a // can't be undone; muted

// Sources. tools/gen_backgrounds.py bakes UI_COLOR_BG and these into the
// now-playing backgrounds and wallpaper; rerun it if they change.
#define UI_TINT_AIRPLAY   0x4ea1ff
#define UI_TINT_BLUETOOTH 0x7c6cff
#define UI_TINT_USB       0x2dd4bf
#define UI_TINT_RADIO     0xf472b6

// Apps: each page's title, ring, primary button and launcher icon.
#define UI_TINT_TALK         0x2dd4bf
#define UI_TINT_LIGHTS       0xffc35a
#define UI_TINT_TIMER        0xff9f43
#define UI_TINT_TOMATO       0xff6347 // focus
#define UI_TINT_TOMATO_BREAK 0x5ad17a
#define UI_TINT_STOPWATCH    0x2dd4a0
#define UI_TINT_WEATHER      0xffc35a // also every weather glyph
#define UI_TINT_AMBIENCE     0xc49bff
#define UI_TINT_SETTINGS     UI_COLOR_TEXT_DIM

// White over the wallpaper or a tinted wash reads best as an opacity, which
// takes on what's behind it, rather than as a fixed grey.
#define UI_OPA_TEXT_SOFT  LV_OPA_80 // a second line that still matters
#define UI_OPA_TEXT_MUTED LV_OPA_60 // details, paused, older entries
#define UI_OPA_DISABLED   LV_OPA_40 // a control that can't act

// White fills, glass-like over whatever is behind.
#define UI_OPA_GLASS    20 // chips, cards, unselected list items
#define UI_OPA_SELECTED 26 // the selected row or item
#define UI_OPA_EDITING  56 // an accent-filled row being edited
#define UI_OPA_TINTED   LV_OPA_30 // a tint as a fill (the launcher's selection)
#define UI_OPA_DOT_OFF  LV_OPA_30 // a dot indicator's inactive dots

// -- type: Inter for text, Montserrat for LV_SYMBOL_* glyphs --

#define UI_FONT_CLOCK      (&inter_bold_52) // the clock face; digits only
#define UI_FONT_READING    (&inter_tnum_52) // big tabular numbers: times, levels, codes
#define UI_FONT_READING_SM (&inter_tnum_26)
#define UI_FONT_TITLE      (&inter_semi_16) // names, track titles, a screen's headline
#define UI_FONT_LABEL      (&inter_semi_13) // chips, buttons, short emphasized values
#define UI_FONT_BODY       (&inter_med_13)  // secondary lines, hints, values
#define UI_FONT_CAPTION    (&inter_med_12)  // the quietest details
#define UI_FONT_OVERLINE   (&inter_semi_11) // UPPERCASE page titles and section headers
#define UI_OVERLINE_SPACING 1               // its letter spacing

#define UI_FONT_ICON    (&icons_48)
#define UI_FONT_ICON_SM (&icons_20)
#define UI_FONT_SYMBOL_SM (&lv_font_montserrat_14) // inside a 32 px button, beside a chip label
#define UI_FONT_SYMBOL    (&lv_font_montserrat_16) // secondary transport buttons
#define UI_FONT_SYMBOL_LG (&lv_font_montserrat_22) // primary buttons, steppers, prompt icons
#define UI_FONT_SYMBOL_XL (&lv_font_montserrat_32)

// -- spacing (4 px grid, plus a hairline) --

#define UI_SPACE_XXS 2  // stacked lines
#define UI_SPACE_XS  4  // icon to its text in a chip, tight stacks
#define UI_SPACE_S   8  // inside a group: a card's padding, columns of a strip
#define UI_SPACE_M   12 // between siblings: chips, a row's name and value
#define UI_SPACE_L   16 // a row's side padding, between buttons
#define UI_SPACE_XL  20

// -- radii --

#define UI_RADIUS_PILL     LV_RADIUS_CIRCLE // buttons, chips, cards, rows
#define UI_RADIUS_CARD     20               // a block too tall to read as a pill
#define UI_RADIUS_HAIRLINE 2                // 4 px bars

// -- touch: the panel is ~46 mm across, about 8 px/mm --

// Smallest hit area, about 7 mm, a fingertip: whatever draws smaller pads
// its hit area out to it (lv_obj_set_ext_click_area()).
#define UI_TOUCH_MIN 56
#define UI_TOUCH_EXT 12 // the usual pad for a small control

#define UI_BUTTON_SIZE    60 // a round button that acts
#define UI_BUTTON_SIZE_SM 48 // a secondary one beside it (padded to UI_TOUCH_MIN)
#define UI_BUTTON_SIZE_XS 32 // inside a card (padded to UI_TOUCH_MIN)
_Static_assert(UI_BUTTON_SIZE_XS + 2 * UI_TOUCH_EXT >= UI_TOUCH_MIN, "too small to touch");

// -- the round panel --

#define UI_TEXT_W    240 // a centered line that stays on the glass mid-screen
#define UI_RING_SIZE 344 // a ring just inside the edge: volume, a page's progress
#define UI_RING_W    6

// The chip safe area. Chips -- the audio chip and a now-playing screen's
// source pill at the top, the clock's app chips at the bottom -- sit in a
// band UI_CHIP_INSET in from the round edge, where the panel is still ~190 px
// wide; they're capped to UI_CHIP_MAX_W so their corners stay on the glass.
// Page content starts at UI_SAFE_TOP so a chip never lands on it.
#define UI_CHIP_H     28
#define UI_CHIP_INSET 28
#define UI_CHIP_MAX_W 176
#define UI_CHIP_PAD_X UI_SPACE_M
#define UI_SAFE_TOP   (UI_CHIP_INSET + UI_CHIP_H + 6)
// A label's LV_ALIGN_CENTER y offset that puts its top at UI_SAFE_TOP.
#define UI_SAFE_TOP_Y(line_h) (UI_SAFE_TOP + (line_h) / 2 - 180)

// A page's overline title: over a dial, or as high as the safe area allows
// for pages that need the room (lists).
#define UI_TITLE_Y     (-96)
#define UI_TITLE_TOP_Y UI_SAFE_TOP_Y(14)
