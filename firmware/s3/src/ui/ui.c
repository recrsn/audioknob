#include "ui.h"
#include "lvgl.h"
#include "fonts.h"
#include "icons.h"
#include "ui_bg_data.h"
#include "ui_internal.h"
#include "ui_launcher.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
// Apps live in the launcher (ui_launcher.c), opened by a long knob press
// anywhere or a tap on the clock; a second long press, a tap off its icons,
// or the pill on an app page goes home. An idle app page drops home on its own, and the clock
// shows chips for whatever is still in progress, so getting back to a
// running timer doesn't need the launcher.

#define ART_TILE_SIZE 132

// Y offsets (from screen center) extracted directly from the mock-react
// reference's rendered layout (localhost:5173, getBoundingClientRect() on
// its 360x360 .screen) rather than eyeballed. The mock centers its whole
// content block per screen (flex column, justify-content:center), so these
// differ by state/screen depending on how tall that block is -- there's no
// LVGL flex equivalent worth building for three fixed cases, so each is
// just pinned to its measured value.
#define WIRELESS_TILE_Y                 (-41)
#define WIRELESS_TRANSPORT_Y            63
#define WIRELESS_TITLE_Y                111
#define WIRELESS_SUBTITLE_Y             131

#define WIRELESS_STATUS_ROW_Y_WAITING (-84)
#define WIRELESS_WAIT_ICON_Y          (-10)
#define WIRELESS_WAIT_CHIP_SIZE       96
#define WIRELESS_WAIT_ICON_SIZE       36
#define WIRELESS_WAIT_TITLE_Y         62
#define WIRELESS_WAIT_SUBTITLE_Y      82

#ifdef AK_USB_AUDIO
#define USB_TILE_Y        (-33)
#define USB_TRANSPORT_Y   71
#define USB_TITLE_Y        118
#endif

// tools/gen_backgrounds.py bakes the tile into the backgrounds at these.
_Static_assert(ART_TILE_SIZE == UI_BG_TILE_SIZE && UI_ART_SIZE == UI_BG_TILE_SIZE, "regenerate backgrounds");
_Static_assert(WIRELESS_TILE_Y == UI_BG_WIRELESS_TILE_Y, "regenerate backgrounds");
#ifdef AK_USB_AUDIO
_Static_assert(USB_TILE_Y == UI_BG_USB_TILE_Y, "regenerate backgrounds");
#endif

#define TRANSPORT_GAP UI_SPACE_L

#define STATUS_ICON_SIZE 14

#define RING_VISIBLE_MS 1400
#define RING_FADE_MS    300

#define HOME_ZONE_W 160
#define HOME_ZONE_H 52

// LVGL only looks for a child under a touch inside its parent's bounds, so
// a padded hit area sticking out of a content-sized container is cut off.
// Padding the container as much as its children's hit areas reach puts
// them back; the containers are centered, so nothing moves.
static void touch_pad(lv_obj_t *container, int32_t ext) {
    lv_obj_set_style_pad_all(container, ext, 0);
}

typedef struct {
    lv_obj_t *screen;
    ui_now_playing_bg_t bg;
    lv_obj_t *ring;
    lv_obj_t *source_icon;
    lv_obj_t *source_label;

    // Shown when state->connected is false, so there's nothing to show a
    // tile/track for yet. For Bluetooth, tapping the chip opens a pairing
    // window; bonded phones reconnect without one.
    lv_obj_t *wait_chip;
    lv_obj_t *wait_icon;
    lv_obj_t *wait_title;
    lv_obj_t *wait_subtitle;
    bool bluetooth;

    // Shown once connected. The tile itself is part of the background.
    lv_obj_t *tile_icon;
    lv_obj_t *art;
    lv_image_dsc_t art_dsc;
    uint32_t art_version;
    lv_obj_t *transport;
    lv_obj_t *prev_icon;
    lv_obj_t *play_icon;
    lv_obj_t *next_icon;
    lv_obj_t *title_label;
    lv_obj_t *subtitle_label;
} wireless_screen_t;

#ifdef AK_USB_AUDIO
typedef struct {
    lv_obj_t *screen;
    ui_now_playing_bg_t bg;
    lv_obj_t *ring;
    lv_obj_t *prev_icon;
    lv_obj_t *play_icon;
    lv_obj_t *next_icon;
} usb_screen_t;
#endif

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *items[2];  // indexed by ui_source_t
} switch_screen_t;

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *time_label;
    lv_obj_t *ampm_label;
    lv_obj_t *date_label;
    lv_obj_t *weather;
    lv_obj_t *weather_icon;
    lv_obj_t *weather_temp;
    lv_obj_t *weather_sky;
    lv_obj_t *weather_range;
    // One per app, shown while that app has something in progress.
    lv_obj_t *chips[UI_APP_COUNT];
    lv_obj_t *chip_labels[UI_APP_COUNT];

    // What's playing, under the clock while a source is connected: the
    // clock's way back to it, in place of the audio chip.
    struct {
        lv_obj_t *card;
        lv_obj_t *thumb;
        lv_obj_t *icon;
        lv_obj_t *idle_icon;
        lv_obj_t *art;
        lv_obj_t *text;
        lv_obj_t *title;
        lv_obj_t *subtitle;
        lv_obj_t *play;
        int8_t quiet; // the idle styling applied: 1, 0, or -1 before either
        uint32_t art_version;
        lv_color_t tint;
        lv_color_t icon_color;
    } np;
} idle_screen_t;

// What's playing, kept from the last ui_update_wireless()/ui_update_usb()
// for the audio chip.
typedef struct {
    bool wireless_connected;
    bool wireless_playing;
#ifdef AK_USB_AUDIO
    bool usb_playing;
#endif
    bool wireless_airplay;
    char wireless_text[64];
    char wireless_subtitle[64];
    char wireless_peer[64];
} audio_state_t;

// The audio chip: a persistent "back to what's playing" pill at the top of
// the clock and every app page, on lv_layer_top() so one widget serves them
// all.
typedef struct {
    lv_obj_t *chip;
    lv_obj_t *icon;     // AirPlay/Bluetooth
#ifdef AK_USB_AUDIO
    lv_obj_t *usb_icon; // LV_SYMBOL_USB has no image asset
#endif
    lv_obj_t *label;
    lv_obj_t *target;   // the source screen a tap goes to
    bool dim;
} audio_chip_t;

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *qr;
    lv_obj_t *status_label;
    lv_obj_t *network_label;
    char qr_payload[128];
} wifi_setup_screen_t;

static idle_screen_t idle_screen;
static wireless_screen_t wireless_screen;
#ifdef AK_USB_AUDIO
static usb_screen_t usb_screen;
#endif
static wifi_setup_screen_t wifi_setup_screen;
static switch_screen_t switch_screen;
static audio_state_t s_audio;
static audio_chip_t audio_chip;

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
static void (*s_transport_handler)(ui_transport_t);
static void (*s_source_handler)(ui_source_t);

// A manual source pick keeps the wireless screen up (its waiting state, or a
// paused stream) instead of dropping to the clock the way an auto-surfaced
// screen does; cleared by a home swipe or the next stream start.
static bool s_pinned;
static ui_source_t s_current_source = UI_SOURCE_AIRPLAY;

void ui_set_source_handler(void (*handler)(ui_source_t source)) {
    s_source_handler = handler;
}

void ui_set_transport_handler(void (*handler)(ui_transport_t button)) {
    s_transport_handler = handler;
}

static void transport_click_cb(lv_event_t *e) {
    if (s_transport_handler) {
        s_transport_handler((ui_transport_t)(uintptr_t)lv_event_get_user_data(e));
    }
}

static void audio_chip_refresh(void);

// Avoids re-loading (and restarting screen-load animations for) a screen
// that's already active, since ui_update_*() calls this on every state push.
// Any real navigation closes the launcher.
static void show_screen(lv_obj_t *target) {
    if (lv_screen_active() != target) {
        launcher_hide();
        lv_screen_load(target);
        audio_chip_refresh();
    }
}

static void go_home(void) {
    s_homed = true;
    s_pinned = false;
    launcher_hide();
    show_screen(s_home);
}

static void swipe_home_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_GESTURE && lv_indev_get_gesture_dir(lv_indev_active()) != LV_DIR_TOP) {
        return;
    }
    go_home();
}

// Opening an app sends a playing source to the background, as a swipe home
// would; otherwise the next state push would bring it straight back.
static void open_app(ui_app_t app) {
    s_last_app = app;
    s_homed = true;
    s_pinned = false;
    show_screen(s_apps[app]);
}

static void switch_back_cb(lv_event_t *e);
static void open_switch_cb(lv_event_t *e);
static lv_obj_t *audio_screen(void);

