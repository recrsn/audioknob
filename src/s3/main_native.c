// Desktop entry point for the LVGL native emulator target (env:native).
// Mirrors main.c's role on real hardware, but drives an SDL window instead
// of the QSPI LCD, so the UI in ui.c can be iterated on without flashing.
#include <time.h>

#include "lvgl.h"
#include "ui.h"

#define WINDOW_WIDTH  360
#define WINDOW_HEIGHT 360

static void clock_tick_cb(lv_timer_t *timer) {
    (void)timer;
    time_t now = time(NULL);
    struct tm local_time;
    localtime_r(&now, &local_time);
    ui_update_clock(true, &local_time);
}

// Stands in for the hardware knob: Up/Down arrows turn it and Enter presses
// it; the mouse wheel (turn + middle-click) works too. Everything lands here
// through one focused, invisible object.
static void knob_key_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        ui_knob_press();
        return;
    }
    // In edit mode the wheel's turns arrive as LEFT/RIGHT; on the keyboard
    // those stay unmapped.
    bool wheel = lv_indev_get_type(lv_indev_active()) == LV_INDEV_TYPE_ENCODER;
    uint32_t key = lv_event_get_key(e);
    if (key == LV_KEY_UP || (wheel && key == LV_KEY_RIGHT)) {
        ui_knob_rotate(1);
    } else if (key == LV_KEY_DOWN || (wheel && key == LV_KEY_LEFT)) {
        ui_knob_rotate(-1);
    }
}

static void knob_emulation_create(void) {
    lv_group_t *group = lv_group_create();
    lv_obj_t *sink = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(sink);
    lv_obj_set_size(sink, 0, 0);
    lv_obj_add_event_cb(sink, knob_key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(sink, knob_key_cb, LV_EVENT_CLICKED, NULL);
    lv_group_add_obj(group, sink);
    // Edit mode makes encoder turns arrive as LEFT/RIGHT keys instead of
    // moving focus.
    lv_group_set_editing(group, true);

    lv_indev_set_group(lv_sdl_mousewheel_create(), group);
    lv_indev_set_group(lv_sdl_keyboard_create(), group);
}

int main(void) {
    lv_init();

    lv_display_t *display = lv_sdl_window_create(WINDOW_WIDTH, WINDOW_HEIGHT);
    lv_sdl_mouse_create();

    ui_init();
    knob_emulation_create();
    lv_timer_create(clock_tick_cb, 1000, NULL);

    while (1) {
        uint32_t idle_ms = lv_timer_handler();
        lv_delay_ms(idle_ms ? idle_ms : 1);
    }

    (void)display;
    return 0;
}
