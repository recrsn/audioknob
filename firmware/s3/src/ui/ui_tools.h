#pragma once

// Private to the app pages: ui_tools.c (shared widgets, the countdown
// engine, page registry) and one file per page (ui_radio.c, ui_weather.c, ui_lights.c,
// ui_timer.c, ui_tomato.c, ui_ambience.c, ui_stopwatch.c, ui_settings.c). ui.c only sees
// ui_tools_*() in ui_internal.h.

#include <stddef.h>

#include "fonts.h"
#include "ui_internal.h"

#define TOOL_HINT_Y          44
#define COUNTDOWN_RING_RANGE 1000

lv_obj_t *tool_ring_create(lv_obj_t *parent, lv_color_t tint, int32_t range);
// A thick closed ring from 12 o'clock over a ticked face, for the time
// pages; returns the ring, driven with tool_ring_set() like the open one.
lv_obj_t *tool_dial_create(lv_obj_t *parent, lv_color_t tint, int32_t range);
void tool_ring_set(lv_obj_t *ring, int32_t value);
// The value of `ring`'s range at `deg` (lv_arc degrees: 0 at 3 o'clock,
// clockwise); a point in an open ring's gap takes the nearer end.
int32_t tool_ring_value_at(lv_obj_t *ring, int32_t deg);

// Touch on the outer band of a page, where its ring is: cb gets the finger's
// angle in lv_arc degrees on the press (`first`) and as it moves. Create it
// before the page's buttons so they stay on top of it.
typedef void (*tool_ring_touch_cb_t)(int32_t deg, bool first, void *user);
void tool_ring_touch_create(lv_obj_t *parent, tool_ring_touch_cb_t cb, void *user);
// A finger circling the ring turns the knob: rotate() gets a step per
// TOOL_JOG_DEG travelled, clockwise positive.
void tool_ring_jog_create(lv_obj_t *parent, bool (*rotate)(int steps));
// `text` must outlive the title: tool_title_flash() goes back to it.
lv_obj_t *tool_title_create(lv_obj_t *parent, const char *text, lv_color_t tint);
// Shows `text` (a value the knob just changed) in place of the title for a
// moment.
void tool_title_flash(lv_obj_t *title, const char *text);
lv_obj_t *tool_time_label_create(lv_obj_t *parent);
lv_obj_t *tool_hint_label_create(lv_obj_t *parent);

// A row holding a secondary text button and a tinted primary play/pause
// button; each create returns the button's label.
lv_obj_t *tool_button_row_create(lv_obj_t *parent);
lv_obj_t *tool_button_create(lv_obj_t *row, const char *text, lv_event_cb_t cb);
lv_obj_t *tool_primary_button_create(lv_obj_t *row, lv_color_t tint, lv_event_cb_t cb);
void tool_button_set_enabled(lv_obj_t *label, bool enabled);
// A small round button for a page's secondary actions (back, delete);
// returns the button, its glyph is its only child. `cb` (on click) may be
// NULL for a button that handles its own events.
lv_obj_t *tool_icon_button_create(lv_obj_t *parent, const char *symbol, lv_event_cb_t cb);

// Settings-style lists (Radio, Talk, Inbox, Ambience): a column of pill rows
// under the title. Rows are the list's only children, in order.
#define TOOL_LIST_W   272
#define TOOL_LIST_TOP (UI_SAFE_TOP + 22)
lv_obj_t *tool_list_create(lv_obj_t *parent, int32_t height);
// A row with a name over a detail line (hidden until needed); trailing glyphs
// are added to the row after it.
lv_obj_t *tool_list_row_create(lv_obj_t *list, int32_t height, lv_obj_t **name, lv_obj_t **detail);
// Rows fade with their distance from the selected one, as the knob moves it.
void tool_list_fade(lv_obj_t *list, int selected, int count);
// Scrolls `index` toward the middle, as far as the ends allow.
void tool_list_center(lv_obj_t *list, int index, lv_anim_enable_t anim);
// After a drag: `selected` if it's still in view, else the row nearest the
// middle, so the knob carries on from what's on screen.
int tool_list_in_view(lv_obj_t *list, int selected, int count);

typedef enum {
    COUNTDOWN_IDLE,
    COUNTDOWN_RUNNING,
    COUNTDOWN_PAUSED,
    COUNTDOWN_ALERT,
} countdown_state_t;

// A countdown that rings when it reaches zero. Time is kept as an lv_tick
// deadline rather than counted refreshes, so a stalled LVGL task delays the
// redraw but never the time itself.
typedef struct countdown countdown_t;
struct countdown {
    lv_obj_t *screen;
    lv_obj_t *ring;
    lv_obj_t *time_label;
    countdown_state_t state;
    uint32_t total_ms;     // what a reset returns to; the ring's full scale
    uint32_t remaining_ms; // authoritative unless running
    uint32_t end_tick;     // authoritative while running
    uint32_t alert_tick;
    ui_alert_t alert; // what its ringing reports
    // Page hooks. changed() runs after every refresh (every tick while
    // running), so it must be cheap and idempotent. expired()/dismissed()
    // are optional and bracket the alert.
    void (*changed)(countdown_t *cd);
    void (*expired)(countdown_t *cd);
    void (*dismissed)(countdown_t *cd);
};

uint32_t countdown_remaining_ms(const countdown_t *cd);
// Only takes effect on the remaining time while idle.
void countdown_set_total(countdown_t *cd, uint32_t ms);
// Idle/paused -> running -> paused; a ringing countdown is dismissed.
void countdown_toggle(countdown_t *cd);
// Back to idle at total_ms, dismissing a ringing alert.
void countdown_reset(countdown_t *cd);
void countdown_refresh(countdown_t *cd);
const char *countdown_play_symbol(const countdown_t *cd);

typedef struct {
    const char *name; // as the launcher shows it
    const char *icon; // an icons_48/icons_20 glyph
    uint32_t tint;
    // Optional: gets ticked, rings, is dismissed by any tap on the screen,
    // and reports its remaining time as the page's status.
    countdown_t *cd;
    bool (*rotate)(int steps);
    void (*press)(void);
    void (*tick)(void); // optional, every refresh period
    // Optional, for progress that isn't a countdown (a stopwatch): writes it
    // and returns true, or returns false when there's none.
    bool (*status)(char *buf, size_t len);
} tool_page_desc_t;

void tool_page_register(lv_obj_t *screen, const tool_page_desc_t *desc);

void inbox_page_build(lv_obj_t *screen);
void talk_page_build(lv_obj_t *screen);
void radio_page_build(lv_obj_t *screen);
void weather_page_build(lv_obj_t *screen);
void lights_page_build(lv_obj_t *screen);
void timer_page_build(lv_obj_t *screen);
void tomato_page_build(lv_obj_t *screen);
void ambience_page_build(lv_obj_t *screen);
void stopwatch_page_build(lv_obj_t *screen);
void settings_page_build(lv_obj_t *screen);
