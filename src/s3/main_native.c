// Desktop entry point for the LVGL native emulator target (env:native).
// Mirrors main.c's role on real hardware, but drives an SDL window instead
// of the QSPI LCD, so the UI in ui.c can be iterated on without flashing.
#include <stdio.h>

#include "lvgl.h"
#include "ui.h"

#define WINDOW_WIDTH  360
#define WINDOW_HEIGHT 360

// No real UART/USB backend exists yet on this desktop target, so feed the
// screens fake state on a timer purely to make the UI demoable/iterable.
// Cycles idle -> wireless -> idle -> USB -> idle to also demo ui.c's
// auto-navigation (there's no mode-select menu; screens switch themselves
// based on which source is actually playing).
#define CYCLE_TICKS       80 // 250ms/tick -> 20s full loop
#define WIRELESS_START    20
#define WIRELESS_END      45
#define USB_START         50
#define USB_END           75

static void demo_tick_cb(lv_timer_t *timer) {
    (void)timer;
    static uint32_t tick = 0;
    tick++;
    uint32_t phase = tick % CYCLE_TICKS;

    bool wireless_playing = phase >= WIRELESS_START && phase < WIRELESS_END;
    bool usb_playing = phase >= USB_START && phase < USB_END;

    wireless_state_t wireless = {
        .volume = (tick * 3) % 101,
        .muted = 0,
        .connected = 1,
        .playing = wireless_playing,
        .source = (tick / CYCLE_TICKS) % 2 == 0 ? SOURCE_AIRPLAY : SOURCE_A2DP,
    };
    ui_track_info_t track = {0};
    snprintf(track.title, sizeof(track.title), "Demo Track %lu", (unsigned long)(tick / CYCLE_TICKS + 1));
    snprintf(track.artist, sizeof(track.artist), "Native Emulator");
    snprintf(track.album, sizeof(track.album), "env:native");
    ui_update_wireless(&wireless, &track);

    ui_update_usb((tick * 5) % 101, false, usb_playing);
}

int main(void) {
    lv_init();

    lv_display_t *display = lv_sdl_window_create(WINDOW_WIDTH, WINDOW_HEIGHT);
    lv_sdl_mouse_create();

    ui_init();
    lv_timer_create(demo_tick_cb, 250, NULL);

    while (1) {
        uint32_t idle_ms = lv_timer_handler();
        lv_delay_ms(idle_ms ? idle_ms : 1);
    }

    (void)display;
    return 0;
}
