#include "ui_tools.h"

#include <stdio.h>

#define TOOL_DIAL_SIZE  336
#define TOOL_DIAL_W     16
#define TOOL_TICKS      8
#define TOOL_TICK_OUT   140
#define TOOL_TICK_MS    33
#define TOOL_BUTTONS_Y  92
#define TOOL_BTN_GAP    UI_SPACE_L
#define TOOL_BTN_EXT    (TOOL_BTN_GAP / 2)
// Inside this radius is a page's own content; the band outside it, ~7 mm
// deep on the round panel, belongs to the ring.
#define TOOL_TOUCH_R_IN 116
// Closer to the middle than this, a moving finger's angle is too jumpy to
// follow.
#define TOOL_TOUCH_R_MIN 48
#define TOOL_JOG_DEG     10

// Haptics may be missing, so a ringing countdown doesn't pulse forever.
#define ALERT_MS 60000
#define PULSE_MS 500

typedef struct {
    lv_obj_t *screen;
    tool_page_desc_t desc;
} tool_page_t;

// Indexed by ui_app_t.
static tool_page_t s_pages[UI_APP_COUNT];
static void (*s_on_alert)(lv_obj_t *screen, bool ringing);

// -- shared widgets --

lv_obj_t *tool_ring_create(lv_obj_t *parent, lv_color_t tint, int32_t range) {
    lv_obj_t *arc = lv_arc_create(parent);
    lv_obj_set_size(arc, UI_RING_SIZE, UI_RING_SIZE);
    lv_arc_set_rotation(arc, 135);
    lv_arc_set_bg_angles(arc, 0, 270);
    lv_arc_set_range(arc, 0, range);
    lv_arc_set_value(arc, 0);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_width(arc, UI_RING_W, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(arc, UI_RING_W, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(UI_COLOR_SURFACE), LV_PART_MAIN);
    // No track: LVGL masks every pixel of an arc's bounding box, so even this
    // thin ring cost ~13 ms per full frame. Only progress is drawn.
    lv_obj_set_style_arc_opa(arc, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, tint, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(arc, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_center(arc);
    return arc;
}

void tool_ring_set(lv_obj_t *ring, int32_t value) {
    if (lv_arc_get_value(ring) == value) {
        return;
    }
    lv_arc_set_value(ring, value);
    // A zero-length arc still paints its round cap as a stray dot.
    lv_obj_set_style_arc_opa(ring, value ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_INDICATOR);
}

int32_t tool_ring_value_at(lv_obj_t *ring, int32_t deg) {
    int32_t start = (int32_t)lv_arc_get_bg_angle_start(ring);
    int32_t sweep = (int32_t)lv_arc_get_bg_angle_end(ring) - start;
    int32_t rel = ((deg - (int32_t)lv_arc_get_rotation(ring) - start) % 360 + 360) % 360;
    if (rel > sweep) {
        rel = rel - sweep < (360 - sweep) / 2 ? sweep : 0;
    }
    int32_t min = lv_arc_get_min_value(ring);
    int32_t max = lv_arc_get_max_value(ring);
    return min + (rel * (max - min) + sweep / 2) / sweep;
}

typedef struct {
    tool_ring_touch_cb_t cb;
    void *user;
    bool active; // this press started on the band
} ring_touch_t;

static void ring_touch_event_cb(lv_event_t *e) {
    ring_touch_t *rt = lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        rt->active = false;
        return;
    }
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    lv_area_t a;
    lv_obj_get_coords(lv_event_get_current_target_obj(e), &a);
    int32_t dx = p.x - (a.x1 + a.x2) / 2;
    int32_t dy = p.y - (a.y1 + a.y2) / 2;
    int32_t r2 = dx * dx + dy * dy;
    bool first = code == LV_EVENT_PRESSED;
    if (first) {
        rt->active = r2 >= TOOL_TOUCH_R_IN * TOOL_TOUCH_R_IN;
    }
    if (rt->active && r2 >= TOOL_TOUCH_R_MIN * TOOL_TOUCH_R_MIN) {
        rt->cb(lv_atan2(dy, dx), first, rt->user);
    }
}

// Covers the whole page so a drag can wander inwards; presses that start
// inside the band fall through to nothing but still bubble their click to
// the screen (which silences a ringing countdown).
void tool_ring_touch_create(lv_obj_t *parent, tool_ring_touch_cb_t cb, void *user) {
    ring_touch_t *rt = lv_malloc_zeroed(sizeof(*rt));
    LV_ASSERT_MALLOC(rt);
    rt->cb = cb;
    rt->user = user;
    lv_obj_t *zone = lv_obj_create(parent);
    lv_obj_remove_style_all(zone);
    lv_obj_remove_flag(zone, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(zone, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_size(zone, lv_pct(100), lv_pct(100));
    lv_obj_center(zone);
    lv_obj_add_event_cb(zone, ring_touch_event_cb, LV_EVENT_PRESSED, rt);
    lv_obj_add_event_cb(zone, ring_touch_event_cb, LV_EVENT_PRESSING, rt);
    lv_obj_add_event_cb(zone, ring_touch_event_cb, LV_EVENT_RELEASED, rt);
    lv_obj_add_event_cb(zone, ring_touch_event_cb, LV_EVENT_PRESS_LOST, rt);
}

typedef struct {
    bool (*rotate)(int steps);
    int32_t last_deg;
    int32_t travel; // degrees not yet turned into steps
} ring_jog_t;

static void ring_jog_cb(int32_t deg, bool first, void *user) {
    ring_jog_t *jog = user;
    if (first) {
        jog->last_deg = deg;
        jog->travel = 0;
        return;
    }
    int32_t d = deg - jog->last_deg;
    d += d > 180 ? -360 : d < -180 ? 360 : 0;
    jog->last_deg = deg;
    jog->travel += d;
    int steps = jog->travel / TOOL_JOG_DEG;
    if (steps) {
        jog->travel -= steps * TOOL_JOG_DEG;
        jog->rotate(steps);
    }
}

void tool_ring_jog_create(lv_obj_t *parent, bool (*rotate)(int steps)) {
    ring_jog_t *jog = lv_malloc_zeroed(sizeof(*jog));
    LV_ASSERT_MALLOC(jog);
    jog->rotate = rotate;
    tool_ring_touch_create(parent, ring_jog_cb, jog);
}

// Eighth-turn ticks inside the dial, quarters longer and brighter, like a
// kitchen timer's face; 6 o'clock is left out, the buttons sit there.
// Static, so it only repaints where the ring does.
static void ticks_draw_cb(lv_event_t *e) {
    lv_obj_t *obj = lv_event_get_target_obj(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    int32_t cx = (a.x1 + a.x2) / 2;
    int32_t cy = (a.y1 + a.y2) / 2;

    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.round_start = 1;
    line.round_end = 1;
    for (int i = 0; i < TOOL_TICKS; i++) {
        // The bottom one would sit under the home pill, the top one in the
        // chip safe area.
        if (i == 0 || i == TOOL_TICKS / 2) {
            continue;
        }
        bool quarter = i % 2 == 0;
        int32_t r_in = TOOL_TICK_OUT - (quarter ? 14 : 8);
        int32_t deg = i * 360 / TOOL_TICKS;
        int32_t s = lv_trigo_sin(deg);
        int32_t c = lv_trigo_cos(deg);
        line.width = quarter ? 4 : 3;
        line.color = lv_color_hex(quarter ? UI_COLOR_TEXT_DIM : UI_COLOR_TEXT_FAINT);
        line.p1.x = cx + (s * r_in >> LV_TRIGO_SHIFT);
        line.p1.y = cy - (c * r_in >> LV_TRIGO_SHIFT);
        line.p2.x = cx + (s * TOOL_TICK_OUT >> LV_TRIGO_SHIFT);
        line.p2.y = cy - (c * TOOL_TICK_OUT >> LV_TRIGO_SHIFT);
        lv_draw_line(layer, &line);
    }
}

lv_obj_t *tool_dial_create(lv_obj_t *parent, lv_color_t tint, int32_t range) {
    lv_obj_t *ticks = lv_obj_create(parent);
    lv_obj_remove_style_all(ticks);
    lv_obj_remove_flag(ticks, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(ticks, TOOL_DIAL_SIZE, TOOL_DIAL_SIZE);
    lv_obj_add_event_cb(ticks, ticks_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_center(ticks);

    lv_obj_t *ring = tool_ring_create(parent, tint, range);
    lv_obj_set_size(ring, TOOL_DIAL_SIZE, TOOL_DIAL_SIZE);
    lv_arc_set_rotation(ring, 270);
    lv_arc_set_bg_angles(ring, 0, 360);
    lv_obj_set_style_arc_width(ring, TOOL_DIAL_W, LV_PART_MAIN);
    lv_obj_set_style_arc_width(ring, TOOL_DIAL_W, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(ring, lv_color_hex(UI_COLOR_TRACK), LV_PART_MAIN);
    return ring;
}

lv_obj_t *tool_title_create(lv_obj_t *parent, const char *text, lv_color_t tint) {
    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, text);
    lv_obj_set_style_text_font(title, UI_FONT_OVERLINE, 0);
    lv_obj_set_style_text_color(title, tint, 0);
    lv_obj_set_style_text_letter_space(title, UI_OVERLINE_SPACING, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, UI_TITLE_Y);
    return title;
}

// Tabular digits (inter_tnum_*) keep a running time from wobbling sideways.
lv_obj_t *tool_time_label_create(lv_obj_t *parent) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, UI_FONT_READING, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_TEXT), 0);
    return label;
}

lv_obj_t *tool_hint_label_create(lv_obj_t *parent) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, UI_FONT_BODY, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_TEXT_DIM), 0);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, TOOL_HINT_Y);
    return label;
}

