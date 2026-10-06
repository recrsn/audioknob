#include "ui_internal.h"

#include <stdio.h>
#include <string.h>

#include "fonts.h"

// Timer and stopwatch keep time as lv_tick deadlines/origins rather than
// counting ticks of the refresh timer, so a stalled LVGL task (WiFi bursts,
// a long flush) delays the redraw but never the time itself.

#define TOOL_RING_SIZE 344
#define TOOL_TICK_MS   33

#define TOOL_TITLE_Y    (-96)
#define TOOL_BUTTONS_Y  96
#define TOOL_BTN_SIZE   56
#define TOOL_BTN_GAP    18

#define TIMER_TINT       0xff9f43
#define TIMER_TIME_Y     0
#define TIMER_HINT_Y     44
#define TIMER_RING_RANGE 1000
#define TIMER_DEFAULT_S  (5 * 60)
#define TIMER_MAX_S      (99 * 60)
// Without haptics/audio the alert is only visual, so it doesn't pulse forever.
#define TIMER_ALERT_MS   60000
#define TIMER_PULSE_MS   500

#define SW_TINT       0x2dd4a0
#define SW_TIME_Y     (-38)
#define SW_LAPS_Y     12
#define SW_LAP_ROWS   3
#define SW_RING_RANGE 600 // one revolution per minute, 0.1 s resolution

typedef enum {
    TIMER_IDLE,
    TIMER_RUNNING,
    TIMER_PAUSED,
    TIMER_ALERT,
} timer_state_t;

static struct {
    lv_obj_t *ring;
    lv_obj_t *time_label;
    lv_obj_t *hint_label;
    lv_obj_t *start_icon;
    lv_obj_t *reset_icon;
    timer_state_t state;
    uint32_t set_s;
    uint32_t remaining_ms; // authoritative unless running
    uint32_t end_tick;     // authoritative while running
    uint32_t alert_tick;
    void (*on_alert)(bool ringing);
} timer;

static struct {
    lv_obj_t *ring;
    lv_obj_t *time_label;
    lv_obj_t *cs_label;
    lv_obj_t *lap_labels[SW_LAP_ROWS];
    lv_obj_t *start_icon;
    lv_obj_t *lap_icon;
    bool running;
    uint32_t base_ms; // elapsed before the current run
    uint32_t start_tick;
    uint32_t last_split_ms;
    uint32_t lap_ms[SW_LAP_ROWS]; // newest first
    uint32_t lap_count;
} sw;

static lv_obj_t *tool_ring_create(lv_obj_t *parent, lv_color_t tint, int32_t range) {
    lv_obj_t *arc = lv_arc_create(parent);
    lv_obj_set_size(arc, TOOL_RING_SIZE, TOOL_RING_SIZE);
    lv_arc_set_rotation(arc, 135);
    lv_arc_set_bg_angles(arc, 0, 270);
    lv_arc_set_range(arc, 0, range);
    lv_arc_set_value(arc, 0);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_width(arc, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(arc, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(0x1c1c24), LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, tint, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(arc, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_center(arc);
    return arc;
}

static void ring_set(lv_obj_t *ring, int32_t value) {
    if (lv_arc_get_value(ring) != value) {
        lv_arc_set_value(ring, value);
    }
    // A zero-length arc still paints its round cap as a stray dot.
    lv_obj_set_style_arc_opa(ring, value ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_INDICATOR);
}

static lv_obj_t *tool_title_create(lv_obj_t *parent, const char *text, lv_color_t tint) {
    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, text);
    lv_obj_set_style_text_font(title, &inter_semi_11, 0);
    lv_obj_set_style_text_color(title, tint, 0);
    lv_obj_set_style_text_letter_space(title, 1, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, TOOL_TITLE_Y);
    return title;
}

static lv_obj_t *tool_button_create(lv_obj_t *row, const char *text, int32_t size, const lv_font_t *font,
                                    lv_event_cb_t cb) {
    lv_obj_t *label = ui_round_button_create(row, text, size, font, false);
    lv_obj_t *btn = lv_obj_get_parent(label);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(btn, 8);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    return label;
}

static lv_obj_t *tool_button_row_create(lv_obj_t *parent) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, TOOL_BTN_GAP, 0);
    lv_obj_align(row, LV_ALIGN_CENTER, 0, TOOL_BUTTONS_Y);
    return row;
}

