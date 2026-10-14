#include "ui.h"
#include "lvgl.h"
#include "fonts.h"
#include "icons.h"
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
#define RING_SIZE     344

// Y offsets (from screen center) extracted directly from the mock-react
// reference's rendered layout (localhost:5173, getBoundingClientRect() on
// its 360x360 .screen) rather than eyeballed. The mock centers its whole
// content block per screen (flex column, justify-content:center), so these
// differ by state/screen depending on how tall that block is -- there's no
// LVGL flex equivalent worth building for three fixed cases, so each is
// just pinned to its measured value.
#define WIRELESS_STATUS_ROW_Y_CONNECTED (-133)
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
#define USB_STATUS_ROW_Y (-126)
#define USB_TILE_Y        (-33)
#define USB_TRANSPORT_Y   71
#define USB_TITLE_Y        118
#endif

#define TRANSPORT_PRIMARY_SIZE   48
#define TRANSPORT_SECONDARY_SIZE 40
#define TRANSPORT_GAP            14

#define STATUS_ICON_SIZE   14
#define STATUS_PILL_PAD_X  12
#define STATUS_PILL_PAD_Y  5

#define RING_VISIBLE_MS 1400
#define RING_FADE_MS    300

#define HOME_ZONE_W 140
#define HOME_ZONE_H 46

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *backdrop;
    lv_obj_t *ring;
    lv_obj_t *source_icon;
    lv_obj_t *source_label;

    // Shown when state->connected is false -- neither source has a local
    // "pairing" step on this end (AirPlay only appears once picked from the
    // phone's Control Center; Bluetooth just advertises until a phone
    // connects), so there's nothing to show a tile/track for yet.
    lv_obj_t *wait_chip;
    lv_obj_t *wait_icon;
    lv_obj_t *wait_title;
    lv_obj_t *wait_subtitle;

    // Shown once connected.
    // The tile is never shown itself: it is snapshotted into tile_img when
    // its tint or art changes (see tile_bake()).
    lv_obj_t *tile;
    lv_obj_t *tile_icon;
    lv_obj_t *art;
    lv_image_dsc_t art_dsc;
    uint32_t art_version;
    lv_obj_t *tile_img;
    lv_draw_buf_t *tile_baked;
    uint32_t baked_art_version;
    lv_color_t baked_tint;
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
    lv_obj_t *ring;
    lv_obj_t *tile;
    lv_obj_t *backdrop;
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
    lv_obj_t *date_label;
    lv_obj_t *weather;
    lv_obj_t *weather_icon;
    lv_obj_t *weather_temp;
    // One per app, shown while that app has something in progress.
    lv_obj_t *chips[UI_APP_COUNT];
    lv_obj_t *chip_labels[UI_APP_COUNT];
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

static void open_launcher(void) {
    int app = ui_tools_app_of(lv_screen_active());
    launcher_show(app >= 0 ? (ui_app_t)app : s_last_app);
}

static void switch_back_cb(lv_event_t *e);
static void open_switch_cb(lv_event_t *e);

// Shared by every source: surfaces `screen` while `playing`, falls back to
// home when it stops, and respects an in-progress swipe home. A fresh
// not-playing -> playing edge always un-homes, so a new stream is never
// hidden behind the clock.
static void surface_source(lv_obj_t *screen, bool playing, bool *was_playing) {
    if (playing && !*was_playing) {
        s_homed = false;
        s_pinned = false;
    }
    *was_playing = playing;

    if (playing) {
        // The switch screen is user-opened and a ringing timer must be seen;
        // state pushes must not yank either away.
        if (!s_homed && lv_screen_active() != switch_screen.screen && !ui_tools_alerting()) {
            show_screen(screen);
        }
    } else if (lv_screen_active() == screen && !s_pinned) {
        show_screen(s_home);
    }
}

static lv_obj_t *screen_create(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0b0b10), 0);
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
    lv_obj_set_style_radius(pill, 2, 0);
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

