#include "ui/apps/tools.h"
#include <stdio.h>
#include <string.h>

// Talk: the knobs on this WiFi as a list (the knob moves the selection, a
// tap opens one), then one knob as a call screen: an avatar (rings pulsing
// out of it while a call is placed or rings), the knob's name, the call's
// state or time, and a row
// of round buttons: Back and Call, Decline and Accept while ringing, or one
// End button. The knob is the call's volume there, shown in place of the
// title while it turns.

#define TALK_LIST_H    (304 - TOOL_LIST_TOP)
#define TALK_ROW_H     48
#define TALK_EMPTY_Y   (-8)
#define TALK_AVATAR    96
#define TALK_AVATAR_Y  (-36)
#define TALK_PULSE_MAX 132 // a pulse ring's size as it fades out
#define TALK_PULSE_MS  1400
#define TALK_NAME_Y    34
#define TALK_STATE_Y   56
#define TALK_ACTIONS_Y 108
#define TALK_PAIR_X    44 // either side of centre for a pair of buttons
#define TALK_END_ANGLE 2250 // the handset laid down, in 0.1 degrees

typedef enum { TALK_LIST, TALK_PEER } talk_view_t;

static struct {
    ui_talk_t data;
    uint32_t peer; // the knob open, 0 for the list
    int selected;  // the list's selection
    bool pulsing;
    lv_obj_t *screen, *title, *list, *empty, *empty_hint;
    lv_obj_t *rows[UI_TALK_MAX], *names[UI_TALK_MAX], *details[UI_TALK_MAX];
    lv_obj_t *pulse, *avatar, *name, *state;
    lv_obj_t *back, *call, *accept, *decline, *end;
    void (*dial)(uint32_t peer);
    void (*answer)(bool accept);
    void (*set_volume)(uint8_t volume);
} tk;

void ui_set_talk_handler(void (*call)(uint32_t), void (*answer)(bool), void (*volume)(uint8_t)) {
    tk.dial = call;
    tk.answer = answer;
    tk.set_volume = volume;
}

static const char *peer_name(uint32_t id) {
    for (int i = 0; i < tk.data.count; i++) {
        if (tk.data.peers[i].id == id) {
            return tk.data.peers[i].name;
        }
    }
    return NULL;
}

static bool busy(void) {
    return tk.data.state != UI_TALK_IDLE;
}

static bool ringing(void) {
    return tk.data.state == UI_TALK_RINGING;
}

static void call_time(char *buf, size_t len, const char *prefix) {
    snprintf(buf, len, "%s%lu:%02lu", prefix, (unsigned long)(tk.data.seconds / 60),
             (unsigned long)(tk.data.seconds % 60));
}

static const char *state_text(char *buf, size_t len) {
    switch (tk.data.state) {
    case UI_TALK_CALLING:
        return "Calling\xE2\x80\xA6";
    case UI_TALK_RINGING:
        return "Incoming call";
    case UI_TALK_IN_CALL:
        call_time(buf, len, "");
        return buf;
    case UI_TALK_IDLE:
        break;
    }
    if (tk.data.message[0]) {
        return tk.data.message;
    }
    return peer_name(tk.peer) ? "" : "Knob unavailable";
}

static void list_render(void) {
    ui_set_visible(tk.list, tk.data.count);
    ui_set_visible(tk.empty, !tk.data.count);
    ui_set_visible(tk.empty_hint, !tk.data.count);
    for (int i = 0; i < UI_TALK_MAX; i++) {
        ui_set_visible(tk.rows[i], i < tk.data.count);
        if (i < tk.data.count) {
            ui_label_set_text(tk.names[i], tk.data.peers[i].name);
            lv_obj_set_style_bg_opa(tk.rows[i], i == tk.selected ? UI_OPA_SELECTED : LV_OPA_TRANSP, 0);
        }
    }
    tool_list_fade(tk.list, tk.selected, tk.data.count);
}

static void pulse_cb(void *obj, int32_t v) {
    int32_t size = TALK_AVATAR + (TALK_PULSE_MAX - TALK_AVATAR) * v / 255;
    lv_obj_set_size(obj, size, size);
    lv_obj_set_style_border_opa(obj, (lv_opa_t)(LV_OPA_80 * (255 - v) / 255), 0);
}

// Rings pulse out of the avatar while a call is placed or rings.
static void pulse_set(bool on) {
    if (on == tk.pulsing) {
        return;
    }
    tk.pulsing = on;
    ui_set_visible(tk.pulse, on);
    lv_anim_delete(tk.pulse, pulse_cb);
    if (on) {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, tk.pulse);
        lv_anim_set_exec_cb(&a, pulse_cb);
        lv_anim_set_values(&a, 0, 255);
        lv_anim_set_duration(&a, TALK_PULSE_MS);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&a);
    }
}

