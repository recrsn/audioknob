#include "ui/now_playing/now_playing.h"

#include <stdio.h>
#include <string.h>

#include "ui/assets/fonts.h"
#include "ui/assets/icons.h"
#include "ui/assets/ui_bg_data.h"
#include "ui/home/band.h"
#include "ui/home/home.h"
#include "ui/now_playing/switch.h"
#include "ui/shell/audio_chip.h"
#include "ui/shell/nav.h"
#include "ui/shell/volume_ring.h"
#include "ui/widgets/widgets.h"

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


typedef struct {
    lv_obj_t *screen;
    ui_now_playing_bg_t bg;
    lv_obj_t *source_icon;
    lv_obj_t *source_label;
    // The top pill shows the time and source, unless something else wants
    // the band: then that app's glyph and status, and a tap opens it.
    lv_obj_t *pill_glyph;
    int pill_app;

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
    lv_obj_t *prev_icon;
    lv_obj_t *play_icon;
    lv_obj_t *next_icon;
} usb_screen_t;
#endif

static const np_source_info_t k_sources[NP_SOURCE_COUNT] = {
    [UI_SOURCE_AIRPLAY] = {"AirPlay", &icon_airplay, UI_TINT_AIRPLAY, "Waiting for AirPlay",
                           "Select this speaker from Control Center"},
    [UI_SOURCE_BLUETOOTH] = {"Bluetooth", &icon_bluetooth, UI_TINT_BLUETOOTH,
                             "Waiting for Bluetooth", "Tap the icon to pair a new phone"},
    [UI_SOURCE_RADIO] = {"Radio", &icon_radio, UI_TINT_RADIO, "Radio",
                         "Pick a station in the Radio app"},
};

static wireless_screen_t wireless_screen;
#ifdef AK_USB_AUDIO
static usb_screen_t usb_screen;
#endif
// What's playing, kept from the last ui_update_wireless()/ui_update_usb().
static np_audio_t s_audio;
static ui_source_t s_current_source = UI_SOURCE_AIRPLAY;
static void (*s_transport_handler)(ui_transport_t);

const np_source_info_t *np_source(ui_source_t source) {
    return &k_sources[source];
}

const np_audio_t *np_audio(void) {
    return &s_audio;
}

lv_obj_t *np_wireless_screen(void) {
    return wireless_screen.screen;
}

#ifdef AK_USB_AUDIO
lv_obj_t *np_usb_screen(void) {
    return usb_screen.screen;
}
#endif

bool np_is_screen(lv_obj_t *screen) {
#ifdef AK_USB_AUDIO
    if (screen == usb_screen.screen) {
        return true;
    }
#endif
    return screen == wireless_screen.screen;
}

ui_source_t np_current_source(void) {
    return s_current_source;
}

void np_set_current_source(ui_source_t source) {
    s_current_source = source;
}

const lv_image_dsc_t *np_art(uint32_t *version) {
    *version = wireless_screen.art_version;
    return &wireless_screen.art_dsc;
}

static ui_source_t source_of(uint8_t wireless_source) {
    switch (wireless_source) {
    case SOURCE_A2DP:
        return UI_SOURCE_BLUETOOTH;
    case SOURCE_RADIO:
        return UI_SOURCE_RADIO;
    default:
        return UI_SOURCE_AIRPLAY;
    }
}

void ui_set_transport_handler(void (*handler)(ui_transport_t button)) {
    s_transport_handler = handler;
}

void np_transport(ui_transport_t button) {
    if (s_transport_handler) {
        s_transport_handler(button);
    }
}

static void transport_click_cb(lv_event_t *e) {
    np_transport((ui_transport_t)(uintptr_t)lv_event_get_user_data(e));
}