static void button_set_enabled(lv_obj_t *label, bool enabled) {
    lv_obj_t *btn = lv_obj_get_parent(label);
    lv_obj_set_style_opa(btn, enabled ? LV_OPA_COVER : LV_OPA_40, 0);
    if (enabled) {
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_obj_remove_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    }
}

// Tabular digits (inter_tnum_*) keep a running time from wobbling sideways.
static lv_obj_t *time_label_create(lv_obj_t *parent) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, &inter_tnum_52, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    return label;
}

static void opa_exec(void *obj, int32_t v) {
    lv_obj_set_style_opa(obj, (lv_opa_t)v, 0);
}

static void pulse_start(lv_obj_t *obj) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, opa_exec);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_20);
    lv_anim_set_duration(&a, TIMER_PULSE_MS);
    lv_anim_set_playback_duration(&a, TIMER_PULSE_MS);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
}

static void pulse_stop(lv_obj_t *obj) {
    lv_anim_delete(obj, opa_exec);
    lv_obj_set_style_opa(obj, LV_OPA_COVER, 0);
}

// -- timer --

// Fine steps where they matter (short timers), coarse ones where turning
// through them would take forever. Decrements use the step just below the
// current value so up/down retrace the same stops.
static uint32_t timer_step_s(uint32_t s) {
    return s < 60 ? 5 : s < 600 ? 30 : 60;
}

static uint32_t timer_remaining_ms(void) {
    if (timer.state != TIMER_RUNNING) {
        return timer.remaining_ms;
    }
    int32_t left = (int32_t)(timer.end_tick - lv_tick_get());
    return left > 0 ? (uint32_t)left : 0;
}

static void timer_refresh(void) {
    uint32_t ms = timer_remaining_ms();
    uint32_t secs = (ms + 999) / 1000;
    char buf[16];
    snprintf(buf, sizeof(buf), "%02lu:%02lu", (unsigned long)(secs / 60), (unsigned long)(secs % 60));
    ui_label_set_text(timer.time_label, buf);

    uint64_t total_ms = (uint64_t)timer.set_s * 1000;
    ring_set(timer.ring, total_ms ? (int32_t)((uint64_t)ms * TIMER_RING_RANGE / total_ms) : 0);

    static const char *const hints[] = {
        [TIMER_IDLE] = "Press to start",
        [TIMER_RUNNING] = "Running",
        [TIMER_PAUSED] = "Paused",
        [TIMER_ALERT] = "Time's up",
    };
    ui_label_set_text(timer.hint_label,
                      timer.state == TIMER_IDLE && timer.set_s == 0 ? "Turn to set" : hints[timer.state]);

    ui_label_set_text(timer.start_icon, timer.state == TIMER_RUNNING ? LV_SYMBOL_PAUSE
                                        : timer.state == TIMER_ALERT ? LV_SYMBOL_STOP
                                                                     : LV_SYMBOL_PLAY);
    button_set_enabled(timer.start_icon, timer.state != TIMER_IDLE || timer.set_s > 0);
    button_set_enabled(timer.reset_icon, timer.state != TIMER_IDLE);
}

static void timer_reset(void) {
    if (timer.state == TIMER_ALERT) {
        pulse_stop(timer.ring);
        pulse_stop(timer.time_label);
        if (timer.on_alert) {
            timer.on_alert(false);
        }
    }
    timer.state = TIMER_IDLE;
    timer.remaining_ms = timer.set_s * 1000;
    timer_refresh();
}

static void timer_toggle(void) {
    switch (timer.state) {
    case TIMER_IDLE:
        if (timer.set_s == 0) {
            return;
        }
        // fallthrough
    case TIMER_PAUSED:
        timer.end_tick = lv_tick_get() + timer.remaining_ms;
        timer.state = TIMER_RUNNING;
        break;
    case TIMER_RUNNING:
        timer.remaining_ms = timer_remaining_ms();
        timer.state = TIMER_PAUSED;
        break;
    case TIMER_ALERT:
        timer_reset();
        return;
    }
    timer_refresh();
}

static void timer_expire(void) {
    timer.state = TIMER_ALERT;
    timer.remaining_ms = 0;
    timer.alert_tick = lv_tick_get();
    timer_refresh();
    ring_set(timer.ring, TIMER_RING_RANGE);
    pulse_start(timer.ring);
    pulse_start(timer.time_label);
    if (timer.on_alert) {
        timer.on_alert(true);
    }
}

static void timer_toggle_cb(lv_event_t *e) {
    (void)e;
    timer_toggle();
}