static bool on_audio_screen(void) {
    lv_obj_t *active = lv_screen_active();
#ifdef AK_USB_AUDIO
    if (active == usb_screen.screen) {
        return true;
    }
#endif
    return active == wireless_screen.screen || active == switch_screen.screen;
}

// From an app page its own icon is selected, from the audio screens the
// audio entry; from the clock, the app last opened.
static void open_launcher(void) {
    int app = ui_tools_app_of(lv_screen_active());
    launcher_show(app >= 0 ? app : on_audio_screen() ? LAUNCHER_AUDIO : (int)s_last_app);
}

// What's playing, or with nothing connected, the source picker and from
// there the "waiting for" instructions.
static void open_audio(void) {
    lv_obj_t *target = audio_screen();
    if (!target) {
        open_switch_cb(NULL);
        return;
    }
    s_homed = false;
    // Paused audio would otherwise drop straight back home.
    s_pinned = true;
    show_screen(target);
}

static void launcher_open(int entry) {
    if (entry == LAUNCHER_AUDIO) {
        open_audio();
    } else {
        open_app((ui_app_t)entry);
    }
}

// After playback stops, a source that's still connected keeps its screen up
// this long before going home.
#define AUDIO_LINGER_MS (300 * 1000)

typedef struct {
    bool was_playing;
    uint32_t stopped_at; // lv_tick when playback last stopped
} surface_state_t;

// Shared by every source: surfaces `screen` while `playing`, falls back to
// home once it stops (AUDIO_LINGER_MS later while still connected), and
// respects an in-progress swipe home. A fresh
// not-playing -> playing edge always un-homes, so a new stream is never
// hidden behind the clock.
static void surface_source(lv_obj_t *screen, bool playing, bool connected, surface_state_t *st) {
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
        if (!s_homed && lv_screen_active() != switch_screen.screen && !ui_tools_alerting()) {
            show_screen(screen);
        }
    } else if (lv_screen_active() == screen && !s_pinned &&
               (!connected || lv_tick_elaps(st->stopped_at) >= AUDIO_LINGER_MS)) {
        // A paused source keeps its screen, controls to hand, for a while.
        show_screen(s_home);
    }
}

static lv_obj_t *screen_create(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    return scr;
}