lv_obj_t *np_audio_screen(void) {
    if (s_audio.wireless_playing) {
        return wireless_screen.screen;
    }
    bool focus;
    if (ui_ambience_playing(NULL, &focus)) {
        return nav_app_screen(focus ? UI_APP_TOMATO : UI_APP_AMBIENCE);
    }
#ifdef AK_USB_AUDIO
    if (s_audio.usb_playing) {
        return usb_screen.screen;
    }
#endif
    return s_audio.wireless_connected ? wireless_screen.screen : NULL;
}

bool np_is_ambience(lv_obj_t *target) {
    return target && ui_tools_app_of(target) >= 0;
}

bool np_status_text(char *buf, size_t len) {
    lv_obj_t *target = np_audio_screen();
    if (!target) {
        return false;
    }
#ifdef AK_USB_AUDIO
    if (target == usb_screen.screen) {
        snprintf(buf, len, "USB Audio");
        return true;
    }
#endif
    if (np_is_ambience(target)) {
        ui_ambience_t kind;
        ui_ambience_playing(&kind, NULL);
        snprintf(buf, len, "%s", ui_ambience_name(kind));
        return true;
    }
    snprintf(buf, len, "%s", s_audio.wireless_text);
    return true;
}

bool ui_stream_playing(void) {
#ifdef AK_USB_AUDIO
    if (s_audio.usb_playing) {
        return true;
    }
#endif
    return s_audio.wireless_playing;
}

static void wait_chip_click_cb(lv_event_t *e) {
    (void)e;
    if (wireless_screen.bluetooth) {
        ui_pairing_request(UI_PAIRING_OPEN);
    }
}


static void source_chip_cb(lv_event_t *e) {
    (void)e;
    if (wireless_screen.pill_app >= 0) {
        nav_open_app((ui_app_t)wireless_screen.pill_app);
    } else if (s_current_source == UI_SOURCE_RADIO) {
        nav_open_app(UI_APP_RADIO);
    } else {
        switch_open();
    }
}

static void wireless_loaded_cb(lv_event_t *e);