static void timer_reset_cb(lv_event_t *e) {
    (void)e;
    timer_reset();
}

// Any tap on the screen silences a ringing timer, not just the buttons.
static void timer_screen_click_cb(lv_event_t *e) {
    (void)e;
    if (timer.state == TIMER_ALERT) {
        timer_reset();
    }
}

bool ui_timer_alerting(void) {
    return timer.state == TIMER_ALERT;
}

void ui_timer_dismiss_alert(void) {
    if (timer.state == TIMER_ALERT) {
        timer_reset();
    }
}

bool ui_timer_knob_rotate(int steps) {
    if (timer.state != TIMER_IDLE) {
        return false;
    }
    uint32_t s = timer.set_s;
    for (; steps > 0 && s < TIMER_MAX_S; steps--) {
        s += timer_step_s(s);
    }
    for (; steps < 0 && s > 0; steps++) {
        s -= timer_step_s(s - 1);
    }
    timer.set_s = s > TIMER_MAX_S ? TIMER_MAX_S : s;
    timer.remaining_ms = timer.set_s * 1000;
    timer_refresh();
    return true;
}

void ui_timer_knob_press(void) {
    timer_toggle();
}

static void timer_build(lv_obj_t *screen, void (*on_alert)(bool ringing)) {
    lv_color_t tint = lv_color_hex(TIMER_TINT);
    timer.on_alert = on_alert;
    timer.set_s = TIMER_DEFAULT_S;
    timer.remaining_ms = timer.set_s * 1000;

    lv_obj_add_event_cb(screen, timer_screen_click_cb, LV_EVENT_CLICKED, NULL);
    timer.ring = tool_ring_create(screen, tint, TIMER_RING_RANGE);
    tool_title_create(screen, "TIMER", tint);

    timer.time_label = time_label_create(screen);
    lv_obj_align(timer.time_label, LV_ALIGN_CENTER, 0, TIMER_TIME_Y);

    timer.hint_label = lv_label_create(screen);
    lv_obj_set_style_text_font(timer.hint_label, &inter_med_13, 0);
    lv_obj_set_style_text_color(timer.hint_label, lv_color_hex(0x9a9aa4), 0);
    lv_obj_align(timer.hint_label, LV_ALIGN_CENTER, 0, TIMER_HINT_Y);

    lv_obj_t *row = tool_button_row_create(screen);
    timer.reset_icon = tool_button_create(row, "Reset", TOOL_BTN_SIZE, &inter_semi_13, timer_reset_cb);
    timer.start_icon = tool_button_create(row, LV_SYMBOL_PLAY, TOOL_BTN_SIZE, &lv_font_montserrat_22,
                                          timer_toggle_cb);
    lv_obj_set_style_bg_color(lv_obj_get_parent(timer.start_icon), tint, 0);

    timer_refresh();
}

// -- stopwatch --

static uint32_t sw_elapsed_ms(void) {
    return sw.base_ms + (sw.running ? lv_tick_elaps(sw.start_tick) : 0);
}

