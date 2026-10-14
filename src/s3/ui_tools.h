#pragma once

// Private to the app pages: ui_tools.c (shared widgets, the countdown
// engine, page registry) and one file per page (ui_weather.c, ui_lights.c,
// ui_timer.c, ui_tomato.c, ui_stopwatch.c, ui_settings.c). ui.c only sees
// ui_tools_*() in ui_internal.h.

#include <stddef.h>

#include "fonts.h"
#include "ui_internal.h"

#define TOOL_TITLE_Y         (-96)
#define TOOL_HINT_Y          44
#define COUNTDOWN_RING_RANGE 1000

lv_obj_t *tool_ring_create(lv_obj_t *parent, lv_color_t tint, int32_t range);
// A thick closed ring from 12 o'clock over a ticked face, for the time
// pages; returns the ring, driven with tool_ring_set() like the open one.
lv_obj_t *tool_dial_create(lv_obj_t *parent, lv_color_t tint, int32_t range);
void tool_ring_set(lv_obj_t *ring, int32_t value);
lv_obj_t *tool_title_create(lv_obj_t *parent, const char *text, lv_color_t tint);
lv_obj_t *tool_time_label_create(lv_obj_t *parent);
lv_obj_t *tool_hint_label_create(lv_obj_t *parent);

// A row holding a secondary text button and a tinted primary play/pause
// button; each create returns the button's label.
lv_obj_t *tool_button_row_create(lv_obj_t *parent);
lv_obj_t *tool_button_create(lv_obj_t *row, const char *text, lv_event_cb_t cb);
lv_obj_t *tool_primary_button_create(lv_obj_t *row, lv_color_t tint, lv_event_cb_t cb);
void tool_button_set_enabled(lv_obj_t *label, bool enabled);

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

void weather_page_build(lv_obj_t *screen);
void lights_page_build(lv_obj_t *screen);
void timer_page_build(lv_obj_t *screen);
void tomato_page_build(lv_obj_t *screen);
void stopwatch_page_build(lv_obj_t *screen);
void settings_page_build(lv_obj_t *screen);