static void wireless_screen_build(void) {
    wireless_screen.screen = ui_screen_create();
    lv_obj_add_event_cb(wireless_screen.screen, wireless_loaded_cb, LV_EVENT_SCREEN_LOAD_START, NULL);
    ui_now_playing_bg_create(&wireless_screen.bg, wireless_screen.screen, UI_BG_WIRELESS, UI_TINT_AIRPLAY,
                             NULL);

    // Y is set per-update (WIRELESS_STATUS_ROW_Y_CONNECTED vs. _WAITING) --
    // the mock's flex column re-centers around whichever block is showing.
    lv_obj_t *row = ui_status_pill_create(wireless_screen.screen, source_chip_cb);
    wireless_screen.source_icon =
        ui_icon_create(row, &icon_airplay, UI_STATUS_ICON_SIZE, lv_color_hex(UI_COLOR_TEXT));
    wireless_screen.pill_glyph = lv_label_create(row);
    lv_obj_set_style_text_font(wireless_screen.pill_glyph, UI_FONT_ICON_SM, 0);
    lv_obj_add_flag(wireless_screen.pill_glyph, LV_OBJ_FLAG_HIDDEN);
    wireless_screen.pill_app = -1;
    wireless_screen.source_label = ui_status_label_create(row);

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
    wireless_screen.wait_icon = ui_icon_create(wireless_screen.wait_chip, &icon_airplay, WIRELESS_WAIT_ICON_SIZE,
                                               lv_color_hex(UI_COLOR_TEXT_DIM));
    lv_obj_center(wireless_screen.wait_icon);

    wireless_screen.wait_title = lv_label_create(wireless_screen.screen);
    lv_obj_set_style_text_font(wireless_screen.wait_title, UI_FONT_TITLE, 0);
    lv_obj_set_style_text_color(wireless_screen.wait_title, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_align(wireless_screen.wait_title, LV_ALIGN_CENTER, 0, WIRELESS_WAIT_TITLE_Y);

    // The longest hint doesn't fit in one line, and an ellipsis would cut it.
    wireless_screen.wait_subtitle =
        ui_line_label_create(wireless_screen.screen, UI_FONT_BODY, lv_color_hex(UI_COLOR_TEXT_DIM));
    lv_label_set_long_mode(wireless_screen.wait_subtitle, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_height(wireless_screen.wait_subtitle, LV_SIZE_CONTENT);
    lv_obj_align(wireless_screen.wait_subtitle, LV_ALIGN_CENTER, 0, WIRELESS_WAIT_SUBTITLE_Y);

    // -- connected / now-playing state --
    wireless_screen.tile_icon = lv_image_create(wireless_screen.screen);
    lv_image_set_src(wireless_screen.tile_icon, &icon_music_note);
    lv_obj_set_style_image_recolor(wireless_screen.tile_icon, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_image_recolor_opa(wireless_screen.tile_icon, LV_OPA_COVER, 0);
    lv_obj_align(wireless_screen.tile_icon, LV_ALIGN_CENTER, 0, WIRELESS_TILE_Y);
    wireless_screen.art = ui_now_playing_art_create(&wireless_screen.bg, wireless_screen.screen, WIRELESS_TILE_Y);

    wireless_screen.transport = ui_flex_row_create(wireless_screen.screen, TRANSPORT_GAP, WIRELESS_TRANSPORT_Y);
    ui_touch_pad(wireless_screen.transport, UI_TOUCH_EXT / 2);
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

    wireless_screen.title_label =
        ui_line_label_create(wireless_screen.screen, UI_FONT_TITLE, lv_color_hex(UI_COLOR_TEXT));
    lv_obj_align(wireless_screen.title_label, LV_ALIGN_CENTER, 0, WIRELESS_TITLE_Y);

    // .np-subtitle is 12.5px in the mock; LVGL's bundled Montserrat only ships
    // even sizes, so 12 is the closer of the two neighbors (vs. 14).
    wireless_screen.subtitle_label =
        ui_line_label_create(wireless_screen.screen, UI_FONT_BODY, lv_color_hex(UI_COLOR_TEXT_DIM));
    lv_obj_align(wireless_screen.subtitle_label, LV_ALIGN_CENTER, 0, WIRELESS_SUBTITLE_Y);

    ui_home_zone_create(wireless_screen.screen, nav_swipe_home_cb);

    ui_track_info_t no_track = {0};
    wireless_state_t idle_state = {.volume = 0, .muted = 0, .connected = 0, .playing = 0, .source = SOURCE_AIRPLAY};
    ui_update_wireless(&idle_state, &no_track);
}

#ifdef AK_USB_AUDIO
static void usb_screen_build(void) {
    usb_screen.screen = ui_screen_create();
    ui_now_playing_bg_create(&usb_screen.bg, usb_screen.screen, UI_BG_USB, UI_TINT_USB, NULL);

    lv_obj_t *row = ui_status_pill_create(usb_screen.screen, NULL);
    ui_chip_align_top(row);
    lv_obj_t *icon = lv_label_create(row);
    lv_label_set_text(icon, LV_SYMBOL_USB);
    lv_obj_set_style_text_font(icon, UI_FONT_SYMBOL_SM, 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(UI_TINT_USB), 0);
    lv_obj_t *label = ui_status_label_create(row);
    lv_label_set_text(label, "USB Audio");

    // The art tile shows a generic speaker glyph, not the USB plug glyph --
    // that one's reserved for the status row above.
    lv_obj_t *tile_icon = lv_label_create(usb_screen.screen);
    lv_label_set_text(tile_icon, LV_SYMBOL_VOLUME_MAX);
    lv_obj_set_style_text_font(tile_icon, UI_FONT_SYMBOL_XL, 0);
    lv_obj_set_style_text_color(tile_icon, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_align(tile_icon, LV_ALIGN_CENTER, 0, USB_TILE_Y);

    lv_obj_t *transport = ui_flex_row_create(usb_screen.screen, TRANSPORT_GAP, USB_TRANSPORT_Y);
    ui_touch_pad(transport, UI_TOUCH_EXT / 2);
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

    ui_home_zone_create(usb_screen.screen, nav_swipe_home_cb);

    ui_update_usb(0, false, false);
}
#endif

void np_build(void) {
    wireless_screen_build();
#ifdef AK_USB_AUDIO
    usb_screen_build();
#endif
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

// Connected, the pill gives way to Talk or a running tool; else
// it's the time and the source. Waiting, it names the source alone.
static void wireless_pill_refresh(bool waiting) {
    band_item_t item;
    bool other = !waiting && band_item(&item);
    for (int i = 0; !waiting && !other && i < UI_APP_COUNT; i++) {
        other = !band_owns((ui_app_t)i) && band_app_item((ui_app_t)i, &item);
    }
    wireless_screen.pill_app = other ? item.app : -1;
    ui_set_visible(wireless_screen.source_icon, !other);
    ui_set_visible(wireless_screen.pill_glyph, other);
    if (other) {
        ui_label_set_text(wireless_screen.pill_glyph, item.icon);
        ui_set_text_color(wireless_screen.pill_glyph, item.tint);
        ui_label_set_text(wireless_screen.source_label, item.text);
        return;
    }
    const char *label = k_sources[s_current_source].label;
    const char *time = home_time_text();
    char buf[32];
    if (!waiting && time[0]) {
        snprintf(buf, sizeof(buf), "%s \xC2\xB7 %s", time, label);
        label = buf;
    }
    ui_label_set_text(wireless_screen.source_label, label);
}

void np_pill_refresh(void) {
    wireless_pill_refresh(!s_audio.wireless_connected);
}

// The last state pushed, drawn when the screen shows: hidden, its widgets
// would only be restyled and invalidated for nothing on every poll.
static struct {
    wireless_state_t state;
    ui_track_info_t track;
    bool has_track_info;
    bool drawn;
} s_last;

static void wireless_render(void) {
    const wireless_state_t *state = &s_last.state;
    const ui_track_info_t *track = s_last.has_track_info ? &s_last.track : NULL;
    ui_source_t source = s_current_source;
    lv_color_t tint = lv_color_hex(k_sources[source].tint);
    s_last.drawn = true;

    ui_image_set_src(wireless_screen.source_icon, k_sources[source].icon);
    ui_set_image_recolor(wireless_screen.source_icon, tint);
    // A station has no art; its tile carries the radio glyph instead.
    const lv_image_dsc_t *tile = source == UI_SOURCE_RADIO ? &icon_radio : &icon_music_note;
    ui_image_set_src(wireless_screen.tile_icon, tile);

    // Unconnected, there's no tile/track to show yet, nor a volume.
    bool waiting = !state->connected;
    wireless_pill_refresh(waiting);
    uint32_t art_tint = track && track->art && ui_settings_current()->art_colors ? track->art_tint : 0;
    ui_now_playing_bg_set_tint(&wireless_screen.bg, art_tint ? art_tint : k_sources[source].tint,
                               !waiting && lv_screen_active() == wireless_screen.screen);
    lv_obj_set_flag(wireless_screen.wait_chip, LV_OBJ_FLAG_CLICKABLE, wireless_screen.bluetooth);
    lv_obj_t *status_row = lv_obj_get_parent(wireless_screen.source_icon);
    // Connected, it's a chip like any other; waiting, it heads the centered
    // block below it.
    if (waiting) {
        lv_obj_align(status_row, LV_ALIGN_CENTER, 0, WIRELESS_STATUS_ROW_Y_WAITING);
    } else {
        ui_chip_align_top(status_row);
    }

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
    const char *source_name = k_sources[source].label;
    const char *peer = track ? track->peer : "";
    // With no track, what's connected is the next best title.
    const char *idle_title = peer[0] ? peer : source_name;

    if (waiting) {
        ui_image_set_src(wireless_screen.wait_icon, k_sources[source].icon);
        ui_label_set_text(wireless_screen.wait_title, k_sources[source].wait_title);
        ui_label_set_text(wireless_screen.wait_subtitle, k_sources[source].wait_hint);
    } else {
        ui_label_set_text(wireless_screen.play_icon,
                           state->muted ? LV_SYMBOL_MUTE : (state->playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY));
        ui_set_text_color(wireless_screen.play_icon, lv_color_hex(state->muted ? UI_COLOR_DANGER : UI_COLOR_TEXT));
        ui_label_set_text(wireless_screen.title_label, has_track ? track->title : idle_title);
        ui_label_set_text(wireless_screen.subtitle_label, subtitle);
    }
    ui_set_visible(wireless_screen.tile_icon, !waiting && !wireless_screen.art_version);
    ui_set_visible(wireless_screen.art, !waiting && wireless_screen.art_version);
}

static void wireless_loaded_cb(lv_event_t *e) {
    (void)e;
    if (!s_last.drawn) {
        wireless_render();
    }
}

void ui_update_wireless(const wireless_state_t *state, const ui_track_info_t *track) {
    static nav_surface_t surface;
    ui_source_t source = source_of(state->source);
    s_current_source = source;
    wireless_screen.bluetooth = source == UI_SOURCE_BLUETOOTH;

    bool changed = memcmp(&s_last.state, state, sizeof(*state)) != 0 || s_last.has_track_info != (track != NULL) ||
                   (track && memcmp(&s_last.track, track, sizeof(*track)) != 0);
    if (changed) {
        s_last.state = *state;
        s_last.has_track_info = track != NULL;
        if (track) {
            s_last.track = *track;
        }
        s_last.drawn = false;
    }
    // Over every screen, the home screen's too: the knob there turns this.
    uint32_t art_tint = track && track->art && ui_settings_current()->art_colors ? track->art_tint : 0;
    volume_ring_set_tint(art_tint ? art_tint : k_sources[source].tint);
    volume_ring_update(source, state->connected, state->volume);
    // The clock's card shows the art too, so it's taken in whatever shows.
    if (state->connected) {
        update_artwork(track);
    }
    if (!s_last.drawn && lv_screen_active() == wireless_screen.screen) {
        wireless_render();
    }

    bool has_track = track && track->title[0];
    const char *peer = track ? track->peer : "";
    const char *idle_title = peer[0] ? peer : k_sources[source].label;
    s_audio.wireless_connected = state->connected;
    s_audio.wireless_playing = state->playing;
    s_audio.wireless_has_track = has_track;
    s_audio.wireless_source = source;
    snprintf(s_audio.wireless_peer, sizeof(s_audio.wireless_peer), "%s", peer);
    snprintf(s_audio.wireless_text, sizeof(s_audio.wireless_text), "%s", has_track ? track->title : idle_title);
    snprintf(s_audio.wireless_subtitle, sizeof(s_audio.wireless_subtitle), "%s",
             !has_track ? "" : track->artist[0] ? track->artist : track->album);
    nav_surface(wireless_screen.screen, state->playing, state->connected, &surface);
    audio_chip_refresh();
}

#ifdef AK_USB_AUDIO
void ui_update_usb(uint8_t volume, bool muted, bool playing) {
    static nav_surface_t surface;
    // Any key apart from the wireless sources' ui_source_t values.
    volume_ring_update(-2, true, volume);
    volume_ring_set_tint(UI_TINT_USB);

    ui_label_set_text(usb_screen.play_icon, muted ? LV_SYMBOL_MUTE : (playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY));
    ui_set_text_color(usb_screen.play_icon, lv_color_hex(muted ? UI_COLOR_DANGER : UI_COLOR_TEXT));

    s_audio.usb_playing = playing;
    nav_surface(usb_screen.screen, playing, false, &surface);
    audio_chip_refresh();
}
#endif