lv_obj_t *tool_button_row_create(lv_obj_t *parent) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, TOOL_BTN_GAP, 0);
    // As far as the buttons' padded hit areas reach: LVGL looks for them
    // only inside the row. Centered, so nothing moves.
    lv_obj_set_style_pad_all(row, TOOL_BTN_EXT, 0);
    lv_obj_align(row, LV_ALIGN_CENTER, 0, TOOL_BUTTONS_Y);
    return row;
}

static lv_obj_t *button_create(lv_obj_t *row, const char *text, const lv_font_t *font, lv_event_cb_t cb) {
    lv_obj_t *label = ui_round_button_create(row, text, UI_BUTTON_SIZE, font, false);
    lv_obj_t *btn = lv_obj_get_parent(label);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(btn, TOOL_BTN_EXT);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    return label;
}

lv_obj_t *tool_button_create(lv_obj_t *row, const char *text, lv_event_cb_t cb) {
    return button_create(row, text, UI_FONT_LABEL, cb);
}

lv_obj_t *tool_primary_button_create(lv_obj_t *row, lv_color_t tint, lv_event_cb_t cb) {
    lv_obj_t *label = button_create(row, LV_SYMBOL_PLAY, UI_FONT_SYMBOL_LG, cb);
    lv_obj_set_style_bg_color(lv_obj_get_parent(label), tint, 0);
    return label;
}

