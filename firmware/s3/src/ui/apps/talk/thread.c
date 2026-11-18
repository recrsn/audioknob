#include <stdio.h>
#include <string.h>

#include "ui/apps/talk/talk.h"

// One knob's conversation: its voice notes as bubbles (theirs on the left,
// ours tinted on the right) and calls as centred lines, oldest first, over a
// caption line and the row of round buttons. A tap plays or stops a note,
// or sends a failed one again; a long press deletes. Recording swaps the
// buttons for delete and send, and the caption for the time and a live
// level trace.

#define LIST_BOTTOM  40 // from the centre
#define LIST_H       (180 + LIST_BOTTOM - TOOL_LIST_TOP)
#define BUBBLE_W     212
#define BUBBLE_H     40
#define CALL_ROW_H   24
#define WAVE_BAR     2
#define WAVE_GAP     2
#define WAVE_W       (UI_NOTE_WAVE * (WAVE_BAR + WAVE_GAP) - WAVE_GAP)
#define WAVE_H       22
#define WAVE_MIN     3
#define DOT          8
#define CAPTION_Y    60
#define LIVE_BARS    20
#define LIVE_MS      100
#define SCROLL_STEP  (BUBBLE_H + UI_SPACE_S)
#define TRIO_X       72 // three buttons
#define AT_END_SLACK 4

typedef struct {
    lv_obj_t *row, *bubble, *glyph, *wave, *length, *mark, *dot;
    lv_obj_t *call, *call_glyph, *call_text;
} row_t;

static struct {
    ui_thread_t *shown; // lv_malloc: the waves draw from it
    char playing[17];
    uint16_t play_tenths;
    char delete_id[17];
    uint8_t live[LIVE_BARS], live_peak;
    uint32_t live_at;
    lv_obj_t *list, *caption, *caption_row, *live_trace, *rec_dot;
    row_t rows[UI_THREAD_ITEMS];
    lv_obj_t *back, *call, *mic, *trash, *send;
} th;

static const ui_note_t *item_at(int i) {
    return i < th.shown->count ? &th.shown->items[i] : NULL;
}

static bool is_note(const ui_note_t *n) {
    return n->kind == UI_NOTE_IN || n->kind == UI_NOTE_OUT;
}

static void bars_draw(lv_layer_t *layer, const lv_area_t *box, const uint8_t *levels, int count, int lit,
                      lv_color_t on, lv_color_t off) {
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.radius = 1;
    int32_t h_max = lv_area_get_height(box);
    int32_t mid = (box->y1 + box->y2) / 2;
    for (int i = 0; i < count; i++) {
        int32_t h = WAVE_MIN + (h_max - WAVE_MIN) * levels[i] / 100;
        lv_area_t bar = {.x1 = box->x1 + i * (WAVE_BAR + WAVE_GAP), .y1 = mid - h / 2};
        bar.x2 = bar.x1 + WAVE_BAR - 1;
        bar.y2 = bar.y1 + h - 1;
        dsc.bg_color = i < lit ? on : off;
        lv_draw_rect(layer, &dsc, &bar);
    }
}

// Played so far in the text colour, the rest dimmer.
static void wave_draw_cb(lv_event_t *e) {
    lv_obj_t *obj = lv_event_get_target_obj(e);
    const ui_note_t *n = item_at((int)(intptr_t)lv_obj_get_user_data(obj));
    if (!n) {
        return;
    }
    lv_area_t box;
    lv_obj_get_content_coords(obj, &box);
    int lit = UI_NOTE_WAVE;
    if (!strcmp(n->id, th.playing) && n->tenths) {
        lit = th.play_tenths >= n->tenths ? UI_NOTE_WAVE : th.play_tenths * UI_NOTE_WAVE / n->tenths;
    }
    bars_draw(lv_event_get_layer(e), &box, n->wave, UI_NOTE_WAVE, lit, lv_color_hex(UI_COLOR_TEXT),
              lv_color_hex(UI_COLOR_TEXT_DIM));
}

static void live_draw_cb(lv_event_t *e) {
    lv_obj_t *obj = lv_event_get_target_obj(e);
    lv_area_t box;
    lv_obj_get_content_coords(obj, &box);
    bars_draw(lv_event_get_layer(e), &box, th.live, LIVE_BARS, LIVE_BARS, lv_color_hex(UI_COLOR_TEXT),
              lv_color_hex(UI_COLOR_TEXT));
}

