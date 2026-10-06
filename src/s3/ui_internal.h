#pragma once

// Shared between ui.c (screens, navigation), ui_tools.c (timer and
// stopwatch content) and ui_wallpaper.c. Not part of the public UI API in ui.h.

#include <stdbool.h>

#include "lvgl.h"

void ui_label_set_text(lv_obj_t *label, const char *text);
void ui_set_visible(lv_obj_t *obj, bool visible);

// A circular, non-clickable chip with a centered label; returns the label.
// Callers make the chip (the label's parent) clickable if it should be.
lv_obj_t *ui_round_button_create(lv_obj_t *parent, const char *symbol, int32_t size,
                                 const lv_font_t *font, bool dim);

// The idle screen's pre-dithered ambient glow, covering the whole display.
lv_obj_t *ui_wallpaper_create(lv_obj_t *parent);

// Fills both screens, which ui.c owns along with their navigation chrome.
// on_alert(true) runs when the countdown reaches zero, so ui.c can bring the
// timer screen forward from wherever the user is; on_alert(false) when the
// alert is dismissed.
void ui_tools_build(lv_obj_t *timer_screen, lv_obj_t *stopwatch_screen, void (*on_alert)(bool ringing));

bool ui_timer_alerting(void);
void ui_timer_dismiss_alert(void);

// Return false when the knob isn't used in the tool's current state.
bool ui_timer_knob_rotate(int steps);
void ui_timer_knob_press(void);
void ui_stopwatch_knob_press(void);
