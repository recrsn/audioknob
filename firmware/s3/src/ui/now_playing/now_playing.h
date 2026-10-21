#pragma once

// The now-playing screens (wireless: AirPlay, Bluetooth, radio; USB when
// built with AK_USB_AUDIO) and what's playing, as last pushed through
// ui_update_wireless()/ui_update_usb(), for the clock's card, the audio
// chip and the launcher.

#include "ui/internal.h"

#define NP_SOURCE_COUNT 3

// How each source looks wherever it's named: the switch screen, the
// now-playing screen, the clock's row and the audio chip.
typedef struct {
    const char *label;
    const lv_image_dsc_t *icon;
    uint32_t tint;
    const char *wait_title;
    const char *wait_hint;
} np_source_info_t;

const np_source_info_t *np_source(ui_source_t source);

typedef struct {
    bool wireless_connected;
    bool wireless_playing;
    bool wireless_has_track;
#ifdef AK_USB_AUDIO
    bool usb_playing;
#endif
    ui_source_t wireless_source;
    char wireless_text[64];
    char wireless_subtitle[64];
    char wireless_peer[64];
} np_audio_t;

const np_audio_t *np_audio(void);

void np_build(void);
lv_obj_t *np_wireless_screen(void);
#ifdef AK_USB_AUDIO
lv_obj_t *np_usb_screen(void);
#endif
// One of the now-playing screens.
bool np_is_screen(lv_obj_t *screen);
// Where "what's playing" leads: a source's screen, the ambience's page, or
// NULL with nothing connected.
lv_obj_t *np_audio_screen(void);
// Ambience has its page, the Pomodoro's or its own, for a screen.
bool np_is_ambience(lv_obj_t *target);
// The launcher's audio entry line; false with nothing to show.
bool np_status_text(char *buf, size_t len);

// The source the wireless screen shows, as last pushed or picked.
ui_source_t np_current_source(void);
void np_set_current_source(ui_source_t source);

// The wireless screen's copy of the cover art; version 0 with none.
const lv_image_dsc_t *np_art(uint32_t *version);
void np_transport(ui_transport_t button);
// The wireless screen's top pill, polled while it shows.
void np_pill_refresh(void);
