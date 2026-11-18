#include <stdio.h>
#include <string.h>
#include <time.h>

#include "ui/apps/talk/talk.h"

// Talk: the knobs as a list of conversations, latest first (the knob moves
// the selection, a tap opens one), each row with what was said last and a
// dot for anything unheard. A conversation (thread.c) holds the voice notes
// and calls with Back, Call and a mic under them. A call takes over the
// page: an avatar (rings pulsing out of it while a call is placed or rings),
// the knob's name, the call's state or time, and Decline and Accept while
// ringing or End. The knob is the call's volume, and a note's while one
// plays, shown in place of the title while it turns.

#define TALK_LIST_H    (304 - TOOL_LIST_TOP)
#define TALK_ROW_H     48
#define TALK_EMPTY_Y   (-8)
#define TALK_AVATAR    96
#define TALK_AVATAR_Y  (-36)
#define TALK_PULSE_MAX 132 // a pulse ring's size as it fades out
#define TALK_PULSE_MS  1400
#define TALK_NAME_Y    34
#define TALK_STATE_Y   56
#define TALK_END_ANGLE 2250 // the handset laid down, in 0.1 degrees
#define TALK_DOT       10
#define TIME_VALID     1577836800 // 2020: before it, the clock wasn't set
#define DAY_S          86400

typedef enum { TALK_LIST, TALK_THREAD, TALK_CALL } talk_view_t;

static struct {
    ui_talk_t data;
    ui_notes_t *notes; // lv_malloc
    char key[UI_TALK_KEY]; // the conversation open, "" for the list
    int selected;          // the list's selection
    bool pulsing, opened;
    int unread_notes;
    char title_text[40];
    lv_obj_t *screen, *title, *list, *empty, *empty_hint;
    lv_obj_t *rows[UI_THREADS_MAX], *names[UI_THREADS_MAX], *details[UI_THREADS_MAX], *dots[UI_THREADS_MAX];
    lv_obj_t *pulse, *avatar, *name, *state;
    lv_obj_t *accept, *decline, *end;
    void (*dial)(uint32_t peer);
    void (*answer)(bool accept);
    void (*set_volume)(uint8_t volume);
    ui_note_handlers_t notes_handlers;
} tk;

void ui_set_talk_handler(void (*call)(uint32_t), void (*answer)(bool), void (*volume)(uint8_t)) {
    tk.dial = call;
    tk.answer = answer;
    tk.set_volume = volume;
}

void ui_set_note_handlers(const ui_note_handlers_t *handlers) {
    tk.notes_handlers = *handlers;
}

const ui_note_handlers_t *talk_note_handlers(void) {
    return &tk.notes_handlers;
}

// ---- Words ----

void talk_duration(uint16_t tenths, char *buf, size_t len) {
    unsigned s = tenths / 10;
    snprintf(buf, len, "%u:%02u", s / 60, s % 60);
}

void talk_time_ago(uint32_t t, char *buf, size_t len) {
    time_t now = time(NULL);
    buf[0] = '\0';
    if (!t || now < TIME_VALID || (time_t)t > now + 60) {
        return;
    }
    uint32_t ago = (uint32_t)(now - t);
    if (ago < 60) {
        snprintf(buf, len, "now");
    } else if (ago < 3600) {
        snprintf(buf, len, "%lum", (unsigned long)(ago / 60));
    } else if (ago < DAY_S) {
        snprintf(buf, len, "%luh", (unsigned long)(ago / 3600));
    } else {
        time_t when = t;
        struct tm tm;
        localtime_r(&when, &tm);
        strftime(buf, len, ago < 6 * DAY_S ? "%a" : "%e %b", &tm);
    }
}

void talk_item_summary(const ui_note_t *n, char *buf, size_t len) {
    char length[12];
    talk_duration(n->tenths, length, sizeof(length));
    switch (n->kind) {
    case UI_NOTE_IN:
        snprintf(buf, len, "Voice note %s", length);
        return;
    case UI_NOTE_OUT:
        if (n->status == UI_NOTE_FAILED) {
            snprintf(buf, len, "Not delivered");
        } else {
            snprintf(buf, len, "You: voice note %s", length);
        }
        return;
    default:
        break;
    }
    switch (n->status) {
    case UI_CALL_ANSWERED:
        snprintf(buf, len, "Call %s", length);
        break;
    case UI_CALL_MISSED:
        snprintf(buf, len, "Missed call");
        break;
    case UI_CALL_DECLINED:
        snprintf(buf, len, n->kind == UI_CALL_IN ? "Declined call" : "Declined");
        break;
    case UI_CALL_NO_ANSWER:
        snprintf(buf, len, "No answer");
        break;
    default:
        snprintf(buf, len, "Busy");
        break;
    }
}