static void peer_render(void) {
    char buf[16];
    bool idle = !busy(), ring = ringing(), in_call = tk.data.state == UI_TALK_IN_CALL;
    const char *name = idle ? peer_name(tk.peer) : tk.data.peer;
    ui_label_set_text(tk.name, name ? name : "");
    ui_label_set_text(tk.state, state_text(buf, sizeof(buf)));
    ui_set_text_color(tk.state, lv_color_hex(in_call ? UI_COLOR_TEXT : UI_COLOR_TEXT_DIM));
    pulse_set(tk.data.state == UI_TALK_CALLING || ring);

    ui_set_visible(tk.back, idle);
    ui_set_visible(tk.call, idle);
    ui_set_opa(tk.call, peer_name(tk.peer) ? LV_OPA_COVER : UI_OPA_DISABLED);
    ui_set_visible(tk.decline, ring);
    ui_set_visible(tk.accept, ring);
    ui_set_visible(tk.end, busy() && !ring);
}

static void render(void) {
    talk_view_t view = tk.peer ? TALK_PEER : TALK_LIST;
    lv_obj_t *peer_objs[] = {tk.avatar, tk.name, tk.state};
    for (size_t i = 0; i < sizeof(peer_objs) / sizeof(peer_objs[0]); i++) {
        ui_set_visible(peer_objs[i], view == TALK_PEER);
    }
    if (view == TALK_PEER) {
        ui_set_visible(tk.list, false);
        ui_set_visible(tk.empty, false);
        ui_set_visible(tk.empty_hint, false);
        peer_render();
    } else {
        lv_obj_t *buttons[] = {tk.back, tk.call, tk.accept, tk.decline, tk.end};
        for (size_t i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++) {
            ui_set_visible(buttons[i], false);
        }
        pulse_set(false);
        list_render();
    }
}

static void open_peer(uint32_t id) {
    tk.peer = id;
    render();
}

static void row_click(lv_event_t *e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < tk.data.count) {
        tk.selected = i;
        open_peer(tk.data.peers[i].id);
    }
}

static void list_scroll_end_cb(lv_event_t *e) {
    (void)e;
    if (tk.data.count) {
        tk.selected = tool_list_in_view(tk.list, tk.selected, tk.data.count);
        list_render();
    }
}

static void back_click(lv_event_t *e) {
    (void)e;
    tk.peer = 0;
    render();
    if (tk.data.count) {
        tool_list_center(tk.list, tk.selected, LV_ANIM_OFF);
    }
}

static void call_click(lv_event_t *e) {
    (void)e;
    if (!busy() && peer_name(tk.peer) && tk.dial) {
        tk.dial(tk.peer);
    }
}

static void end_click(lv_event_t *e) {
    (void)e;
    if (tk.dial) {
        tk.dial(0);
    }
}

static void answer_click(lv_event_t *e) {
    if (tk.answer) {
        tk.answer(lv_event_get_user_data(e) != NULL);
    }
}

static bool rotate(int steps) {
    if (!tk.peer) {
        if (tk.data.count) {
            tk.selected = LV_CLAMP(0, tk.selected + steps, tk.data.count - 1);
            list_render();
            tool_list_center(tk.list, tk.selected, LV_ANIM_ON);
        }
        return true;
    }
    tk.data.volume = (uint8_t)LV_CLAMP(0, tk.data.volume + steps, 100);
    if (tk.set_volume) {
        tk.set_volume(tk.data.volume);
    }
    char text[24];
    snprintf(text, sizeof(text), "VOLUME %u%%", tk.data.volume);
    tool_title_flash(tk.title, text);
    return true;
}

static bool status(char *buf, size_t len) {
    if (!busy()) {
        return false;
    }
    if (tk.data.state == UI_TALK_IN_CALL) {
        call_time(buf, len, "In call ");
    } else {
        snprintf(buf, len, "%s", ringing() ? "Incoming call" : "Calling\xE2\x80\xA6");
    }
    return true;
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, text);
    return l;
}

void ui_talk_page_update(const ui_talk_t *data) {
    if (!tk.screen || !memcmp(data, &tk.data, sizeof(*data))) {
        return;
    }
    tk.data = *data;
    // A call opens its knob, whichever side placed it.
    if (busy() && data->peer_id) {
        tk.peer = data->peer_id;
    }
    if (tk.selected >= data->count) {
        tk.selected = data->count ? data->count - 1 : 0;
    }
    render();
}

