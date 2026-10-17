#include "ui_tools.h"
#include <stdio.h>
#include <string.h>

#define TALK_LIST_W 264
#define TALK_LIST_Y (UI_SAFE_TOP + 26)
#define TALK_LIST_H 194
#define TALK_ROW_H 48
#define TALK_HOLD_SIZE 112

static struct {
    ui_talk_t data;
    uint32_t selected;
    bool held, note_held;
    lv_obj_t *screen, *list, *empty, *peer, *hint, *hold, *hold_text, *back, *ring, *volume, *note;
    lv_obj_t *rows[UI_TALK_MAX], *names[UI_TALK_MAX];
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
        ui_voice_record(tk.selected, false);
    }
    if (tk.held) {
        tk.held = false;
        if (tk.press)
            tk.press(tk.selected, false);
    }
}
static const char *selected_name(void) {
    for (int i = 0; i < tk.data.count; i++)
        if (tk.data.peers[i].id == tk.selected)
            return tk.data.peers[i].name;
    return NULL;
}
static void render(void) {
    bool incoming = tk.data.state == UI_TALK_RECEIVING;
    bool panel = tk.selected || incoming;
    const char *name = incoming ? tk.data.peer : selected_name();
    ui_set_visible(tk.list, !panel && tk.data.count);
    ui_set_visible(tk.empty, !panel && !tk.data.count);
    ui_set_visible(tk.note, panel && !incoming && ui_voice_current()->storage_ready);
    ui_set_visible(tk.peer, panel);
    ui_set_visible(tk.hold, panel);
    ui_set_visible(tk.back, panel && tk.data.state == UI_TALK_IDLE);
    ui_set_visible(tk.ring, panel);
    ui_set_visible(tk.volume, panel);
    for (int i = 0; i < UI_TALK_MAX; i++) {
        ui_set_visible(tk.rows[i], i < tk.data.count);
        if (i < tk.data.count)
            ui_label_set_text(tk.names[i], tk.data.peers[i].name);
    }
    ui_label_set_text(tk.peer, name ? name : "Knob unavailable");
    const char *text = incoming                              ? "Listening"
                       : tk.data.state == UI_TALK_SENDING    ? "Talking"
                       : tk.data.state == UI_TALK_CONNECTING ? "Connecting"
                                                             : "Hold to talk";
    ui_label_set_text(tk.hold_text, text);
    bool ready = name && !incoming && ui_voice_current()->state == UI_VOICE_IDLE;
    if (ready)
        lv_obj_remove_state(tk.hold, LV_STATE_DISABLED);
    else
        lv_obj_add_state(tk.hold, LV_STATE_DISABLED);
    lv_obj_set_style_bg_opa(tk.hold, tk.held || incoming ? UI_OPA_TINTED : UI_OPA_GLASS, 0);
    ui_label_set_text(tk.hint, tk.data.message[0] ? tk.data.message
                               : incoming         ? "Listen, then hold to reply"
                               : panel            ? "Release to listen"
                               : tk.data.count    ? "Choose a knob"
                                                  : "Looking for knobs on your WiFi");
    ui_label_set_text(tk.note, tk.note_held ? "Release to send note" : "Hold for a voice note");
    tool_ring_set(tk.ring, ui_voice_current()->state == UI_VOICE_RECORDING ? ui_voice_current()->level
                           : tk.data.state == UI_TALK_SENDING || incoming  ? tk.data.level
                                                                           : 0);
    char volume[24];
    snprintf(volume, sizeof(volume), "Volume %u%%", tk.data.volume);
    ui_label_set_text(tk.volume, volume);
}
static void row_click(lv_event_t *e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < tk.data.count) {
        tk.selected = tk.data.peers[i].id;
        render();
    }
}
static void back_click(lv_event_t *e) {
    (void)e;
    release();
    tk.selected = 0;
    render();
}
static void hold_event(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED && tk.data.state == UI_TALK_IDLE && selected_name()) {
        tk.held = true;
        if (tk.press)
            tk.press(tk.selected, true);
        render();
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        release();
        render();
    }
}
static void note_event(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED && tk.data.state == UI_TALK_IDLE && ui_voice_current()->state == UI_VOICE_IDLE &&
        selected_name()) {
        tk.note_held = true;
        ui_voice_record(tk.selected, true);
        render();
    } else if ((code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) && tk.note_held) {
        tk.note_held = false;
        ui_voice_record(tk.selected, false);
        render();
    }
}
void ui_talk_voice_changed(void) {
    if (tk.screen)
        render();
}
static void unloaded(lv_event_t *e) {
    (void)e;
    release();
}
static bool rotate(int steps) {
    int v = tk.data.volume + steps;
    tk.data.volume = v < 0 ? 0 : v > 100 ? 100 : v;
    if (tk.set_volume)
        tk.set_volume(tk.data.volume);
    render();
    return true;
}
static bool status(char *buf, size_t len) {
    if (tk.note_held) {
        snprintf(buf, len, "Recording note");
        return true;
    }
    if (tk.data.state == UI_TALK_IDLE)
        return false;
    snprintf(buf, len, "%s", tk.data.state == UI_TALK_RECEIVING ? "Listening" : "Talking");
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
    if (!tk.screen || !memcmp(data, &tk.data, sizeof(*data)))
        return;
    tk.data = *data;
    if (data->state == UI_TALK_RECEIVING && data->peer_id)
        tk.selected = data->peer_id;
    render();
}
void talk_page_build(lv_obj_t *screen) {
    tk.screen = screen;
    tool_page_register(
        screen, &(tool_page_desc_t){
                    .name = "Talk", .icon = ICON_TALK, .tint = UI_TINT_TALK, .rotate = rotate, .status = status});
    lv_obj_t *title = tool_title_create(screen, "TALK", lv_color_hex(UI_TINT_TALK));
    lv_obj_align(title, LV_ALIGN_CENTER, 0, UI_TITLE_TOP_Y);
    tk.list = lv_obj_create(screen);
    lv_obj_remove_style_all(tk.list);
    lv_obj_set_size(tk.list, TALK_LIST_W, TALK_LIST_H);
    lv_obj_set_flex_flow(tk.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(tk.list, UI_SPACE_XS, 0);
    lv_obj_set_scroll_dir(tk.list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(tk.list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(tk.list, LV_ALIGN_TOP_MID, 0, TALK_LIST_Y);
    for (int i = 0; i < UI_TALK_MAX; i++) {
        lv_obj_t *row = lv_obj_create(tk.list);
        tk.rows[i] = row;
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), TALK_ROW_H);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_radius(row, UI_RADIUS_PILL, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(UI_COLOR_TEXT), 0);
        lv_obj_set_style_bg_opa(row, UI_OPA_GLASS, 0);
        lv_obj_add_event_cb(row, row_click, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        tk.names[i] = label(row, UI_FONT_TITLE, UI_COLOR_TEXT, "");
        lv_obj_set_width(tk.names[i], TALK_LIST_W - UI_SPACE_L * 2);
        lv_label_set_long_mode(tk.names[i], LV_LABEL_LONG_MODE_DOTS);
        lv_obj_center(tk.names[i]);
    }
    tk.empty = label(screen, UI_FONT_TITLE, UI_COLOR_TEXT, "No other knobs yet");
    lv_obj_center(tk.empty);
    tk.peer = label(screen, UI_FONT_TITLE, UI_COLOR_TEXT, "");
    lv_obj_set_width(tk.peer, UI_TEXT_W);
    lv_label_set_long_mode(tk.peer, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(tk.peer, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(tk.peer, LV_ALIGN_CENTER, 0, -66);
    tk.hold = lv_obj_create(screen);
    lv_obj_remove_style_all(tk.hold);
    lv_obj_set_size(tk.hold, TALK_HOLD_SIZE - UI_SPACE_L, TALK_HOLD_SIZE - UI_SPACE_L);
    lv_obj_align(tk.hold, LV_ALIGN_CENTER, 0, -UI_SPACE_S);
    lv_obj_set_style_radius(tk.hold, UI_RADIUS_PILL, 0);
    lv_obj_set_style_bg_color(tk.hold, lv_color_hex(UI_TINT_TALK), 0);
    lv_obj_add_flag(tk.hold, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_remove_flag(tk.hold, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_add_event_cb(tk.hold, hold_event, LV_EVENT_ALL, NULL);
    tk.hold_text = label(tk.hold, UI_FONT_LABEL, UI_COLOR_TEXT, "Hold to talk");
    lv_obj_center(tk.hold_text);
    tk.back = label(screen, UI_FONT_LABEL, UI_COLOR_TEXT_DIM, "Other knobs");
    lv_obj_align(tk.back, LV_ALIGN_CENTER, 0, 120);
    lv_obj_add_flag(tk.back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(tk.back, UI_TOUCH_EXT);
    lv_obj_add_event_cb(tk.back, back_click, LV_EVENT_CLICKED, NULL);
    tk.hint = label(screen, UI_FONT_CAPTION, UI_COLOR_TEXT_DIM, "");
    lv_obj_set_width(tk.hint, UI_TEXT_W);
    lv_obj_set_style_text_align(tk.hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(tk.hint, LV_ALIGN_CENTER, 0, 54);
    tk.volume = label(screen, UI_FONT_CAPTION, UI_COLOR_TEXT_DIM, "");
    lv_obj_align(tk.volume, LV_ALIGN_CENTER, 0, -90);
    tk.note = label(screen, UI_FONT_LABEL, UI_TINT_TALK, "Hold for a voice note");
    lv_obj_align(tk.note, LV_ALIGN_CENTER, 0, 88);
    lv_obj_add_flag(tk.note, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(tk.note, UI_TOUCH_EXT);
    lv_obj_add_event_cb(tk.note, note_event, LV_EVENT_ALL, NULL);
    tk.ring = tool_ring_create(screen, lv_color_hex(UI_TINT_TALK), 100);
    lv_obj_add_event_cb(screen, unloaded, LV_EVENT_SCREEN_UNLOAD_START, NULL);
    render();
}
