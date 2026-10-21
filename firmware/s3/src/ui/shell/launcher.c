#include "ui/shell/launcher.h"

#include "ui/assets/fonts.h"

#define LAUNCHER_RADIUS    116
#define LAUNCHER_ITEM      64
#define LAUNCHER_ICON_SCALE 224 // 7/8: 48 px app glyphs appear at 42 px
#define LAUNCHER_CENTER    140
#define LAUNCHER_NAME_Y    (-9)
#define LAUNCHER_STATUS_Y  13
#define LAUNCHER_STATUS_MS 250

// Ring positions, clockwise from the top: the audio entry, then the apps.
#define LAUNCHER_COUNT (UI_APP_COUNT + 1)

static struct {
    lv_obj_t *scrim;
    lv_obj_t *items[LAUNCHER_COUNT];
    lv_obj_t *name;
    lv_obj_t *status;
    lv_timer_t *timer;
    int selected; // a ring position
    launcher_handlers_t handlers;
} ln;

static int entry_at(int pos) {
    return pos == 0 ? LAUNCHER_AUDIO : pos - 1;
}

static int pos_of(int entry) {
    return entry == LAUNCHER_AUDIO ? 0 : entry + 1;
}

static const char *entry_name(int pos) {
    return pos == 0 ? "Audio" : ui_tools_info((ui_app_t)entry_at(pos)).name;
}

static lv_color_t entry_tint(int pos) {
    return pos == 0 ? lv_color_hex(UI_TINT_AIRPLAY) : ui_tools_info((ui_app_t)entry_at(pos)).tint;
}

bool launcher_visible(void) {
    return ln.scrim && !lv_obj_has_flag(ln.scrim, LV_OBJ_FLAG_HIDDEN);
}

static void refresh_status(void) {
    char buf[64];
    bool any = ln.selected == 0 ? ln.handlers.audio_status(buf, sizeof(buf))
                                : ui_tools_status((ui_app_t)entry_at(ln.selected), buf, sizeof(buf));
    ui_label_set_text(ln.status, any ? buf : "");
}

static void status_timer_cb(lv_timer_t *t) {
    (void)t;
    refresh_status();
}

