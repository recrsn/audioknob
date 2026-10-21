#include "ui/ui.h"

#include "ui/home/home.h"
#include "ui/internal.h"
#include "ui/now_playing/now_playing.h"
#include "ui/now_playing/switch.h"
#include "ui/shell/audio_chip.h"
#include "ui/shell/launcher.h"
#include "ui/shell/nav.h"
#include "ui/shell/sleep.h"

// Screens are display-only for state that another MCU owns (wireless
// playback/volume live on esp32, per the control-ownership rule in
// CLAUDE.md), and interactive only where s3 genuinely owns the state.
// Nothing here talks to UART or USB directly -- ui_update_*() is the only
// way data gets in, so this file stays pure LVGL and reusable by env:native.
//
// Layout convention: the panel is round, so every screen keeps its chrome
// (home swipe pill, status row) on the compass points -- top-center, bottom-
// center -- and out of the four corners, which a round bezel clips.
//
// Navigation: there is no mode-select menu. Real single-purpose audio
// devices land on an idle/clock screen and switch to a now-playing screen
// on their own the moment a source starts playing, then fall back to idle
// when it stops -- they don't make you pick a mode first. ui_update_*()
// drives that switch; the swipe-up pill on each now-playing screen sends it
// home while the source keeps playing, and the audio chip at the top of the
// clock and every app page brings it back.
//
// Apps live in the launcher (shell/launcher.c), opened by a long knob press
// anywhere or a tap on the clock; a second long press, a tap off its icons,
// or the pill on an app page goes home. An idle app page drops home on its own, and the clock
// shows chips for whatever is still in progress, so getting back to a
// running timer doesn't need the launcher.

// The screens live under home/ (the clock), now_playing/ (the source
// screens and picker) and apps/ (the pages); shell/ holds what spans them:
// navigation, the launcher, the audio chip, display sleep and the prompts.

// The knob doesn't go through an LVGL input device, so ui_wake() reports
// its activity for the idle timeouts.
bool ui_knob_rotate(int steps) {
    if (ui_wake()) {
        return true;
    }
    // A prompt answers to press and long press only.
    if (ui_pairing_visible() || ui_confirm_visible() || ui_firmware_visible()) {
        return true;
    }
    if (launcher_visible()) {
        launcher_rotate(steps);
        return true;
    }
    // Ambience playing is what the knob's volume reaches for: no stream
    // plays alongside it.
    return ui_tools_knob_rotate(lv_screen_active(), steps) || ui_ambience_knob(steps);
}

bool ui_knob_press(void) {
    if (ui_wake()) {
        return true;
    }
    if (ui_pairing_knob_press() || ui_confirm_knob_press() || ui_firmware_knob_press()) {
        return true;
    }
    if (launcher_visible()) {
        launcher_activate();
        return true;
    }
    return ui_tools_knob_press(lv_screen_active());
}

void ui_knob_long_press(void) {
    if (ui_wake()) {
        return;
    }
    if (ui_pairing_knob_long_press() || ui_confirm_knob_long_press() || ui_firmware_knob_long_press()) {
        return;
    }
    if (launcher_visible()) {
        nav_go_home();
    } else {
        nav_open_launcher();
    }
}

void ui_init(void) {
    // The clock's chips take each app's icon and tint, so the apps go first.
    nav_build();
    home_build();
    np_build();
    switch_build();
    audio_chip_build();
    launcher_build(&(launcher_handlers_t){.open = nav_launcher_open,
                                          .dismissed = nav_go_home,
                                          .visibility_changed = audio_chip_refresh,
                                          .audio_status = np_status_text});
    ui_sleep_build();
    nav_start(home_screen());

    // The clock's audio row starts idle, before any state push.
    audio_chip_refresh();
    home_refresh();
}

void ui_update_talk(const ui_talk_t *talk) {
    static bool receiving;
    bool incoming = talk->state == UI_TALK_RECEIVING;
    ui_talk_page_update(talk);
    if (incoming && !receiving && !ui_firmware_visible() && !ui_pairing_visible() && !ui_tools_alerting()) {
        ui_wake();
        nav_open_app(UI_APP_TALK);
    }
    receiving = incoming;
}