// mock-react's .status-row.tappable: translucent pill, the only hint that
// it's the source indicator.
static lv_obj_t *status_pill_create(lv_obj_t *parent, int32_t y) {
    lv_obj_t *row = flex_row_create(parent, 6, y);
    lv_obj_set_style_pad_hor(row, STATUS_PILL_PAD_X, 0);
    lv_obj_set_style_pad_ver(row, STATUS_PILL_PAD_Y, 0);
    lv_obj_set_style_radius(row, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(row, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(row, 20, 0);
    return row;
}

static lv_obj_t *status_label_create(lv_obj_t *row) {
    lv_obj_t *label = lv_label_create(row);
    lv_obj_set_style_text_color(label, lv_color_hex(0x9a9aa4), 0);
    lv_obj_set_style_text_font(label, &inter_med_13, 0);
    return label;
}

static lv_obj_t *marquee_label_create(lv_obj_t *parent, const lv_font_t *font, lv_color_t color) {
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_set_width(label, 240);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    return label;
}

// Ambient art placeholder: a flat tinted rounded tile, since no album-art
// transfer exists over the inter-MCU link yet. Caller centers its own icon
// inside (an image for a real asset, or a label for an LV_SYMBOL glyph).
static lv_obj_t *art_tile_create(lv_obj_t *parent, lv_color_t tint, int32_t y) {
    lv_obj_t *tile = lv_obj_create(parent);
    lv_obj_set_size(tile, ART_TILE_SIZE, ART_TILE_SIZE);
    lv_obj_set_style_radius(tile, 28, 0);
    lv_obj_set_style_border_width(tile, 0, 0);
    lv_obj_set_style_bg_color(tile, tint, 0);
    lv_obj_set_style_bg_grad_color(tile, lv_color_hex(0x0b0b10), 0);
    lv_obj_set_style_bg_grad_dir(tile, LV_GRAD_DIR_VER, 0);
    lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(tile, LV_ALIGN_CENTER, 0, y);
    return tile;
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
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x1c1c24), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    if (dim) {
        lv_obj_set_style_opa(btn, LV_OPA_40, 0);
    }

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, symbol);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
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
    lv_obj_set_size(arc, RING_SIZE, RING_SIZE);
    lv_arc_set_rotation(arc, 135);
    lv_arc_set_bg_angles(arc, 0, 270);
    lv_arc_set_range(arc, 0, 100);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_width(arc, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(arc, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(0x1c1c24), LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, tint, LV_PART_INDICATOR);
    lv_obj_set_style_opa(arc, LV_OPA_TRANSP, 0);
    lv_obj_center(arc);
    return arc;
}

// Stand-in for mock-react's blurred, darkened album-art wash (.art-bg): LVGL
// has no blur, so a flat tint at low opacity gives the screen the same
// source-coloured cast instead of a flat black.
static lv_obj_t *backdrop_create(lv_obj_t *parent) {
    lv_obj_t *bd = lv_obj_create(parent);
    lv_obj_remove_style_all(bd);
    lv_obj_remove_flag(bd, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(bd, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(bd, LV_OPA_10, 0);
    return bd;
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
    lv_obj_set_width(label, 240);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_opa(label, opa, 0);
    return label;
}

#define CLOCK_CHIPS_Y   (-34)
#define CLOCK_CHIP_GAP  6

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
    lv_obj_t *pill = flex_row_create(parent, 4, 0);
    lv_obj_set_style_pad_hor(pill, 10, 0);
    lv_obj_set_style_pad_ver(pill, 3, 0);
    lv_obj_set_style_radius(pill, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(pill, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(pill, 20, 0);
    lv_obj_add_flag(pill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(pill, 6);
    lv_obj_add_event_cb(pill, cb, LV_EVENT_CLICKED, user_data);

    lv_obj_t *glyph = lv_label_create(pill);
    lv_label_set_text(glyph, icon);
    lv_obj_set_style_text_font(glyph, &icons_20, 0);
    lv_obj_set_style_text_color(glyph, tint, 0);

    lv_obj_t *label = lv_label_create(pill);
    lv_obj_set_style_text_font(label, &inter_semi_13, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    return label;
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
    lv_obj_set_style_pad_row(col, 5, 0);
    lv_obj_center(col);

    idle_screen.date_label = glance_label_create(col, &inter_semi_13, LV_OPA_60);
    idle_screen.time_label = glance_label_create(col, &inter_bold_52, LV_OPA_COVER);

    idle_screen.weather_temp = clock_pill_create(col, ICON_CLOUD, lv_color_hex(0xffc35a), clock_weather_cb, NULL);
    idle_screen.weather = lv_obj_get_parent(idle_screen.weather_temp);
    idle_screen.weather_icon = lv_obj_get_child(idle_screen.weather, 0);
    lv_obj_add_flag(idle_screen.weather, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *chips = flex_row_create(idle_screen.screen, CLOCK_CHIP_GAP, 0);
    lv_obj_remove_flag(chips, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(chips, LV_ALIGN_BOTTOM_MID, 0, CLOCK_CHIPS_Y);
    for (int i = 0; i < UI_APP_COUNT; i++) {
        ui_app_info_t info = ui_tools_info((ui_app_t)i);
        idle_screen.chip_labels[i] = clock_pill_create(chips, info.icon, info.tint, clock_chip_cb, (void *)(intptr_t)i);
        idle_screen.chips[i] = lv_obj_get_parent(idle_screen.chip_labels[i]);
        lv_obj_add_flag(idle_screen.chips[i], LV_OBJ_FLAG_HIDDEN);
    }

    ui_update_clock(false, NULL);
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
    const char *icon;
    bool have_weather = ui_weather_glance(&icon, buf, sizeof(buf));
    ui_set_visible(idle_screen.weather, have_weather);
    if (have_weather) {
        ui_label_set_text(idle_screen.weather_icon, icon);
        ui_label_set_text(idle_screen.weather_temp, buf);
    }
}

static void wireless_screen_build(void) {
    wireless_screen.screen = screen_create();
    wireless_screen.backdrop = backdrop_create(wireless_screen.screen);
    wireless_screen.ring = volume_ring_create(wireless_screen.screen, lv_color_hex(0x4ea1ff));

    // Y is set per-update (WIRELESS_STATUS_ROW_Y_CONNECTED vs. _WAITING) --
    // the mock's flex column re-centers around whichever block is showing.
    lv_obj_t *row = status_pill_create(wireless_screen.screen, WIRELESS_STATUS_ROW_Y_WAITING);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, open_switch_cb, LV_EVENT_CLICKED, NULL);
    wireless_screen.source_icon = icon_create(row, &icon_airplay, STATUS_ICON_SIZE, lv_color_white());
    wireless_screen.source_label = status_label_create(row);

    // -- waiting-for-connection state (mock's .wait-icon / np-meta) --
    wireless_screen.wait_chip = lv_obj_create(wireless_screen.screen);
    lv_obj_remove_flag(wireless_screen.wait_chip, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(wireless_screen.wait_chip, WIRELESS_WAIT_CHIP_SIZE, WIRELESS_WAIT_CHIP_SIZE);
    lv_obj_set_style_radius(wireless_screen.wait_chip, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(wireless_screen.wait_chip, 0, 0);
    lv_obj_set_style_pad_all(wireless_screen.wait_chip, 0, 0);
    lv_obj_set_style_bg_color(wireless_screen.wait_chip, lv_color_hex(0x1c1c24), 0);
    lv_obj_align(wireless_screen.wait_chip, LV_ALIGN_CENTER, 0, WIRELESS_WAIT_ICON_Y);
    wireless_screen.wait_icon =
        icon_create(wireless_screen.wait_chip, &icon_airplay, WIRELESS_WAIT_ICON_SIZE, lv_color_hex(0x9a9aa4));
    lv_obj_center(wireless_screen.wait_icon);

    wireless_screen.wait_title = lv_label_create(wireless_screen.screen);
    lv_obj_set_style_text_font(wireless_screen.wait_title, &inter_semi_16, 0);
    lv_obj_set_style_text_color(wireless_screen.wait_title, lv_color_white(), 0);
    lv_obj_align(wireless_screen.wait_title, LV_ALIGN_CENTER, 0, WIRELESS_WAIT_TITLE_Y);

    // Montserrat runs wider than the mock's SF, so the longest hint doesn't
    // fit in one line; scrolling keeps the mock's single-line layout.
    wireless_screen.wait_subtitle =
        marquee_label_create(wireless_screen.screen, &inter_med_13, lv_color_hex(0x9a9aa4));
    lv_obj_align(wireless_screen.wait_subtitle, LV_ALIGN_CENTER, 0, WIRELESS_WAIT_SUBTITLE_Y);

    // -- connected / now-playing state --
    wireless_screen.tile = art_tile_create(wireless_screen.screen, lv_color_hex(0x4ea1ff), WIRELESS_TILE_Y);
    wireless_screen.tile_icon = lv_image_create(wireless_screen.tile);
    lv_image_set_src(wireless_screen.tile_icon, &icon_music_note);
    lv_obj_set_style_image_recolor(wireless_screen.tile_icon, lv_color_white(), 0);
    lv_obj_set_style_image_recolor_opa(wireless_screen.tile_icon, LV_OPA_COVER, 0);
    lv_obj_center(wireless_screen.tile_icon);

    lv_obj_set_style_clip_corner(wireless_screen.tile, true, 0);
    lv_obj_set_style_pad_all(wireless_screen.tile, 0, 0);
    wireless_screen.art = lv_image_create(wireless_screen.tile);
    lv_obj_center(wireless_screen.art);
    lv_obj_add_flag(wireless_screen.art, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_flag(wireless_screen.tile, LV_OBJ_FLAG_HIDDEN);
    wireless_screen.tile_img = lv_image_create(wireless_screen.screen);
    lv_obj_align(wireless_screen.tile_img, LV_ALIGN_CENTER, 0, WIRELESS_TILE_Y);
    lv_obj_move_to_index(wireless_screen.tile_img, lv_obj_get_index(wireless_screen.tile) + 1);

    wireless_screen.transport = flex_row_create(wireless_screen.screen, TRANSPORT_GAP, WIRELESS_TRANSPORT_Y);
    wireless_screen.prev_icon = ui_round_button_create(wireless_screen.transport, LV_SYMBOL_PREV,
                                                       TRANSPORT_SECONDARY_SIZE, &lv_font_montserrat_16, true);
    wireless_screen.play_icon = ui_round_button_create(wireless_screen.transport, LV_SYMBOL_PLAY,
                                                       TRANSPORT_PRIMARY_SIZE, &lv_font_montserrat_22, false);
    wireless_screen.next_icon = ui_round_button_create(wireless_screen.transport, LV_SYMBOL_NEXT,
                                                       TRANSPORT_SECONDARY_SIZE, &lv_font_montserrat_16, true);

    static const ui_transport_t buttons[] = {UI_TRANSPORT_PREV, UI_TRANSPORT_PLAY_PAUSE, UI_TRANSPORT_NEXT};
    lv_obj_t *labels[] = {wireless_screen.prev_icon, wireless_screen.play_icon, wireless_screen.next_icon};
    for (int i = 0; i < 3; i++) {
        lv_obj_t *btn = lv_obj_get_parent(labels[i]);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(btn, 8);
        lv_obj_set_style_opa(btn, LV_OPA_COVER, 0);
        lv_obj_add_event_cb(btn, transport_click_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)buttons[i]);
    }

    wireless_screen.title_label = marquee_label_create(wireless_screen.screen, &inter_semi_16, lv_color_white());
    lv_obj_align(wireless_screen.title_label, LV_ALIGN_CENTER, 0, WIRELESS_TITLE_Y);

    // .np-subtitle is 12.5px in the mock; LVGL's bundled Montserrat only ships
    // even sizes, so 12 is the closer of the two neighbors (vs. 14).
    wireless_screen.subtitle_label =
        marquee_label_create(wireless_screen.screen, &inter_med_13, lv_color_hex(0x9a9aa4));
    lv_obj_align(wireless_screen.subtitle_label, LV_ALIGN_CENTER, 0, WIRELESS_SUBTITLE_Y);

    home_zone_create(wireless_screen.screen, swipe_home_cb);

    ui_track_info_t no_track = {0};
    wireless_state_t idle_state = {.volume = 0, .muted = 0, .connected = 0, .playing = 0, .source = SOURCE_AIRPLAY};
    ui_update_wireless(&idle_state, &no_track);
}

#ifdef AK_USB_AUDIO
static void usb_screen_build(void) {
    usb_screen.screen = screen_create();
    usb_screen.backdrop = backdrop_create(usb_screen.screen);
    lv_obj_set_style_bg_color(usb_screen.backdrop, lv_color_hex(0x2dd4bf), 0);
    usb_screen.ring = volume_ring_create(usb_screen.screen, lv_color_hex(0x2dd4bf));

    lv_obj_t *row = status_pill_create(usb_screen.screen, USB_STATUS_ROW_Y);
    lv_obj_t *icon = lv_label_create(row);
    lv_label_set_text(icon, LV_SYMBOL_USB);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(0x2dd4bf), 0);
    lv_obj_t *label = status_label_create(row);
    lv_label_set_text(label, "USB Audio");

    usb_screen.tile = art_tile_create(usb_screen.screen, lv_color_hex(0x2dd4bf), USB_TILE_Y);
    // mock-react's USB art-tile shows a generic speaker glyph, not the USB
    // plug glyph -- that one's reserved for the status row above.
    lv_obj_t *tile_icon = lv_label_create(usb_screen.tile);
    lv_label_set_text(tile_icon, LV_SYMBOL_VOLUME_MAX);
    lv_obj_set_style_text_font(tile_icon, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(tile_icon, lv_color_white(), 0);
    lv_obj_center(tile_icon);

    lv_obj_t *transport = flex_row_create(usb_screen.screen, TRANSPORT_GAP, USB_TRANSPORT_Y);
    usb_screen.prev_icon = ui_round_button_create(transport, LV_SYMBOL_PREV, TRANSPORT_SECONDARY_SIZE,
                                                  &lv_font_montserrat_16, true);
    usb_screen.play_icon = ui_round_button_create(transport, LV_SYMBOL_PLAY, TRANSPORT_PRIMARY_SIZE,
                                                  &lv_font_montserrat_22, false);
    usb_screen.next_icon = ui_round_button_create(transport, LV_SYMBOL_NEXT, TRANSPORT_SECONDARY_SIZE,
                                                  &lv_font_montserrat_16, true);

    // USB has no metadata channel, so the title slot just names the output.
    lv_obj_t *title = lv_label_create(usb_screen.screen);
    lv_label_set_text(title, "USB Audio");
    lv_obj_set_style_text_font(title, &inter_semi_16, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, USB_TITLE_Y);

    home_zone_create(usb_screen.screen, swipe_home_cb);

    ui_update_usb(0, false, false);
}
#endif

#define SWITCH_TITLE_Y     (-110)
#define SWITCH_LIST_Y      4
#define SWITCH_ITEM_W      190
#define SWITCH_ITEM_GAP    12

static const struct {
    const char *label;
    const lv_image_dsc_t *icon;
    uint32_t tint;
} k_sources[] = {
    [UI_SOURCE_AIRPLAY] = {"AirPlay", &icon_airplay, 0x4ea1ff},
    [UI_SOURCE_BLUETOOTH] = {"Bluetooth", &icon_bluetooth, 0x7c6cff},
};

static void switch_screen_refresh(void) {
    for (int i = 0; i < 2; i++) {
        bool active = i == (int)s_current_source;
        lv_obj_set_style_bg_opa(switch_screen.items[i], active ? 26 : 15, 0);
        lv_obj_set_style_border_color(switch_screen.items[i], lv_color_hex(k_sources[i].tint), 0);
        lv_obj_set_style_border_opa(switch_screen.items[i], active ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(switch_screen.items[i], active ? lv_color_white() : lv_color_hex(0x9a9aa4), 0);
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
    lv_obj_set_style_text_font(title, &inter_semi_11, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x5a5a64), 0);
    lv_obj_set_style_text_letter_space(title, 1, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, SWITCH_TITLE_Y);

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
        lv_obj_set_style_pad_column(item, 10, 0);
        lv_obj_set_style_pad_hor(item, 18, 0);
        lv_obj_set_style_pad_ver(item, 12, 0);
        lv_obj_set_style_radius(item, 16, 0);
        lv_obj_set_style_border_width(item, 1, 0);
        lv_obj_set_style_bg_color(item, lv_color_white(), 0);
        lv_obj_add_event_cb(item, source_pick_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);

        icon_create(item, k_sources[i].icon, 16, lv_color_hex(k_sources[i].tint));
        lv_obj_t *label = lv_label_create(item);
        lv_label_set_text(label, k_sources[i].label);
        lv_obj_set_style_text_font(label, &inter_semi_14, 0);
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

#define AUDIO_CHIP_Y      16
#define AUDIO_CHIP_TEXT_W 100

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
    return audio_chip.target && !launcher_visible() &&
           (active == idle_screen.screen || ui_tools_app_of(active) >= 0);
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

// Runs on every state push; each setter below skips no-op changes.
static void audio_chip_refresh(void) {
    if (!audio_chip.chip) {
        return;
    }
    lv_obj_t *target = audio_screen();
    audio_chip.target = target;
    ui_set_visible(audio_chip.chip, audio_chip_wanted());
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
            lv_obj_set_style_image_recolor(audio_chip.icon, lv_color_hex(s_audio.wireless_airplay ? 0x4ea1ff : 0x7c6cff),
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
        lv_obj_set_style_text_opa(audio_chip.label, dim ? LV_OPA_50 : LV_OPA_COVER, 0);
    }
}

static void audio_chip_cb(lv_event_t *e) {
    (void)e;
    if (audio_chip.target) {
        s_homed = false;
        // Paused audio would otherwise drop straight back home.
        s_pinned = true;
        show_screen(audio_chip.target);
    }
}

static void audio_chip_build(void) {
    lv_obj_t *chip = status_pill_create(lv_layer_top(), 0);
    lv_obj_align(chip, LV_ALIGN_TOP_MID, 0, AUDIO_CHIP_Y);
    lv_obj_add_flag(chip, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_ext_click_area(chip, 8);
    lv_obj_add_event_cb(chip, audio_chip_cb, LV_EVENT_CLICKED, NULL);
    audio_chip.chip = chip;

    audio_chip.icon = icon_create(chip, &icon_airplay, STATUS_ICON_SIZE, lv_color_hex(0x4ea1ff));
#ifdef AK_USB_AUDIO
    audio_chip.usb_icon = lv_label_create(chip);
    lv_label_set_text(audio_chip.usb_icon, LV_SYMBOL_USB);
    lv_obj_set_style_text_font(audio_chip.usb_icon, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(audio_chip.usb_icon, lv_color_hex(0x2dd4bf), 0);
#endif

    audio_chip.label = status_label_create(chip);
    lv_obj_set_style_text_color(audio_chip.label, lv_color_white(), 0);
    lv_label_set_long_mode(audio_chip.label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(audio_chip.label, lv_font_get_line_height(&inter_med_13));
}

static void launcher_visibility_cb(void) {
    audio_chip_refresh();
}

#define NAV_TICK_MS     250
#define APP_IDLE_HOME_MS 30000

static void nav_tick_cb(lv_timer_t *t) {
    (void)t;
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

// The knob doesn't go through an LVGL input device, so it has to report its
// own activity for the idle timeout.
static void knob_activity(void) {
    lv_display_trigger_activity(NULL);
}

bool ui_knob_rotate(int steps) {
    knob_activity();
    if (launcher_visible()) {
        launcher_rotate(steps);
        return true;
    }
    return ui_tools_knob_rotate(lv_screen_active(), steps);
}

bool ui_knob_press(void) {
    knob_activity();
    if (launcher_visible()) {
        launcher_activate();
        return true;
    }
    return ui_tools_knob_press(lv_screen_active());
}

void ui_knob_long_press(void) {
    knob_activity();
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
    launcher_build(open_app, go_home, launcher_visibility_cb);
    lv_timer_create(nav_tick_cb, NAV_TICK_MS, NULL);

    lv_screen_load(s_home);
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
        lv_obj_add_flag(wireless_screen.art, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(wireless_screen.tile_icon, LV_OBJ_FLAG_HIDDEN);
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
    lv_obj_invalidate(wireless_screen.art);
    lv_obj_remove_flag(wireless_screen.art, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(wireless_screen.tile_icon, LV_OBJ_FLAG_HIDDEN);
}

// A rounded, clipped, gradient tile costs a few ms per full frame to
// rasterise, and it only changes with the source tint or the art. Its
// snapshot keeps alpha so the antialiased corners blend over the backdrop
// (ARGB8888: LVGL can't blend ARGB8565 onto the panel's swapped RGB565).
static void tile_bake(lv_color_t tint) {
    if (wireless_screen.tile_baked && wireless_screen.baked_art_version == wireless_screen.art_version &&
        lv_color_eq(wireless_screen.baked_tint, tint)) {
        return;
    }
    lv_obj_remove_flag(wireless_screen.tile, LV_OBJ_FLAG_HIDDEN);
    lv_obj_update_layout(wireless_screen.tile);
    lv_draw_buf_t *baked = lv_snapshot_take(wireless_screen.tile, LV_COLOR_FORMAT_ARGB8888);
    lv_obj_add_flag(wireless_screen.tile, LV_OBJ_FLAG_HIDDEN);
    if (!baked) {
        return;
    }
    lv_image_set_src(wireless_screen.tile_img, baked);
    if (wireless_screen.tile_baked) {
        lv_draw_buf_destroy(wireless_screen.tile_baked);
    }
    wireless_screen.tile_baked = baked;
    wireless_screen.baked_art_version = wireless_screen.art_version;
    wireless_screen.baked_tint = tint;
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
    static bool was_playing;
    bool airplay = state->source == SOURCE_AIRPLAY;
    lv_color_t tint = airplay ? lv_color_hex(0x4ea1ff) : lv_color_hex(0x7c6cff);
    s_current_source = airplay ? UI_SOURCE_AIRPLAY : UI_SOURCE_BLUETOOTH;

    ring_set_volume(wireless_screen.ring, state->volume);
    lv_image_set_src(wireless_screen.source_icon, airplay ? &icon_airplay : &icon_bluetooth);
    lv_obj_set_style_image_recolor(wireless_screen.source_icon, tint, 0);
    ui_label_set_text(wireless_screen.source_label, airplay ? "AirPlay" : "Bluetooth");
    lv_obj_set_style_arc_color(wireless_screen.ring, tint, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(wireless_screen.tile, tint, 0);
    lv_obj_set_style_bg_color(wireless_screen.backdrop, tint, 0);

    // Neither source has a local "pairing" step on this end -- AirPlay only
    // appears once picked from the phone's Control Center, and Bluetooth
    // just advertises until a phone connects -- so an unconnected wireless
    // screen has nothing to show a tile/track for yet, nor a volume to show.
    bool waiting = !state->connected;
    lv_obj_t *status_row = lv_obj_get_parent(wireless_screen.source_icon);
    lv_obj_align(status_row, LV_ALIGN_CENTER, 0,
                 waiting ? WIRELESS_STATUS_ROW_Y_WAITING : WIRELESS_STATUS_ROW_Y_CONNECTED);

    ui_set_visible(wireless_screen.ring, !waiting);
    ui_set_visible(wireless_screen.backdrop, !waiting);
    ui_set_visible(wireless_screen.wait_chip, waiting);
    ui_set_visible(wireless_screen.wait_title, waiting);
    ui_set_visible(wireless_screen.wait_subtitle, waiting);
    ui_set_visible(wireless_screen.tile_img, !waiting);
    ui_set_visible(wireless_screen.transport, !waiting);
    ui_set_visible(wireless_screen.title_label, !waiting);
    ui_set_visible(wireless_screen.subtitle_label, !waiting);

    char subtitle[128];
    format_subtitle(track, subtitle, sizeof(subtitle));
    bool has_track = track && track->title[0];
    const char *source_name = airplay ? "AirPlay" : "Bluetooth";

    if (waiting) {
        lv_image_set_src(wireless_screen.wait_icon, airplay ? &icon_airplay : &icon_bluetooth);
        ui_label_set_text(wireless_screen.wait_title, airplay ? "Waiting for AirPlay" : "Ready to pair");
        ui_label_set_text(wireless_screen.wait_subtitle,
                           airplay ? "Select this speaker from Control Center" : "Discoverable over Bluetooth");
    } else {
        ui_label_set_text(wireless_screen.play_icon,
                           state->muted ? LV_SYMBOL_MUTE : (state->playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY));
        lv_obj_set_style_text_color(wireless_screen.play_icon, state->muted ? lv_color_hex(0xff5a5a) : lv_color_white(),
                                     0);
        ui_label_set_text(wireless_screen.title_label, has_track ? track->title : source_name);
        ui_label_set_text(wireless_screen.subtitle_label, subtitle);
        update_artwork(track);
        tile_bake(tint);
    }

    s_audio.wireless_connected = state->connected;
    s_audio.wireless_playing = state->playing;
    s_audio.wireless_airplay = airplay;
    snprintf(s_audio.wireless_text, sizeof(s_audio.wireless_text), "%s", has_track ? track->title : source_name);
    surface_source(wireless_screen.screen, state->playing, &was_playing);
    audio_chip_refresh();
}

#ifdef AK_USB_AUDIO
void ui_update_usb(uint8_t volume, bool muted, bool playing) {
    static bool was_playing;
    ring_set_volume(usb_screen.ring, volume);

    ui_label_set_text(usb_screen.play_icon, muted ? LV_SYMBOL_MUTE : (playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY));
    lv_obj_set_style_text_color(usb_screen.play_icon, muted ? lv_color_hex(0xff5a5a) : lv_color_white(), 0);

    s_audio.usb_playing = playing;
    surface_source(usb_screen.screen, playing, &was_playing);
    audio_chip_refresh();
}
#endif

void ui_update_clock(bool synced, const struct tm *local_time) {
    if (!synced || !local_time) {
        ui_label_set_text(idle_screen.time_label, "--:--");
        ui_label_set_text(idle_screen.date_label, "Waiting for time sync");
        return;
    }

    // The clock font has digits and ':' only, so 12-hour time carries its
    // AM/PM on the date line.
    bool h24 = ui_settings_current()->clock_24h;
    int hour = h24 ? local_time->tm_hour : (local_time->tm_hour % 12 ? local_time->tm_hour % 12 : 12);
    char buf[32];
    snprintf(buf, sizeof(buf), h24 ? "%02d:%02d" : "%d:%02d", hour, local_time->tm_min);
    ui_label_set_text(idle_screen.time_label, buf);

    // "Sun 4 Oct" like the mock; %d would zero-pad and %-d isn't portable.
    char weekday[8], month[8];
    strftime(weekday, sizeof(weekday), "%a", local_time);
    strftime(month, sizeof(month), "%b", local_time);
    snprintf(buf, sizeof(buf), "%s %d %s%s", weekday, local_time->tm_mday, month,
             h24 ? "" : local_time->tm_hour < 12 ? " \xC2\xB7 AM" : " \xC2\xB7 PM");
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
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_opa(label, opa, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(label, 240);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, y);
    return label;
}

static void wifi_setup_screen_build(void) {
    wifi_setup_screen.screen = screen_create();

    lv_obj_t *title = setup_label_create(wifi_setup_screen.screen, &inter_semi_16, LV_OPA_COVER, SETUP_TITLE_Y);
    lv_label_set_text(title, "Set up WiFi");

    wifi_setup_screen.qr = lv_qrcode_create(wifi_setup_screen.screen);
    lv_qrcode_set_size(wifi_setup_screen.qr, SETUP_QR_SIZE);
    lv_qrcode_set_dark_color(wifi_setup_screen.qr, lv_color_black());
    lv_qrcode_set_light_color(wifi_setup_screen.qr, lv_color_white());
    // Phone cameras need a light margin around the code on a dark screen.
    lv_qrcode_set_quiet_zone(wifi_setup_screen.qr, true);
    lv_obj_align(wifi_setup_screen.qr, LV_ALIGN_CENTER, 0, SETUP_QR_Y);

    wifi_setup_screen.status_label =
        setup_label_create(wifi_setup_screen.screen, &inter_med_13, LV_OPA_COVER, SETUP_STATUS_Y);
    wifi_setup_screen.network_label =
        setup_label_create(wifi_setup_screen.screen, &inter_med_12, LV_OPA_60, SETUP_NETWORK_Y);
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
