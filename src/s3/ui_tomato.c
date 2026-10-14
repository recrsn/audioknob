#include "ui_tools.h"

// Pomodoro: focus, short break, repeat; every TOMATO_SETS-th focus earns a long
// break instead. Each phase rings at its end and waits for a press to start
// the next one, so a break never begins while you're still away.

#define TOMATO_FOCUS_TINT 0xff6347
#define TOMATO_BREAK_TINT 0x5ad17a
#define TOMATO_SETS       4
#define TOMATO_MAX_MIN    90
#define TOMATO_DOT        8

typedef enum {
    PHASE_FOCUS,
    PHASE_SHORT_BREAK,
    PHASE_LONG_BREAK,
    PHASE_COUNT,
} phase_t;

static const struct {
    const char *name;
    const char *done; // shown while its end rings
    uint32_t default_min;
} k_phases[PHASE_COUNT] = {
    [PHASE_FOCUS] = {"Focus", "Focus done", 25},
    [PHASE_SHORT_BREAK] = {"Short break", "Break over", 5},
    [PHASE_LONG_BREAK] = {"Long break", "Break over", 15},
};

static struct {
    countdown_t cd;
    lv_obj_t *dots[TOMATO_SETS];
    lv_obj_t *phase_label;
    lv_obj_t *start_icon;
    lv_obj_t *side_icon; // Skip while idle, Reset otherwise
    phase_t phase;
    uint32_t minutes[PHASE_COUNT];
    uint32_t sessions; // focus sessions completed in this set
} tomato;

static lv_color_t phase_tint(void) {
    return lv_color_hex(tomato.phase == PHASE_FOCUS ? TOMATO_FOCUS_TINT : TOMATO_BREAK_TINT);
}

// Restyling is a full redraw, so it runs on phase/session changes only, not
// from the per-tick changed() hook.
static void restyle(void) {
    lv_color_t tint = phase_tint();
    lv_obj_set_style_arc_color(tomato.cd.ring, tint, LV_PART_INDICATOR);
    lv_obj_set_style_text_color(tomato.phase_label, tint, 0);
    lv_obj_set_style_bg_color(lv_obj_get_parent(tomato.start_icon), tint, 0);
    for (uint32_t i = 0; i < TOMATO_SETS; i++) {
        lv_obj_set_style_bg_color(tomato.dots[i], lv_color_hex(TOMATO_FOCUS_TINT), 0);
        lv_obj_set_style_bg_opa(tomato.dots[i], i < tomato.sessions ? LV_OPA_COVER : LV_OPA_20, 0);
    }
}

static void set_phase(phase_t phase) {
    tomato.phase = phase;
    restyle();
    countdown_set_total(&tomato.cd, tomato.minutes[phase] * 60 * 1000);
}

static phase_t next_phase(void) {
    if (tomato.phase != PHASE_FOCUS) {
        return PHASE_FOCUS;
    }
    return tomato.sessions == TOMATO_SETS ? PHASE_LONG_BREAK : PHASE_SHORT_BREAK;
}

static void advance(void) {
    phase_t next = next_phase();
    if (tomato.phase == PHASE_LONG_BREAK) {
        tomato.sessions = 0;
    }
    set_phase(next);
}

static void tomato_changed(countdown_t *cd) {
    const char *text = k_phases[tomato.phase].name;
    char buf[32];
    if (cd->state == COUNTDOWN_ALERT) {
        text = k_phases[tomato.phase].done;
    } else if (cd->state == COUNTDOWN_PAUSED) {
        lv_snprintf(buf, sizeof(buf), "%s \xC2\xB7 Paused", text);
        text = buf;
    }
    ui_label_set_text(tomato.phase_label, text);
    ui_label_set_text(tomato.start_icon, countdown_play_symbol(cd));
    ui_label_set_text(tomato.side_icon, cd->state == COUNTDOWN_IDLE ? "Skip" : "Reset");
}

// Only a phase that ran out counts toward the long break; skipped ones don't.
static void tomato_expired(countdown_t *cd) {
    (void)cd;
    if (tomato.phase == PHASE_FOCUS && tomato.sessions < TOMATO_SETS) {
        tomato.sessions++;
        restyle();
    }
}

static void tomato_dismissed(countdown_t *cd) {
    (void)cd;
    advance();
}

// Whole minutes for the current phase, remembered per phase.
static bool tomato_rotate(int steps) {
    if (tomato.cd.state != COUNTDOWN_IDLE) {
        return false;
    }
    int32_t min = (int32_t)tomato.minutes[tomato.phase] + steps;
    tomato.minutes[tomato.phase] = min < 1 ? 1 : min > TOMATO_MAX_MIN ? TOMATO_MAX_MIN : (uint32_t)min;
    countdown_set_total(&tomato.cd, tomato.minutes[tomato.phase] * 60 * 1000);
    return true;
}

static void tomato_press(void) {
    countdown_toggle(&tomato.cd);
}

static void toggle_cb(lv_event_t *e) {
    (void)e;
    countdown_toggle(&tomato.cd);
}

static void side_cb(lv_event_t *e) {
    (void)e;
    if (tomato.cd.state == COUNTDOWN_IDLE) {
        advance();
    } else {
        countdown_reset(&tomato.cd);
    }
}

static void dots_create(lv_obj_t *screen) {
    lv_obj_t *row = lv_obj_create(screen);
    lv_obj_remove_style_all(row);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, TOMATO_DOT, 0);
    lv_obj_align(row, LV_ALIGN_CENTER, 0, TOOL_TITLE_Y);
    for (int i = 0; i < TOMATO_SETS; i++) {
        lv_obj_t *dot = lv_obj_create(row);
        lv_obj_remove_style_all(dot);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(dot, TOMATO_DOT, TOMATO_DOT);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        tomato.dots[i] = dot;
    }
}

void tomato_page_build(lv_obj_t *screen) {
    lv_color_t tint = lv_color_hex(TOMATO_FOCUS_TINT);
    tool_page_register(screen, &(tool_page_desc_t){.name = "Pomodoro",
                                                   .icon = ICON_NUTRITION,
                                                   .tint = TOMATO_FOCUS_TINT,
                                                   .cd = &tomato.cd,
                                                   .rotate = tomato_rotate,
                                                   .press = tomato_press});
    for (int i = 0; i < PHASE_COUNT; i++) {
        tomato.minutes[i] = k_phases[i].default_min;
    }

    tomato.cd.ring = tool_dial_create(screen, tint, COUNTDOWN_RING_RANGE);
    dots_create(screen);
    tomato.cd.time_label = tool_time_label_create(screen);
    lv_obj_align(tomato.cd.time_label, LV_ALIGN_CENTER, 0, 0);
    tomato.phase_label = tool_hint_label_create(screen);
    lv_obj_set_style_text_font(tomato.phase_label, &inter_semi_16, 0);

    lv_obj_t *row = tool_button_row_create(screen);
    tomato.side_icon = tool_button_create(row, "Skip", side_cb);
    tomato.start_icon = tool_primary_button_create(row, tint, toggle_cb);

    tomato.cd.changed = tomato_changed;
    tomato.cd.expired = tomato_expired;
    tomato.cd.dismissed = tomato_dismissed;
    set_phase(PHASE_FOCUS);
}
