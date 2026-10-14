#include "ui_launcher.h"

#include "fonts.h"

#define LAUNCHER_RADIUS    116
#define LAUNCHER_ITEM      72
#define LAUNCHER_CENTER    140
#define LAUNCHER_NAME_Y    (-9)
#define LAUNCHER_STATUS_Y  13
#define LAUNCHER_STATUS_MS 250
#define LAUNCHER_IDLE_ITEM 0x1c1c24

static struct {
    lv_obj_t *scrim;
    lv_obj_t *items[UI_APP_COUNT];
    lv_obj_t *name;
    lv_obj_t *status;
    lv_timer_t *timer;
    int selected;
    void (*open)(ui_app_t app);
    void (*dismissed)(void);
    void (*visibility_changed)(void);
} ln;

bool launcher_visible(void) {
    return ln.scrim && !lv_obj_has_flag(ln.scrim, LV_OBJ_FLAG_HIDDEN);
}

static void refresh_status(void) {
    char buf[16];
    ui_label_set_text(ln.status, ui_tools_status((ui_app_t)ln.selected, buf, sizeof(buf)) ? buf : "");
}

static void status_timer_cb(lv_timer_t *t) {
    (void)t;
    refresh_status();
}

static void item_style(int i, bool selected) {
    lv_color_t tint = ui_tools_info((ui_app_t)i).tint;
    lv_obj_t *item = ln.items[i];
    lv_obj_set_style_bg_color(item, selected ? tint : lv_color_hex(LAUNCHER_IDLE_ITEM), 0);
    lv_obj_set_style_bg_opa(item, selected ? LV_OPA_30 : LV_OPA_COVER, 0);
    lv_obj_set_style_border_opa(item, selected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
}

static void select_app(int app) {
    if (app != ln.selected) {
        item_style(ln.selected, false);
        item_style(app, true);
        ln.selected = app;
    }
    ui_label_set_text(ln.name, ui_tools_info((ui_app_t)app).name);
    lv_obj_set_style_text_color(ln.status, ui_tools_info((ui_app_t)app).tint, 0);
    refresh_status();
}

void launcher_show(ui_app_t selected) {
    select_app(selected);
    if (!launcher_visible()) {
        lv_obj_remove_flag(ln.scrim, LV_OBJ_FLAG_HIDDEN);
        lv_timer_resume(ln.timer);
        ln.visibility_changed();
    }
}

void launcher_hide(void) {
    if (launcher_visible()) {
        lv_obj_add_flag(ln.scrim, LV_OBJ_FLAG_HIDDEN);
        lv_timer_pause(ln.timer);
        ln.visibility_changed();
    }
}

void launcher_rotate(int steps) {
    int n = UI_APP_COUNT;
    select_app(((ln.selected + steps) % n + n) % n);
}

static void open_app(int app) {
    launcher_hide();
    ln.open((ui_app_t)app);
}

void launcher_activate(void) {
    open_app(ln.selected);
}

static void item_click_cb(lv_event_t *e) {
    open_app((int)(intptr_t)lv_event_get_user_data(e));
}

static void scrim_click_cb(lv_event_t *e) {
    (void)e;
    launcher_hide();
    ln.dismissed();
}

static lv_obj_t *item_create(int i) {
    ui_app_info_t info = ui_tools_info((ui_app_t)i);
    lv_obj_t *item = lv_obj_create(ln.scrim);
    lv_obj_remove_style_all(item);
    lv_obj_remove_flag(item, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(item, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(item, LAUNCHER_ITEM, LAUNCHER_ITEM);
    lv_obj_set_style_radius(item, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(item, 2, 0);
    lv_obj_set_style_border_color(item, info.tint, 0);
    lv_obj_add_event_cb(item, item_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

    // Clockwise from the top, like the knob turning right.
    int32_t angle = -90 + i * 360 / UI_APP_COUNT;
    int32_t x = LAUNCHER_RADIUS * lv_trigo_cos(angle) / LV_TRIGO_SIN_MAX;
    int32_t y = LAUNCHER_RADIUS * lv_trigo_sin(angle) / LV_TRIGO_SIN_MAX;
    lv_obj_align(item, LV_ALIGN_CENTER, x, y);

    lv_obj_t *icon = lv_label_create(item);
    lv_label_set_text(icon, info.icon);
    lv_obj_set_style_text_font(icon, &icons_48, 0);
    lv_obj_set_style_text_color(icon, info.tint, 0);
    lv_obj_center(icon);
    return item;
}

static lv_obj_t *center_label_create(lv_obj_t *parent, const lv_font_t *font, int32_t y) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, y);
    return label;
}

void launcher_build(void (*open)(ui_app_t app), void (*dismissed)(void), void (*visibility_changed)(void)) {
    ln.open = open;
    ln.dismissed = dismissed;
    ln.visibility_changed = visibility_changed;

    // Opaque: anything showing through competes with the app name in the
    // middle. Still an overlay, so opening it reloads no screen.
    ln.scrim = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(ln.scrim);
    lv_obj_remove_flag(ln.scrim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ln.scrim, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(ln.scrim, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(ln.scrim, lv_color_hex(0x0b0b10), 0);
    lv_obj_set_style_bg_opa(ln.scrim, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(ln.scrim, scrim_click_cb, LV_EVENT_CLICKED, NULL);

    for (int i = 0; i < UI_APP_COUNT; i++) {
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

    ln.name = center_label_create(center, &inter_semi_16, LAUNCHER_NAME_Y);
    ln.status = center_label_create(center, &inter_med_13, LAUNCHER_STATUS_Y);

    ln.timer = lv_timer_create(status_timer_cb, LAUNCHER_STATUS_MS, NULL);
    lv_timer_pause(ln.timer);
    select_app(0);
}