static void bubble_tap_cb(lv_event_t *e) {
    const ui_note_t *n = item_at((int)(intptr_t)lv_event_get_user_data(e));
    const ui_note_handlers_t *h = talk_note_handlers();
    if (!n || !is_note(n)) {
        return;
    }
    if (n->kind == UI_NOTE_OUT && n->status == UI_NOTE_FAILED) {
        if (h->resend) {
            h->resend(n->id);
        }
    } else if (!strcmp(n->id, th.playing)) {
        if (h->stop) {
            h->stop();
        }
    } else if (h->play) {
        h->play(n->id);
    }
}

static void delete_confirmed(void) {
    const ui_note_handlers_t *h = talk_note_handlers();
    if (h->remove && th.delete_id[0]) {
        h->remove(th.delete_id);
    }
}

static void row_hold_cb(lv_event_t *e) {
    const ui_note_t *n = item_at((int)(intptr_t)lv_event_get_user_data(e));
    if (!n) {
        return;
    }
    snprintf(th.delete_id, sizeof(th.delete_id), "%s", n->id);
    ui_confirm(LV_SYMBOL_TRASH, is_note(n) ? "Delete this note?" : "Delete this call?",
               "Only from this knob", delete_confirmed);
}

static lv_obj_t *plain(lv_obj_t *parent) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static lv_obj_t *text(lv_obj_t *parent, const lv_font_t *font, uint32_t color) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, "");
    return l;
}