// Called on every refresh; restyling unconditionally would redraw the
// button each tick.
void tool_button_set_enabled(lv_obj_t *label, bool enabled) {
    lv_obj_t *btn = lv_obj_get_parent(label);
    if (lv_obj_has_flag(btn, LV_OBJ_FLAG_CLICKABLE) == enabled) {
        return;
    }
    lv_obj_set_style_opa(btn, enabled ? LV_OPA_COVER : UI_OPA_DISABLED, 0);
    if (enabled) {
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_obj_remove_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    }
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
    lv_anim_set_duration(&a, PULSE_MS);
    lv_anim_set_playback_duration(&a, PULSE_MS);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
}

static void pulse_stop(lv_obj_t *obj) {
    lv_anim_delete(obj, opa_exec);
    lv_obj_set_style_opa(obj, LV_OPA_COVER, 0);
}

// -- countdown --

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
static void format_mmss(uint32_t ms, char *buf, size_t len) {
    uint32_t secs = (ms + 999) / 1000;
    snprintf(buf, len, "%02lu:%02lu", (unsigned long)(secs / 60), (unsigned long)(secs % 60));
}

void countdown_refresh(countdown_t *cd) {
    uint32_t ms = countdown_remaining_ms(cd);
    char buf[16];
    format_mmss(ms, buf, sizeof(buf));
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
        if (s_on_alert) {
            s_on_alert(cd->screen, false);
        }
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
    if (s_on_alert) {
        s_on_alert(cd->screen, true);
    }
}

