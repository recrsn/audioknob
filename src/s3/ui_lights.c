#include "ui_tools.h"

#include <stdio.h>
#include <string.h>

// One light at a time: the ring is its dimmer, the knob turns it, a tap on
// the bulb toggles it, a knob press (or a tap on the name) moves to the
// next light.

#define LIGHTS_TINT     0xffc35a
#define LIGHTS_OFF      0x5a5a64
#define LIGHTS_BULB_Y   (-44)
#define LIGHTS_BULB     88
#define LIGHTS_LEVEL_Y  28
#define LIGHTS_NAME_Y   76
#define LIGHTS_DOTS_Y   100
#define LIGHTS_DOT      6
#define LIGHTS_STEP_PCT 5
#define LIGHTS_MIN_PCT  1

static struct {
    lv_obj_t *ring;
    lv_obj_t *bulb_btn;
    lv_obj_t *bulb;
    lv_obj_t *level;
    lv_obj_t *name;
    lv_obj_t *dots_row;
    lv_obj_t *dots[UI_LIGHTS_MAX];
    lv_obj_t *empty;
    ui_light_t lights[UI_LIGHTS_MAX];
    int count;
    int selected;
    const ui_lights_handler_t *handler;
} lt;

void ui_set_lights_handler(const ui_lights_handler_t *handler) {
    lt.handler = handler;
}

static void render(void) {
    bool any = lt.count > 0;
    ui_set_visible(lt.bulb_btn, any);
    ui_set_visible(lt.level, any);
    ui_set_visible(lt.name, any);
    ui_set_visible(lt.dots_row, lt.count > 1);
    ui_set_visible(lt.empty, !any);
    if (!any) {
        tool_ring_set(lt.ring, 0);
        return;
    }

    const ui_light_t *l = &lt.lights[lt.selected];
    bool lit = l->reachable && l->on;
    tool_ring_set(lt.ring, lit ? l->brightness : 0);
    lv_obj_set_style_text_color(lt.bulb, lv_color_hex(lit ? LIGHTS_TINT : LIGHTS_OFF), 0);

    // The big tabular font has digits and '%' only, so words drop to a
    // text font.
    char buf[16];
    if (lit) {
        snprintf(buf, sizeof(buf), "%u%%", l->brightness);
    } else {
        snprintf(buf, sizeof(buf), "%s", l->reachable ? "Off" : "--");
    }
    lv_obj_set_style_text_font(lt.level, lit ? &inter_tnum_52 : &inter_semi_16, 0);
    lv_obj_set_style_text_color(lt.level, lv_color_hex(lit ? 0xffffff : LIGHTS_OFF), 0);
    ui_label_set_text(lt.level, buf);
    ui_label_set_text(lt.name, l->reachable ? l->name : "Unreachable");

    for (int i = 0; i < UI_LIGHTS_MAX; i++) {
        ui_set_visible(lt.dots[i], i < lt.count);
        lv_obj_set_style_bg_opa(lt.dots[i], i == lt.selected ? LV_OPA_80 : LV_OPA_30, 0);
    }
}

void ui_update_lights(const ui_light_t *lights, int count) {
    if (count > UI_LIGHTS_MAX) {
        count = UI_LIGHTS_MAX;
    }
    if (count == lt.count && !memcmp(lights, lt.lights, sizeof(lights[0]) * count)) {
        return;
    }
    memcpy(lt.lights, lights, sizeof(lights[0]) * count);
    lt.count = count;
    if (lt.selected >= count) {
        lt.selected = 0;
    }
    render();
}

// Changes show locally right away; the backend's next update confirms or
// corrects them.
static void set_power(bool on) {
    ui_light_t *l = &lt.lights[lt.selected];
    l->on = on;
    render();
    if (lt.handler && lt.handler->set_power) {
        lt.handler->set_power(lt.selected, on);
    }
}

static void set_brightness(uint8_t pct) {
    ui_light_t *l = &lt.lights[lt.selected];
    l->brightness = pct;
    l->on = true;
    render();
    if (lt.handler && lt.handler->set_brightness) {
        lt.handler->set_brightness(lt.selected, pct);
    }
}

