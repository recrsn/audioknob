#include "ui/shell/nav.h"

#include "ui/home/home.h"
#include "ui/now_playing/now_playing.h"
#include "ui/now_playing/switch.h"
#include "ui/shell/audio_chip.h"
#include "ui/shell/launcher.h"
#include "ui/shell/sleep.h"
#include "ui/widgets/widgets.h"

// After playback stops, a source that's still connected keeps its screen up
// this long before going home, where the clock's card resumes it. Long enough
// to ride out the gap between tracks and a quick pause.
#define AUDIO_LINGER_MS (10 * 1000)

#define NAV_TICK_MS      250
#define APP_IDLE_HOME_MS 30000

static lv_obj_t *s_apps[UI_APP_COUNT];
// Where the launcher's selection starts when not opened from an app page.
static ui_app_t s_last_app;

// Where the home swipe and "playback stopped" return to: the clock normally,
// the WiFi setup QR while setup is pending.
static lv_obj_t *s_home;

// The user sent a playing source into the background (swiped home, or
// opened an app): it stays there until the audio chip is tapped or a source
// newly starts playing.
static bool s_homed;

// A manual source pick keeps the wireless screen up (its waiting state, or a
// paused stream) instead of dropping to the clock the way an auto-surfaced
// screen does; cleared by a home swipe or the next stream start.
static bool s_pinned;

static void (*s_alert_handler)(ui_alert_t alert);
static bool s_ringing;

lv_obj_t *nav_app_screen(ui_app_t app) {
    return s_apps[app];
}

lv_obj_t *nav_home(void) {
    return s_home;
}

void nav_set_home(lv_obj_t *screen) {
    if (s_home == screen) {
        return;
    }
    bool was_home = s_home && lv_screen_active() == s_home;
    s_home = screen;
    if (was_home) {
        nav_show(s_home);
    }
}

// Avoids re-loading (and restarting screen-load animations for) a screen
// that's already active, since ui_update_*() calls this on every state push.
void nav_show(lv_obj_t *target) {
    if (lv_screen_active() != target) {
        launcher_hide();
        lv_screen_load(target);
        audio_chip_refresh();
    }
}

void nav_go_home(void) {
    s_homed = true;
    s_pinned = false;
    launcher_hide();
    nav_show(s_home);
}

void nav_swipe_home_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_GESTURE && lv_indev_get_gesture_dir(lv_indev_active()) != LV_DIR_TOP) {
        return;
    }
    nav_go_home();
}

// Otherwise the next state push would bring a playing source straight back.
void nav_open_app(ui_app_t app) {
    s_last_app = app;
    s_homed = true;
    s_pinned = false;
    nav_show(s_apps[app]);
}

void nav_open_pinned(lv_obj_t *screen) {
    s_homed = false;
    s_pinned = true;
    nav_show(screen);
}

static bool on_audio_screen(void) {
    lv_obj_t *active = lv_screen_active();
    return np_is_screen(active) || active == switch_screen();
}

// From an app page its own icon is selected, from the audio screens the
// audio entry; from the clock, the app last opened.
void nav_open_launcher(void) {
    int app = ui_tools_app_of(lv_screen_active());
    launcher_show(app >= 0 ? app : on_audio_screen() ? LAUNCHER_AUDIO : (int)s_last_app);
}

// From there the source picker leads to the "waiting for" instructions.
void nav_open_audio(void) {
    lv_obj_t *target = np_audio_screen();
    if (!target) {
        switch_open();
        return;
    }
    // Paused audio would otherwise drop straight back home.
    nav_open_pinned(target);
}

void nav_launcher_open(int entry) {
    if (entry == LAUNCHER_AUDIO) {
        nav_open_audio();
    } else {
        nav_open_app((ui_app_t)entry);
    }
}

void nav_surface(lv_obj_t *screen, bool playing, bool connected, nav_surface_t *st) {
    if (playing && !st->was_playing) {
        s_homed = false;
        s_pinned = false;
        ui_wake();
    }
    if (!playing && st->was_playing) {
        st->stopped_at = lv_tick_get();
    }
    st->was_playing = playing;

    if (playing) {
        // The switch screen is user-opened and a ringing timer must be seen;
        // state pushes must not yank either away.
        if (!s_homed && lv_screen_active() != switch_screen() && !ui_tools_alerting()) {
            nav_show(screen);
        }
    } else if (lv_screen_active() == screen && !s_pinned &&
               (!connected || lv_tick_elaps(st->stopped_at) >= AUDIO_LINGER_MS)) {
        // A paused source keeps its screen, controls to hand, for a while.
        nav_show(s_home);
    }
}

void ui_set_alert_handler(void (*handler)(ui_alert_t alert)) {
    s_alert_handler = handler;
}

// Timer and tomato can ring at once: the handler hears each one start, so
// the latest one's chime plays, but only the last dismissal.
static void tool_alert_cb(lv_obj_t *screen, ui_alert_t alert) {
    bool ringing = alert != UI_ALERT_NONE;
    if (ringing) {
        ui_wake();
        launcher_hide();
        nav_show(screen);
    }
    bool any = ui_tools_alerting();
    if (ringing || any != s_ringing) {
        s_ringing = any;
        if (s_alert_handler) {
            s_alert_handler(any ? alert : UI_ALERT_NONE);
        }
    }
}

// Paging away is acknowledgement enough; a countdown left ringing off-screen
// would also keep nav_surface() from showing a new stream.
static void tool_unloaded_cb(lv_event_t *e) {
    ui_tools_dismiss(lv_event_get_current_target(e));
}

static void nav_tick_cb(lv_timer_t *t) {
    (void)t;
    ui_sleep_tick();
    lv_obj_t *active = lv_screen_active();
    if (active == home_screen()) {
        home_refresh();
        return;
    }
    if (active == np_wireless_screen()) {
        np_pill_refresh();
        return;
    }
    // A page left alone goes home, unless it's showing progress the user
    // may be watching; the clock's chips lead back to it either way.
    int app = ui_tools_app_of(active);
    char buf[16];
    if (app >= 0 && !launcher_visible() && !ui_tools_status((ui_app_t)app, buf, sizeof(buf)) &&
        lv_display_get_inactive_time(NULL) >= APP_IDLE_HOME_MS) {
        nav_go_home();
    }
}

void nav_build(void) {
    for (int i = 0; i < UI_APP_COUNT; i++) {
        s_apps[i] = ui_screen_create();
    }
    ui_tools_build(s_apps, tool_alert_cb);

    for (int i = 0; i < UI_APP_COUNT; i++) {
        ui_home_zone_create(s_apps[i], nav_swipe_home_cb);
        lv_obj_add_event_cb(s_apps[i], tool_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
    }
}

void nav_start(lv_obj_t *home) {
    s_home = home;
    lv_timer_create(nav_tick_cb, NAV_TICK_MS, NULL);
    lv_screen_load(s_home);
}
