#include "ui_tools.h"

// Ambient sound: the Ambience page and the playing state it shares with the
// Pomodoro, which plays one during focus blocks, and with ui.c's now-playing
// row and audio chip. The page lists the sounds like Radio's stations: a tap
// plays one, a tap on the one playing stops it. The volume is on the ring
// and the knob, shown in place of the title while it turns.

#define AMB_LIST_TOP (UI_SAFE_TOP + 26)
#define AMB_ROW_H    42
#define AMB_LIST_W   248 // clear of the ring beside the outer rows

static const char *const k_names[UI_AMBIENCE_COUNT] = {
    [UI_AMBIENCE_BROWN] = "Brown noise",
    [UI_AMBIENCE_PINK] = "Pink noise",
    [UI_AMBIENCE_RAIN] = "Rain",
    [UI_AMBIENCE_OCEAN] = "Ocean",
    [UI_AMBIENCE_FOREST] = "Forest",
};

static const char *const k_details[UI_AMBIENCE_COUNT] = {
    [UI_AMBIENCE_BROWN] = "Deep, steady rumble",
    [UI_AMBIENCE_PINK] = "Softer, even hiss",
    [UI_AMBIENCE_RAIN] = "Steady rain and drops",
    [UI_AMBIENCE_OCEAN] = "Waves rolling in",
    [UI_AMBIENCE_FOREST] = "Breeze and birdsong",
};

static struct {
    ui_ambience_handler_t handler;
    bool playing; // as last pushed, or as just asked for
    bool focus;   // the Pomodoro started it
    ui_ambience_t kind;

    lv_obj_t *ring;
    lv_obj_t *title;
    lv_obj_t *rows[UI_AMBIENCE_COUNT];
    lv_obj_t *details[UI_AMBIENCE_COUNT];
    lv_obj_t *names[UI_AMBIENCE_COUNT];
    lv_obj_t *stops[UI_AMBIENCE_COUNT];
    int8_t shown_sel;   // what the page last drew, -1 before any
    int8_t shown_play;  // 0 stopped, 1 playing, 2 playing for focus
} amb = {.shown_sel = -1, .shown_play = -1};

const char *ui_ambience_name(ui_ambience_t kind) {
    return kind < UI_AMBIENCE_COUNT ? k_names[kind] : "";
}

static uint8_t volume(void) {
    return ui_settings_current()->ambience_volume;
}

// The sound playing, else the one it would start with.
static ui_ambience_t selected(void) {
    uint8_t k = ui_settings_current()->ambience_sound;
    return amb.playing ? amb.kind : k < UI_AMBIENCE_COUNT ? (ui_ambience_t)k : UI_AMBIENCE_BROWN;
}

static void page_refresh(void) {
    if (!amb.ring) {
        return;
    }
    int sel = (int)selected();
    int play = amb.playing ? amb.focus ? 2 : 1 : 0;
    if (sel != amb.shown_sel || play != amb.shown_play) {
        amb.shown_sel = (int8_t)sel;
        amb.shown_play = (int8_t)play;
        for (int i = 0; i < UI_AMBIENCE_COUNT; i++) {
            bool on = i == sel;
            bool now = on && amb.playing;
            lv_obj_set_style_bg_opa(amb.rows[i], on ? UI_OPA_SELECTED : LV_OPA_TRANSP, 0);
            lv_obj_set_style_text_color(amb.names[i], lv_color_hex(now ? UI_TINT_AMBIENCE : UI_COLOR_TEXT), 0);
            ui_label_set_text(amb.details[i], !now ? k_details[i] : amb.focus ? "Playing for focus" : "Playing");
            ui_set_visible(amb.stops[i], now);
        }
    }
    tool_ring_set(amb.ring, volume());
}

static void changed(void) {
    page_refresh();
    ui_audio_changed();
}

bool ui_ambience_playing(ui_ambience_t *kind, bool *focus) {
    if (kind) {
        *kind = amb.kind;
    }
    if (focus) {
        *focus = amb.playing && amb.focus;
    }
    return amb.playing;
}