static void item_style(int pos, bool selected) {
    lv_color_t tint = entry_tint(pos);
    lv_obj_t *item = ln.items[pos];
    lv_obj_set_style_bg_color(item, selected ? tint : lv_color_hex(UI_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(item, selected ? UI_OPA_TINTED : LV_OPA_COVER, 0);
    lv_obj_set_style_border_opa(item, selected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
}

static void select_pos(int pos) {
    if (pos != ln.selected) {
        item_style(ln.selected, false);
        item_style(pos, true);
        ln.selected = pos;
    }
    ui_label_set_text(ln.name, entry_name(pos));
    lv_obj_set_style_text_color(ln.status, entry_tint(pos), 0);
    refresh_status();
}

void launcher_show(int entry) {
    select_pos(pos_of(entry));
    if (!launcher_visible()) {
        lv_obj_remove_flag(ln.scrim, LV_OBJ_FLAG_HIDDEN);
        lv_timer_resume(ln.timer);
        ln.handlers.visibility_changed();
    }
}

void launcher_hide(void) {
    if (launcher_visible()) {
        lv_obj_add_flag(ln.scrim, LV_OBJ_FLAG_HIDDEN);
        lv_timer_pause(ln.timer);
        ln.handlers.visibility_changed();
    }
}

void launcher_rotate(int steps) {
    int n = LAUNCHER_COUNT;
    select_pos(((ln.selected + steps) % n + n) % n);
}

static void open_pos(int pos) {
    launcher_hide();
    ln.handlers.open(entry_at(pos));
}

void launcher_activate(void) {
    open_pos(ln.selected);
}

static void item_click_cb(lv_event_t *e) {
    open_pos((int)(intptr_t)lv_event_get_user_data(e));
}

static void scrim_click_cb(lv_event_t *e) {
    (void)e;
    launcher_hide();
    ln.handlers.dismissed();
}

// The apps' icons are icons_48 glyphs; the audio one is the speaker the
// clock's idle audio row shows, from Montserrat's symbols.
static void item_icon_create(lv_obj_t *item, int pos) {
    lv_color_t tint = entry_tint(pos);
    lv_obj_t *icon = lv_label_create(item);
    if (pos == 0) {
        lv_label_set_text(icon, LV_SYMBOL_VOLUME_MAX);
        lv_obj_set_style_text_font(icon, UI_FONT_SYMBOL_XL, 0);
    } else {
        lv_label_set_text(icon, ui_tools_info((ui_app_t)entry_at(pos)).icon);
        lv_obj_set_style_text_font(icon, UI_FONT_ICON, 0);
    }
    lv_obj_set_style_text_color(icon, tint, 0);
    lv_obj_set_style_transform_pivot_x(icon, lv_pct(50), 0);
    lv_obj_set_style_transform_pivot_y(icon, lv_pct(50), 0);
    lv_obj_set_style_transform_scale(icon, LAUNCHER_ICON_SCALE, 0);
    lv_obj_center(icon);
}

static lv_obj_t *item_create(int pos) {
    lv_obj_t *item = lv_obj_create(ln.scrim);
    lv_obj_remove_style_all(item);
    lv_obj_remove_flag(item, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(item, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(item, LAUNCHER_ITEM, LAUNCHER_ITEM);
    lv_obj_set_style_radius(item, UI_RADIUS_PILL, 0);
    lv_obj_set_style_border_width(item, 2, 0);
    lv_obj_set_style_border_color(item, entry_tint(pos), 0);
    lv_obj_add_event_cb(item, item_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)pos);

    // Clockwise from the top, like the knob turning right.
    int32_t angle = -90 + pos * 360 / LAUNCHER_COUNT;
    int32_t x = LAUNCHER_RADIUS * lv_trigo_cos(angle) / LV_TRIGO_SIN_MAX;
    int32_t y = LAUNCHER_RADIUS * lv_trigo_sin(angle) / LV_TRIGO_SIN_MAX;
    lv_obj_align(item, LV_ALIGN_CENTER, x, y);
    item_icon_create(item, pos);
    return item;
}

static lv_obj_t *center_label_create(lv_obj_t *parent, const lv_font_t *font, int32_t y) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, y);
    return label;
}

void launcher_build(const launcher_handlers_t *handlers) {
    ln.handlers = *handlers;

    // Opaque: anything showing through competes with the app name in the
    // middle. Still an overlay, so opening it reloads no screen.
    ln.scrim = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(ln.scrim);
    lv_obj_remove_flag(ln.scrim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ln.scrim, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(ln.scrim, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(ln.scrim, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(ln.scrim, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(ln.scrim, scrim_click_cb, LV_EVENT_CLICKED, NULL);

    for (int i = 0; i < LAUNCHER_COUNT; i++) {
        ln.items[i] = item_create(i);
        item_style(i, i == 0);
    }

    // Only the icons are buttons; a tap in the middle is off them like
    // anywhere else.
    lv_obj_t *center = lv_obj_create(ln.scrim);
    lv_obj_remove_style_all(center);
    lv_obj_remove_flag(center, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(center, LAUNCHER_CENTER, LAUNCHER_CENTER);
    lv_obj_center(center);

    ln.name = center_label_create(center, UI_FONT_TITLE, LAUNCHER_NAME_Y);
    ln.status = center_label_create(center, UI_FONT_BODY, LAUNCHER_STATUS_Y);
    // A track title can run long; keep it to one line inside the ring (a
    // content-tall label wraps rather than dots).
    lv_obj_set_size(ln.status, LAUNCHER_CENTER, lv_font_get_line_height(UI_FONT_BODY));
    lv_label_set_long_mode(ln.status, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(ln.status, LV_TEXT_ALIGN_CENTER, 0);

    ln.timer = lv_timer_create(status_timer_cb, LAUNCHER_STATUS_MS, NULL);
    lv_timer_pause(ln.timer);
    select_pos(0);
}
