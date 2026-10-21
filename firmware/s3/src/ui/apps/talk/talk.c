#include "ui/apps/tools.h"
#include <stdio.h>
#include <string.h>

// Talk: the knobs on this WiFi as a list (the knob moves the selection, a
// tap opens one), then one knob as a walkie-talkie: a big push-to-talk button
// under its name, ringed by the mic or speaker level while audio flows. Under
// it, back to the list and hold-to-record a voice note for that knob. The
// knob is Talk's volume there, shown in place of the title while it turns.

#define TALK_LIST_H    (304 - TOOL_LIST_TOP)
#define TALK_ROW_H     48
#define TALK_EMPTY_Y   (-8)
#define TALK_NAME_Y    (-80)
#define TALK_PTT_SIZE  104
#define TALK_PTT_Y     4
#define TALK_PTT_LEVEL 12 // the level ring's widest, past the button's edge
#define TALK_STATE_Y   80
#define TALK_ACTIONS_Y 124
#define TALK_ACTIONS_X 40

typedef enum { TALK_LIST, TALK_PEER } talk_view_t;

static struct {
    ui_talk_t data;
    uint32_t peer; // the knob open, 0 for the list
    int selected;  // the list's selection
    bool held, note_held;
    lv_obj_t *screen, *title, *list, *empty, *empty_hint;
    lv_obj_t *rows[UI_TALK_MAX], *names[UI_TALK_MAX], *details[UI_TALK_MAX];
    lv_obj_t *name, *ptt, *ptt_icon, *state, *back, *note;
    void (*press)(uint32_t peer, bool held);
    void (*set_volume)(uint8_t volume);
} tk;

void ui_set_talk_handler(void (*press)(uint32_t, bool), void (*volume)(uint8_t)) {
    tk.press = press;
    tk.set_volume = volume;
}

static void release(void) {
    if (tk.note_held) {
        tk.note_held = false;
        ui_voice_record(tk.peer, false);
    }
    if (tk.held) {
        tk.held = false;
        if (tk.press) {
            tk.press(tk.peer, false);
        }
    }
}

static const char *peer_name(uint32_t id) {
    for (int i = 0; i < tk.data.count; i++) {
        if (tk.data.peers[i].id == id) {
            return tk.data.peers[i].name;
        }
    }
    return NULL;
}

static bool incoming(void) {
    return tk.data.state == UI_TALK_RECEIVING;
}