static void countdown_tick(countdown_t *cd) {
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

// -- page registry --

static tool_page_t *page_for(lv_obj_t *screen) {
    for (int i = 0; i < UI_APP_COUNT; i++) {
        if (screen && s_pages[i].screen == screen) {
            return &s_pages[i];
        }
    }
    return NULL;
}

// Any tap on the screen silences a ringing countdown, not just the buttons.
static void screen_click_cb(lv_event_t *e) {
    countdown_t *cd = lv_event_get_user_data(e);
    if (cd->state == COUNTDOWN_ALERT) {
        countdown_reset(cd);
    }
}

// ui_tools_build() placed every screen already; registering only attaches
// the page's behavior to its slot.
void tool_page_register(lv_obj_t *screen, const tool_page_desc_t *desc) {
    tool_page_t *page = page_for(screen);
    LV_ASSERT_NULL(page);
    page->desc = *desc;
    if (desc->cd) {
        desc->cd->screen = screen;
        lv_obj_add_event_cb(screen, screen_click_cb, LV_EVENT_CLICKED, desc->cd);
    }
}

static void tools_tick_cb(lv_timer_t *t) {
    (void)t;
    for (int i = 0; i < UI_APP_COUNT; i++) {
        if (s_pages[i].desc.cd) {
            countdown_tick(s_pages[i].desc.cd);
        }
        if (s_pages[i].desc.tick) {
            s_pages[i].desc.tick();
        }
    }
}

void ui_tools_build(lv_obj_t *const screens[UI_APP_COUNT], void (*on_alert)(lv_obj_t *screen, bool ringing)) {
    static void (*const builders[UI_APP_COUNT])(lv_obj_t *) = {
        [UI_APP_RADIO] = radio_page_build,
#ifdef AK_LIGHTS
        [UI_APP_LIGHTS] = lights_page_build,
#endif
        [UI_APP_WEATHER] = weather_page_build,     [UI_APP_TIMER] = timer_page_build,
        [UI_APP_TOMATO] = tomato_page_build,       [UI_APP_AMBIENCE] = ambience_page_build,
        [UI_APP_STOPWATCH] = stopwatch_page_build,
        [UI_APP_SETTINGS] = settings_page_build,
    };
    s_on_alert = on_alert;
    for (int i = 0; i < UI_APP_COUNT; i++) {
        s_pages[i].screen = screens[i];
    }
    for (int i = 0; i < UI_APP_COUNT; i++) {
        builders[i](screens[i]);
    }
    lv_timer_create(tools_tick_cb, TOOL_TICK_MS, NULL);
}

ui_app_info_t ui_tools_info(ui_app_t app) {
    const tool_page_desc_t *d = &s_pages[app].desc;
    return (ui_app_info_t){d->name, d->icon, lv_color_hex(d->tint)};
}

int ui_tools_app_of(lv_obj_t *screen) {
    tool_page_t *page = page_for(screen);
    return page ? (int)(page - s_pages) : -1;
}

bool ui_tools_status(ui_app_t app, char *buf, size_t len) {
    const tool_page_desc_t *d = &s_pages[app].desc;
    if (d->cd) {
        if (d->cd->state == COUNTDOWN_IDLE) {
            return false;
        }
        format_mmss(countdown_remaining_ms(d->cd), buf, len);
        return true;
    }
    return d->status && d->status(buf, len);
}

bool ui_tools_alerting(void) {
    for (int i = 0; i < UI_APP_COUNT; i++) {
        if (s_pages[i].desc.cd && s_pages[i].desc.cd->state == COUNTDOWN_ALERT) {
            return true;
        }
    }
    return false;
}

void ui_tools_dismiss(lv_obj_t *screen) {
    tool_page_t *page = page_for(screen);
    if (page && page->desc.cd && page->desc.cd->state == COUNTDOWN_ALERT) {
        countdown_reset(page->desc.cd);
    }
}

bool ui_tools_knob_rotate(lv_obj_t *screen, int steps) {
    tool_page_t *page = page_for(screen);
    return page && page->desc.rotate && page->desc.rotate(steps);
}

bool ui_tools_knob_press(lv_obj_t *screen) {
    tool_page_t *page = page_for(screen);
    if (!page || !page->desc.press) {
        return false;
    }
    page->desc.press();
    return true;
}