static void row_build(row_t *r, int i) {
    void *index = (void *)(intptr_t)i;
    r->row = plain(th.list);
    lv_obj_set_size(r->row, lv_pct(100), LV_SIZE_CONTENT);

    r->bubble = plain(r->row);
    lv_obj_add_flag(r->bubble, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(r->bubble, BUBBLE_W, BUBBLE_H);
    lv_obj_set_style_radius(r->bubble, UI_RADIUS_PILL, 0);
    lv_obj_set_style_bg_opa(r->bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(r->bubble, UI_SPACE_M, 0);
    lv_obj_set_style_pad_column(r->bubble, UI_SPACE_XS, 0);
    lv_obj_set_flex_flow(r->bubble, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r->bubble, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(r->bubble, bubble_tap_cb, LV_EVENT_SHORT_CLICKED, index);
    lv_obj_add_event_cb(r->bubble, row_hold_cb, LV_EVENT_LONG_PRESSED, index);

    r->glyph = text(r->bubble, UI_FONT_ICON_SM, UI_COLOR_TEXT);
    r->wave = plain(r->bubble);
    lv_obj_set_size(r->wave, WAVE_W, WAVE_H);
    lv_obj_set_user_data(r->wave, index);
    lv_obj_add_event_cb(r->wave, wave_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    r->length = text(r->bubble, UI_FONT_CAPTION, UI_COLOR_TEXT_DIM);
    lv_obj_set_flex_grow(r->length, 1);
    r->mark = text(r->bubble, UI_FONT_ICON_SM, UI_COLOR_TEXT_DIM);
    r->dot = plain(r->bubble);
    lv_obj_set_size(r->dot, DOT, DOT);
    lv_obj_set_style_radius(r->dot, UI_RADIUS_PILL, 0);
    lv_obj_set_style_bg_color(r->dot, lv_color_hex(UI_COLOR_ACCENT), 0);
    lv_obj_set_style_bg_opa(r->dot, LV_OPA_COVER, 0);

    r->call = plain(r->row);
    lv_obj_set_size(r->call, LV_SIZE_CONTENT, CALL_ROW_H);
    lv_obj_set_flex_flow(r->call, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r->call, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r->call, UI_SPACE_XS, 0);
    lv_obj_align(r->call, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_add_flag(r->call, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(r->call, row_hold_cb, LV_EVENT_LONG_PRESSED, index);
    r->call_glyph = text(r->call, UI_FONT_ICON_SM, UI_COLOR_TEXT_DIM);
    r->call_text = text(r->call, UI_FONT_CAPTION, UI_COLOR_TEXT_DIM);
}

static const char *call_glyph(const ui_note_t *n) {
    if (n->kind == UI_CALL_OUT) {
        return ICON_CALL_OUT;
    }
    return n->status == UI_CALL_MISSED ? ICON_CALL_MISSED : ICON_CALL_IN;
}

static void call_render(row_t *r, const ui_note_t *n) {
    char summary[48], ago[16], line[96];
    talk_item_summary(n, summary, sizeof(summary));
    talk_time_ago(n->time, ago, sizeof(ago));
    snprintf(line, sizeof(line), "%s%s%s", summary, ago[0] ? " \xC2\xB7 " : "", ago);
    ui_label_set_text(r->call_text, line);
    ui_label_set_text(r->call_glyph, call_glyph(n));
    lv_color_t color = lv_color_hex(n->status == UI_CALL_MISSED ? UI_COLOR_DANGER : UI_COLOR_TEXT_DIM);
    ui_set_text_color(r->call_text, color);
    ui_set_text_color(r->call_glyph, color);
}

static void note_render(row_t *r, const ui_note_t *n) {
    bool mine = n->kind == UI_NOTE_OUT, playing = !strcmp(n->id, th.playing);
    lv_obj_align(r->bubble, mine ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(r->bubble, lv_color_hex(mine ? UI_TINT_TALK : UI_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(r->bubble, mine ? UI_OPA_TINTED : LV_OPA_COVER, 0);
    ui_label_set_text(r->glyph, playing ? ICON_NOTE_STOP : ICON_NOTE_PLAY);
    char length[12];
    talk_duration(playing ? th.play_tenths : n->tenths, length, sizeof(length));
    ui_label_set_text(r->length, length);
    const char *mark = NULL;
    uint32_t mark_color = UI_COLOR_TEXT_DIM;
    if (mine) {
        static const char *const k_marks[] = {
            [UI_NOTE_SENDING] = ICON_NOTE_SENDING,
            [UI_NOTE_SENT] = ICON_NOTE_SENT,
            [UI_NOTE_PLAYED] = ICON_NOTE_PLAYED,
            [UI_NOTE_FAILED] = ICON_NOTE_FAILED,
        };
        mark = n->status <= UI_NOTE_FAILED ? k_marks[n->status] : NULL;
        mark_color = n->status == UI_NOTE_PLAYED   ? UI_COLOR_ACCENT
                     : n->status == UI_NOTE_FAILED ? UI_COLOR_DANGER
                                                   : UI_COLOR_TEXT_DIM;
    }
    ui_set_visible(r->mark, mark != NULL);
    if (mark) {
        ui_label_set_text(r->mark, mark);
        ui_set_text_color(r->mark, lv_color_hex(mark_color));
    }
    ui_set_visible(r->dot, !mine && n->unread);
    lv_obj_invalidate(r->wave);
}

static void rows_render(void) {
    for (int i = 0; i < UI_THREAD_ITEMS; i++) {
        row_t *r = &th.rows[i];
        const ui_note_t *n = item_at(i);
        ui_set_visible(r->row, n != NULL);
        if (!n) {
            continue;
        }
        bool note = is_note(n);
        ui_set_visible(r->bubble, note);
        ui_set_visible(r->call, !note);
        if (note) {
            note_render(r, n);
        } else {
            call_render(r, n);
        }
    }
}

static bool at_end(void) {
    return lv_obj_get_scroll_bottom(th.list) <= AT_END_SLACK;
}

static void scroll_to_end(void) {
    if (!th.shown->count) {
        return;
    }
    lv_obj_update_layout(th.list);
    lv_obj_scroll_to_view(th.rows[th.shown->count - 1].row, LV_ANIM_OFF);
}

// The live trace: the loudest frame of each LIVE_MS, newest on the right.
static void live_push(uint8_t level) {
    th.live_peak = level > th.live_peak ? level : th.live_peak;
    if (lv_tick_elaps(th.live_at) < LIVE_MS) {
        return;
    }
    th.live_at = lv_tick_get();
    memmove(th.live, th.live + 1, LIVE_BARS - 1);
    th.live[LIVE_BARS - 1] = th.live_peak;
    th.live_peak = 0;
    lv_obj_invalidate(th.live_trace);
}

static bool call_failed(const ui_talk_t *talk) {
    return !strcmp(talk->message, "No answer") || !strcmp(talk->message, "Declined") ||
           !strcmp(talk->message, "Busy");
}

static void caption_render(const ui_thread_t *t, const ui_talk_t *talk, bool mine_rec) {
    char buf[64], length[12];
    bool recording = mine_rec && talk->rec == UI_NOTE_REC_RECORDING;
    ui_set_visible(th.rec_dot, recording);
    ui_set_visible(th.live_trace, recording);
    if (recording) {
        live_push(talk->level);
        talk_duration(talk->rec_tenths, length, sizeof(length));
        ui_label_set_text(th.caption, length);
        ui_set_text_color(th.caption, lv_color_hex(UI_COLOR_TEXT));
    } else if (mine_rec) {
        talk_duration(talk->rec_tenths, length, sizeof(length));
        snprintf(buf, sizeof(buf), "Draft \xC2\xB7 %s", length);
        ui_label_set_text(th.caption, buf);
        ui_set_text_color(th.caption, lv_color_hex(UI_COLOR_TEXT_DIM));
    } else {
        memset(th.live, 0, sizeof(th.live));
        bool ours = talk->message[0] && t->peer && talk->peer_id == t->peer;
        if (ours && call_failed(talk)) {
            snprintf(buf, sizeof(buf), "%s \xC2\xB7 leave a voice note", talk->message);
        } else if (ours) {
            snprintf(buf, sizeof(buf), "%s", talk->message);
        } else if (!t->peer) {
            snprintf(buf, sizeof(buf), "Not on this WiFi now");
        } else {
            buf[0] = '\0';
        }
        ui_label_set_text(th.caption, buf);
        ui_set_text_color(th.caption, lv_color_hex(UI_COLOR_TEXT_DIM));
    }
}

static void buttons_render(const ui_thread_t *t, const ui_talk_t *talk, bool mine_rec) {
    bool other_rec = talk->rec != UI_NOTE_REC_IDLE && !mine_rec;
    ui_set_visible(th.back, !mine_rec);
    ui_set_visible(th.call, !mine_rec);
    ui_set_visible(th.mic, !mine_rec);
    ui_set_visible(th.trash, mine_rec);
    ui_set_visible(th.send, mine_rec);
    if (mine_rec) {
        return;
    }
    // After a call nobody took, the note is the thing to do next.
    bool offer = talk->message[0] && t->peer && talk->peer_id == t->peer && call_failed(talk);
    lv_obj_set_style_bg_color(th.call, lv_color_hex(offer ? UI_COLOR_SURFACE : UI_TINT_TALK), 0);
    lv_obj_set_style_bg_color(th.mic, lv_color_hex(offer ? UI_TINT_TALK : UI_COLOR_SURFACE), 0);
    ui_set_opa(th.call, t->peer ? LV_OPA_COVER : UI_OPA_DISABLED);
    ui_set_opa(th.mic, t->peer && !other_rec ? LV_OPA_COVER : UI_OPA_DISABLED);
}

bool talk_thread_render(const ui_thread_t *t, const ui_talk_t *talk, bool scroll_end) {
    bool end = at_end();
    bool changed = memcmp(th.shown, t, sizeof(*t)) != 0;
    bool progress = strcmp(th.playing, talk->playing) || th.play_tenths != talk->play_tenths;
    if (changed) {
        *th.shown = *t;
    }
    snprintf(th.playing, sizeof(th.playing), "%s", talk->playing);
    th.play_tenths = talk->play_tenths;
    ui_set_visible(th.list, t->count > 0);
    ui_set_visible(th.caption_row, true);
    if (changed || progress) {
        rows_render();
    }
    if (scroll_end || (changed && end)) {
        scroll_to_end();
    }
    bool mine_rec = talk->rec != UI_NOTE_REC_IDLE && !strcmp(talk->rec_key, t->key);
    caption_render(t, talk, mine_rec);
    buttons_render(t, talk, mine_rec);
    return t->count == 0;
}

void talk_thread_hide(void) {
    lv_obj_t *objs[] = {th.list, th.caption_row, th.back, th.call, th.mic, th.trash, th.send};
    for (size_t i = 0; i < sizeof(objs) / sizeof(objs[0]); i++) {
        ui_set_visible(objs[i], false);
    }
}

void talk_thread_scroll(int steps) {
    lv_obj_scroll_by_bounded(th.list, 0, -steps * SCROLL_STEP, LV_ANIM_ON);
}

static void back_cb(lv_event_t *e) {
    (void)e;
    talk_back();
}

static void call_cb(lv_event_t *e) {
    (void)e;
    if (th.shown->peer) {
        talk_dial(th.shown->peer);
    }
}

static void mic_cb(lv_event_t *e) {
    (void)e;
    const ui_note_handlers_t *h = talk_note_handlers();
    if (th.shown->peer && lv_obj_get_style_opa(th.mic, 0) == LV_OPA_COVER && h->record) {
        h->record(th.shown->key);
    }
}

static void trash_cb(lv_event_t *e) {
    (void)e;
    const ui_note_handlers_t *h = talk_note_handlers();
    if (h->discard) {
        h->discard();
    }
}

static void send_cb(lv_event_t *e) {
    (void)e;
    const ui_note_handlers_t *h = talk_note_handlers();
    if (h->send) {
        h->send();
    }
}

static lv_obj_t *button(lv_obj_t *screen, int32_t x, uint32_t fill, const lv_font_t *font, const char *glyph,
                        uint32_t color, lv_event_cb_t cb) {
    lv_obj_t *btn = plain(screen);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(btn, UI_BUTTON_SIZE, UI_BUTTON_SIZE);
    lv_obj_align(btn, LV_ALIGN_CENTER, x, TALK_ACTIONS_Y);
    lv_obj_set_style_radius(btn, UI_RADIUS_PILL, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(fill), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *icon = text(btn, font, color);
    lv_label_set_text(icon, glyph);
    lv_obj_center(icon);
    return btn;
}

static void caption_build(lv_obj_t *screen) {
    th.caption_row = plain(screen);
    lv_obj_set_size(th.caption_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(th.caption_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(th.caption_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(th.caption_row, UI_SPACE_S, 0);
    lv_obj_align(th.caption_row, LV_ALIGN_CENTER, 0, CAPTION_Y);
    th.rec_dot = plain(th.caption_row);
    lv_obj_set_size(th.rec_dot, DOT, DOT);
    lv_obj_set_style_radius(th.rec_dot, UI_RADIUS_PILL, 0);
    lv_obj_set_style_bg_color(th.rec_dot, lv_color_hex(UI_COLOR_DANGER), 0);
    lv_obj_set_style_bg_opa(th.rec_dot, LV_OPA_COVER, 0);
    th.caption = text(th.caption_row, UI_FONT_BODY, UI_COLOR_TEXT_DIM);
    th.live_trace = plain(th.caption_row);
    lv_obj_set_size(th.live_trace, LIVE_BARS * (WAVE_BAR + WAVE_GAP) - WAVE_GAP, WAVE_H);
    lv_obj_add_event_cb(th.live_trace, live_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
}

void talk_thread_build(lv_obj_t *screen) {
    th.shown = lv_malloc_zeroed(sizeof(*th.shown));
    LV_ASSERT_MALLOC(th.shown);

    th.list = lv_obj_create(screen);
    lv_obj_remove_style_all(th.list);
    lv_obj_set_size(th.list, TOOL_LIST_W, LIST_H);
    lv_obj_align(th.list, LV_ALIGN_TOP_MID, 0, TOOL_LIST_TOP);
    lv_obj_set_flex_flow(th.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(th.list, UI_SPACE_S, 0);
    lv_obj_set_scroll_dir(th.list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(th.list, LV_SCROLLBAR_MODE_OFF);
    for (int i = 0; i < UI_THREAD_ITEMS; i++) {
        row_build(&th.rows[i], i);
    }
    caption_build(screen);

    th.back = button(screen, -TRIO_X, UI_COLOR_SURFACE, UI_FONT_SYMBOL_LG, LV_SYMBOL_LEFT, UI_COLOR_TEXT, back_cb);
    th.call = button(screen, 0, UI_TINT_TALK, UI_FONT_SYMBOL_LG, LV_SYMBOL_CALL, UI_COLOR_TEXT, call_cb);
    th.mic = button(screen, TRIO_X, UI_COLOR_SURFACE, &icons_talk_24, ICON_NOTE_MIC, UI_COLOR_TEXT, mic_cb);
    th.trash = button(screen, -TALK_PAIR_X, UI_COLOR_SURFACE, &icons_talk_24, ICON_NOTE_DELETE, UI_COLOR_DANGER,
                      trash_cb);
    th.send = button(screen, TALK_PAIR_X, UI_TINT_TALK, &icons_talk_24, ICON_NOTE_SEND, UI_COLOR_TEXT, send_cb);
    talk_thread_hide();
}
