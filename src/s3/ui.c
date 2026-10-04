#include "ui.h"
#include "lvgl.h"
#include "icons.h"

#include <stdio.h>
#include <string.h>

// Screens are display-only for state that another MCU owns (wireless
// playback/volume live on esp32, per the control-ownership rule in
// CLAUDE.md), and interactive only where s3 genuinely owns the state.
// Nothing here talks to UART or USB directly -- ui_update_*() is the only
// way data gets in, so this file stays pure LVGL and reusable by env:native.
//
// Layout convention: the panel is round, so every screen keeps its chrome
// (back button, status row) on the compass points -- top-center, bottom-
// center -- and out of the four corners, which a round bezel clips.
//
// Navigation: there is no mode-select menu. Real single-purpose audio
// devices land on an idle/clock screen and switch to a now-playing screen
// on their own the moment a source starts playing, then fall back to idle
// when it stops -- they don't make you pick a mode first. ui_update_*()
// drives that switch; the back button on each now-playing screen is the
// only manual navigation, and it always returns to idle.

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
// Measured mock values (111/131) collide with our real, visible back button
// at the south compass point -- the mock's equivalent "go home" affordance
// is an invisible swipe zone with just a thin 4px pill, not a solid 40px
// button, so it never overlapped this text there. Pulled up just enough to
// clear it.
#define WIRELESS_TITLE_Y                96
#define WIRELESS_SUBTITLE_Y             116

#define WIRELESS_STATUS_ROW_Y_WAITING (-84)
#define WIRELESS_WAIT_ICON_Y          (-10)
#define WIRELESS_WAIT_ICON_SIZE       96
#define WIRELESS_WAIT_TITLE_Y         62
#define WIRELESS_WAIT_SUBTITLE_Y      82

#define USB_STATUS_ROW_Y (-126)
#define USB_TILE_Y        (-33)
#define USB_TRANSPORT_Y   71
#define USB_LABEL_Y        118

#define TRANSPORT_PRIMARY_SIZE   48
#define TRANSPORT_SECONDARY_SIZE 40
#define TRANSPORT_GAP            14

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *ring;
    lv_obj_t *source_icon;
    lv_obj_t *source_label;

    // Shown when state->connected is false -- neither source has a local
    // "pairing" step on this end (AirPlay only appears once picked from the
    // phone's Control Center; Bluetooth just advertises until a phone
    // connects), so there's nothing to show a tile/track for yet.
    lv_obj_t *wait_icon;
    lv_obj_t *wait_title;
    lv_obj_t *wait_subtitle;

    // Shown once connected.
    lv_obj_t *tile;
    lv_obj_t *transport;
    lv_obj_t *prev_icon;
    lv_obj_t *play_icon;
    lv_obj_t *next_icon;
    lv_obj_t *title_label;
    lv_obj_t *subtitle_label;
} wireless_screen_t;

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *ring;
    lv_obj_t *tile;
    lv_obj_t *volume_label;
    lv_obj_t *prev_icon;
    lv_obj_t *play_icon;
    lv_obj_t *next_icon;
} usb_screen_t;

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *time_label;
    lv_obj_t *date_label;
} idle_screen_t;

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *qr;
    lv_obj_t *status_label;
    lv_obj_t *network_label;
    char qr_payload[128];
} wifi_setup_screen_t;

static idle_screen_t idle_screen;
static wireless_screen_t wireless_screen;
static usb_screen_t usb_screen;
static wifi_setup_screen_t wifi_setup_screen;

// Where back buttons and "playback stopped" return to: the clock normally,
// the WiFi setup QR while setup is pending.
static lv_obj_t *s_home;

// Avoids re-loading (and restarting screen-load animations for) a screen
// that's already active, since ui_update_*() calls this on every state push.
static void show_screen(lv_obj_t *target) {
    if (lv_screen_active() != target) {
        lv_screen_load(target);
    }
}

static void go_idle_cb(lv_event_t *e) {
    (void)e;
    show_screen(s_home);
}

static lv_obj_t *screen_create(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0b0b10), 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    return scr;
}