// ---- State ----

static int thread_count(void) {
    return tk.notes ? tk.notes->count : 0;
}

static const ui_thread_t *thread_of(const char *key) {
    for (int i = 0; key[0] && i < thread_count(); i++) {
        if (!strcmp(tk.notes->threads[i].key, key)) {
            return &tk.notes->threads[i];
        }
    }
    return NULL;
}

static const char *key_of_peer(uint32_t ip) {
    for (int i = 0; i < tk.data.count; i++) {
        if (tk.data.peers[i].id == ip) {
            return tk.data.peers[i].key;
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

static talk_view_t view(void) {
    return busy() ? TALK_CALL : thread_of(tk.key) ? TALK_THREAD : TALK_LIST;
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
    return "";
}

// ---- The list ----

static void row_detail(int i) {
    const ui_thread_t *t = &tk.notes->threads[i];
    char summary[48], ago[16], line[80] = "";
    if (t->count) {
        const ui_note_t *last = &t->items[t->count - 1];
        talk_item_summary(last, summary, sizeof(summary));
        talk_time_ago(last->time, ago, sizeof(ago));
        snprintf(line, sizeof(line), "%s%s%s", summary, ago[0] ? " \xC2\xB7 " : "", ago);
    }
    if (!t->peer) {
        strncat(line, line[0] ? " \xC2\xB7 away" : "Away", sizeof(line) - strlen(line) - 1);
    }
    ui_set_visible(tk.details[i], line[0]);
    ui_label_set_text(tk.details[i], line);
    ui_set_text_color(tk.details[i], lv_color_hex(t->unread ? UI_COLOR_TEXT : UI_COLOR_TEXT_DIM));
}

static void list_render(void) {
    int count = thread_count();
    for (int i = 0; i < UI_THREADS_MAX; i++) {
        ui_set_visible(tk.rows[i], i < count);
        if (i >= count) {
            continue;
        }
        const ui_thread_t *t = &tk.notes->threads[i];
        ui_label_set_text(tk.names[i], t->name);
        ui_set_text_color(tk.names[i], lv_color_hex(t->peer ? UI_COLOR_TEXT : UI_COLOR_TEXT_DIM));
        row_detail(i);
        ui_set_visible(tk.dots[i], t->unread);
        lv_obj_set_style_bg_opa(tk.rows[i], i == tk.selected ? UI_OPA_SELECTED : LV_OPA_TRANSP, 0);
    }
    tool_list_fade(tk.list, tk.selected, count);
}

// ---- The call screen ----

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

static void call_render(void) {
    char buf[16];
    bool ring = ringing(), in_call = tk.data.state == UI_TALK_IN_CALL;
    ui_label_set_text(tk.name, tk.data.peer);
    ui_label_set_text(tk.state, state_text(buf, sizeof(buf)));
    ui_set_text_color(tk.state, lv_color_hex(in_call ? UI_COLOR_TEXT : UI_COLOR_TEXT_DIM));
    pulse_set(tk.data.state == UI_TALK_CALLING || ring);
    ui_set_visible(tk.decline, ring);
    ui_set_visible(tk.accept, ring);
    ui_set_visible(tk.end, !ring);
}

// ---- Routing ----

// The title's text lives in title_text, which tool_title_flash() goes back to.
static void title_set(const char *text) {
    char upper[sizeof(tk.title_text)];
    snprintf(upper, sizeof(upper), "%s", text);
    for (char *c = upper; *c; c++) {
        if (*c >= 'a' && *c <= 'z') {
            *c -= 'a' - 'A';
        }
    }
    if (strcmp(upper, tk.title_text)) {
        memcpy(tk.title_text, upper, sizeof(upper));
        ui_label_set_text(tk.title, tk.title_text);
    }
}

// Missed calls are seen by opening their conversation; notes stay unheard
// until played.
static void mark_seen(const ui_thread_t *t) {
    for (int i = 0; t->unread && i < t->count; i++) {
        if (t->items[i].unread && t->items[i].kind != UI_NOTE_IN) {
            if (tk.notes_handlers.seen) {
                tk.notes_handlers.seen(t->key);
            }
            return;
        }
    }
}

static void render(bool scroll_end) {
    talk_view_t v = view();
    bool avatar = v == TALK_CALL;
    if (v == TALK_THREAD) {
        const ui_thread_t *t = thread_of(tk.key);
        avatar = talk_thread_render(t, &tk.data, scroll_end);
        title_set(avatar ? "Talk" : t->name);
        ui_label_set_text(tk.name, t->name);
        mark_seen(t);
    } else {
        talk_thread_hide();
        title_set("Talk");
    }
    ui_set_visible(tk.avatar, avatar);
    ui_set_visible(tk.name, avatar);
    ui_set_visible(tk.state, v == TALK_CALL);
    bool list = v == TALK_LIST;
    ui_set_visible(tk.list, list && thread_count());
    ui_set_visible(tk.empty, list && !thread_count());
    ui_set_visible(tk.empty_hint, list && !thread_count());
    if (v == TALK_CALL) {
        call_render();
    } else {
        pulse_set(false);
        ui_set_visible(tk.decline, false);
        ui_set_visible(tk.accept, false);
        ui_set_visible(tk.end, false);
    }
    if (list) {
        list_render();
    }
}

static void open_thread(const char *key) {
    snprintf(tk.key, sizeof(tk.key), "%s", key);
    render(true);
}

// Leaving a conversation keeps a recording as a draft.
static void leave_thread(void) {
    if (tk.data.rec == UI_NOTE_REC_RECORDING && tk.notes_handlers.pause) {
        tk.notes_handlers.pause();
    }
}

void talk_back(void) {
    leave_thread();
    tk.key[0] = '\0';
    render(false);
    if (thread_count()) {
        tool_list_center(tk.list, tk.selected, LV_ANIM_OFF);
    }
}

void talk_dial(uint32_t peer) {
    if (!busy() && tk.dial) {
        if (tk.data.playing[0] && tk.notes_handlers.stop) {
            tk.notes_handlers.stop();
        }
        tk.dial(peer);
    }
}

static void row_click(lv_event_t *e) {
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < thread_count()) {
        tk.selected = i;
        open_thread(tk.notes->threads[i].key);
    }
}

static void list_scroll_end_cb(lv_event_t *e) {
    (void)e;
    if (thread_count()) {
        tk.selected = tool_list_in_view(tk.list, tk.selected, thread_count());
        list_render();
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

static void unload_cb(lv_event_t *e) {
    (void)e;
    leave_thread();
}

static bool rotate(int steps) {
    talk_view_t v = view();
    if (v == TALK_LIST) {
        if (thread_count()) {
            tk.selected = LV_CLAMP(0, tk.selected + steps, thread_count() - 1);
            list_render();
            tool_list_center(tk.list, tk.selected, LV_ANIM_ON);
        }
        return true;
    }
    if (v == TALK_THREAD && !tk.data.playing[0]) {
        talk_thread_scroll(steps);
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
    if (busy()) {
        if (tk.data.state == UI_TALK_IN_CALL) {
            call_time(buf, len, "In call ");
        } else {
            snprintf(buf, len, "%s", ringing() ? "Incoming call" : "Calling\xE2\x80\xA6");
        }
        return true;
    }
    if (tk.data.rec == UI_NOTE_REC_RECORDING) {
        snprintf(buf, len, "Recording");
        return true;
    }
    return false;
}

// The newest thing unheard: a note, or a missed call.
static bool notice(char *buf, size_t len) {
    const ui_thread_t *who = NULL;
    const ui_note_t *newest = NULL;
    for (int t = 0; t < thread_count(); t++) {
        const ui_thread_t *th = &tk.notes->threads[t];
        for (int i = 0; th->unread && i < th->count; i++) {
            const ui_note_t *n = &th->items[i];
            if (n->unread && (!newest || n->time >= newest->time)) {
                newest = n;
                who = th;
            }
        }
    }
    if (!newest) {
        return false;
    }
    // What it is first: a long name gets cut.
    if (newest->kind == UI_NOTE_IN) {
        snprintf(buf, len, "Voice note \xC2\xB7 %s", who->name);
    } else {
        snprintf(buf, len, "Missed call \xC2\xB7 %s", who->name);
    }
    return true;
}

// ---- Updates ----

void ui_talk_page_update(const ui_talk_t *data) {
    if (!tk.screen || !memcmp(data, &tk.data, sizeof(*data))) {
        return;
    }
    bool was_busy = busy();
    tk.data = *data;
    // A call opens its knob's conversation, whichever side placed it, and
    // that's where it goes back to after.
    if (busy() && !was_busy) {
        const char *key = key_of_peer(data->peer_id);
        if (key) {
            leave_thread();
            snprintf(tk.key, sizeof(tk.key), "%s", key);
        }
    }
    render(false);
}

static int unread_notes(void) {
    int count = 0;
    for (int t = 0; t < thread_count(); t++) {
        const ui_thread_t *th = &tk.notes->threads[t];
        for (int i = 0; th->unread && i < th->count; i++) {
            count += th->items[i].kind == UI_NOTE_IN && th->items[i].unread;
        }
    }
    return count;
}

void ui_talk_notes_update(const ui_notes_t *notes) {
    if (!tk.screen) {
        return;
    }
    if (!tk.notes) {
        tk.notes = lv_malloc(sizeof(*tk.notes));
        LV_ASSERT_MALLOC(tk.notes);
    }
    *tk.notes = *notes;
    if (tk.selected >= notes->count) {
        tk.selected = notes->count ? notes->count - 1 : 0;
    }
    int unread = unread_notes();
    // A new note lights the display, unless music plays: its chime says it.
    if (tk.opened && unread > tk.unread_notes && !ui_stream_playing()) {
        ui_wake();
    }
    tk.opened = true;
    tk.unread_notes = unread;
    render(false);
}

// ---- Building ----

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, text);
    return l;
}

static void list_build(lv_obj_t *screen) {
    tk.list = tool_list_create(screen, TALK_LIST_H);
    lv_obj_add_event_cb(tk.list, list_scroll_end_cb, LV_EVENT_SCROLL_END, NULL);
    for (int i = 0; i < UI_THREADS_MAX; i++) {
        tk.rows[i] = tool_list_row_create(tk.list, TALK_ROW_H, &tk.names[i], &tk.details[i]);
        lv_obj_add_event_cb(tk.rows[i], row_click, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        tk.dots[i] = lv_obj_create(tk.rows[i]);
        lv_obj_remove_style_all(tk.dots[i]);
        lv_obj_remove_flag(tk.dots[i], LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(tk.dots[i], TALK_DOT, TALK_DOT);
        lv_obj_set_style_radius(tk.dots[i], UI_RADIUS_PILL, 0);
        lv_obj_set_style_bg_color(tk.dots[i], lv_color_hex(UI_COLOR_ACCENT), 0);
        lv_obj_set_style_bg_opa(tk.dots[i], LV_OPA_COVER, 0);
    }
    tk.empty = label(screen, UI_FONT_TITLE, UI_COLOR_TEXT, "No other knobs yet");
    lv_obj_align(tk.empty, LV_ALIGN_CENTER, 0, TALK_EMPTY_Y);
    tk.empty_hint = label(screen, UI_FONT_BODY, UI_COLOR_TEXT_DIM, "Knobs on this WiFi show up here");
    lv_obj_set_width(tk.empty_hint, UI_TEXT_W);
    lv_obj_set_style_text_align(tk.empty_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align_to(tk.empty_hint, tk.empty, LV_ALIGN_OUT_BOTTOM_MID, 0, UI_SPACE_S);
}

// A filled round button with a phone (tipped over for ending a call) on it.
static lv_obj_t *round_button(lv_obj_t *screen, int32_t x, uint32_t fill, bool tipped, lv_event_cb_t cb,
                              void *user_data) {
    lv_obj_t *btn = lv_obj_create(screen);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, UI_BUTTON_SIZE, UI_BUTTON_SIZE);
    lv_obj_align(btn, LV_ALIGN_CENTER, x, TALK_ACTIONS_Y);
    lv_obj_set_style_radius(btn, UI_RADIUS_PILL, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(fill), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    lv_obj_t *icon = label(btn, UI_FONT_SYMBOL_LG, UI_COLOR_TEXT, LV_SYMBOL_CALL);
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

static void call_build(lv_obj_t *screen) {
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

    tk.decline = round_button(screen, -TALK_PAIR_X, UI_COLOR_DANGER, true, answer_click, NULL);
    tk.accept = round_button(screen, TALK_PAIR_X, UI_TINT_TALK, false, answer_click, (void *)1);
    tk.end = round_button(screen, 0, UI_COLOR_DANGER, true, end_click, NULL);
}

void talk_page_build(lv_obj_t *screen) {
    tk.screen = screen;
    tool_page_register(screen, &(tool_page_desc_t){.name = "Talk",
                                                   .icon = ICON_TALK,
                                                   .tint = UI_TINT_TALK,
                                                   .rotate = rotate,
                                                   .status = status,
                                                   .notice = notice});
    snprintf(tk.title_text, sizeof(tk.title_text), "TALK");
    tk.title = tool_title_create(screen, tk.title_text, lv_color_hex(UI_TINT_TALK));
    lv_obj_set_width(tk.title, UI_TEXT_W);
    lv_obj_set_style_text_align(tk.title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(tk.title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(tk.title, LV_ALIGN_CENTER, 0, UI_TITLE_TOP_Y);
    list_build(screen);
    call_build(screen);
    talk_thread_build(screen);
    lv_obj_add_event_cb(screen, unload_cb, LV_EVENT_SCREEN_UNLOAD_START, NULL);
    render(false);
}
