#include "ui_tools.h"

// Pomodoro: focus, short break, repeat; every TOMATO_SETS-th focus earns a long
// break instead. Each phase rings at its end and waits for a press to start
// the next one, so a break never begins while you're still away. A focus
// sound picked here plays while a focus block runs and fades out with it.

#define TOMATO_SETS       4
#define TOMATO_MAX_MIN    90
#define TOMATO_DOT        8
#define TOMATO_SOUND_Y    (-58)

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
    lv_obj_t *sound_icon;
    lv_obj_t *sound_label;
    int sound_shown;  // the pick last drawn, -1 before any
    bool sound_wanted; // a focus block with a sound picked is running
    phase_t phase;
    uint32_t minutes[PHASE_COUNT];
    uint32_t sessions; // focus sessions completed in this set
} tomato;

static lv_color_t phase_tint(void) {
    return lv_color_hex(tomato.phase == PHASE_FOCUS ? UI_TINT_TOMATO : UI_TINT_TOMATO_BREAK);
}

// Restyling is a full redraw, so it runs on phase/session changes only, not
// from the per-tick changed() hook.
static void restyle(void) {
    lv_color_t tint = phase_tint();
    lv_obj_set_style_arc_color(tomato.cd.ring, tint, LV_PART_INDICATOR);
    lv_obj_set_style_text_color(tomato.phase_label, tint, 0);
    lv_obj_set_style_bg_color(lv_obj_get_parent(tomato.start_icon), tint, 0);
    for (uint32_t i = 0; i < TOMATO_SETS; i++) {
        lv_obj_set_style_bg_color(tomato.dots[i], lv_color_hex(UI_TINT_TOMATO), 0);
        lv_obj_set_style_bg_opa(tomato.dots[i], i < tomato.sessions ? LV_OPA_COVER : UI_OPA_DOT_OFF, 0);
    }
}

