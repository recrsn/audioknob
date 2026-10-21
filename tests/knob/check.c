#include "runtime.h"
#include "../../firmware/s3/src/device/drivers/knob.c"
#include <stdio.h>
static int turns, clicks, longs, calls;
static int order[128];
static void turn(int steps) { turns += steps; order[calls++] = steps; }
static void click(void) { ++clicks; order[calls++] = 10; }
static void hold(void) { ++longs; order[calls++] = 20; }
static lv_indev_t *encoder;
static void read_at(uint32_t ms) { test_ms = ms; lv_indev_read(encoder); }
static void edge_at(uint32_t ms, unsigned levels) {
    test_ms = ms; test_levels = levels; edge_isr(NULL);
}
static void clear_counts(void) { turns = clicks = longs = calls = 0; }
int main(void) {
    setbuf(stdout, NULL);
    lv_init();
    lv_display_t *display = lv_display_create(360, 360);
    knob_start(turn, click, hold);
    encoder = lv_indev_get_next(NULL);
    assert(encoder && lv_indev_get_type(encoder) == LV_INDEV_TYPE_ENCODER);
    // Capture several detents without letting LVGL run; preserve their order.
    edge_at(1200, 2); edge_at(1208, 3);
    edge_at(1220, 2); edge_at(1228, 3);
    edge_at(1300, 1); edge_at(1308, 3);
    assert(calls == 0);
    read_at(1350);
    assert(turns == 1 && calls == 3 && order[0] == 1 && order[1] == 1 && order[2] == -1);
    clear_counts();
    // Bounce and opposite-line chatter must not become detents.
    edge_at(1400, 2); edge_at(1403, 3);
    edge_at(1450, 2); edge_at(1458, 3);
    edge_at(1460, 1); edge_at(1468, 3);
    read_at(1500);
    assert(turns == 1 && calls == 1);
    clear_counts();
    // Short press: callback only on debounced release; no release turn.
    edge_at(1600, 0); read_at(1640); assert(calls == 0);
    edge_at(1700, 1); edge_at(1701, 3);
    read_at(1740);
    assert(clicks == 1 && longs == 0 && turns == 0);
    clear_counts();
    // Long press fires while held, once, and never short-clicks on release.
    edge_at(1900, 0); read_at(1940); read_at(2440);
    assert(longs == 1 && clicks == 0);
    read_at(3000); assert(longs == 1);
    edge_at(3010, 3); read_at(3050);
    assert(longs == 1 && clicks == 0 && turns == 0);
    clear_counts();
    // A whole long hold can happen during a blocked renderer.
    edge_at(3200, 0); edge_at(3900, 3); read_at(4000);
    assert(longs == 1 && clicks == 0 && turns == 0);
    clear_counts();
    // A bounced press doesn't click.
    edge_at(4200, 0); edge_at(4210, 3); read_at(4300);
    assert(calls == 0);
    // Queue overflow cancels uncertain history, rather than replaying a click.
    for (int i = 0; i < 80; ++i) edge_at(4400 + i, i % 2 ? 3 : 2);
    read_at(4550); assert(calls == 0);
    edge_at(4700, 2); edge_at(4710, 3); read_at(4750);
    assert(turns == 1 && calls == 1);
    clear_counts();
    // Overflow during a press must cancel it, including a later release.
    edge_at(4900, 0); read_at(4940);
    for (int i = 0; i < 80; ++i) edge_at(5000 + i, i % 2 ? 0 : 1);
    read_at(5200); read_at(5800);
    edge_at(5900, 3); read_at(6000);
    assert(calls == 0);
    edge_at(6100, 0); edge_at(6200, 3); read_at(6300);
    assert(clicks == 1 && calls == 1);
    clear_counts();
    // An ordinary long LVGL target would short-click on release in edit
    // mode; ensure this dedicated target remains non-scrollable/editable.
    assert(!lv_obj_is_scrollable(lv_group_get_focused(lv_indev_get_group(encoder))));
    assert(!lv_obj_is_editable(lv_group_get_focused(lv_indev_get_group(encoder))));
    // Debounce across the uint32_t millisecond wrap.
    test_ms = UINT32_MAX - 100;
    reset_decoder(pins());
    edge_at(UINT32_MAX - 50, 0); read_at(UINT32_MAX - 10);
    edge_at(80, 3); read_at(120);
    assert(clicks == 1 && longs == 0 && turns == 0);
    lv_indev_delete(encoder);
    lv_display_delete(display);
    lv_deinit();
    puts("knob: ordered turns, bounce, release suppression, short/long holds, delayed LVGL and overflow passed");
}
