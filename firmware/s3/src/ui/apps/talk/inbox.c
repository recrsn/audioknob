#include "ui/apps/tools.h"
#include <stdio.h>
#include <string.h>

// Inbox: messages and voice notes as a list (the knob moves the selection, a
// tap opens one; unread ones carry a dot), with hold-to-record a note to self
// under it. An open message shows its sender and text, which the knob
// scrolls, or a play button for a voice note; under it, its receipt, then
// back and delete.

#define INBOX_LIST_H      (262 - TOOL_LIST_TOP)
#define INBOX_LIST_H_FULL (304 - TOOL_LIST_TOP)
#define INBOX_ROW_H       52
#define INBOX_DOT         8
#define INBOX_EMPTY_Y     (-20)
#define INBOX_RECORD_W    176
#define INBOX_RECORD_H    40
#define INBOX_RECORD_Y    106
#define INBOX_SENDER_Y    (-76)
#define INBOX_BODY_Y      (-6)
#define INBOX_BODY_H      104
#define INBOX_LENGTH_Y    42
#define INBOX_RECEIPT_Y   80
#define INBOX_ACTIONS_Y   124
#define INBOX_ACTIONS_X   40
#define INBOX_SCROLL_STEP 20

typedef struct {
    lv_obj_t *row, *name, *preview, *dot;
} message_row_t;

static struct {
    ui_inbox_t *data;
    int selected;
    bool recording;
    char open[17]; // the id of the message open, "" for the list
    lv_obj_t *list, *empty, *empty_hint, *record, *record_text;
    message_row_t rows[UI_INBOX_MAX];
    lv_obj_t *sender, *body, *text, *play, *play_icon, *length, *receipt, *back, *remove;
    void (*on_ack)(const char *), (*on_remove)(const char *);
} ib;

void ui_set_inbox_handler(void (*ack)(const char *), void (*remove)(const char *)) {
    ib.on_ack = ack;
    ib.on_remove = remove;
}

static ui_message_t *open_message(void) {
    for (int i = 0; i < ib.data->count; i++) {
        if (!strcmp(ib.data->messages[i].id, ib.open)) {
            return &ib.data->messages[i];
        }
    }
    return NULL;
}

static bool unread(const ui_message_t *m) {
    return !m->acknowledged && !m->local;
}

static bool playing(const ui_message_t *m) {
    return ui_voice_current()->state == UI_VOICE_PLAYING && !strcmp(ui_voice_current()->id, m->id);
}

static const char *record_text(void) {
    static char buf[32];
    switch (ui_voice_current()->state) {
    case UI_VOICE_RECORDING:
        if (ib.recording) {
            snprintf(buf, sizeof(buf), "Release to save \xC2\xB7 %u s", ui_voice_current()->seconds);
            return buf;
        }
        break;
    case UI_VOICE_SAVING:
        return "Saving\xE2\x80\xA6";
    default:
        break;
    }
    return LV_SYMBOL_PLUS "  Hold to record";
}

static void list_render(void) {
    bool any = ib.data->count > 0;
    bool can_record = ui_voice_current()->storage_ready;
    ui_set_visible(ib.list, any);
    ui_set_visible(ib.empty, !any);
    ui_set_visible(ib.empty_hint, !any);
    ui_set_visible(ib.record, can_record);
    lv_obj_set_height(ib.list, can_record ? INBOX_LIST_H : INBOX_LIST_H_FULL);
    ui_label_set_text(ib.record_text, record_text());
    lv_obj_set_style_bg_opa(ib.record, ib.recording ? LV_OPA_COVER : UI_OPA_TINTED, 0);
    for (int i = 0; i < UI_INBOX_MAX; i++) {
        message_row_t *r = &ib.rows[i];
        ui_set_visible(r->row, i < ib.data->count);
        if (i >= ib.data->count) {
            continue;
        }
        const ui_message_t *m = &ib.data->messages[i];
        ui_label_set_text(r->name, m->sender);
        ui_label_set_text(r->preview, m->pending ? "Sending\xE2\x80\xA6" : m->text);
        lv_obj_set_style_text_color(r->name, lv_color_hex(unread(m) ? UI_COLOR_TEXT : UI_COLOR_TEXT_DIM), 0);
        ui_set_visible(r->dot, unread(m));
        lv_obj_set_style_bg_opa(r->row, i == ib.selected ? UI_OPA_SELECTED : LV_OPA_TRANSP, 0);
    }
    tool_list_fade(ib.list, ib.selected, ib.data->count);
}