void ui_ambience_play(ui_ambience_t kind, bool focus) {
    amb.focus = focus;
    amb.playing = true;
    amb.kind = kind;
    if (amb.handler.play) {
        amb.handler.play(kind);
    }
    changed();
}

void ui_ambience_stop(void) {
    if (!amb.playing) {
        return;
    }
    amb.playing = false;
    amb.focus = false;
    if (amb.handler.stop) {
        amb.handler.stop();
    }
    changed();
}

void ui_update_ambience(bool playing, ui_ambience_t kind) {
    if (playing == amb.playing && (!playing || kind == amb.kind)) {
        return;
    }
    amb.playing = playing;
    amb.kind = kind;
    if (!playing) {
        amb.focus = false;
    }
    changed();
}

void ui_set_ambience_handler(const ui_ambience_handler_t *handler) {
    amb.handler = *handler;
}

static void set_volume(int32_t v) {
    v = LV_CLAMP(0, v, 100);
    if (v != volume()) {
        ui_settings_change(UI_SETTING_AMBIENCE_VOLUME, (int)v);
    }
    page_refresh();
}

static void page_set_volume(int32_t v) {
    set_volume(v);
    char text[24];
    lv_snprintf(text, sizeof(text), "VOLUME %d%%", volume());
    tool_title_flash(amb.title, text);
}

bool ui_ambience_knob(int steps) {
    if (!amb.playing) {
        return false;
    }
    set_volume((int32_t)volume() + steps);
    return true;
}

// The page always takes the knob as volume, playing or not, so it can be
// set before starting.
static bool page_rotate(int steps) {
    page_set_volume((int32_t)volume() + steps);
    return true;
}

static void ring_touch_cb(int32_t deg, bool first, void *user) {
    (void)first;
    (void)user;
    page_set_volume(tool_ring_value_at(amb.ring, deg));
}

static void row_cb(lv_event_t *e) {
    ui_ambience_t kind = (ui_ambience_t)(uintptr_t)lv_event_get_user_data(e);
    if (kind != ui_settings_current()->ambience_sound) {
        ui_settings_change(UI_SETTING_AMBIENCE_SOUND, kind);
    }
    if (amb.playing && kind == amb.kind) {
        ui_ambience_stop();
    } else {
        // Swapping sounds keeps a focus block's claim on it.
        ui_ambience_play(kind, amb.playing && amb.focus);
    }
}

static void list_create(lv_obj_t *screen) {
    lv_obj_t *list = tool_list_create(screen, AMB_ROW_H * UI_AMBIENCE_COUNT);
    lv_obj_set_width(list, AMB_LIST_W);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, AMB_LIST_TOP);
    lv_obj_remove_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < UI_AMBIENCE_COUNT; i++) {
        amb.rows[i] = tool_list_row_create(list, AMB_ROW_H, &amb.names[i], &amb.details[i]);
        ui_label_set_text(amb.names[i], k_names[i]);
        ui_set_visible(amb.details[i], true);
        lv_obj_add_event_cb(amb.rows[i], row_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        amb.stops[i] = lv_label_create(amb.rows[i]);
        lv_label_set_text(amb.stops[i], LV_SYMBOL_STOP);
        lv_obj_set_style_text_font(amb.stops[i], UI_FONT_SYMBOL_SM, 0);
        lv_obj_set_style_text_color(amb.stops[i], lv_color_hex(UI_TINT_AMBIENCE), 0);
    }
}

void ambience_page_build(lv_obj_t *screen) {
    lv_color_t tint = lv_color_hex(UI_TINT_AMBIENCE);
    tool_page_register(screen, &(tool_page_desc_t){.name = "Ambience",
                                                   .icon = ICON_RAIN,
                                                   .tint = UI_TINT_AMBIENCE,
                                                   .rotate = page_rotate,
                                                   .tick = page_refresh});
    tool_ring_touch_create(screen, ring_touch_cb, NULL);
    amb.ring = tool_ring_create(screen, tint, 100);
    amb.title = tool_title_create(screen, "AMBIENCE", tint);
    lv_obj_align(amb.title, LV_ALIGN_CENTER, 0, UI_TITLE_TOP_Y);
    list_create(screen);
    page_refresh();
}
