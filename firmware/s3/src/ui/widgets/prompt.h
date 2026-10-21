#pragma once

// A full-screen prompt over every screen, on lv_layer_top(): an icon, a
// title, an optional big code, a message, and Cancel beside a round accept
// button. The pairing prompt and every confirmation are built on it, so all
// of them read and answer the same way: tap a button, or press the knob to
// accept and long-press it to back out.

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

typedef struct {
    lv_obj_t *scrim;
    lv_obj_t *image;
    lv_obj_t *symbol;
    lv_obj_t *title;
    lv_obj_t *code;
    lv_obj_t *body;
    lv_obj_t *buttons;
    lv_obj_t *cancel;
    lv_obj_t *accept;
} ui_prompt_t;

typedef struct {
    const lv_image_dsc_t *image; // the icon, else
    const char *symbol;          // a Montserrat LV_SYMBOL_*; neither for none
    uint32_t tint;               // the icon and the accept button
    const char *title;
    const char *code;            // in big tabular figures; NULL for none
    const char *body;
    bool cancel;                 // which buttons show
    bool accept;
} ui_prompt_content_t;

// Hidden until shown. on_scrim runs for a tap off the buttons; any
// callback may be NULL.
void ui_prompt_build(ui_prompt_t *p, void (*on_cancel)(void), void (*on_accept)(void), void (*on_scrim)(void));
void ui_prompt_render(ui_prompt_t *p, const ui_prompt_content_t *content);
bool ui_prompt_visible(const ui_prompt_t *p);
void ui_prompt_show(ui_prompt_t *p, bool show);