static const char *receipt_text(const ui_message_t *m) {
    if (m->local) {
        return m->pending ? "Sending\xE2\x80\xA6" : "";
    }
    // Opening a message acknowledges it; only a failed receipt is tapped.
    return m->acknowledged ? LV_SYMBOL_OK " Acknowledged"
           : m->failed     ? LV_SYMBOL_REFRESH " Tap to acknowledge again"
                           : "Acknowledging\xE2\x80\xA6";
}

static void message_render(const ui_message_t *m) {
    ui_label_set_text(ib.sender, m->sender);
    ui_set_visible(ib.body, !m->voice);
    ui_set_visible(ib.play, m->voice);
    ui_set_visible(ib.length, m->voice);
    if (m->voice) {
        char length[16];
        uint32_t s = (m->duration_ms + 500) / 1000;
        snprintf(length, sizeof(length), "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
        ui_label_set_text(ib.length, length);
        ui_label_set_text(ib.play_icon, playing(m) ? LV_SYMBOL_STOP : LV_SYMBOL_PLAY);
    } else {
        ui_label_set_text(ib.text, m->text);
    }
    ui_label_set_text(ib.receipt, receipt_text(m));
    lv_obj_set_style_text_color(ib.receipt, lv_color_hex(m->failed ? UI_TINT_TALK : UI_COLOR_TEXT_DIM), 0);
    ui_set_visible(ib.remove, !m->pending);
}

static void render(void) {
    ui_message_t *m = open_message();
    lv_obj_t *message_objs[] = {ib.sender, ib.body, ib.play, ib.length, ib.receipt, ib.back, ib.remove};
    lv_obj_t *list_objs[] = {ib.list, ib.empty, ib.empty_hint, ib.record};
    for (size_t i = 0; i < sizeof(message_objs) / sizeof(message_objs[0]); i++) {
        ui_set_visible(message_objs[i], m);
    }
    if (m) {
        for (size_t i = 0; i < sizeof(list_objs) / sizeof(list_objs[0]); i++) {
            ui_set_visible(list_objs[i], false);
        }
        message_render(m);
    } else {
        ib.open[0] = 0;
        list_render();
    }
}

static void row_click(lv_event_t *e) {
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    if (index >= ib.data->count) {
        return;
    }
    ui_message_t *m = &ib.data->messages[index];
    ib.selected = index;
    snprintf(ib.open, sizeof(ib.open), "%s", m->id);
    if (unread(m) && !m->pending && !m->failed && ib.on_ack) {
        ib.on_ack(m->id);
    }
    lv_obj_scroll_to_y(ib.body, 0, LV_ANIM_OFF);
    render();
}

static void list_scroll_end_cb(lv_event_t *e) {
    (void)e;
    if (ib.data->count) {
        ib.selected = tool_list_in_view(ib.list, ib.selected, ib.data->count);
        list_render();
    }
}

static void receipt_click(lv_event_t *e) {
    (void)e;
    ui_message_t *m = open_message();
    if (m && m->failed && !m->pending && !m->acknowledged && ib.on_ack) {
        ib.on_ack(m->id);
    }
}

static void remove_confirmed(void) {
    ui_message_t *m = open_message();
    if (m && ib.on_remove) {
        ib.on_remove(m->id);
    }
}

static void remove_click(lv_event_t *e) {
    (void)e;
    ui_message_t *m = open_message();
    if (m) {
        ui_confirm(LV_SYMBOL_TRASH, "Delete message?", m->voice ? "The voice note is deleted from this knob."
                                                                : "It's deleted from this knob.",
                   remove_confirmed);
    }
}

static void play_click(lv_event_t *e) {
    (void)e;
    ui_message_t *m = open_message();
    if (!m) {
        return;
    }
    if (ui_voice_current()->state == UI_VOICE_PLAYING) {
        ui_voice_stop();
    } else if (ui_voice_current()->state == UI_VOICE_IDLE) {
        ui_voice_play(m->id);
    }
}

static void record_event(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED && ui_voice_current()->state == UI_VOICE_IDLE) {
        ib.recording = true;
        ui_voice_record(0, true);
        render();
    } else if ((code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) && ib.recording) {
        ib.recording = false;
        ui_voice_record(0, false);
        render();
    }
}