static void list_build(lv_obj_t *screen) {
    tk.list = tool_list_create(screen, TALK_LIST_H);
    lv_obj_add_event_cb(tk.list, list_scroll_end_cb, LV_EVENT_SCROLL_END, NULL);
    for (int i = 0; i < UI_TALK_MAX; i++) {
        tk.rows[i] = tool_list_row_create(tk.list, TALK_ROW_H, &tk.names[i], &tk.details[i]);
        lv_obj_add_event_cb(tk.rows[i], row_click, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        label(tk.rows[i], UI_FONT_ICON_SM, UI_TINT_TALK, ICON_TALK);
    }
    tk.empty = label(screen, UI_FONT_TITLE, UI_COLOR_TEXT, "No other knobs yet");
    lv_obj_align(tk.empty, LV_ALIGN_CENTER, 0, TALK_EMPTY_Y);
    tk.empty_hint = label(screen, UI_FONT_BODY, UI_COLOR_TEXT_DIM, "Knobs on this WiFi show up here");
    lv_obj_set_width(tk.empty_hint, UI_TEXT_W);
    lv_obj_set_style_text_align(tk.empty_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align_to(tk.empty_hint, tk.empty, LV_ALIGN_OUT_BOTTOM_MID, 0, UI_SPACE_S);
}

// A filled round button with a phone (tipped over for ending a call) or a
// symbol on it.
static lv_obj_t *round_button(lv_obj_t *screen, int32_t x, uint32_t fill, lv_opa_t opa, const char *symbol,
                              bool tipped, lv_event_cb_t cb, void *user_data) {
    lv_obj_t *btn = lv_obj_create(screen);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, UI_BUTTON_SIZE, UI_BUTTON_SIZE);
    lv_obj_align(btn, LV_ALIGN_CENTER, x, TALK_ACTIONS_Y);
    lv_obj_set_style_radius(btn, UI_RADIUS_PILL, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(fill), 0);
    lv_obj_set_style_bg_opa(btn, opa, 0);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    lv_obj_t *icon = label(btn, UI_FONT_SYMBOL_LG, UI_COLOR_TEXT, symbol);
    if (tipped) {
        lv_obj_set_style_transform_pivot_x(icon, lv_pct(50), 0);
        lv_obj_set_style_transform_pivot_y(icon, lv_pct(50), 0);
        lv_obj_set_style_transform_rotation(icon, TALK_END_ANGLE, 0);
    }
    lv_obj_center(icon);
    return btn;
}

static lv_obj_t *centered_label(lv_obj_t *screen, const lv_font_t *font, uint32_t color, int32_t y) {
    lv_obj_t *l = label(screen, font, color, "");
    lv_obj_set_width(l, UI_TEXT_W);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, y);
    return l;
}

static void peer_build(lv_obj_t *screen) {
    // Behind the avatar, centred on it as it grows.
    tk.pulse = lv_obj_create(screen);
    lv_obj_remove_style_all(tk.pulse);
    lv_obj_set_size(tk.pulse, TALK_AVATAR, TALK_AVATAR);
    lv_obj_align(tk.pulse, LV_ALIGN_CENTER, 0, TALK_AVATAR_Y);
    lv_obj_set_style_radius(tk.pulse, UI_RADIUS_PILL, 0);
    lv_obj_set_style_border_width(tk.pulse, 3, 0);
    lv_obj_set_style_border_color(tk.pulse, lv_color_hex(UI_TINT_TALK), 0);
    lv_obj_remove_flag(tk.pulse, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    ui_set_visible(tk.pulse, false);

    tk.avatar = lv_obj_create(screen);
    lv_obj_remove_style_all(tk.avatar);
    lv_obj_set_size(tk.avatar, TALK_AVATAR, TALK_AVATAR);
    lv_obj_align(tk.avatar, LV_ALIGN_CENTER, 0, TALK_AVATAR_Y);
    lv_obj_set_style_radius(tk.avatar, UI_RADIUS_PILL, 0);
    lv_obj_set_style_bg_color(tk.avatar, lv_color_hex(UI_TINT_TALK), 0);
    lv_obj_set_style_bg_opa(tk.avatar, UI_OPA_TINTED, 0);
    lv_obj_remove_flag(tk.avatar, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(label(tk.avatar, UI_FONT_ICON, UI_TINT_TALK, ICON_TALK));

    tk.name = centered_label(screen, UI_FONT_TITLE, UI_COLOR_TEXT, TALK_NAME_Y);
    tk.state = centered_label(screen, UI_FONT_BODY, UI_COLOR_TEXT_DIM, TALK_STATE_Y);

    tk.back = round_button(screen, -TALK_PAIR_X, UI_COLOR_SURFACE, LV_OPA_COVER, LV_SYMBOL_LEFT, false, back_click,
                           NULL);
    tk.call = round_button(screen, TALK_PAIR_X, UI_TINT_TALK, LV_OPA_COVER, LV_SYMBOL_CALL, false, call_click, NULL);
    tk.decline = round_button(screen, -TALK_PAIR_X, UI_COLOR_DANGER, LV_OPA_COVER, LV_SYMBOL_CALL, true,
                              answer_click, NULL);
    tk.accept = round_button(screen, TALK_PAIR_X, UI_TINT_TALK, LV_OPA_COVER, LV_SYMBOL_CALL, false, answer_click,
                             (void *)1);
    tk.end = round_button(screen, 0, UI_COLOR_DANGER, LV_OPA_COVER, LV_SYMBOL_CALL, true, end_click, NULL);
}

void talk_page_build(lv_obj_t *screen) {
    tk.screen = screen;
    tool_page_register(
        screen, &(tool_page_desc_t){
                    .name = "Talk", .icon = ICON_TALK, .tint = UI_TINT_TALK, .rotate = rotate, .status = status});
    tk.title = tool_title_create(screen, "TALK", lv_color_hex(UI_TINT_TALK));
    lv_obj_align(tk.title, LV_ALIGN_CENTER, 0, UI_TITLE_TOP_Y);
    list_build(screen);
    peer_build(screen);
    render();
}