static void set_phase(phase_t phase) {
    tomato.phase = phase;
    tomato.cd.alert = phase == PHASE_FOCUS ? UI_ALERT_FOCUS_END : UI_ALERT_BREAK_END;
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

static uint8_t sound_pick(void) {
    uint8_t pick = ui_settings_current()->focus_sound;
    return pick <= UI_AMBIENCE_COUNT ? pick : 0;
}

static void sound_chip_refresh(void) {
    int pick = sound_pick();
    if (pick == tomato.sound_shown) {
        return;
    }
    tomato.sound_shown = pick;
    ui_label_set_text(tomato.sound_label, pick ? ui_ambience_name((ui_ambience_t)(pick - 1)) : "No sound");
    lv_obj_set_style_text_color(tomato.sound_icon, lv_color_hex(pick ? UI_TINT_AMBIENCE : UI_COLOR_TEXT_FAINT), 0);
    lv_obj_set_style_text_opa(tomato.sound_label, pick ? LV_OPA_COVER : UI_OPA_TEXT_MUTED, 0);
}

// Starts the sound as a focus block starts (or resumes) and stops it as the
// block ends, pauses or is reset. It never starts over a stream that's
// playing, nor takes over ambience started on its own page; `picked`
// applies a new pick mid-block.
static void sound_sync(bool picked) {
    uint8_t pick = sound_pick();
    bool want = tomato.phase == PHASE_FOCUS && tomato.cd.state == COUNTDOWN_RUNNING && pick;
    bool started = want && !tomato.sound_wanted;
    tomato.sound_wanted = want;

    ui_ambience_t kind;
    bool ours;
    bool playing = ui_ambience_playing(&kind, &ours);
    if (!want) {
        if (ours) {
            ui_ambience_stop();
        }
    } else if (started || picked ? ours || (!playing && !ui_stream_playing()) : ours && kind != pick - 1) {
        ui_ambience_play((ui_ambience_t)(pick - 1), true);
    }
    sound_chip_refresh();
}

static void sound_chip_cb(lv_event_t *e) {
    (void)e;
    ui_settings_change(UI_SETTING_FOCUS_SOUND, (sound_pick() + 1) % (UI_AMBIENCE_COUNT + 1));
    sound_sync(true);
}

static void sound_chip_create(lv_obj_t *screen) {
    lv_obj_t *chip = lv_obj_create(screen);
    lv_obj_remove_style_all(chip);
    lv_obj_remove_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(chip, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(chip, LV_SIZE_CONTENT, UI_CHIP_H);
    lv_obj_set_style_pad_hor(chip, UI_CHIP_PAD_X, 0);
    lv_obj_set_style_pad_column(chip, UI_SPACE_XS, 0);
    lv_obj_set_style_radius(chip, UI_RADIUS_PILL, 0);
    lv_obj_set_style_bg_color(chip, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(chip, UI_OPA_GLASS, 0);
    lv_obj_set_flex_flow(chip, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(chip, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_ext_click_area(chip, UI_TOUCH_EXT);
    lv_obj_add_event_cb(chip, sound_chip_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_align(chip, LV_ALIGN_CENTER, 0, TOMATO_SOUND_Y);

    tomato.sound_icon = lv_label_create(chip);
    lv_label_set_text(tomato.sound_icon, ICON_RAIN);
    lv_obj_set_style_text_font(tomato.sound_icon, UI_FONT_ICON_SM, 0);
    tomato.sound_label = lv_label_create(chip);
    lv_obj_set_style_text_font(tomato.sound_label, UI_FONT_CAPTION, 0);
    lv_obj_set_style_text_color(tomato.sound_label, lv_color_hex(UI_COLOR_TEXT), 0);
    tomato.sound_shown = -1;
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
    sound_sync(false);
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

// Whole minutes for the current phase, remembered per phase. Past idle the
// page has no use for the knob: it falls through to what's playing, a
// focus sound included.
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
    lv_obj_align(row, LV_ALIGN_CENTER, 0, UI_TITLE_Y);
    for (int i = 0; i < TOMATO_SETS; i++) {
        lv_obj_t *dot = lv_obj_create(row);
        lv_obj_remove_style_all(dot);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(dot, TOMATO_DOT, TOMATO_DOT);
        lv_obj_set_style_radius(dot, UI_RADIUS_PILL, 0);
        tomato.dots[i] = dot;
    }
}

void tomato_page_build(lv_obj_t *screen) {
    lv_color_t tint = lv_color_hex(UI_TINT_TOMATO);
    tool_page_register(screen, &(tool_page_desc_t){.name = "Pomodoro",
                                                   .icon = ICON_NUTRITION,
                                                   .tint = UI_TINT_TOMATO,
                                                   .cd = &tomato.cd,
                                                   .rotate = tomato_rotate,
                                                   .press = tomato_press,
                                                   .tick = sound_chip_refresh});
    for (int i = 0; i < PHASE_COUNT; i++) {
        tomato.minutes[i] = k_phases[i].default_min;
    }

    tomato.cd.ring = tool_dial_create(screen, tint, COUNTDOWN_RING_RANGE);
    tool_ring_jog_create(screen, tomato_rotate);
    dots_create(screen);
    sound_chip_create(screen);
    tomato.cd.time_label = tool_time_label_create(screen);
    lv_obj_align(tomato.cd.time_label, LV_ALIGN_CENTER, 0, 0);
    tomato.phase_label = tool_hint_label_create(screen);
    lv_obj_set_style_text_font(tomato.phase_label, UI_FONT_TITLE, 0);

    lv_obj_t *row = tool_button_row_create(screen);
    tomato.side_icon = tool_button_create(row, "Skip", side_cb);
    tomato.start_icon = tool_primary_button_create(row, tint, toggle_cb);

    tomato.cd.changed = tomato_changed;
    tomato.cd.expired = tomato_expired;
    tomato.cd.dismissed = tomato_dismissed;
    set_phase(PHASE_FOCUS);
}