static void unloaded(lv_event_t *e) {
    (void)e;
    if (ib.recording) {
        ib.recording = false;
        ui_voice_record(0, false);
    }
}

void ui_inbox_voice_changed(void) {
    if (ib.data) {
        render();
    }
}

static void back_click(lv_event_t *e) {
    (void)e;
    ib.open[0] = 0;
    render();
    if (ib.data->count) {
        tool_list_center(ib.list, ib.selected, LV_ANIM_OFF);
    }
}

static bool rotate(int steps) {
    if (open_message()) {
        lv_obj_scroll_by_bounded(ib.body, 0, -steps * INBOX_SCROLL_STEP, LV_ANIM_ON);
    } else if (ib.data->count) {
        ib.selected = LV_CLAMP(0, ib.selected + steps, ib.data->count - 1);
        list_render();
        tool_list_center(ib.list, ib.selected, LV_ANIM_ON);
    }
    return true;
}

static bool status(char *buf, size_t len) {
    if (ui_voice_current()->state != UI_VOICE_IDLE) {
        snprintf(buf, len, "%s",
                 ui_voice_current()->state == UI_VOICE_RECORDING ? "Recording"
                 : ui_voice_current()->state == UI_VOICE_PLAYING ? "Playing note"
                                                                 : "Saving note");
        return true;
    }
    int count = 0;
    for (int i = 0; i < ib.data->count; i++) {
        count += unread(&ib.data->messages[i]);
    }
    if (!count) {
        return false;
    }
    snprintf(buf, len, "%d message%s", count, count == 1 ? "" : "s");
    return true;
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, text);
    return l;
}

void ui_update_inbox(const ui_inbox_t *data) {
    if (ib.data && memcmp(data, ib.data, sizeof(*data))) {
        *ib.data = *data;
        if (ib.selected >= data->count) {
            ib.selected = data->count ? data->count - 1 : 0;
        }
        render();
    }
}

