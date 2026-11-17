#include "ui/shell/volume_ring.h"

#include "lvgl.h"
#include "ui/theme.h"

#define RING_VISIBLE_MS 1400
#define RING_FADE_MS 300
// AirPlay and Bluetooth senders push their volume a moment after connecting.
#define SETTLE_MS 2000

static struct {
    lv_obj_t *arc;
    int source;
    bool connected;
    uint32_t since;
    int volume;
} s_ring = {.source = -1, .volume = -1};

static void opa_exec(void *obj, int32_t v) {
    lv_obj_set_style_opa(obj, (lv_opa_t)v, 0);
}

static void faded_cb(lv_anim_t *a) {
    lv_obj_add_flag(a->var, LV_OBJ_FLAG_HIDDEN);
}

static void flash(void) {
    lv_obj_t *arc = s_ring.arc;
    lv_anim_delete(arc, opa_exec);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(arc, LV_OPA_COVER, 0);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, arc);
    lv_anim_set_exec_cb(&a, opa_exec);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_delay(&a, RING_VISIBLE_MS);
    lv_anim_set_duration(&a, RING_FADE_MS);
    lv_anim_set_completed_cb(&a, faded_cb);
    lv_anim_start(&a);
}

// Read-only (volume authority lives off-screen): a full ring except a bottom
// gap for the home swipe zone.
void volume_ring_build(void) {
    lv_obj_t *arc = lv_arc_create(lv_layer_top());
    lv_obj_set_size(arc, UI_RING_SIZE, UI_RING_SIZE);
    lv_arc_set_rotation(arc, 135);
    lv_arc_set_bg_angles(arc, 0, 270);
    lv_arc_set_range(arc, 0, 100);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_width(arc, UI_RING_W, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(arc, UI_RING_W, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(UI_COLOR_SURFACE), LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(UI_TINT_AIRPLAY), LV_PART_INDICATOR);
    lv_obj_add_flag(arc, LV_OBJ_FLAG_HIDDEN);
    lv_obj_center(arc);
    s_ring.arc = arc;
}

void volume_ring_update(int source, bool connected, uint8_t volume) {
    if (!s_ring.arc) {
        return;
    }
    uint32_t now = lv_tick_get();
    if (!connected) {
        s_ring.connected = false;
        return;
    }
    if (!s_ring.connected || source != s_ring.source) {
        s_ring.connected = true;
        s_ring.source = source;
        s_ring.since = now;
    }
    if (volume == s_ring.volume) {
        return;
    }
    s_ring.volume = volume;
    // Hidden, these don't invalidate: a silent update draws nothing.
    lv_arc_set_value(s_ring.arc, volume);
    // A zero-length arc still paints its round cap as a stray dot.
    lv_obj_set_style_arc_opa(s_ring.arc, volume ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_INDICATOR);
    if (lv_tick_elaps(s_ring.since) >= SETTLE_MS) {
        flash();
    }
}

void volume_ring_set_tint(uint32_t tint) {
    lv_color_t color = lv_color_hex(tint);
    if (s_ring.arc && !lv_color_eq(lv_obj_get_style_arc_color(s_ring.arc, LV_PART_INDICATOR), color)) {
        lv_obj_set_style_arc_color(s_ring.arc, color, LV_PART_INDICATOR);
    }
}
