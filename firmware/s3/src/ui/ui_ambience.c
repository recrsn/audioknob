#include "ui_tools.h"

// Ambient sound: the Ambience page (a sound to pick, the volume on the
// ring and the knob, play/stop) and the playing state it shares with the
// Pomodoro, which plays one during focus blocks, and with ui.c's
// now-playing row and audio chip.

#define AMB_GRID_Y   (-22)
#define AMB_PILL_W   84
#define AMB_PER_ROW  3
#define AMB_PILL_H   44
#define AMB_PILL_GAP UI_SPACE_S
#define AMB_HINT_Y   46

static const char *const k_names[UI_AMBIENCE_COUNT] = {
    [UI_AMBIENCE_BROWN] = "Brown noise",
    [UI_AMBIENCE_PINK] = "Pink noise",
    [UI_AMBIENCE_RAIN] = "Rain",
    [UI_AMBIENCE_OCEAN] = "Ocean",
    [UI_AMBIENCE_FOREST] = "Forest",
};

// Shorter, for the pills.
static const char *const k_short[UI_AMBIENCE_COUNT] = {
    [UI_AMBIENCE_BROWN] = "Brown",
    [UI_AMBIENCE_PINK] = "Pink",
    [UI_AMBIENCE_RAIN] = "Rain",
    [UI_AMBIENCE_OCEAN] = "Ocean",
    [UI_AMBIENCE_FOREST] = "Forest",
};

static struct {
    ui_ambience_handler_t handler;
    bool playing; // as last pushed, or as just asked for
    bool focus;   // the Pomodoro started it
    ui_ambience_t kind;

    lv_obj_t *ring;
    lv_obj_t *pills[UI_AMBIENCE_COUNT];
    lv_obj_t *hint;
    lv_obj_t *play_icon;
    int8_t shown_sel;   // what the page last drew, -1 before any
    int8_t shown_play;
    int16_t shown_vol;
} amb = {.shown_sel = -1, .shown_play = -1, .shown_vol = -1};

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
    if (sel != amb.shown_sel) {
        amb.shown_sel = (int8_t)sel;
        for (int i = 0; i < UI_AMBIENCE_COUNT; i++) {
            bool on = i == sel;
            lv_obj_set_style_bg_color(amb.pills[i], on ? lv_color_hex(UI_TINT_AMBIENCE) : lv_color_white(), 0);
            lv_obj_set_style_bg_opa(amb.pills[i], on ? UI_OPA_TINTED : UI_OPA_GLASS, 0);
            lv_obj_set_style_border_opa(amb.pills[i], on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
            lv_obj_set_style_text_color(amb.pills[i], lv_color_hex(on ? UI_COLOR_TEXT : UI_COLOR_TEXT_DIM), 0);
        }
    }
    int vol = volume();
    if (vol != amb.shown_vol || amb.playing != amb.shown_play) {
        amb.shown_vol = (int16_t)vol;
        amb.shown_play = amb.playing;
        tool_ring_set(amb.ring, vol);
        char buf[24];
        lv_snprintf(buf, sizeof(buf), "Volume %d%%", vol);
        ui_label_set_text(amb.hint, buf);
        ui_label_set_text(amb.play_icon, amb.playing ? LV_SYMBOL_STOP : LV_SYMBOL_PLAY);
    }
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
    v = v < 0 ? 0 : v > 100 ? 100 : v;
    if (v != volume()) {
        ui_settings_change(UI_SETTING_AMBIENCE_VOLUME, (int)v);
    }
    page_refresh();
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
    set_volume((int32_t)volume() + steps);
    return true;
}

static void ring_touch_cb(int32_t deg, bool first, void *user) {
    (void)first;
    (void)user;
    set_volume(tool_ring_value_at(amb.ring, deg));
}

static void pill_cb(lv_event_t *e) {
    ui_ambience_t kind = (ui_ambience_t)(uintptr_t)lv_event_get_user_data(e);
    if (kind != ui_settings_current()->ambience_sound) {
        ui_settings_change(UI_SETTING_AMBIENCE_SOUND, kind);
    }
    if (amb.playing && kind != amb.kind) {
        ui_ambience_play(kind, amb.focus);
    }
    page_refresh();
}

static void play_cb(lv_event_t *e) {
    (void)e;
    if (amb.playing) {
        ui_ambience_stop();
    } else {
        ui_ambience_play(selected(), false);
    }
}

static void grid_create(lv_obj_t *screen) {
    lv_obj_t *grid = lv_obj_create(screen);
    lv_obj_remove_style_all(grid);
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(grid, AMB_PER_ROW * AMB_PILL_W + (AMB_PER_ROW - 1) * AMB_PILL_GAP, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(grid, AMB_PILL_GAP, 0);
    lv_obj_set_style_pad_row(grid, AMB_PILL_GAP, 0);
    lv_obj_align(grid, LV_ALIGN_CENTER, 0, AMB_GRID_Y);

    for (int i = 0; i < UI_AMBIENCE_COUNT; i++) {
        lv_obj_t *pill = lv_obj_create(grid);
        lv_obj_remove_style_all(pill);
        lv_obj_remove_flag(pill, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(pill, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(pill, AMB_PILL_W, AMB_PILL_H);
        lv_obj_set_style_radius(pill, UI_RADIUS_PILL, 0);
        lv_obj_set_style_border_width(pill, 1, 0);
        lv_obj_set_style_border_color(pill, lv_color_hex(UI_TINT_AMBIENCE), 0);
        lv_obj_add_event_cb(pill, pill_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        lv_obj_t *label = lv_label_create(pill);
        lv_label_set_text(label, k_short[i]);
        lv_obj_set_style_text_font(label, UI_FONT_LABEL, 0);
        lv_obj_center(label);
        amb.pills[i] = pill;
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
    tool_title_create(screen, "AMBIENCE", tint);
    grid_create(screen);
    amb.hint = tool_hint_label_create(screen);
    lv_obj_align(amb.hint, LV_ALIGN_CENTER, 0, AMB_HINT_Y);

    lv_obj_t *row = tool_button_row_create(screen);
    amb.play_icon = tool_primary_button_create(row, tint, play_cb);
    page_refresh();
}
