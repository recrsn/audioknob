#include "ui_tools.h"
#include <stdio.h>
#include <string.h>

#define INBOX_LIST_W 264
#define INBOX_LIST_TOP (UI_SAFE_TOP + 26)
#define INBOX_LIST_H 196
#define INBOX_ROW_H 52
static struct {
    ui_inbox_t *data;
    lv_obj_t *list, *empty, *rows[UI_INBOX_MAX], *names[UI_INBOX_MAX], *previews[UI_INBOX_MAX];
    lv_obj_t *sender, *body, *text, *ack, *back, *remove, *play, *record;
    bool recording;
    char selected[17];
    void (*on_ack)(const char *), (*on_remove)(const char *);
} ib;
void ui_set_inbox_handler(void (*ack)(const char *), void (*remove)(const char *)) {
    ib.on_ack = ack;
    ib.on_remove = remove;
}
static ui_message_t *selected(void) {
    for (int i = 0; i < ib.data->count; i++)
        if (!strcmp(ib.data->messages[i].id, ib.selected))
            return &ib.data->messages[i];
    return NULL;
}
static void render(void) {
    ui_message_t *m = selected();
    ui_set_visible(ib.list, !m && ib.data->count);
    ui_set_visible(ib.empty, !m && !ib.data->count);
    ui_set_visible(ib.play, m && m->voice);
    ui_set_visible(ib.record, !m && ui_voice_current()->storage_ready);
    ui_label_set_text(ib.record, ib.recording                                   ? "Release to save note"
                                 : ui_voice_current()->state == UI_VOICE_SAVING ? "Saving voice note"
                                                                                : "Hold to record note");
    ui_label_set_text(ib.play, ui_voice_current()->state == UI_VOICE_PLAYING ? "Stop playback" : "Play voice note");
    ui_set_visible(ib.sender, m);
    ui_set_visible(ib.body, m);
    ui_set_visible(ib.ack, m && !m->local);
    ui_set_visible(ib.back, m);
    ui_set_visible(ib.remove, m && !m->pending);
    if (m) {
        ui_label_set_text(ib.sender, m->sender);
        ui_label_set_text(ib.text, m->text);
        ui_label_set_text(ib.ack, m->acknowledged ? "Acknowledged"
                                  : m->pending    ? "Sending acknowledgement"
                                  : m->failed     ? "Retry acknowledgement"
                                                  : "Acknowledge");
        lv_obj_set_style_text_color(ib.ack,
                                    lv_color_hex(m->acknowledged || m->pending ? UI_COLOR_TEXT_DIM : UI_TINT_TALK), 0);
    }
    for (int i = 0; i < UI_INBOX_MAX; i++) {
        ui_set_visible(ib.rows[i], i < ib.data->count);
        if (i < ib.data->count) {
            ui_label_set_text(ib.names[i], ib.data->messages[i].sender);
            ui_label_set_text(ib.previews[i], ib.data->messages[i].text);
        }
    }
}
static void open_message(lv_event_t *e) {
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    if (index < ib.data->count) {
        snprintf(ib.selected, sizeof(ib.selected), "%s", ib.data->messages[index].id);
        render();
    }
}
static void acknowledge(lv_event_t *e) {
    (void)e;
    ui_message_t *m = selected();
    if (m && !m->pending && !m->acknowledged && ib.on_ack)
        ib.on_ack(m->id);
}
static void remove_message(lv_event_t *e) {
    (void)e;
    ui_message_t *m = selected();
    if (m && ib.on_remove)
        ib.on_remove(m->id);
}
static void play(lv_event_t *e) {
    (void)e;
    ui_message_t *m = selected();
    if (!m)
        return;
    if (ui_voice_current()->state == UI_VOICE_PLAYING)
        ui_voice_stop();
    else if (ui_voice_current()->state == UI_VOICE_IDLE)
        ui_voice_play(m->id);
}
static void record(lv_event_t *e) {
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
    if (ib.data)
        render();
}
static void back(lv_event_t *e) {
    (void)e;
    ib.selected[0] = 0;
    render();
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
    for (int i = 0; i < ib.data->count; i++)
        if (!ib.data->messages[i].acknowledged)
            count++;
    if (!count)
        return false;
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
static void clickable(lv_obj_t *label, lv_event_cb_t cb) {
    lv_obj_add_flag(label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(label, UI_SPACE_XL);
    lv_obj_add_event_cb(label, cb, LV_EVENT_CLICKED, NULL);
}
void ui_update_inbox(const ui_inbox_t *data) {
    if (ib.data && memcmp(data, ib.data, sizeof(*data))) {
        *ib.data = *data;
        render();
    }
}
void inbox_page_build(lv_obj_t *screen) {
    ib.data = lv_malloc_zeroed(sizeof(*ib.data));
    LV_ASSERT_MALLOC(ib.data);
    tool_page_register(
        screen, &(tool_page_desc_t){.name = "Inbox", .icon = ICON_INBOX, .tint = UI_TINT_TALK, .status = status});
    lv_obj_t *title = tool_title_create(screen, "INBOX", lv_color_hex(UI_TINT_TALK));
    lv_obj_align(title, LV_ALIGN_CENTER, 0, UI_TITLE_TOP_Y);
    ib.list = lv_obj_create(screen);
    lv_obj_remove_style_all(ib.list);
    lv_obj_set_size(ib.list, INBOX_LIST_W, INBOX_LIST_H);
    lv_obj_set_flex_flow(ib.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(ib.list, UI_SPACE_XS, 0);
    lv_obj_set_scroll_dir(ib.list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(ib.list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(ib.list, LV_ALIGN_TOP_MID, 0, INBOX_LIST_TOP);
    for (int i = 0; i < UI_INBOX_MAX; i++) {
        lv_obj_t *r = lv_obj_create(ib.list);
        ib.rows[i] = r;
        lv_obj_remove_style_all(r);
        lv_obj_set_size(r, lv_pct(100), INBOX_ROW_H);
        lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(r, lv_color_hex(UI_COLOR_TEXT), 0);
        lv_obj_set_style_bg_opa(r, UI_OPA_GLASS, 0);
        lv_obj_set_style_radius(r, UI_RADIUS_CARD, 0);
        lv_obj_add_event_cb(r, open_message, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        ib.names[i] = label(r, UI_FONT_LABEL, UI_COLOR_TEXT, "");
        ib.previews[i] = label(r, UI_FONT_CAPTION, UI_COLOR_TEXT_DIM, "");
        lv_obj_t *labels[] = {ib.names[i], ib.previews[i]};
        for (int j = 0; j < 2; j++) {
            lv_obj_set_width(labels[j], INBOX_LIST_W - 2 * UI_SPACE_L);
            lv_obj_set_height(labels[j], lv_font_get_line_height(j ? UI_FONT_CAPTION : UI_FONT_LABEL));
            lv_label_set_long_mode(labels[j], LV_LABEL_LONG_MODE_DOTS);
            lv_obj_align(labels[j], LV_ALIGN_TOP_LEFT, UI_SPACE_L, UI_SPACE_S + j * 20);
        }
    }
    ib.empty = label(screen, UI_FONT_TITLE, UI_COLOR_TEXT_DIM, "Your inbox is empty");
    lv_obj_center(ib.empty);
    ib.sender = label(screen, UI_FONT_TITLE, UI_COLOR_TEXT, "");
    lv_obj_set_width(ib.sender, UI_TEXT_W);
    lv_label_set_long_mode(ib.sender, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(ib.sender, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(ib.sender, LV_ALIGN_CENTER, 0, -66);
    ib.body = lv_obj_create(screen);
    lv_obj_remove_style_all(ib.body);
    lv_obj_set_size(ib.body, UI_TEXT_W, 92);
    lv_obj_align(ib.body, LV_ALIGN_CENTER, 0, -UI_SPACE_S);
    lv_obj_set_scroll_dir(ib.body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(ib.body, LV_SCROLLBAR_MODE_OFF);
    ib.text = label(ib.body, UI_FONT_BODY, UI_COLOR_TEXT, "");
    lv_obj_set_width(ib.text, lv_pct(100));
    lv_label_set_long_mode(ib.text, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(ib.text, LV_TEXT_ALIGN_CENTER, 0);
    ib.ack = label(screen, UI_FONT_LABEL, UI_TINT_TALK, "Acknowledge");
    lv_obj_align(ib.ack, LV_ALIGN_CENTER, 0, 88);
    clickable(ib.ack, acknowledge);
    ib.back = label(screen, UI_FONT_LABEL, UI_COLOR_TEXT_DIM, "Inbox");
    lv_obj_align(ib.back, LV_ALIGN_CENTER, -48, 124);
    clickable(ib.back, back);
    ib.remove = label(screen, UI_FONT_LABEL, UI_COLOR_DANGER, "Delete");
    lv_obj_align(ib.remove, LV_ALIGN_CENTER, 48, 124);
    clickable(ib.remove, remove_message);
    ib.play = label(screen, UI_FONT_LABEL, UI_TINT_TALK, "Play voice note");
    lv_obj_align(ib.play, LV_ALIGN_CENTER, 0, 52);
    clickable(ib.play, play);
    ib.record = label(screen, UI_FONT_LABEL, UI_TINT_TALK, "Hold to record note");
    lv_obj_align(ib.record, LV_ALIGN_CENTER, 0, 124);
    lv_obj_add_flag(ib.record, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(ib.record, UI_TOUCH_EXT);
    lv_obj_add_event_cb(ib.record, record, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(screen, unloaded, LV_EVENT_SCREEN_UNLOAD_START, NULL);
    render();
}
