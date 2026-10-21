#include "ui/apps/countdown/countdown.h"

#include <stdio.h>

#include "ui/apps/tools.h"

// Haptics may be missing, so a ringing countdown doesn't pulse forever.
#define ALERT_MS 60000
#define PULSE_MS 500

static void opa_exec(void *obj, int32_t v) {
    lv_obj_set_style_opa(obj, (lv_opa_t)v, 0);
}

static void pulse_start(lv_obj_t *obj) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, opa_exec);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_20);
    lv_anim_set_duration(&a, PULSE_MS);
    lv_anim_set_playback_duration(&a, PULSE_MS);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
}

static void pulse_stop(lv_obj_t *obj) {
    lv_anim_delete(obj, opa_exec);
    lv_obj_set_style_opa(obj, LV_OPA_COVER, 0);
}

uint32_t countdown_remaining_ms(const countdown_t *cd) {
    if (cd->state != COUNTDOWN_RUNNING) {
        return cd->remaining_ms;
    }
    int32_t left = (int32_t)(cd->end_tick - lv_tick_get());
    return left > 0 ? (uint32_t)left : 0;
}

const char *countdown_play_symbol(const countdown_t *cd) {
    switch (cd->state) {
    case COUNTDOWN_RUNNING:
        return LV_SYMBOL_PAUSE;
    case COUNTDOWN_ALERT:
        return LV_SYMBOL_STOP;
    default:
        return LV_SYMBOL_PLAY;
    }
}

// Rounds up, so a countdown reads 00:00 only once it's done.
void countdown_format(uint32_t ms, char *buf, size_t len) {
    uint32_t secs = (ms + 999) / 1000;
    snprintf(buf, len, "%02lu:%02lu", (unsigned long)(secs / 60), (unsigned long)(secs % 60));
}

void countdown_refresh(countdown_t *cd) {
    uint32_t ms = countdown_remaining_ms(cd);
    char buf[16];
    countdown_format(ms, buf, sizeof(buf));
    ui_label_set_text(cd->time_label, buf);

    if (cd->state != COUNTDOWN_ALERT) {
        tool_ring_set(cd->ring,
                      cd->total_ms ? (int32_t)((uint64_t)ms * COUNTDOWN_RING_RANGE / cd->total_ms) : 0);
    }
    if (cd->changed) {
        cd->changed(cd);
    }
}

void countdown_set_total(countdown_t *cd, uint32_t ms) {
    cd->total_ms = ms;
    if (cd->state == COUNTDOWN_IDLE) {
        cd->remaining_ms = ms;
    }
    countdown_refresh(cd);
}

void countdown_reset(countdown_t *cd) {
    bool was_ringing = cd->state == COUNTDOWN_ALERT;
    cd->state = COUNTDOWN_IDLE;
    cd->remaining_ms = cd->total_ms;
    if (was_ringing) {
        pulse_stop(cd->ring);
        pulse_stop(cd->time_label);
        tool_alert(cd->screen, UI_ALERT_NONE);
        if (cd->dismissed) {
            cd->dismissed(cd);
        }
    }
    countdown_refresh(cd);
}

void countdown_toggle(countdown_t *cd) {
    switch (cd->state) {
    case COUNTDOWN_IDLE:
        if (cd->total_ms == 0) {
            return;
        }
        // fallthrough
    case COUNTDOWN_PAUSED:
        cd->end_tick = lv_tick_get() + cd->remaining_ms;
        cd->state = COUNTDOWN_RUNNING;
        break;
    case COUNTDOWN_RUNNING:
        cd->remaining_ms = countdown_remaining_ms(cd);
        cd->state = COUNTDOWN_PAUSED;
        break;
    case COUNTDOWN_ALERT:
        countdown_reset(cd);
        return;
    }
    countdown_refresh(cd);
}

static void countdown_expire(countdown_t *cd) {
    cd->state = COUNTDOWN_ALERT;
    cd->remaining_ms = 0;
    cd->alert_tick = lv_tick_get();
    if (cd->expired) {
        cd->expired(cd);
    }
    tool_ring_set(cd->ring, COUNTDOWN_RING_RANGE);
    countdown_refresh(cd);
    pulse_start(cd->ring);
    pulse_start(cd->time_label);
    tool_alert(cd->screen, cd->alert);
}

void countdown_tick(countdown_t *cd) {
    if (cd->state == COUNTDOWN_RUNNING) {
        if (countdown_remaining_ms(cd) == 0) {
            countdown_expire(cd);
        } else {
            countdown_refresh(cd);
        }
    } else if (cd->state == COUNTDOWN_ALERT && lv_tick_elaps(cd->alert_tick) >= ALERT_MS) {
        countdown_reset(cd);
    }
}