static bool selected_usable(void) {
    return lt.count > 0 && lt.lights[lt.selected].reachable;
}

// Turning an off light up starts it from the bottom of the range, not from
// its remembered level.
static bool lights_rotate(int steps) {
    if (!selected_usable()) {
        return lt.count > 0;
    }
    const ui_light_t *l = &lt.lights[lt.selected];
    int base = l->on ? l->brightness : 0;
    int pct = base + steps * LIGHTS_STEP_PCT;
    if (pct < LIGHTS_MIN_PCT) {
        if (l->on) {
            set_power(false);
        }
        return true;
    }
    set_brightness((uint8_t)(pct > 100 ? 100 : pct));
    return true;
}

static void next_light(void) {
    if (lt.count > 1) {
        lt.selected = (lt.selected + 1) % lt.count;
        render();
    }
}

static void bulb_cb(lv_event_t *e) {
    (void)e;
    if (selected_usable()) {
        set_power(!lt.lights[lt.selected].on);
    }
}

static void name_cb(lv_event_t *e) {
    (void)e;
    next_light();
}

static lv_obj_t *label_create(lv_obj_t *parent, const lv_font_t *font, uint32_t color, int32_t y) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, y);
    return label;
}

void lights_page_build(lv_obj_t *screen) {
    tool_page_register(screen, &(tool_page_desc_t){.name = "Lights",
                                                   .icon = ICON_BULB,
                                                   .tint = LIGHTS_TINT,
                                                   .rotate = lights_rotate,
                                                   .press = next_light});

    lt.ring = tool_ring_create(screen, lv_color_hex(LIGHTS_TINT), 100);
    tool_title_create(screen, "LIGHTS", lv_color_hex(LIGHTS_TINT));

    lt.bulb_btn = lv_obj_create(screen);
    lv_obj_remove_style_all(lt.bulb_btn);
    lv_obj_remove_flag(lt.bulb_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(lt.bulb_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(lt.bulb_btn, LIGHTS_BULB, LIGHTS_BULB);
    lv_obj_set_style_radius(lt.bulb_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(lt.bulb_btn, lv_color_hex(0x1c1c24), 0);
    lv_obj_set_style_bg_opa(lt.bulb_btn, LV_OPA_COVER, 0);
    lv_obj_align(lt.bulb_btn, LV_ALIGN_CENTER, 0, LIGHTS_BULB_Y);
    lv_obj_add_event_cb(lt.bulb_btn, bulb_cb, LV_EVENT_CLICKED, NULL);
    lt.bulb = lv_label_create(lt.bulb_btn);
    lv_obj_set_style_text_font(lt.bulb, &icons_48, 0);
    lv_label_set_text(lt.bulb, ICON_BULB);
    lv_obj_center(lt.bulb);

    lt.level = tool_time_label_create(screen);
    lv_obj_align(lt.level, LV_ALIGN_CENTER, 0, LIGHTS_LEVEL_Y);

    lt.name = label_create(screen, &inter_semi_16, 0xffffff, LIGHTS_NAME_Y);
    lv_obj_add_flag(lt.name, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(lt.name, 12);
    lv_obj_add_event_cb(lt.name, name_cb, LV_EVENT_CLICKED, NULL);

    lt.dots_row = lv_obj_create(screen);
    lv_obj_remove_style_all(lt.dots_row);
    lv_obj_remove_flag(lt.dots_row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(lt.dots_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(lt.dots_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(lt.dots_row, LIGHTS_DOT, 0);
    lv_obj_align(lt.dots_row, LV_ALIGN_CENTER, 0, LIGHTS_DOTS_Y);
    for (int i = 0; i < UI_LIGHTS_MAX; i++) {
        lv_obj_t *dot = lv_obj_create(lt.dots_row);
        lv_obj_remove_style_all(dot);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(dot, LIGHTS_DOT, LIGHTS_DOT);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(LIGHTS_TINT), 0);
        lt.dots[i] = dot;
    }

    lt.empty = label_create(screen, &inter_med_13, 0x9a9aa4, 0);
    lv_label_set_text(lt.empty, "No lights found");

    render();
}