// mock-react's .home-swipe-zone: a thin pill at the south compass point is
// the only affordance, the whole transparent zone is the hit target. Swipe
// up (or tap) goes home while the source keeps playing.
static void home_zone_create(lv_obj_t *parent, lv_event_cb_t cb) {
    lv_obj_t *zone = lv_obj_create(parent);
    lv_obj_remove_style_all(zone);
    lv_obj_set_size(zone, HOME_ZONE_W, HOME_ZONE_H);
    lv_obj_align(zone, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_remove_flag(zone, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(zone, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(zone, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(zone, cb, LV_EVENT_GESTURE, NULL);

    lv_obj_t *pill = lv_obj_create(zone);
    lv_obj_remove_style_all(pill);
    lv_obj_remove_flag(pill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(pill, 46, 4);
    lv_obj_set_style_radius(pill, UI_RADIUS_HAIRLINE, 0);
    lv_obj_set_style_bg_color(pill, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_60, 0);
    // Pages may draw under it (the tool dials), so it carries its own
    // contrast instead of relying on a dark backdrop.
    lv_obj_set_style_outline_width(pill, 2, 0);
    lv_obj_set_style_outline_color(pill, lv_color_black(), 0);
    lv_obj_set_style_outline_opa(pill, LV_OPA_50, 0);
    lv_obj_align(pill, LV_ALIGN_BOTTOM_MID, 0, -14);
}

// Status row / now-playing transport row: icon(s) + text, laid out with
// flex row so LVGL computes the internal spacing, but positioned via a
// caller-supplied Y since the surrounding screens don't all agree on where
// that Y should be (see the WIRELESS_*_Y / USB_*_Y offsets above).
static lv_obj_t *flex_row_create(lv_obj_t *parent, int32_t pad_column, int32_t y) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, pad_column, 0);
    lv_obj_align(row, LV_ALIGN_CENTER, 0, y);
    return row;
}

// lv_image_set_scale() only scales the drawn pixels, not the layout box, so
// a scaled-down icon would still reserve its full 64px in a flex row.
// Stretching into an explicitly sized box shrinks both.
static lv_obj_t *icon_create(lv_obj_t *parent, const lv_image_dsc_t *src, int32_t size, lv_color_t color) {
    lv_obj_t *icon = lv_image_create(parent);
    lv_image_set_src(icon, src);
    lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_STRETCH);
    lv_obj_set_size(icon, size, size);
    lv_obj_set_style_image_recolor(icon, color, 0);
    lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
    return icon;
}

// Every chip: a translucent pill of icon + text, one height, capped to the
// safe area's width, tappable past its edges when given a callback.
static lv_obj_t *chip_create(lv_obj_t *parent, int32_t pad_column, lv_event_cb_t cb, void *user_data) {
    lv_obj_t *chip = flex_row_create(parent, pad_column, 0);
    lv_obj_set_height(chip, UI_CHIP_H);
    lv_obj_set_style_max_width(chip, UI_CHIP_MAX_W, 0);
    lv_obj_set_style_pad_hor(chip, UI_CHIP_PAD_X, 0);
    lv_obj_set_style_radius(chip, UI_RADIUS_PILL, 0);
    lv_obj_set_style_bg_color(chip, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(chip, UI_OPA_GLASS, 0);
    if (cb) {
        lv_obj_add_flag(chip, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(chip, UI_TOUCH_EXT);
        lv_obj_add_event_cb(chip, cb, LV_EVENT_CLICKED, user_data);
    }
    return chip;
}

// In the top chip band.
static void chip_align_top(lv_obj_t *chip) {
    lv_obj_align(chip, LV_ALIGN_TOP_MID, 0, UI_CHIP_INSET);
}

// mock-react's .status-row.tappable: the pill is the only hint that it's
// the source indicator.
static lv_obj_t *status_pill_create(lv_obj_t *parent, lv_event_cb_t cb) {
    return chip_create(parent, UI_SPACE_XS, cb, NULL);
}

static lv_obj_t *status_label_create(lv_obj_t *row) {
    lv_obj_t *label = lv_label_create(row);
    lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_TEXT_DIM), 0);
    lv_obj_set_style_text_font(label, UI_FONT_BODY, 0);
    return label;
}

static lv_obj_t *marquee_label_create(lv_obj_t *parent, const lv_font_t *font, lv_color_t color) {
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_set_width(label, UI_TEXT_W);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    return label;
}

// One now-playing transport button (mock-react's .now-btn / .now-btn.primary):
// a circular chip with a centered glyph. Returns the inner label so callers
// can swap its symbol/color later (e.g. play <-> pause). `dim` renders it as
// mock-react's `.now-btn.disabled` -- there's no reverse UART channel yet for
// touch-originated transport commands (no MSG_TRANSPORT_CMD in protocol.h),
// so prev/next are shown for layout fidelity but stay visually inert rather
// than pretending to work.
lv_obj_t *ui_round_button_create(lv_obj_t *parent, const char *symbol, int32_t size,
                                 const lv_font_t *font, bool dim) {
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(btn, size, size);
    lv_obj_set_style_radius(btn, UI_RADIUS_PILL, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(UI_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    if (dim) {
        lv_obj_set_style_opa(btn, UI_OPA_DISABLED, 0);
    }

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, symbol);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_center(label);
    return label;
}

static void ring_opa_exec(void *obj, int32_t v) {
    lv_obj_set_style_opa(obj, (lv_opa_t)v, 0);
}

// The ring is a HUD, not chrome (mock-react's .ring-svg.volume): it appears
// for a beat only when the volume moves, then fades so
// the full circle belongs to the art.
static void ring_flash(lv_obj_t *ring) {
    lv_anim_delete(ring, ring_opa_exec);
    lv_obj_set_style_opa(ring, LV_OPA_COVER, 0);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, ring);
    lv_anim_set_exec_cb(&a, ring_opa_exec);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_delay(&a, RING_VISIBLE_MS);
    lv_anim_set_duration(&a, RING_FADE_MS);
    lv_anim_start(&a);
}

static void ring_set_volume(lv_obj_t *ring, uint8_t volume) {
    if (lv_arc_get_value(ring) != volume) {
        lv_arc_set_value(ring, volume);
        ring_flash(ring);
    }
    // A zero-length arc still paints its round cap as a stray dot.
    lv_obj_set_style_arc_opa(ring, volume ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_INDICATOR);
}

// Perimeter volume gauge: read-only (volume authority lives off-screen), a
// full ring except a bottom gap for the home swipe zone.
static lv_obj_t *volume_ring_create(lv_obj_t *parent, lv_color_t tint) {
    lv_obj_t *arc = lv_arc_create(parent);
    lv_obj_set_size(arc, UI_RING_SIZE, UI_RING_SIZE);
    lv_arc_set_rotation(arc, 135);
    lv_arc_set_bg_angles(arc, 0, 270);
    lv_arc_set_range(arc, 0, 100);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_width(arc, UI_RING_W, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(arc, UI_RING_W, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(UI_COLOR_SURFACE), LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, tint, LV_PART_INDICATOR);
    lv_obj_set_style_opa(arc, LV_OPA_TRANSP, 0);
    lv_obj_center(arc);
    return arc;
}

// The state poll pushes every 33 ms; lv_label_set_text() frees and reallocates
// the string and redraws even when it's unchanged, churning LVGL's heap.
void ui_label_set_text(lv_obj_t *label, const char *text) {
    if (strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
}

void ui_set_visible(lv_obj_t *obj, bool visible) {
    if (visible) {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

static lv_obj_t *glance_label_create(lv_obj_t *parent, const lv_font_t *font, lv_opa_t opa) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_width(label, UI_TEXT_W);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_text_opa(label, opa, 0);
    return label;
}

#define CLOCK_CHIP_GAP UI_SPACE_S

static void clock_tap_cb(lv_event_t *e) {
    (void)e;
    open_launcher();
}

static void clock_chip_cb(lv_event_t *e) {
    open_app((ui_app_t)(intptr_t)lv_event_get_user_data(e));
}

static void clock_weather_cb(lv_event_t *e) {
    (void)e;
    open_app(UI_APP_WEATHER);
}

// A small tappable pill: an icons_20 glyph plus a label, which is returned.
static lv_obj_t *clock_pill_create(lv_obj_t *parent, const char *icon, lv_color_t tint, lv_event_cb_t cb,
                                   void *user_data) {
    lv_obj_t *pill = chip_create(parent, UI_SPACE_XS, cb, user_data);

    lv_obj_t *glyph = lv_label_create(pill);
    lv_label_set_text(glyph, icon);
    lv_obj_set_style_text_font(glyph, UI_FONT_ICON_SM, 0);
    lv_obj_set_style_text_color(glyph, tint, 0);

    lv_obj_t *label = lv_label_create(pill);
    lv_obj_set_style_text_font(label, UI_FONT_LABEL, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_TEXT), 0);
    return label;
}

// Space between the clock (date + time) and the complications above and
// below it, on top of the column's own spacing.
#define FACE_COMP_GAP UI_SPACE_M

#define NP_CARD_W     212
#define NP_IDLE_GLYPH   UI_COLOR_ACCENT
#define NP_IDLE_GLYPH_W 18
#define NP_CARD_H     48
#define NP_CARD_PAD   UI_SPACE_S
#define NP_THUMB      UI_BUTTON_SIZE_XS


static void np_card_cb(lv_event_t *e) {
    (void)e;
    open_audio();
}

// The clock face's top complication, heading its column: what's playing,
// or with nothing connected, that the device is ready to. Its button
// plays/pauses without leaving the clock.
static void np_card_build(lv_obj_t *col) {
    lv_obj_t *card = lv_obj_create(col);
    lv_obj_remove_style_all(card);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, np_card_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_size(card, NP_CARD_W, NP_CARD_H);
    lv_obj_set_style_margin_bottom(card, FACE_COMP_GAP, 0);
    lv_obj_set_style_pad_all(card, NP_CARD_PAD, 0);
    lv_obj_set_style_pad_column(card, UI_SPACE_S, 0);
    lv_obj_set_style_radius(card, UI_RADIUS_PILL, 0);
    // A bigger chip, not a panel: the clock stays the face.
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(card, UI_OPA_GLASS, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    idle_screen.np.card = card;
    idle_screen.np.quiet = -1;

    // Cover art when the source sends some, else the source's icon.
    lv_obj_t *thumb = lv_obj_create(card);
    lv_obj_remove_style_all(thumb);
    lv_obj_remove_flag(thumb, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(thumb, NP_THUMB, NP_THUMB);
    lv_obj_set_style_radius(thumb, UI_RADIUS_PILL, 0);
    lv_obj_set_style_clip_corner(thumb, true, 0);
    lv_obj_set_style_bg_opa(thumb, LV_OPA_COVER, 0);
    idle_screen.np.thumb = thumb;
    idle_screen.np.icon = icon_create(thumb, &icon_airplay, 16, lv_color_hex(UI_COLOR_TEXT));
    lv_obj_center(idle_screen.np.icon);
    idle_screen.np.idle_icon = lv_label_create(thumb);
    lv_label_set_text(idle_screen.np.idle_icon, LV_SYMBOL_VOLUME_MAX);
    lv_obj_set_style_text_font(idle_screen.np.idle_icon, UI_FONT_SYMBOL, 0);
    lv_obj_set_style_text_color(idle_screen.np.idle_icon, lv_color_hex(NP_IDLE_GLYPH), 0);
    lv_obj_center(idle_screen.np.idle_icon);
    idle_screen.np.art = lv_image_create(thumb);
    lv_image_set_inner_align(idle_screen.np.art, LV_IMAGE_ALIGN_STRETCH);
    lv_obj_set_size(idle_screen.np.art, NP_THUMB, NP_THUMB);
    lv_obj_add_flag(idle_screen.np.art, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *text = lv_obj_create(card);
    idle_screen.np.text = text;
    lv_obj_remove_style_all(text);
    lv_obj_remove_flag(text, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_height(text, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(text, 1);
    lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(text, UI_SPACE_XXS, 0);
    idle_screen.np.title = lv_label_create(text);
    idle_screen.np.subtitle = lv_label_create(text);
    lv_obj_t *labels[] = {idle_screen.np.title, idle_screen.np.subtitle};
    for (int i = 0; i < 2; i++) {
        lv_obj_set_width(labels[i], lv_pct(100));
        lv_label_set_long_mode(labels[i], LV_LABEL_LONG_MODE_DOTS);
        lv_label_set_text(labels[i], "");
    }
    lv_obj_set_style_text_font(idle_screen.np.title, UI_FONT_LABEL, 0);
    lv_obj_set_style_text_color(idle_screen.np.title, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(idle_screen.np.subtitle, UI_FONT_CAPTION, 0);
    lv_obj_set_style_text_color(idle_screen.np.subtitle, lv_color_hex(UI_COLOR_TEXT_DIM), 0);
    // A content-tall label wraps rather than dots.
    for (int i = 0; i < 2; i++) {
        lv_obj_set_height(labels[i], lv_font_get_line_height(lv_obj_get_style_text_font(labels[i], 0)));
    }

    idle_screen.np.play = ui_round_button_create(card, LV_SYMBOL_PLAY, UI_BUTTON_SIZE_XS, UI_FONT_SYMBOL_SM, false);
    lv_obj_t *btn = lv_obj_get_parent(idle_screen.np.play);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    // Out to the card's edge, which is as far as LVGL looks for it; the
    // rest of the card opens the source.
    lv_obj_set_ext_click_area(btn, NP_CARD_PAD);
    lv_obj_add_event_cb(btn, transport_click_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)UI_TRANSPORT_PLAY_PAUSE);
}

// The clock face's bottom complication, two quiet lines -- conditions
// glyph, temperature and sky, then the day's range -- opening the weather
// page on tap.
static void weather_complication_build(lv_obj_t *col) {
    lv_obj_t *comp = lv_obj_create(col);
    lv_obj_remove_style_all(comp);
    lv_obj_remove_flag(comp, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(comp, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(comp, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(comp, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_margin_top(comp, FACE_COMP_GAP, 0);
    lv_obj_add_flag(comp, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_ext_click_area(comp, UI_TOUCH_EXT);
    lv_obj_add_event_cb(comp, clock_weather_cb, LV_EVENT_CLICKED, NULL);
    idle_screen.weather = comp;

    lv_obj_t *now = flex_row_create(comp, UI_SPACE_S, 0);
    lv_obj_remove_flag(now, LV_OBJ_FLAG_CLICKABLE);
    idle_screen.weather_icon = lv_label_create(now);
    lv_obj_set_style_text_font(idle_screen.weather_icon, UI_FONT_ICON_SM, 0);
    lv_obj_set_style_text_color(idle_screen.weather_icon, lv_color_hex(UI_TINT_WEATHER), 0);
    lv_label_set_text(idle_screen.weather_icon, ICON_CLOUD);
    idle_screen.weather_temp = lv_label_create(now);
    lv_obj_set_style_text_font(idle_screen.weather_temp, UI_FONT_TITLE, 0);
    lv_obj_set_style_text_color(idle_screen.weather_temp, lv_color_hex(UI_COLOR_TEXT), 0);
    idle_screen.weather_sky = lv_label_create(now);
    lv_obj_set_style_text_font(idle_screen.weather_sky, UI_FONT_BODY, 0);
    lv_obj_set_style_text_color(idle_screen.weather_sky, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_text_opa(idle_screen.weather_sky, UI_OPA_TEXT_MUTED, 0);

    idle_screen.weather_range = lv_label_create(comp);
    lv_obj_set_style_text_font(idle_screen.weather_range, UI_FONT_CAPTION, 0);
    lv_obj_set_style_text_color(idle_screen.weather_range, lv_color_hex(UI_COLOR_TEXT_DIM), 0);
}

static void idle_screen_build(void) {
    idle_screen.screen = screen_create();
    lv_obj_add_event_cb(idle_screen.screen, clock_tap_cb, LV_EVENT_CLICKED, NULL);

    ui_wallpaper_create(idle_screen.screen);

    // Everything stacks in one centered column (mock's .clock-idle), so the
    // weather pill can show or hide without re-pinning the clock.
    lv_obj_t *col = lv_obj_create(idle_screen.screen);
    lv_obj_remove_style_all(col);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(col, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, UI_SPACE_XS, 0);
    touch_pad(col, UI_TOUCH_EXT);
    lv_obj_center(col);

    np_card_build(col);
    idle_screen.date_label = glance_label_create(col, UI_FONT_TITLE, UI_OPA_TEXT_SOFT);

    // Content-wide so the time stays centered; AM/PM hangs off its right
    // edge on the digits' baseline, outside the column's layout.
    lv_obj_t *time = lv_label_create(col);
    lv_obj_set_style_text_font(time, UI_FONT_CLOCK, 0);
    lv_obj_set_style_text_color(time, lv_color_hex(UI_COLOR_TEXT), 0);
    idle_screen.time_label = time;
    lv_obj_t *ampm = lv_label_create(col);
    lv_obj_add_flag(ampm, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_style_text_font(ampm, UI_FONT_TITLE, 0);
    lv_obj_set_style_text_color(ampm, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_text_opa(ampm, UI_OPA_TEXT_SOFT, 0);
    lv_obj_add_flag(ampm, LV_OBJ_FLAG_HIDDEN);
    idle_screen.ampm_label = ampm;

    weather_complication_build(col);

    lv_obj_t *chips = flex_row_create(idle_screen.screen, CLOCK_CHIP_GAP, 0);
    lv_obj_remove_flag(chips, LV_OBJ_FLAG_CLICKABLE);
    touch_pad(chips, UI_TOUCH_EXT);
    lv_obj_set_style_max_width(chips, UI_CHIP_MAX_W + 2 * UI_TOUCH_EXT, 0);
    lv_obj_align(chips, LV_ALIGN_BOTTOM_MID, 0, -UI_CHIP_INSET + UI_TOUCH_EXT);
    for (int i = 0; i < UI_APP_COUNT; i++) {
        ui_app_info_t info = ui_tools_info((ui_app_t)i);
        idle_screen.chip_labels[i] = clock_pill_create(chips, info.icon, info.tint, clock_chip_cb, (void *)(intptr_t)i);
        idle_screen.chips[i] = lv_obj_get_parent(idle_screen.chip_labels[i]);
        lv_obj_add_flag(idle_screen.chips[i], LV_OBJ_FLAG_HIDDEN);
    }

    ui_update_clock(false, NULL);
}

// Out of the column's layout, so it follows the time by hand: after the
// time's text changes, and whenever the column reflows around it.
static void ampm_place(void) {
    if (lv_obj_has_flag(idle_screen.ampm_label, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    lv_obj_update_layout(idle_screen.time_label);
    lv_obj_align_to(idle_screen.ampm_label, idle_screen.time_label, LV_ALIGN_OUT_RIGHT_BOTTOM, UI_SPACE_XS,
                    -(UI_FONT_CLOCK->base_line - UI_FONT_TITLE->base_line));
}

// Polled: what's in progress changes every second, the weather every 15 min.
static void idle_screen_refresh(void) {
    char buf[16];
    for (int i = 0; i < UI_APP_COUNT; i++) {
        bool busy = ui_tools_status((ui_app_t)i, buf, sizeof(buf));
        ui_set_visible(idle_screen.chips[i], busy);
        if (busy) {
            ui_label_set_text(idle_screen.chip_labels[i], buf);
        }
    }
    ui_weather_glance_t wx;
    bool have_weather = ui_weather_glance(&wx);
    ui_set_visible(idle_screen.weather, have_weather);
    if (have_weather) {
        ui_label_set_text(idle_screen.weather_icon, wx.icon);
        snprintf(buf, sizeof(buf), "%d\xC2\xB0", wx.temp);
        ui_label_set_text(idle_screen.weather_temp, buf);
        ui_label_set_text(idle_screen.weather_sky, wx.sky);
        char range[24];
        snprintf(range, sizeof(range), "H %d\xC2\xB0  L %d\xC2\xB0", wx.high, wx.low);
        ui_label_set_text(idle_screen.weather_range, range);
    }
    ampm_place();
}

static void wait_chip_click_cb(lv_event_t *e) {
    (void)e;
    if (wireless_screen.bluetooth) {
        ui_pairing_request(UI_PAIRING_OPEN);
    }
}

// The volume ring takes the wash's tint, the art's colour while there's art.
static void wireless_tint_cb(uint32_t tint) {
    if (wireless_screen.ring) {
        lv_obj_set_style_arc_color(wireless_screen.ring, lv_color_hex(tint), LV_PART_INDICATOR);
    }
}

static void wireless_screen_build(void) {
    wireless_screen.screen = screen_create();
    ui_now_playing_bg_create(&wireless_screen.bg, wireless_screen.screen, UI_BG_WIRELESS, UI_TINT_AIRPLAY,
                             wireless_tint_cb);
    wireless_screen.ring = volume_ring_create(wireless_screen.screen, lv_color_hex(UI_TINT_AIRPLAY));

    // Y is set per-update (WIRELESS_STATUS_ROW_Y_CONNECTED vs. _WAITING) --
    // the mock's flex column re-centers around whichever block is showing.
    lv_obj_t *row = status_pill_create(wireless_screen.screen, open_switch_cb);
    wireless_screen.source_icon = icon_create(row, &icon_airplay, STATUS_ICON_SIZE, lv_color_hex(UI_COLOR_TEXT));
    wireless_screen.source_label = status_label_create(row);

    // -- waiting-for-connection state (mock's .wait-icon / np-meta) --
    wireless_screen.wait_chip = lv_obj_create(wireless_screen.screen);
    lv_obj_remove_flag(wireless_screen.wait_chip, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(wireless_screen.wait_chip, wait_chip_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_size(wireless_screen.wait_chip, WIRELESS_WAIT_CHIP_SIZE, WIRELESS_WAIT_CHIP_SIZE);
    lv_obj_set_style_radius(wireless_screen.wait_chip, UI_RADIUS_PILL, 0);
    lv_obj_set_style_border_width(wireless_screen.wait_chip, 0, 0);
    lv_obj_set_style_pad_all(wireless_screen.wait_chip, 0, 0);
    lv_obj_set_style_bg_color(wireless_screen.wait_chip, lv_color_hex(UI_COLOR_SURFACE), 0);
    lv_obj_align(wireless_screen.wait_chip, LV_ALIGN_CENTER, 0, WIRELESS_WAIT_ICON_Y);
    wireless_screen.wait_icon =
        icon_create(wireless_screen.wait_chip, &icon_airplay, WIRELESS_WAIT_ICON_SIZE, lv_color_hex(UI_COLOR_TEXT_DIM));
    lv_obj_center(wireless_screen.wait_icon);

    wireless_screen.wait_title = lv_label_create(wireless_screen.screen);
    lv_obj_set_style_text_font(wireless_screen.wait_title, UI_FONT_TITLE, 0);
    lv_obj_set_style_text_color(wireless_screen.wait_title, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_align(wireless_screen.wait_title, LV_ALIGN_CENTER, 0, WIRELESS_WAIT_TITLE_Y);

    // Montserrat runs wider than the mock's SF, so the longest hint doesn't
    // fit in one line; scrolling keeps the mock's single-line layout.
    wireless_screen.wait_subtitle =
        marquee_label_create(wireless_screen.screen, UI_FONT_BODY, lv_color_hex(UI_COLOR_TEXT_DIM));
    lv_obj_align(wireless_screen.wait_subtitle, LV_ALIGN_CENTER, 0, WIRELESS_WAIT_SUBTITLE_Y);

    // -- connected / now-playing state --
    wireless_screen.tile_icon = lv_image_create(wireless_screen.screen);
    lv_image_set_src(wireless_screen.tile_icon, &icon_music_note);
    lv_obj_set_style_image_recolor(wireless_screen.tile_icon, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_image_recolor_opa(wireless_screen.tile_icon, LV_OPA_COVER, 0);
    lv_obj_align(wireless_screen.tile_icon, LV_ALIGN_CENTER, 0, WIRELESS_TILE_Y);
    wireless_screen.art = ui_now_playing_art_create(&wireless_screen.bg, wireless_screen.screen, WIRELESS_TILE_Y);

    wireless_screen.transport = flex_row_create(wireless_screen.screen, TRANSPORT_GAP, WIRELESS_TRANSPORT_Y);
    touch_pad(wireless_screen.transport, UI_TOUCH_EXT / 2);
    wireless_screen.prev_icon = ui_round_button_create(wireless_screen.transport, LV_SYMBOL_PREV,
                                                       UI_BUTTON_SIZE_SM, UI_FONT_SYMBOL, true);
    wireless_screen.play_icon = ui_round_button_create(wireless_screen.transport, LV_SYMBOL_PLAY,
                                                       UI_BUTTON_SIZE, UI_FONT_SYMBOL_LG, false);
    wireless_screen.next_icon = ui_round_button_create(wireless_screen.transport, LV_SYMBOL_NEXT,
                                                       UI_BUTTON_SIZE_SM, UI_FONT_SYMBOL, true);

    static const ui_transport_t buttons[] = {UI_TRANSPORT_PREV, UI_TRANSPORT_PLAY_PAUSE, UI_TRANSPORT_NEXT};
    lv_obj_t *labels[] = {wireless_screen.prev_icon, wireless_screen.play_icon, wireless_screen.next_icon};
    for (int i = 0; i < 3; i++) {
        lv_obj_t *btn = lv_obj_get_parent(labels[i]);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(btn, UI_TOUCH_EXT / 2);
        lv_obj_set_style_opa(btn, LV_OPA_COVER, 0);
        lv_obj_add_event_cb(btn, transport_click_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)buttons[i]);
    }

    wireless_screen.title_label = marquee_label_create(wireless_screen.screen, UI_FONT_TITLE, lv_color_hex(UI_COLOR_TEXT));
    lv_obj_align(wireless_screen.title_label, LV_ALIGN_CENTER, 0, WIRELESS_TITLE_Y);

    // .np-subtitle is 12.5px in the mock; LVGL's bundled Montserrat only ships
    // even sizes, so 12 is the closer of the two neighbors (vs. 14).
    wireless_screen.subtitle_label =
        marquee_label_create(wireless_screen.screen, UI_FONT_BODY, lv_color_hex(UI_COLOR_TEXT_DIM));
    lv_obj_align(wireless_screen.subtitle_label, LV_ALIGN_CENTER, 0, WIRELESS_SUBTITLE_Y);

    home_zone_create(wireless_screen.screen, swipe_home_cb);

    ui_track_info_t no_track = {0};
    wireless_state_t idle_state = {.volume = 0, .muted = 0, .connected = 0, .playing = 0, .source = SOURCE_AIRPLAY};
    ui_update_wireless(&idle_state, &no_track);
}

#ifdef AK_USB_AUDIO
static void usb_screen_build(void) {
    usb_screen.screen = screen_create();
    ui_now_playing_bg_create(&usb_screen.bg, usb_screen.screen, UI_BG_USB, UI_TINT_USB, NULL);
    usb_screen.ring = volume_ring_create(usb_screen.screen, lv_color_hex(UI_TINT_USB));

    lv_obj_t *row = status_pill_create(usb_screen.screen, NULL);
    chip_align_top(row);
    lv_obj_t *icon = lv_label_create(row);
    lv_label_set_text(icon, LV_SYMBOL_USB);
    lv_obj_set_style_text_font(icon, UI_FONT_SYMBOL_SM, 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(UI_TINT_USB), 0);
    lv_obj_t *label = status_label_create(row);
    lv_label_set_text(label, "USB Audio");

    // The art tile shows a generic speaker glyph, not the USB plug glyph --
    // that one's reserved for the status row above.
    lv_obj_t *tile_icon = lv_label_create(usb_screen.screen);
    lv_label_set_text(tile_icon, LV_SYMBOL_VOLUME_MAX);
    lv_obj_set_style_text_font(tile_icon, UI_FONT_SYMBOL_XL, 0);
    lv_obj_set_style_text_color(tile_icon, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_align(tile_icon, LV_ALIGN_CENTER, 0, USB_TILE_Y);

    lv_obj_t *transport = flex_row_create(usb_screen.screen, TRANSPORT_GAP, USB_TRANSPORT_Y);
    touch_pad(transport, UI_TOUCH_EXT / 2);
    usb_screen.prev_icon = ui_round_button_create(transport, LV_SYMBOL_PREV, UI_BUTTON_SIZE_SM,
                                                  UI_FONT_SYMBOL, true);
    usb_screen.play_icon = ui_round_button_create(transport, LV_SYMBOL_PLAY, UI_BUTTON_SIZE,
                                                  UI_FONT_SYMBOL_LG, false);
    usb_screen.next_icon = ui_round_button_create(transport, LV_SYMBOL_NEXT, UI_BUTTON_SIZE_SM,
                                                  UI_FONT_SYMBOL, true);

    // USB has no metadata channel, so the title slot just names the output.
    lv_obj_t *title = lv_label_create(usb_screen.screen);
    lv_label_set_text(title, "USB Audio");
    lv_obj_set_style_text_font(title, UI_FONT_TITLE, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, USB_TITLE_Y);

    home_zone_create(usb_screen.screen, swipe_home_cb);

    ui_update_usb(0, false, false);
}
#endif

#define SWITCH_LIST_Y      4
#define SWITCH_ITEM_W      220
#define SWITCH_ITEM_GAP    UI_SPACE_M

static const struct {
    const char *label;
    const lv_image_dsc_t *icon;
    uint32_t tint;
} k_sources[] = {
    [UI_SOURCE_AIRPLAY] = {"AirPlay", &icon_airplay, UI_TINT_AIRPLAY},
    [UI_SOURCE_BLUETOOTH] = {"Bluetooth", &icon_bluetooth, UI_TINT_BLUETOOTH},
};

static void switch_screen_refresh(void) {
    for (int i = 0; i < 2; i++) {
        bool active = i == (int)s_current_source;
        lv_obj_set_style_bg_opa(switch_screen.items[i], active ? UI_OPA_SELECTED : UI_OPA_GLASS, 0);
        lv_obj_set_style_border_color(switch_screen.items[i], lv_color_hex(k_sources[i].tint), 0);
        lv_obj_set_style_border_opa(switch_screen.items[i], active ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(switch_screen.items[i], lv_color_hex(active ? UI_COLOR_TEXT : UI_COLOR_TEXT_DIM), 0);
    }
}

static void open_switch_cb(lv_event_t *e) {
    (void)e;
    switch_screen_refresh();
    show_screen(switch_screen.screen);
}

static void switch_back_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_GESTURE && lv_indev_get_gesture_dir(lv_indev_active()) != LV_DIR_TOP) {
        return;
    }
    show_screen(wireless_screen.screen);
}

static void source_pick_cb(lv_event_t *e) {
    ui_source_t source = (ui_source_t)(uintptr_t)lv_event_get_user_data(e);
    if (source != s_current_source) {
        s_current_source = source;
        if (s_source_handler) {
            s_source_handler(source);
        }
    }
    s_homed = false;
    s_pinned = true;
    show_screen(wireless_screen.screen);
}

// Its own page rather than an inline toggle (mock-react's SourceSwitchScreen).
// USB is not offered: it isn't implemented yet.
static void switch_screen_build(void) {
    switch_screen.screen = screen_create();

    lv_obj_t *title = lv_label_create(switch_screen.screen);
    lv_label_set_text(title, "SWITCH SOURCE");
    lv_obj_set_style_text_font(title, UI_FONT_OVERLINE, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(UI_COLOR_TEXT_DIM), 0);
    lv_obj_set_style_text_letter_space(title, UI_OVERLINE_SPACING, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, UI_TITLE_TOP_Y);

    lv_obj_t *list = lv_obj_create(switch_screen.screen);
    lv_obj_remove_style_all(list);
    lv_obj_remove_flag(list, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(list, SWITCH_ITEM_W, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, SWITCH_ITEM_GAP, 0);
    lv_obj_align(list, LV_ALIGN_CENTER, 0, SWITCH_LIST_Y);

    for (int i = 0; i < 2; i++) {
        lv_obj_t *item = lv_obj_create(list);
        lv_obj_remove_style_all(item);
        lv_obj_remove_flag(item, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(item, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(item, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(item, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(item, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(item, UI_SPACE_M, 0);
        lv_obj_set_style_pad_hor(item, UI_SPACE_XL, 0);
        lv_obj_set_style_pad_ver(item, UI_SPACE_L, 0);
        lv_obj_set_style_radius(item, UI_RADIUS_CARD, 0);
        lv_obj_set_style_border_width(item, 1, 0);
        lv_obj_set_style_bg_color(item, lv_color_white(), 0);
        lv_obj_add_event_cb(item, source_pick_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);

        icon_create(item, k_sources[i].icon, 20, lv_color_hex(k_sources[i].tint));
        lv_obj_t *label = lv_label_create(item);
        lv_label_set_text(label, k_sources[i].label);
        lv_obj_set_style_text_font(label, UI_FONT_TITLE, 0);
        switch_screen.items[i] = item;
    }
    switch_screen_refresh();

    home_zone_create(switch_screen.screen, switch_back_cb);
}

static void (*s_alert_handler)(bool ringing);
static bool s_ringing;

void ui_set_alert_handler(void (*handler)(bool ringing)) {
    s_alert_handler = handler;
}

// Timer and tomato can ring at once; the handler hears only the edges of
// "anything ringing".
static void tool_alert_cb(lv_obj_t *screen, bool ringing) {
    if (ringing) {
        ui_wake();
        launcher_hide();
        show_screen(screen);
    }
    bool any = ui_tools_alerting();
    if (any != s_ringing) {
        s_ringing = any;
        if (s_alert_handler) {
            s_alert_handler(any);
        }
    }
}

// Paging away is acknowledgement enough; a countdown left ringing off-screen
// would also keep surface_source() from showing a new stream.
static void tool_unloaded_cb(lv_event_t *e) {
    ui_tools_dismiss(lv_event_get_current_target(e));
}

static void apps_build(void) {
    for (int i = 0; i < UI_APP_COUNT; i++) {
        s_apps[i] = screen_create();
    }
    ui_tools_build(s_apps, tool_alert_cb);

    for (int i = 0; i < UI_APP_COUNT; i++) {
        home_zone_create(s_apps[i], swipe_home_cb);
        lv_obj_add_event_cb(s_apps[i], tool_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
    }
}

// Whatever of the safe area the icon and padding leave.
#define AUDIO_CHIP_TEXT_W (UI_CHIP_MAX_W - 2 * UI_CHIP_PAD_X - STATUS_ICON_SIZE - UI_SPACE_XS)

static lv_obj_t *audio_screen(void) {
    if (s_audio.wireless_playing) {
        return wireless_screen.screen;
    }
#ifdef AK_USB_AUDIO
    if (s_audio.usb_playing) {
        return usb_screen.screen;
    }
#endif
    return s_audio.wireless_connected ? wireless_screen.screen : NULL;
}

// Only where the user went from the audio, not on the source screens
// themselves or the setup QR.
static bool audio_chip_wanted(void) {
    lv_obj_t *active = lv_screen_active();
    return audio_chip.target && !launcher_visible() && ui_tools_app_of(active) >= 0;
}

// A content-sized label wraps rather than dots, so the label is one line
// tall and its width is set per text: the text's own, up to a cap.
static void audio_chip_set_text(const char *text) {
    if (strcmp(lv_label_get_text(audio_chip.label), text) == 0) {
        return;
    }
    lv_point_t size;
    lv_text_get_size(&size, text, lv_obj_get_style_text_font(audio_chip.label, 0), 0, 0, LV_COORD_MAX,
                     LV_TEXT_FLAG_NONE);
    lv_obj_set_width(audio_chip.label, LV_MIN(size.x, AUDIO_CHIP_TEXT_W));
    lv_label_set_text(audio_chip.label, text);
}

// Not playing, the row is a plain line like the weather's: a glyph and its
// text, centered, with no pill, tile or button. Playing, it's the pill with
// art and play/pause. Applied only on a change:
// the row refreshes on every state push.
static void np_card_set_quiet(bool quiet) {
    if (idle_screen.np.quiet == quiet) {
        return;
    }
    idle_screen.np.quiet = quiet;
    lv_obj_set_style_bg_opa(idle_screen.np.card, quiet ? LV_OPA_TRANSP : UI_OPA_GLASS, 0);
    lv_obj_set_flex_align(idle_screen.np.card, quiet ? LV_FLEX_ALIGN_CENTER : LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(idle_screen.np.thumb, quiet ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
    lv_obj_set_width(idle_screen.np.thumb, quiet ? NP_IDLE_GLYPH_W : NP_THUMB);
    lv_obj_set_flex_grow(idle_screen.np.text, quiet ? 0 : 1);
    lv_obj_set_width(idle_screen.np.text, quiet ? LV_SIZE_CONTENT : 0);
    lv_obj_set_width(idle_screen.np.title, quiet ? LV_SIZE_CONTENT : lv_pct(100));
    lv_obj_set_width(idle_screen.np.subtitle, quiet ? LV_SIZE_CONTENT : lv_pct(100));
    lv_obj_set_style_text_opa(idle_screen.np.title, quiet ? UI_OPA_TEXT_MUTED : LV_OPA_COVER, 0);
    ui_set_visible(idle_screen.np.subtitle, !quiet);
    ui_set_visible(lv_obj_get_parent(idle_screen.np.play), !quiet);
}

// With nothing connected the row still says what the device is for.
static void np_card_idle(void) {
    np_card_set_quiet(true);
    ui_set_visible(idle_screen.np.art, false);
    ui_set_visible(idle_screen.np.icon, false);
    ui_set_visible(idle_screen.np.idle_icon, true);
    ui_label_set_text(idle_screen.np.title, "Ready to play");
}

// The source's icon, white on the pill's tinted tile while playing, in the
// source's tint on its own when connected but idle.
static void np_card_source_icon(lv_color_t tint, bool on_tile) {
    const lv_image_dsc_t *icon = s_audio.wireless_airplay ? &icon_airplay : &icon_bluetooth;
    if (lv_image_get_src(idle_screen.np.icon) != icon) {
        lv_image_set_src(idle_screen.np.icon, icon);
    }
    lv_color_t color = on_tile ? lv_color_white() : tint;
    if (!lv_color_eq(color, idle_screen.np.icon_color)) {
        idle_screen.np.icon_color = color;
        lv_obj_set_style_image_recolor(idle_screen.np.icon, color, 0);
    }
}

static void np_card_refresh(lv_obj_t *target) {
    if (!target) {
        np_card_idle();
        return;
    }
#ifdef AK_USB_AUDIO
    bool usb = target == usb_screen.screen;
    bool playing = usb ? s_audio.usb_playing : s_audio.wireless_playing;
#else
    bool usb = false;
    bool playing = s_audio.wireless_playing;
#endif
    lv_color_t tint = lv_color_hex(usb ? UI_TINT_USB : s_audio.wireless_airplay ? UI_TINT_AIRPLAY : UI_TINT_BLUETOOTH);
    if (!lv_color_eq(tint, idle_screen.np.tint)) {
        idle_screen.np.tint = tint;
        lv_obj_set_style_bg_color(idle_screen.np.thumb, tint, 0);
    }
    ui_set_visible(idle_screen.np.idle_icon, false);

    // Connected but not playing: which device, quietly.
    if (!playing) {
        np_card_set_quiet(true);
        np_card_source_icon(tint, false);
        ui_set_visible(idle_screen.np.art, false);
        ui_set_visible(idle_screen.np.icon, !usb);
        const char *name = s_audio.wireless_peer[0] ? s_audio.wireless_peer
                           : s_audio.wireless_airplay ? "AirPlay"
                                                      : "Bluetooth";
        ui_label_set_text(idle_screen.np.title, usb ? "USB Audio" : name);
        return;
    }

    np_card_set_quiet(false);
    np_card_source_icon(tint, true);

    // Shares the now-playing screen's copy of the art.
    uint32_t art_version = usb ? 0 : wireless_screen.art_version;
    if (art_version != idle_screen.np.art_version) {
        idle_screen.np.art_version = art_version;
        if (art_version) {
            lv_image_set_src(idle_screen.np.art, &wireless_screen.art_dsc);
        }
    }
    ui_set_visible(idle_screen.np.art, art_version != 0);
    ui_set_visible(idle_screen.np.icon, !usb && art_version == 0);

    const char *subtitle = usb ? "" : s_audio.wireless_subtitle;
    ui_label_set_text(idle_screen.np.title, usb ? "USB Audio" : s_audio.wireless_text);
    ui_label_set_text(idle_screen.np.subtitle, subtitle[0] ? subtitle : "Playing");
    ui_label_set_text(idle_screen.np.play, LV_SYMBOL_PAUSE);
}

// Runs on every state push; each setter below skips no-op changes.
static void audio_chip_refresh(void) {
    if (!audio_chip.chip) {
        return;
    }
    lv_obj_t *target = audio_screen();
    audio_chip.target = target;
    ui_set_visible(audio_chip.chip, audio_chip_wanted());
    np_card_refresh(target);
    if (!target) {
        return;
    }

#ifdef AK_USB_AUDIO
    bool usb = target == usb_screen.screen;
    ui_set_visible(audio_chip.usb_icon, usb);
#else
    bool usb = false;
#endif
    ui_set_visible(audio_chip.icon, !usb);
    if (!usb) {
        const lv_image_dsc_t *src = s_audio.wireless_airplay ? &icon_airplay : &icon_bluetooth;
        if (lv_image_get_src(audio_chip.icon) != src) {
            lv_image_set_src(audio_chip.icon, src);
            lv_obj_set_style_image_recolor(audio_chip.icon, lv_color_hex(s_audio.wireless_airplay ? UI_TINT_AIRPLAY : UI_TINT_BLUETOOTH),
                                           0);
        }
    }
    audio_chip_set_text(usb ? "USB Audio" : s_audio.wireless_text);

    // Paused but still connected reads dimmer than playing.
#ifdef AK_USB_AUDIO
    bool dim = usb ? !s_audio.usb_playing : !s_audio.wireless_playing;
#else
    bool dim = !s_audio.wireless_playing;
#endif
    if (dim != audio_chip.dim) {
        audio_chip.dim = dim;
        lv_obj_set_style_text_opa(audio_chip.label, dim ? UI_OPA_TEXT_MUTED : LV_OPA_COVER, 0);
    }
}

static void audio_chip_cb(lv_event_t *e) {
    (void)e;
    open_audio();
}

static void audio_chip_build(void) {
    lv_obj_t *chip = status_pill_create(lv_layer_top(), audio_chip_cb);
    chip_align_top(chip);
    lv_obj_add_flag(chip, LV_OBJ_FLAG_HIDDEN);
    audio_chip.chip = chip;

    audio_chip.icon = icon_create(chip, &icon_airplay, STATUS_ICON_SIZE, lv_color_hex(UI_TINT_AIRPLAY));
#ifdef AK_USB_AUDIO
    audio_chip.usb_icon = lv_label_create(chip);
    lv_label_set_text(audio_chip.usb_icon, LV_SYMBOL_USB);
    lv_obj_set_style_text_font(audio_chip.usb_icon, UI_FONT_SYMBOL_SM, 0);
    lv_obj_set_style_text_color(audio_chip.usb_icon, lv_color_hex(UI_TINT_USB), 0);
#endif

    audio_chip.label = status_label_create(chip);
    lv_obj_set_style_text_color(audio_chip.label, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_label_set_long_mode(audio_chip.label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(audio_chip.label, lv_font_get_line_height(UI_FONT_BODY));
}

static bool launcher_audio_status(char *buf, size_t len) {
    lv_obj_t *target = audio_screen();
    if (!target) {
        return false;
    }
#ifdef AK_USB_AUDIO
    if (target == usb_screen.screen) {
        snprintf(buf, len, "USB Audio");
        return true;
    }
#endif
    snprintf(buf, len, "%s", s_audio.wireless_text);
    return true;
}

static void launcher_visibility_cb(void) {
    audio_chip_refresh();
}

#define NAV_TICK_MS     250
#define APP_IDLE_HOME_MS 30000

// Display sleep: with no input for the user's timeout, an opaque catcher on
// the system layer blacks out everything (which is all the emulator shows)
// and the display handler turns the backlight off. The input that wakes it
// is swallowed, so a tap or knob turn only lights the screen.
static struct {
    lv_obj_t *catcher;
    bool asleep;
    void (*handler)(bool on);
} s_sleep;

void ui_set_display_handler(void (*handler)(bool on)) {
    s_sleep.handler = handler;
}

bool ui_wake(void) {
    lv_display_trigger_activity(NULL);
    if (!s_sleep.asleep) {
        return false;
    }
    s_sleep.asleep = false;
    ui_set_visible(s_sleep.catcher, false);
    if (s_sleep.handler) {
        s_sleep.handler(true);
    }
    return true;
}

static void display_sleep(void) {
    s_sleep.asleep = true;
    ui_set_visible(s_sleep.catcher, true);
    if (s_sleep.handler) {
        s_sleep.handler(false);
    }
}

// Hiding the catcher mid-press would hand the press to whatever is under
// it, so the touch is ignored until the finger lifts.
static void sleep_catcher_cb(lv_event_t *e) {
    (void)e;
    lv_indev_wait_release(lv_indev_active());
    ui_wake();
}

static void sleep_build(void) {
    s_sleep.catcher = lv_obj_create(lv_layer_sys());
    lv_obj_remove_style_all(s_sleep.catcher);
    lv_obj_set_size(s_sleep.catcher, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_sleep.catcher, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_sleep.catcher, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_sleep.catcher, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_sleep.catcher, sleep_catcher_cb, LV_EVENT_PRESSED, NULL);
}

// Something that must be seen (a ringing countdown, the pairing or update
// prompt) keeps the display on.
static void sleep_tick(void) {
    uint32_t sleep_s = ui_settings_current()->sleep_s;
    if (s_sleep.asleep || !sleep_s || ui_tools_alerting() || ui_pairing_visible() ||
        ui_firmware_visible()) {
        return;
    }
    if (lv_display_get_inactive_time(NULL) >= sleep_s * 1000) {
        display_sleep();
    }
}

static void nav_tick_cb(lv_timer_t *t) {
    (void)t;
    sleep_tick();
    lv_obj_t *active = lv_screen_active();
    if (active == idle_screen.screen) {
        idle_screen_refresh();
        return;
    }
    // A page left alone goes home, unless it's showing progress the user
    // may be watching; the clock's chips lead back to it either way.
    int app = ui_tools_app_of(active);
    char buf[16];
    if (app >= 0 && !launcher_visible() && !ui_tools_status((ui_app_t)app, buf, sizeof(buf)) &&
        lv_display_get_inactive_time(NULL) >= APP_IDLE_HOME_MS) {
        go_home();
    }
}

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
    return ui_tools_knob_rotate(lv_screen_active(), steps);
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
        go_home();
    } else {
        open_launcher();
    }
}

void ui_init(void) {
    // The clock's chips take each app's icon and tint, so the apps go first.
    apps_build();
    idle_screen_build();
    s_home = idle_screen.screen;
    wireless_screen_build();
#ifdef AK_USB_AUDIO
    usb_screen_build();
#endif
    switch_screen_build();
    audio_chip_build();
    launcher_build(&(launcher_handlers_t){.open = launcher_open,
                                          .dismissed = go_home,
                                          .visibility_changed = launcher_visibility_cb,
                                          .audio_status = launcher_audio_status});
    sleep_build();
    lv_timer_create(nav_tick_cb, NAV_TICK_MS, NULL);

    lv_screen_load(s_home);
    // The clock's audio row starts idle, before any state push.
    audio_chip_refresh();
    idle_screen_refresh();
}

static void update_artwork(const ui_track_info_t *track) {
    const uint16_t *art = track ? track->art : NULL;
    uint32_t version = art ? track->art_version : 0;
    if (version == wireless_screen.art_version) {
        return;
    }
    wireless_screen.art_version = version;
    if (!art) {
        return;
    }
    lv_image_dsc_t *dsc = &wireless_screen.art_dsc;
    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565;
    dsc->header.w = UI_ART_SIZE;
    dsc->header.h = UI_ART_SIZE;
    dsc->header.stride = UI_ART_SIZE * 2;
    dsc->data_size = UI_ART_SIZE * UI_ART_SIZE * 2;
    dsc->data = (const uint8_t *)art;
    lv_image_set_src(wireless_screen.art, dsc);
}

static void format_subtitle(const ui_track_info_t *track, char *out, size_t len) {
    out[0] = '\0';
    if (!track || !track->title[0]) {
        return;
    }
    if (track->artist[0] && track->album[0]) {
        snprintf(out, len, "%s \xE2\x80\x94 %s", track->artist, track->album);
    } else if (track->artist[0]) {
        snprintf(out, len, "%s", track->artist);
    } else if (track->album[0]) {
        snprintf(out, len, "%s", track->album);
    }
}

void ui_update_wireless(const wireless_state_t *state, const ui_track_info_t *track) {
    static surface_state_t surface;
    bool airplay = state->source == SOURCE_AIRPLAY;
    lv_color_t tint = lv_color_hex(airplay ? UI_TINT_AIRPLAY : UI_TINT_BLUETOOTH);
    s_current_source = airplay ? UI_SOURCE_AIRPLAY : UI_SOURCE_BLUETOOTH;

    ring_set_volume(wireless_screen.ring, state->volume);
    lv_image_set_src(wireless_screen.source_icon, airplay ? &icon_airplay : &icon_bluetooth);
    lv_obj_set_style_image_recolor(wireless_screen.source_icon, tint, 0);
    ui_label_set_text(wireless_screen.source_label, airplay ? "AirPlay" : "Bluetooth");

    // Unconnected, there's no tile/track to show yet, nor a volume.
    bool waiting = !state->connected;

    // Eased only where it shows; elsewhere the next look finds it done.
    uint32_t art_tint = track && track->art && ui_settings_current()->art_colors ? track->art_tint : 0;
    ui_now_playing_bg_set_tint(&wireless_screen.bg, art_tint ? art_tint : airplay ? UI_TINT_AIRPLAY : UI_TINT_BLUETOOTH,
                               !waiting && lv_screen_active() == wireless_screen.screen);
    wireless_screen.bluetooth = !airplay;
    if (airplay) {
        lv_obj_remove_flag(wireless_screen.wait_chip, LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_obj_add_flag(wireless_screen.wait_chip, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_t *status_row = lv_obj_get_parent(wireless_screen.source_icon);
    // Connected, it's a chip like any other; waiting, it heads the centered
    // block below it.
    if (waiting) {
        lv_obj_align(status_row, LV_ALIGN_CENTER, 0, WIRELESS_STATUS_ROW_Y_WAITING);
    } else {
        chip_align_top(status_row);
    }

    ui_set_visible(wireless_screen.ring, !waiting);
    ui_set_visible(wireless_screen.bg.img, !waiting);
    ui_set_visible(wireless_screen.wait_chip, waiting);
    ui_set_visible(wireless_screen.wait_title, waiting);
    ui_set_visible(wireless_screen.wait_subtitle, waiting);
    ui_set_visible(wireless_screen.transport, !waiting);
    ui_set_visible(wireless_screen.title_label, !waiting);
    ui_set_visible(wireless_screen.subtitle_label, !waiting);

    char subtitle[128];
    format_subtitle(track, subtitle, sizeof(subtitle));
    bool has_track = track && track->title[0];
    const char *source_name = airplay ? "AirPlay" : "Bluetooth";
    const char *peer = track ? track->peer : "";
    // With no track, what's connected is the next best title.
    const char *idle_title = peer[0] ? peer : source_name;

    if (waiting) {
        lv_image_set_src(wireless_screen.wait_icon, airplay ? &icon_airplay : &icon_bluetooth);
        ui_label_set_text(wireless_screen.wait_title, airplay ? "Waiting for AirPlay" : "Waiting for Bluetooth");
        ui_label_set_text(wireless_screen.wait_subtitle,
                           airplay ? "Select this speaker from Control Center" : "Tap the icon to pair a new phone");
    } else {
        ui_label_set_text(wireless_screen.play_icon,
                           state->muted ? LV_SYMBOL_MUTE : (state->playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY));
        lv_obj_set_style_text_color(wireless_screen.play_icon, lv_color_hex(state->muted ? UI_COLOR_DANGER : UI_COLOR_TEXT), 0);
        ui_label_set_text(wireless_screen.title_label, has_track ? track->title : idle_title);
        ui_label_set_text(wireless_screen.subtitle_label, subtitle);
        update_artwork(track);
    }
    ui_set_visible(wireless_screen.tile_icon, !waiting && !wireless_screen.art_version);
    ui_set_visible(wireless_screen.art, !waiting && wireless_screen.art_version);

    s_audio.wireless_connected = state->connected;
    s_audio.wireless_playing = state->playing;
    s_audio.wireless_airplay = airplay;
    snprintf(s_audio.wireless_peer, sizeof(s_audio.wireless_peer), "%s", peer);
    snprintf(s_audio.wireless_text, sizeof(s_audio.wireless_text), "%s", has_track ? track->title : idle_title);
    snprintf(s_audio.wireless_subtitle, sizeof(s_audio.wireless_subtitle), "%s",
             !has_track ? "" : track->artist[0] ? track->artist : track->album);
    surface_source(wireless_screen.screen, state->playing, state->connected, &surface);
    audio_chip_refresh();
}

#ifdef AK_USB_AUDIO
void ui_update_usb(uint8_t volume, bool muted, bool playing) {
    static surface_state_t surface;
    ring_set_volume(usb_screen.ring, volume);

    ui_label_set_text(usb_screen.play_icon, muted ? LV_SYMBOL_MUTE : (playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY));
    lv_obj_set_style_text_color(usb_screen.play_icon, lv_color_hex(muted ? UI_COLOR_DANGER : UI_COLOR_TEXT), 0);

    s_audio.usb_playing = playing;
    surface_source(usb_screen.screen, playing, false, &surface);
    audio_chip_refresh();
}
#endif

void ui_update_clock(bool synced, const struct tm *local_time) {
    if (!synced || !local_time) {
        ui_label_set_text(idle_screen.time_label, "--:--");
        ui_set_visible(idle_screen.ampm_label, false);
        ui_label_set_text(idle_screen.date_label, "Waiting for time sync");
        return;
    }

    bool h24 = ui_settings_current()->clock_24h;
    int hour = h24 ? local_time->tm_hour : (local_time->tm_hour % 12 ? local_time->tm_hour % 12 : 12);
    char buf[32];
    snprintf(buf, sizeof(buf), h24 ? "%02d:%02d" : "%d:%02d", hour, local_time->tm_min);
    ui_label_set_text(idle_screen.time_label, buf);
    // The clock font has digits and ':' only, so AM/PM is its own label.
    ui_set_visible(idle_screen.ampm_label, !h24);
    if (!h24) {
        ui_label_set_text(idle_screen.ampm_label, local_time->tm_hour < 12 ? "AM" : "PM");
        ampm_place();
    }

    // "Sun 4 Oct" like the mock; %d would zero-pad and %-d isn't portable.
    char weekday[8], month[8];
    strftime(weekday, sizeof(weekday), "%a", local_time);
    strftime(month, sizeof(month), "%b", local_time);
    snprintf(buf, sizeof(buf), "%s %d %s", weekday, local_time->tm_mday, month);
    ui_label_set_text(idle_screen.date_label, buf);
}

// Layout for the round panel: title at the top, QR in the middle, status and
// manual-join details below, all kept inside the circle.
#define SETUP_TITLE_Y   (-132)
#define SETUP_QR_SIZE   168
#define SETUP_QR_Y      (-12)
#define SETUP_STATUS_Y  100
#define SETUP_NETWORK_Y 128

static lv_obj_t *setup_label_create(lv_obj_t *parent, const lv_font_t *font, lv_opa_t opa, int32_t y) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_text_opa(label, opa, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(label, UI_TEXT_W);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, y);
    return label;
}

static void wifi_setup_screen_build(void) {
    wifi_setup_screen.screen = screen_create();

    lv_obj_t *title = setup_label_create(wifi_setup_screen.screen, UI_FONT_TITLE, LV_OPA_COVER, SETUP_TITLE_Y);
    lv_label_set_text(title, "Set up WiFi");

    wifi_setup_screen.qr = lv_qrcode_create(wifi_setup_screen.screen);
    lv_qrcode_set_size(wifi_setup_screen.qr, SETUP_QR_SIZE);
    lv_qrcode_set_dark_color(wifi_setup_screen.qr, lv_color_black());
    lv_qrcode_set_light_color(wifi_setup_screen.qr, lv_color_white());
    // Phone cameras need a light margin around the code on a dark screen.
    lv_qrcode_set_quiet_zone(wifi_setup_screen.qr, true);
    lv_obj_align(wifi_setup_screen.qr, LV_ALIGN_CENTER, 0, SETUP_QR_Y);

    wifi_setup_screen.status_label =
        setup_label_create(wifi_setup_screen.screen, UI_FONT_BODY, LV_OPA_COVER, SETUP_STATUS_Y);
    wifi_setup_screen.network_label =
        setup_label_create(wifi_setup_screen.screen, UI_FONT_CAPTION, UI_OPA_TEXT_MUTED, SETUP_NETWORK_Y);
}

void ui_show_wifi_setup(const char *qr_payload, const char *ssid, const char *password, const char *status) {
    if (!wifi_setup_screen.screen) {
        wifi_setup_screen_build();
    }

    // Regenerating the QR is the expensive part; only redo it on change.
    if (strcmp(wifi_setup_screen.qr_payload, qr_payload) != 0) {
        snprintf(wifi_setup_screen.qr_payload, sizeof(wifi_setup_screen.qr_payload), "%s", qr_payload);
        lv_qrcode_update(wifi_setup_screen.qr, qr_payload, strlen(qr_payload));
        lv_label_set_text_fmt(wifi_setup_screen.network_label, "%s  /  %s", ssid, password);
    }
    if (strcmp(lv_label_get_text(wifi_setup_screen.status_label), status) != 0) {
        lv_label_set_text(wifi_setup_screen.status_label, status);
    }

    if (s_home != wifi_setup_screen.screen) {
        bool was_home = lv_screen_active() == s_home;
        s_home = wifi_setup_screen.screen;
        if (was_home) {
            show_screen(s_home);
        }
    }
}
