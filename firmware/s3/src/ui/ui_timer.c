#include "ui_tools.h"

#define TIMER_DEFAULT_S (5 * 60)
#define TIMER_MAX_S     (99 * 60)

static struct {
    countdown_t cd;
    lv_obj_t *hint_label;
    lv_obj_t *start_icon;
    lv_obj_t *reset_icon;
} timer;

// Fine steps where they matter (short timers), coarse ones where turning
// through them would take forever. Decrements use the step just below the
// current value so up/down retrace the same stops.
static uint32_t step_s(uint32_t s) {
    return s < 60 ? 5 : s < 600 ? 30 : 60;
}

static void timer_changed(countdown_t *cd) {
    static const char *const hints[] = {
        [COUNTDOWN_IDLE] = "Press to start",
        [COUNTDOWN_RUNNING] = "Running",
        [COUNTDOWN_PAUSED] = "Paused",
        [COUNTDOWN_ALERT] = "Time's up",
    };
    ui_label_set_text(timer.hint_label,
                      cd->state == COUNTDOWN_IDLE && cd->total_ms == 0 ? "Turn or drag the dial" : hints[cd->state]);
    ui_label_set_text(timer.start_icon, countdown_play_symbol(cd));
    tool_button_set_enabled(timer.start_icon, cd->state != COUNTDOWN_IDLE || cd->total_ms > 0);
    tool_button_set_enabled(timer.reset_icon, cd->state != COUNTDOWN_IDLE);
}

static bool timer_rotate(int steps) {
    if (timer.cd.state != COUNTDOWN_IDLE) {
        return false;
    }
    uint32_t s = timer.cd.total_ms / 1000;
    for (; steps > 0 && s < TIMER_MAX_S; steps--) {
        s += step_s(s);
    }
    for (; steps < 0 && s > 0; steps++) {
        s -= step_s(s - 1);
    }
    countdown_set_total(&timer.cd, (s > TIMER_MAX_S ? TIMER_MAX_S : s) * 1000);
    return true;
}

static void timer_press(void) {
    countdown_toggle(&timer.cd);
}

static void toggle_cb(lv_event_t *e) {
    (void)e;
    countdown_toggle(&timer.cd);
}

static void reset_cb(lv_event_t *e) {
    (void)e;
    countdown_reset(&timer.cd);
}

void timer_page_build(lv_obj_t *screen) {
    lv_color_t tint = lv_color_hex(UI_TINT_TIMER);
    tool_page_register(screen, &(tool_page_desc_t){.name = "Timer",
                                                   .icon = ICON_HOURGLASS,
                                                   .tint = UI_TINT_TIMER,
                                                   .cd = &timer.cd,
                                                   .rotate = timer_rotate,
                                                   .press = timer_press});

    timer.cd.ring = tool_dial_create(screen, tint, COUNTDOWN_RING_RANGE);
    tool_ring_jog_create(screen, timer_rotate);
    tool_title_create(screen, "TIMER", tint);
    timer.cd.time_label = tool_time_label_create(screen);
    lv_obj_align(timer.cd.time_label, LV_ALIGN_CENTER, 0, 0);
    timer.hint_label = tool_hint_label_create(screen);

    lv_obj_t *row = tool_button_row_create(screen);
    timer.reset_icon = tool_button_create(row, "Reset", reset_cb);
    timer.start_icon = tool_primary_button_create(row, tint, toggle_cb);

    timer.cd.changed = timer_changed;
    countdown_set_total(&timer.cd, TIMER_DEFAULT_S * 1000);
}