static const char *state_text(void) {
    if (tk.note_held) {
        return "Release to send the note";
    }
    switch (tk.data.state) {
    case UI_TALK_RECEIVING:
        return "Listening";
    case UI_TALK_SENDING:
        return "Talking";
    case UI_TALK_CONNECTING:
        return "Connecting\xE2\x80\xA6";
    case UI_TALK_IDLE:
        break;
    }
    if (tk.data.message[0]) {
        return tk.data.message;
    }
    if (ui_voice_current()->state == UI_VOICE_SAVING) {
        return "Sending note\xE2\x80\xA6";
    }
    return peer_name(tk.peer) ? "Hold to talk" : "Knob unavailable";
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

static void ptt_style(void) {
    bool sending = tk.held || tk.note_held || tk.data.state == UI_TALK_SENDING;
    bool live = sending || incoming() || ui_voice_current()->state == UI_VOICE_RECORDING;
    bool ready = peer_name(tk.peer) && !incoming() && ui_voice_current()->state == UI_VOICE_IDLE;
    uint32_t tint = UI_TINT_TALK;
    lv_obj_set_style_bg_color(tk.ptt, lv_color_hex(incoming() ? UI_COLOR_SURFACE : tint), 0);
    lv_obj_set_style_bg_opa(tk.ptt, sending || incoming() ? LV_OPA_COVER : UI_OPA_TINTED, 0);
    lv_obj_set_style_border_opa(tk.ptt, sending || incoming() ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(tk.ptt_icon, lv_color_hex(incoming() ? tint : UI_COLOR_TEXT), 0);
    lv_obj_set_style_opa(tk.ptt, ready || live ? LV_OPA_COVER : UI_OPA_DISABLED, 0);
    // The level as a halo around the button: the mic's while sending, the
    // other knob's voice while listening.
    int level = ui_voice_current()->state == UI_VOICE_RECORDING ? ui_voice_current()->level
                : live                                           ? tk.data.level
                                                                 : 0;
    lv_obj_set_style_outline_width(tk.ptt, level * TALK_PTT_LEVEL / 100, 0);
}

static void peer_render(void) {
    bool busy = tk.data.state != UI_TALK_IDLE || tk.note_held || ui_voice_current()->state != UI_VOICE_IDLE;
    const char *name = incoming() ? tk.data.peer : peer_name(tk.peer);
    ui_label_set_text(tk.name, name ? name : "");
    ui_label_set_text(tk.state, state_text());
    ui_set_visible(tk.back, !busy);
    ui_set_visible(tk.note, (!busy || tk.note_held) && ui_voice_current()->storage_ready && peer_name(tk.peer));
    ptt_style();
}

static void render(void) {
    talk_view_t view = tk.peer ? TALK_PEER : TALK_LIST;
    lv_obj_t *peer_objs[] = {tk.name, tk.ptt, tk.state, tk.back, tk.note};
    for (size_t i = 0; i < sizeof(peer_objs) / sizeof(peer_objs[0]); i++) {
        ui_set_visible(peer_objs[i], view == TALK_PEER);
    }
    if (view == TALK_PEER) {
        ui_set_visible(tk.list, false);
        ui_set_visible(tk.empty, false);
        ui_set_visible(tk.empty_hint, false);
        peer_render();
    } else {
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
    release();
    tk.peer = 0;
    render();
    if (tk.data.count) {
        tool_list_center(tk.list, tk.selected, LV_ANIM_OFF);
    }
}

static void ptt_event(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED && tk.data.state == UI_TALK_IDLE && ui_voice_current()->state == UI_VOICE_IDLE &&
        peer_name(tk.peer)) {
        tk.held = true;
        if (tk.press) {
            tk.press(tk.peer, true);
        }
        render();
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        release();
        render();
    }
}

static void note_event(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED && tk.data.state == UI_TALK_IDLE && ui_voice_current()->state == UI_VOICE_IDLE &&
        peer_name(tk.peer)) {
        tk.note_held = true;
        ui_voice_record(tk.peer, true);
        render();
    } else if ((code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) && tk.note_held) {
        tk.note_held = false;
        ui_voice_record(tk.peer, false);
        render();
    }
}

void ui_talk_voice_changed(void) {
    if (tk.screen) {
        render();
    }
}

static void unloaded(lv_event_t *e) {
    (void)e;
    release();
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
    if (tk.note_held) {
        snprintf(buf, len, "Recording note");
        return true;
    }
    if (tk.data.state == UI_TALK_IDLE) {
        return false;
    }
    snprintf(buf, len, "%s", incoming() ? "Listening" : "Talking");
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
    // A call coming in opens its knob.
    if (data->state == UI_TALK_RECEIVING && data->peer_id) {
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

static void peer_build(lv_obj_t *screen) {
    tk.name = label(screen, UI_FONT_TITLE, UI_COLOR_TEXT, "");
    lv_obj_set_width(tk.name, UI_TEXT_W);
    lv_label_set_long_mode(tk.name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(tk.name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(tk.name, LV_ALIGN_CENTER, 0, TALK_NAME_Y);

    tk.ptt = lv_obj_create(screen);
    lv_obj_remove_style_all(tk.ptt);
    lv_obj_set_size(tk.ptt, TALK_PTT_SIZE, TALK_PTT_SIZE);
    lv_obj_align(tk.ptt, LV_ALIGN_CENTER, 0, TALK_PTT_Y);
    lv_obj_set_style_radius(tk.ptt, UI_RADIUS_PILL, 0);
    lv_obj_set_style_border_width(tk.ptt, 2, 0);
    lv_obj_set_style_border_color(tk.ptt, lv_color_hex(UI_TINT_TALK), 0);
    lv_obj_set_style_outline_color(tk.ptt, lv_color_hex(UI_TINT_TALK), 0);
    lv_obj_set_style_outline_opa(tk.ptt, UI_OPA_TINTED, 0);
    lv_obj_remove_flag(tk.ptt, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(tk.ptt, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(tk.ptt, ptt_event, LV_EVENT_ALL, NULL);
    tk.ptt_icon = label(tk.ptt, UI_FONT_ICON, UI_COLOR_TEXT, ICON_TALK);
    lv_obj_center(tk.ptt_icon);

    tk.state = label(screen, UI_FONT_LABEL, UI_COLOR_TEXT_DIM, "");
    lv_obj_set_width(tk.state, UI_TEXT_W);
    lv_label_set_long_mode(tk.state, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(tk.state, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(tk.state, LV_ALIGN_CENTER, 0, TALK_STATE_Y);

    tk.back = tool_icon_button_create(screen, LV_SYMBOL_LEFT, back_click);
    lv_obj_align(tk.back, LV_ALIGN_CENTER, -TALK_ACTIONS_X, TALK_ACTIONS_Y);
    // Hold for a voice note: an envelope, since it lands in their inbox.
    tk.note = tool_icon_button_create(screen, ICON_INBOX, NULL);
    lv_obj_t *icon = lv_obj_get_child(tk.note, 0);
    lv_obj_set_style_text_font(icon, UI_FONT_ICON_SM, 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(UI_TINT_TALK), 0);
    lv_obj_add_event_cb(tk.note, note_event, LV_EVENT_ALL, NULL);
    lv_obj_align(tk.note, LV_ALIGN_CENTER, TALK_ACTIONS_X, TALK_ACTIONS_Y);
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
    lv_obj_add_event_cb(screen, unloaded, LV_EVENT_SCREEN_UNLOAD_START, NULL);
    render();
}
