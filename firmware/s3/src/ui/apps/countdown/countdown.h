#pragma once

// The ringing countdown the timer and the tomato share; the page registry in
// ui/apps/tools.c ticks it and reports its alerts.

#include <stddef.h>

#include "ui/internal.h"

#define COUNTDOWN_RING_RANGE 1000

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
// Every refresh period, from the page registry.
void countdown_tick(countdown_t *cd);
// mm:ss, rounded up so a countdown reads 00:00 only once it's done.
void countdown_format(uint32_t ms, char *buf, size_t len);