static void format_split(uint32_t ms, char *out, size_t len) {
    uint32_t s = ms / 1000;
    if (s >= 3600) {
        snprintf(out, len, "%lu:%02lu:%02lu", (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60),
                 (unsigned long)(s % 60));
    } else {
        snprintf(out, len, "%02lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
    }
}

static void sw_refresh(void) {
    uint32_t ms = sw_elapsed_ms();
    char buf[24];
    format_split(ms, buf, sizeof(buf));
    ui_label_set_text(sw.time_label, buf);
    snprintf(buf, sizeof(buf), ".%02lu", (unsigned long)(ms / 10 % 100));
    ui_label_set_text(sw.cs_label, buf);

    ring_set(sw.ring, (int32_t)(ms % 60000 / 100));

    for (uint32_t i = 0; i < SW_LAP_ROWS; i++) {
        bool shown = i < sw.lap_count;
        ui_set_visible(sw.lap_labels[i], shown);
        if (shown) {
            char split[16];
            format_split(sw.lap_ms[i], split, sizeof(split));
            snprintf(buf, sizeof(buf), "Lap %lu   %s.%02lu", (unsigned long)(sw.lap_count - i), split,
                     (unsigned long)(sw.lap_ms[i] / 10 % 100));
            ui_label_set_text(sw.lap_labels[i], buf);
        }
    }

    ui_label_set_text(sw.start_icon, sw.running ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    ui_label_set_text(sw.lap_icon, sw.running || ms == 0 ? "Lap" : "Reset");
    button_set_enabled(sw.lap_icon, sw.running || ms > 0);
}

static void sw_toggle(void) {
    if (sw.running) {
        sw.base_ms = sw_elapsed_ms();
        sw.running = false;
    } else {
        sw.start_tick = lv_tick_get();
        sw.running = true;
    }
    sw_refresh();
}

static void sw_lap_or_reset(void) {
    if (sw.running) {
        uint32_t now = sw_elapsed_ms();
        memmove(&sw.lap_ms[1], &sw.lap_ms[0], sizeof(sw.lap_ms[0]) * (SW_LAP_ROWS - 1));
        sw.lap_ms[0] = now - sw.last_split_ms;
        sw.last_split_ms = now;
        sw.lap_count++;
    } else {
        sw.base_ms = 0;
        sw.last_split_ms = 0;
        sw.lap_count = 0;
    }
    sw_refresh();
}

static void sw_toggle_cb(lv_event_t *e) {
    (void)e;
    sw_toggle();
}

static void sw_lap_cb(lv_event_t *e) {
    (void)e;
    sw_lap_or_reset();
}

void ui_stopwatch_knob_press(void) {
    sw_toggle();
}

static void tools_tick_cb(lv_timer_t *t) {
    (void)t;
    if (timer.state == TIMER_RUNNING) {
        if (timer_remaining_ms() == 0) {
            timer_expire();
        } else {
            timer_refresh();
        }
    } else if (timer.state == TIMER_ALERT && lv_tick_elaps(timer.alert_tick) >= TIMER_ALERT_MS) {
        timer_reset();
    }
    if (sw.running) {
        sw_refresh();
    }
}

static void stopwatch_build(lv_obj_t *screen) {
    lv_color_t tint = lv_color_hex(SW_TINT);
    sw.ring = tool_ring_create(screen, tint, SW_RING_RANGE);
    tool_title_create(screen, "STOPWATCH", tint);

    // Hundredths ride beside the big digits at half height, baseline-aligned
    // to read as one number.
    lv_obj_t *row = lv_obj_create(screen);
    lv_obj_remove_style_all(row);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_align(row, LV_ALIGN_CENTER, 0, SW_TIME_Y);

    sw.time_label = time_label_create(row);
    sw.cs_label = lv_label_create(row);
    lv_obj_set_style_text_font(sw.cs_label, &inter_tnum_26, 0);
    lv_obj_set_style_text_color(sw.cs_label, lv_color_hex(0x9a9aa4), 0);
    lv_obj_set_style_pad_bottom(sw.cs_label, inter_tnum_52.base_line - inter_tnum_26.base_line, 0);

    lv_obj_t *laps = lv_obj_create(screen);
    lv_obj_remove_style_all(laps);
    lv_obj_remove_flag(laps, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(laps, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(laps, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(laps, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(laps, 2, 0);
    lv_obj_align(laps, LV_ALIGN_TOP_MID, 0, LV_VER_RES / 2 + SW_LAPS_Y);
    for (int i = 0; i < SW_LAP_ROWS; i++) {
        sw.lap_labels[i] = lv_label_create(laps);
        lv_obj_set_style_text_font(sw.lap_labels[i], &inter_med_13, 0);
        lv_obj_set_style_text_color(sw.lap_labels[i], lv_color_white(), 0);
        lv_obj_set_style_text_opa(sw.lap_labels[i], i == 0 ? LV_OPA_80 : LV_OPA_50, 0);
    }

    lv_obj_t *buttons = tool_button_row_create(screen);
    sw.lap_icon = tool_button_create(buttons, "Lap", TOOL_BTN_SIZE, &inter_semi_13, sw_lap_cb);
    sw.start_icon = tool_button_create(buttons, LV_SYMBOL_PLAY, TOOL_BTN_SIZE, &lv_font_montserrat_22,
                                       sw_toggle_cb);
    lv_obj_set_style_bg_color(lv_obj_get_parent(sw.start_icon), tint, 0);

    sw_refresh();
}

void ui_tools_build(lv_obj_t *timer_screen, lv_obj_t *stopwatch_screen, void (*on_alert)(bool ringing)) {
    timer_build(timer_screen, on_alert);
    stopwatch_build(stopwatch_screen);
    lv_timer_create(tools_tick_cb, TOOL_TICK_MS, NULL);
}