// South compass point: fully visible on a round panel, unlike the corners.
static void back_button_create(lv_obj_t *parent) {
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, 40, 40);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -14);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x1c1c24), 0);
    lv_obj_add_event_cb(btn, go_idle_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *icon = lv_label_create(btn);
    lv_label_set_text(icon, LV_SYMBOL_LEFT);
    lv_obj_center(icon);
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

static lv_obj_t *status_icon_create(lv_obj_t *row, const lv_image_dsc_t *src) {
    lv_obj_t *icon = lv_image_create(row);
    lv_image_set_src(icon, src);
    lv_image_set_scale(icon, 128); // 64px source -> 32px, half-size for the status row
    lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
    return icon;
}

static lv_obj_t *status_label_create(lv_obj_t *row) {
    lv_obj_t *label = lv_label_create(row);
    lv_obj_set_style_text_color(label, lv_color_hex(0x9a9aa4), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
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
static lv_obj_t *transport_button_create(lv_obj_t *parent, const char *symbol, int32_t size,
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

// Perimeter volume gauge: read-only (volume authority lives off-screen), a
// full ring except a bottom gap so it doesn't fight the back button there.
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
    lv_obj_center(arc);
    return arc;
}

// Ambient wallpaper glow (mock-react's .idle-art): the mock layers several
// blurred `radial-gradient(circle at X% Y%, hsla(...), transparent 40%)`
// blobs. LVGL has no blur filter, but LV_USE_DRAW_SW_COMPLEX_GRADIENTS gives
// a real radial gradient, which reads close enough to a blurred glow -- style
// and grad descriptors must be static, since LVGL keeps pointers into them
// for the object's lifetime.
static lv_style_t s_idle_blob_style[2];
static lv_grad_dsc_t s_idle_blob_grad[2];

static void idle_blob_create(lv_obj_t *parent, int idx, lv_align_t align, int32_t ofs_x, int32_t ofs_y,
                              int32_t size, lv_color_t color, lv_opa_t peak_opa) {
    lv_grad_dsc_t *grad = &s_idle_blob_grad[idx];
    lv_color_t colors[2] = {color, color};
    lv_opa_t opas[2] = {peak_opa, LV_OPA_TRANSP};
    lv_grad_init_stops(grad, colors, opas, NULL, 2);
    int32_t r = size / 2;
    lv_grad_radial_init(grad, r, r, size, r, LV_GRAD_EXTEND_PAD);

    lv_style_t *style = &s_idle_blob_style[idx];
    lv_style_init(style);
    lv_style_set_bg_opa(style, LV_OPA_COVER);
    lv_style_set_bg_grad(style, grad);
    lv_style_set_border_width(style, 0);
    lv_style_set_radius(style, LV_RADIUS_CIRCLE);

    lv_obj_t *blob = lv_obj_create(parent);
    lv_obj_remove_flag(blob, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_style(blob, style, 0);
    lv_obj_set_size(blob, size, size);
    lv_obj_align(blob, align, ofs_x, ofs_y);
}

static void idle_screen_build(void) {
    idle_screen.screen = screen_create();

    idle_blob_create(idle_screen.screen, 0, LV_ALIGN_TOP_LEFT, -100, -100, 320, lv_color_hex(0x4ea1ff), LV_OPA_70);
    idle_blob_create(idle_screen.screen, 1, LV_ALIGN_BOTTOM_RIGHT, 80, 80, 300, lv_color_hex(0x7c6cff), LV_OPA_60);

    // .idle-scrim: darkens the blobs so the clock stays readable. Mock uses a
    // radial vignette; LVGL has no radial-opacity gradient, so this is a flat
    // dark layer instead.
    lv_obj_t *scrim = lv_obj_create(idle_screen.screen);
    lv_obj_remove_style_all(scrim);
    lv_obj_remove_flag(scrim, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(scrim, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(scrim, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scrim, LV_OPA_60, 0);

    // .idle-date above .idle-clock, matching the mock's stacking order.
    idle_screen.date_label = lv_label_create(idle_screen.screen);
    lv_obj_set_style_text_font(idle_screen.date_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(idle_screen.date_label, lv_color_white(), 0);
    lv_obj_set_style_text_opa(idle_screen.date_label, LV_OPA_60, 0);
    lv_obj_align(idle_screen.date_label, LV_ALIGN_CENTER, 0, -39);

    idle_screen.time_label = lv_label_create(idle_screen.screen);
    lv_obj_set_style_text_font(idle_screen.time_label, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(idle_screen.time_label, lv_color_white(), 0);
    lv_obj_align(idle_screen.time_label, LV_ALIGN_CENTER, 0, 6);

    ui_update_clock(false, NULL);
}

static void wireless_screen_build(void) {
    wireless_screen.screen = screen_create();
    wireless_screen.ring = volume_ring_create(wireless_screen.screen, lv_color_hex(0x4ea1ff));

    // Y is set per-update (WIRELESS_STATUS_ROW_Y_CONNECTED vs. _WAITING) --
    // the mock's flex column re-centers around whichever block is showing.
    lv_obj_t *row = flex_row_create(wireless_screen.screen, 6, WIRELESS_STATUS_ROW_Y_WAITING);
    wireless_screen.source_icon = status_icon_create(row, &icon_airplay);
    wireless_screen.source_label = status_label_create(row);

    // -- waiting-for-connection state (mock's .wait-icon / np-meta) --
    wireless_screen.wait_icon = lv_image_create(wireless_screen.screen);
    lv_image_set_src(wireless_screen.wait_icon, &icon_airplay);
    lv_image_set_scale(wireless_screen.wait_icon, WIRELESS_WAIT_ICON_SIZE * 256 / 64);
    lv_obj_set_style_image_recolor(wireless_screen.wait_icon, lv_color_hex(0x9a9aa4), 0);
    lv_obj_set_style_image_recolor_opa(wireless_screen.wait_icon, LV_OPA_COVER, 0);
    lv_obj_align(wireless_screen.wait_icon, LV_ALIGN_CENTER, 0, WIRELESS_WAIT_ICON_Y);

    wireless_screen.wait_title = lv_label_create(wireless_screen.screen);
    lv_obj_set_style_text_font(wireless_screen.wait_title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(wireless_screen.wait_title, lv_color_white(), 0);
    lv_obj_align(wireless_screen.wait_title, LV_ALIGN_CENTER, 0, WIRELESS_WAIT_TITLE_Y);

    wireless_screen.wait_subtitle = lv_label_create(wireless_screen.screen);
    lv_obj_set_style_text_font(wireless_screen.wait_subtitle, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(wireless_screen.wait_subtitle, lv_color_hex(0x9a9aa4), 0);
    lv_obj_set_width(wireless_screen.wait_subtitle, 220);
    lv_obj_set_style_text_align(wireless_screen.wait_subtitle, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(wireless_screen.wait_subtitle, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align(wireless_screen.wait_subtitle, LV_ALIGN_CENTER, 0, WIRELESS_WAIT_SUBTITLE_Y);

    // -- connected / now-playing state --
    wireless_screen.tile = art_tile_create(wireless_screen.screen, lv_color_hex(0x4ea1ff), WIRELESS_TILE_Y);
    lv_obj_t *tile_icon = lv_image_create(wireless_screen.tile);
    lv_image_set_src(tile_icon, &icon_music_note);
    lv_obj_set_style_image_recolor(tile_icon, lv_color_white(), 0);
    lv_obj_set_style_image_recolor_opa(tile_icon, LV_OPA_COVER, 0);
    lv_obj_center(tile_icon);

    wireless_screen.transport = flex_row_create(wireless_screen.screen, TRANSPORT_GAP, WIRELESS_TRANSPORT_Y);
    wireless_screen.prev_icon = transport_button_create(wireless_screen.transport, LV_SYMBOL_PREV,
                                                          TRANSPORT_SECONDARY_SIZE, &lv_font_montserrat_16, true);
    wireless_screen.play_icon = transport_button_create(wireless_screen.transport, LV_SYMBOL_PLAY,
                                                          TRANSPORT_PRIMARY_SIZE, &lv_font_montserrat_22, false);
    wireless_screen.next_icon = transport_button_create(wireless_screen.transport, LV_SYMBOL_NEXT,
                                                          TRANSPORT_SECONDARY_SIZE, &lv_font_montserrat_16, true);

    wireless_screen.title_label = marquee_label_create(wireless_screen.screen, &lv_font_montserrat_16, lv_color_white());
    lv_obj_align(wireless_screen.title_label, LV_ALIGN_CENTER, 0, WIRELESS_TITLE_Y);

    // .np-subtitle is 12.5px in the mock; LVGL's bundled Montserrat only ships
    // even sizes, so 12 is the closer of the two neighbors (vs. 14).
    wireless_screen.subtitle_label =
        marquee_label_create(wireless_screen.screen, &lv_font_montserrat_12, lv_color_hex(0x9a9aa4));
    lv_obj_align(wireless_screen.subtitle_label, LV_ALIGN_CENTER, 0, WIRELESS_SUBTITLE_Y);

    back_button_create(wireless_screen.screen);

    ui_track_info_t no_track = {0};
    wireless_state_t idle_state = {.volume = 0, .muted = 0, .connected = 0, .playing = 0, .source = SOURCE_AIRPLAY};
    ui_update_wireless(&idle_state, &no_track);
}

static void usb_screen_build(void) {
    usb_screen.screen = screen_create();
    usb_screen.ring = volume_ring_create(usb_screen.screen, lv_color_hex(0x2dd4bf));

    lv_obj_t *row = flex_row_create(usb_screen.screen, 6, USB_STATUS_ROW_Y);
    lv_obj_t *icon = lv_label_create(row);
    lv_label_set_text(icon, LV_SYMBOL_USB);
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
    usb_screen.prev_icon = transport_button_create(transport, LV_SYMBOL_PREV, TRANSPORT_SECONDARY_SIZE,
                                                    &lv_font_montserrat_16, true);
    usb_screen.play_icon = transport_button_create(transport, LV_SYMBOL_PLAY, TRANSPORT_PRIMARY_SIZE,
                                                    &lv_font_montserrat_22, false);
    usb_screen.next_icon = transport_button_create(transport, LV_SYMBOL_NEXT, TRANSPORT_SECONDARY_SIZE,
                                                     &lv_font_montserrat_16, true);

    usb_screen.volume_label = lv_label_create(usb_screen.screen);
    lv_obj_set_style_text_font(usb_screen.volume_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(usb_screen.volume_label, lv_color_white(), 0);
    lv_obj_align(usb_screen.volume_label, LV_ALIGN_CENTER, 0, USB_LABEL_Y);

    back_button_create(usb_screen.screen);

    ui_update_usb(0, false, false);
}

void ui_init(void) {
    // idle_screen is the back-navigation target and the landing screen, so
    // it must exist before the other two screens wire their back buttons to it.
    idle_screen_build();
    s_home = idle_screen.screen;
    wireless_screen_build();
    usb_screen_build();

    lv_screen_load(s_home);
}

void ui_update_wireless(const wireless_state_t *state, const ui_track_info_t *track) {
    lv_arc_set_value(wireless_screen.ring, state->volume);

    bool airplay = state->source == SOURCE_AIRPLAY;
    lv_color_t tint = airplay ? lv_color_hex(0x4ea1ff) : lv_color_hex(0x7c6cff);

    lv_image_set_src(wireless_screen.source_icon, airplay ? &icon_airplay : &icon_bluetooth);
    lv_obj_set_style_image_recolor_opa(wireless_screen.source_icon, state->connected ? LV_OPA_COVER : LV_OPA_50, 0);
    lv_obj_set_style_image_recolor(wireless_screen.source_icon, tint, 0);
    lv_label_set_text(wireless_screen.source_label, airplay ? "AirPlay" : "Bluetooth");
    lv_obj_set_style_arc_color(wireless_screen.ring, tint, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(wireless_screen.tile, tint, 0);

    // Neither source has a local "pairing" step on this end -- AirPlay only
    // appears once picked from the phone's Control Center, and Bluetooth
    // just advertises until a phone connects -- so an unconnected wireless
    // screen has nothing to show a tile/track for yet.
    bool waiting = !state->connected;
    lv_obj_t *status_row = lv_obj_get_parent(wireless_screen.source_icon);
    lv_obj_align(status_row, LV_ALIGN_CENTER, 0,
                 waiting ? WIRELESS_STATUS_ROW_Y_WAITING : WIRELESS_STATUS_ROW_Y_CONNECTED);

    if (waiting) {
        lv_obj_add_flag(wireless_screen.tile, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(wireless_screen.transport, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(wireless_screen.title_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(wireless_screen.subtitle_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(wireless_screen.wait_icon, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(wireless_screen.wait_title, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(wireless_screen.wait_subtitle, LV_OBJ_FLAG_HIDDEN);

        lv_image_set_src(wireless_screen.wait_icon, airplay ? &icon_airplay : &icon_bluetooth);
        lv_obj_set_style_image_recolor(wireless_screen.wait_icon, tint, 0);
        lv_label_set_text(wireless_screen.wait_title, airplay ? "Waiting for AirPlay" : "Ready to pair");
        lv_label_set_text(wireless_screen.wait_subtitle,
                           airplay ? "Select this speaker from Control Center" : "Discoverable over Bluetooth");
    } else {
        lv_obj_add_flag(wireless_screen.wait_icon, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(wireless_screen.wait_title, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(wireless_screen.wait_subtitle, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(wireless_screen.tile, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(wireless_screen.transport, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(wireless_screen.title_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(wireless_screen.subtitle_label, LV_OBJ_FLAG_HIDDEN);

        lv_label_set_text(wireless_screen.play_icon,
                           state->muted ? LV_SYMBOL_MUTE : (state->playing ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE));
        lv_obj_set_style_text_color(wireless_screen.play_icon, state->muted ? lv_color_hex(0xff5a5a) : lv_color_white(),
                                     0);

        bool has_track = track && track->title[0];
        lv_label_set_text(wireless_screen.title_label, has_track ? track->title : (airplay ? "AirPlay" : "Bluetooth"));

        char subtitle[128] = "";
        if (has_track) {
            if (track->artist[0] && track->album[0]) {
                // Plain ASCII separator: LVGL's built-in Montserrat subset has no em dash glyph.
                snprintf(subtitle, sizeof(subtitle), "%s - %s", track->artist, track->album);
            } else if (track->artist[0]) {
                snprintf(subtitle, sizeof(subtitle), "%s", track->artist);
            } else if (track->album[0]) {
                snprintf(subtitle, sizeof(subtitle), "%s", track->album);
            }
        }
        lv_label_set_text(wireless_screen.subtitle_label, subtitle);
    }

    // No manual mode picker -- surface this screen the moment audio is
    // actually flowing, drop back to idle the moment it stops.
    if (state->playing) {
        show_screen(wireless_screen.screen);
    } else if (lv_screen_active() == wireless_screen.screen) {
        show_screen(s_home);
    }
}

void ui_update_usb(uint8_t volume, bool muted, bool playing) {
    lv_arc_set_value(usb_screen.ring, volume);
    lv_label_set_text_fmt(usb_screen.volume_label, "%d%%", volume);

    lv_label_set_text(usb_screen.play_icon, muted ? LV_SYMBOL_MUTE : (playing ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE));
    lv_obj_set_style_text_color(usb_screen.play_icon, muted ? lv_color_hex(0xff5a5a) : lv_color_white(), 0);

    if (playing) {
        show_screen(usb_screen.screen);
    } else if (lv_screen_active() == usb_screen.screen) {
        show_screen(s_home);
    }
}

void ui_update_clock(bool synced, const struct tm *local_time) {
    if (!synced || !local_time) {
        lv_label_set_text(idle_screen.time_label, "--:--");
        lv_label_set_text(idle_screen.date_label, "Waiting for time sync");
        return;
    }

    char buf[32];
    strftime(buf, sizeof(buf), "%H:%M", local_time);
    lv_label_set_text(idle_screen.time_label, buf);
    strftime(buf, sizeof(buf), "%a, %b %d", local_time);
    lv_label_set_text(idle_screen.date_label, buf);
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

    lv_obj_t *title = setup_label_create(wifi_setup_screen.screen, &lv_font_montserrat_16, LV_OPA_COVER, SETUP_TITLE_Y);
    lv_label_set_text(title, "Set up WiFi");

    wifi_setup_screen.qr = lv_qrcode_create(wifi_setup_screen.screen);
    lv_qrcode_set_size(wifi_setup_screen.qr, SETUP_QR_SIZE);
    lv_qrcode_set_dark_color(wifi_setup_screen.qr, lv_color_black());
    lv_qrcode_set_light_color(wifi_setup_screen.qr, lv_color_white());
    // Phone cameras need a light margin around the code on a dark screen.
    lv_qrcode_set_quiet_zone(wifi_setup_screen.qr, true);
    lv_obj_align(wifi_setup_screen.qr, LV_ALIGN_CENTER, 0, SETUP_QR_Y);

    wifi_setup_screen.status_label =
        setup_label_create(wifi_setup_screen.screen, &lv_font_montserrat_14, LV_OPA_COVER, SETUP_STATUS_Y);
    wifi_setup_screen.network_label =
        setup_label_create(wifi_setup_screen.screen, &lv_font_montserrat_12, LV_OPA_60, SETUP_NETWORK_Y);
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