static void list_build(lv_obj_t *screen) {
    ib.list = tool_list_create(screen, INBOX_LIST_H);
    lv_obj_add_event_cb(ib.list, list_scroll_end_cb, LV_EVENT_SCROLL_END, NULL);
    for (int i = 0; i < UI_INBOX_MAX; i++) {
        message_row_t *r = &ib.rows[i];
        r->row = tool_list_row_create(ib.list, INBOX_ROW_H, &r->name, &r->preview);
        ui_set_visible(r->preview, true);
        lv_obj_add_event_cb(r->row, row_click, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        r->dot = lv_obj_create(r->row);
        lv_obj_remove_style_all(r->dot);
        lv_obj_set_size(r->dot, INBOX_DOT, INBOX_DOT);
        lv_obj_set_style_radius(r->dot, UI_RADIUS_PILL, 0);
        lv_obj_set_style_bg_color(r->dot, lv_color_hex(UI_TINT_TALK), 0);
        lv_obj_set_style_bg_opa(r->dot, LV_OPA_COVER, 0);
    }
    ib.empty = label(screen, UI_FONT_TITLE, UI_COLOR_TEXT, "No messages");
    lv_obj_align(ib.empty, LV_ALIGN_CENTER, 0, INBOX_EMPTY_Y);
    ib.empty_hint = label(screen, UI_FONT_BODY, UI_COLOR_TEXT_DIM, "Messages and voice notes from\nother knobs land here");
    lv_obj_set_width(ib.empty_hint, UI_TEXT_W);
    lv_obj_set_style_text_align(ib.empty_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align_to(ib.empty_hint, ib.empty, LV_ALIGN_OUT_BOTTOM_MID, 0, UI_SPACE_S);

    ib.record = lv_obj_create(screen);
    lv_obj_remove_style_all(ib.record);
    lv_obj_remove_flag(ib.record, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ib.record, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(ib.record, INBOX_RECORD_W, INBOX_RECORD_H);
    lv_obj_set_ext_click_area(ib.record, (UI_TOUCH_MIN - INBOX_RECORD_H) / 2);
    lv_obj_set_style_radius(ib.record, UI_RADIUS_PILL, 0);
    lv_obj_set_style_bg_color(ib.record, lv_color_hex(UI_TINT_TALK), 0);
    lv_obj_set_style_border_width(ib.record, 1, 0);
    lv_obj_set_style_border_color(ib.record, lv_color_hex(UI_TINT_TALK), 0);
    lv_obj_align(ib.record, LV_ALIGN_CENTER, 0, INBOX_RECORD_Y);
    lv_obj_add_event_cb(ib.record, record_event, LV_EVENT_ALL, NULL);
    ib.record_text = label(ib.record, UI_FONT_LABEL, UI_COLOR_TEXT, "");
    lv_obj_center(ib.record_text);
}

static void message_build(lv_obj_t *screen) {
    ib.sender = label(screen, UI_FONT_TITLE, UI_COLOR_TEXT, "");
    lv_obj_set_width(ib.sender, UI_TEXT_W);
    lv_label_set_long_mode(ib.sender, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(ib.sender, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(ib.sender, LV_ALIGN_CENTER, 0, INBOX_SENDER_Y);

    ib.body = lv_obj_create(screen);
    lv_obj_remove_style_all(ib.body);
    lv_obj_set_size(ib.body, UI_TEXT_W, INBOX_BODY_H);
    lv_obj_align(ib.body, LV_ALIGN_CENTER, 0, INBOX_BODY_Y);
    lv_obj_set_scroll_dir(ib.body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(ib.body, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(ib.body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ib.body, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    ib.text = label(ib.body, UI_FONT_TITLE, UI_COLOR_TEXT, "");
    lv_obj_set_width(ib.text, lv_pct(100));
    lv_label_set_long_mode(ib.text, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(ib.text, LV_TEXT_ALIGN_CENTER, 0);

    ib.play_icon = ui_round_button_create(screen, LV_SYMBOL_PLAY, UI_BUTTON_SIZE, UI_FONT_SYMBOL_LG, false);
    ib.play = lv_obj_get_parent(ib.play_icon);
    lv_obj_set_style_bg_color(ib.play, lv_color_hex(UI_TINT_TALK), 0);
    lv_obj_add_flag(ib.play, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ib.play, play_click, LV_EVENT_CLICKED, NULL);
    lv_obj_align(ib.play, LV_ALIGN_CENTER, 0, INBOX_BODY_Y - UI_SPACE_S);
    ib.length = label(screen, UI_FONT_READING_SM, UI_COLOR_TEXT_DIM, "");
    lv_obj_align(ib.length, LV_ALIGN_CENTER, 0, INBOX_LENGTH_Y);

    ib.receipt = label(screen, UI_FONT_CAPTION, UI_COLOR_TEXT_DIM, "");
    lv_obj_align(ib.receipt, LV_ALIGN_CENTER, 0, INBOX_RECEIPT_Y);
    lv_obj_add_flag(ib.receipt, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(ib.receipt, UI_TOUCH_EXT);
    lv_obj_add_event_cb(ib.receipt, receipt_click, LV_EVENT_CLICKED, NULL);

    ib.back = tool_icon_button_create(screen, LV_SYMBOL_LEFT, back_click);
    lv_obj_align(ib.back, LV_ALIGN_CENTER, -INBOX_ACTIONS_X, INBOX_ACTIONS_Y);
    ib.remove = tool_icon_button_create(screen, LV_SYMBOL_TRASH, remove_click);
    lv_obj_set_style_text_color(lv_obj_get_child(ib.remove, 0), lv_color_hex(UI_COLOR_DANGER), 0);
    lv_obj_align(ib.remove, LV_ALIGN_CENTER, INBOX_ACTIONS_X, INBOX_ACTIONS_Y);
}

void inbox_page_build(lv_obj_t *screen) {
    ib.data = lv_malloc_zeroed(sizeof(*ib.data));
    LV_ASSERT_MALLOC(ib.data);
    tool_page_register(screen, &(tool_page_desc_t){
                                   .name = "Inbox", .icon = ICON_INBOX, .tint = UI_TINT_TALK, .rotate = rotate, .status = status});
    lv_obj_t *title = tool_title_create(screen, "INBOX", lv_color_hex(UI_TINT_TALK));
    lv_obj_align(title, LV_ALIGN_CENTER, 0, UI_TITLE_TOP_Y);
    list_build(screen);
    message_build(screen);
    lv_obj_add_event_cb(screen, unloaded, LV_EVENT_SCREEN_UNLOAD_START, NULL);
    render();
}
