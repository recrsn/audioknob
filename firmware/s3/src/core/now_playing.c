#include "core/now_playing.h"

#include <string.h>

#include "airplay/airplay.h"
#include "audio/source.h"
#include "freertos/FreeRTOS.h"
#include "radio/radio.h"
#include "talk/talk.h"
#include "drivers/uart_link.h"

typedef enum { PICK_NONE, PICK_AIRPLAY, PICK_BLUETOOTH } source_pick_t;
// Read from the LVGL, knob and web server tasks; the pair changes together.
static portMUX_TYPE s_pick_lock = portMUX_INITIALIZER_UNLOCKED;
static source_pick_t s_pick = PICK_NONE;
static audio_source_t s_pick_dac;

now_playing_front_t now_playing_front(void) {
    audio_source_t active = audio_source_active();
    taskENTER_CRITICAL(&s_pick_lock);
    if (s_pick != PICK_NONE && active != s_pick_dac) {
        s_pick = PICK_NONE;
    }
    source_pick_t pick = s_pick;
    taskEXIT_CRITICAL(&s_pick_lock);
    if (pick != PICK_NONE) {
        return pick == PICK_AIRPLAY ? NOW_PLAYING_AIRPLAY : NOW_PLAYING_BLUETOOTH;
    }
    switch (active) {
    case AUDIO_SOURCE_AIRPLAY:
        return NOW_PLAYING_AIRPLAY;
    case AUDIO_SOURCE_RADIO:
        return NOW_PLAYING_RADIO;
    case AUDIO_SOURCE_ESP32:
    case AUDIO_SOURCE_LOCAL:
    case AUDIO_SOURCE_TALK:
    case AUDIO_SOURCE_NOTE:
        break;
    }
    wireless_state_t esp32;
    if (uart_link_get_latest(&esp32) && esp32.playing) {
        return NOW_PLAYING_BLUETOOTH;
    }
    // Nothing plays: an open AirPlay session, else a stopped station (A2DP
    // playing since would have dropped it), else the phone.
    if (airplay_is_connected()) {
        return NOW_PLAYING_AIRPLAY;
    }
    return radio_is_loaded() ? NOW_PLAYING_RADIO : NOW_PLAYING_BLUETOOTH;
}

bool now_playing_airplay_in_front(void) {
    return now_playing_front() == NOW_PLAYING_AIRPLAY;
}

bool now_playing_get(wireless_state_t *state, track_info_t *track) {
    memset(track, 0, sizeof(*track));
    switch (now_playing_front()) {
    case NOW_PLAYING_AIRPLAY:
        airplay_get_state(state, track);
        return true;
    case NOW_PLAYING_RADIO:
        radio_get_state(state, track);
        return true;
    case NOW_PLAYING_BLUETOOTH:
        break;
    }
    if (!uart_link_get_latest(state)) {
        return false;
    }
    // Nothing has played since boot when this fails; the zeroed track then
    // shows as "Not playing".
    uart_link_get_latest_track(track);
    return true;
}

void now_playing_peer_name(char *out, size_t len) {
    switch (now_playing_front()) {
    case NOW_PLAYING_AIRPLAY:
        airplay_get_client_name(out, len);
        break;
    case NOW_PLAYING_RADIO:
        radio_get_station_name(out, len);
        break;
    case NOW_PLAYING_BLUETOOTH:
        uart_link_get_peer_name(out, len);
        break;
    }
}

const uint16_t *now_playing_artwork(uint32_t *version) {
    switch (now_playing_front()) {
    case NOW_PLAYING_AIRPLAY:
        return airplay_get_artwork(version);
    case NOW_PLAYING_RADIO:
        *version = 0;
        return NULL;
    case NOW_PLAYING_BLUETOOTH:
        break;
    }
    return uart_link_get_artwork(version);
}

static bool voice_in_front(void) {
    audio_source_t source = audio_source_active();
    return source == AUDIO_SOURCE_TALK || source == AUDIO_SOURCE_NOTE;
}

bool now_playing_adjust_volume(int steps) {
    if (voice_in_front()) {
        ui_talk_t voice;
        talk_get_state(&voice);
        int volume = (int)voice.volume + steps;
        talk_set_volume(volume < 0 ? 0 : volume > 100 ? 100 : volume);
        return true;
    }
    switch (now_playing_front()) {
    case NOW_PLAYING_AIRPLAY:
        airplay_adjust_volume(steps);
        return true;
    case NOW_PLAYING_RADIO:
        radio_adjust_volume(steps);
        return true;
    case NOW_PLAYING_BLUETOOTH:
        break;
    }
    wireless_state_t esp32;
    if (!uart_link_get_latest(&esp32) || !esp32.connected) {
        return false;
    }
    // A detent at a time, so Bluetooth moves 1% per step just like AirPlay.
    protocol_cmd_t cmd = steps > 0 ? CMD_VOLUME_UP : CMD_VOLUME_DOWN;
    for (int i = steps > 0 ? steps : -steps; i > 0; i--) {
        uart_link_send_command(cmd);
    }
    return true;
}

bool now_playing_set_volume(uint8_t pct) {
    pct = pct > 100 ? 100 : pct;
    if (voice_in_front()) { talk_set_volume(pct); return true; }
    wireless_state_t state;
    track_info_t track;
    now_playing_front_t front = now_playing_front();
    if (front != NOW_PLAYING_BLUETOOTH) {
        if (front == NOW_PLAYING_AIRPLAY) {
            airplay_get_state(&state, &track);
        } else {
            radio_get_state(&state, &track);
        }
        int steps = (int)pct - state.volume;
        return steps == 0 || now_playing_adjust_volume(steps);
    }
    if (!uart_link_get_latest(&state) || !state.connected) {
        return false;
    }
    uart_link_send_command_value(CMD_VOLUME_SET, pct);
    return true;
}

void now_playing_transport(ui_transport_t button) {
    now_playing_front_t front = now_playing_front();
    if (front == NOW_PLAYING_RADIO) {
        switch (button) {
        case UI_TRANSPORT_PREV:
            radio_prev();
            break;
        case UI_TRANSPORT_PLAY_PAUSE:
            radio_toggle_play_pause();
            break;
        case UI_TRANSPORT_NEXT:
            radio_next();
            break;
        }
        return;
    }
    if (front == NOW_PLAYING_AIRPLAY) {
        switch (button) {
        case UI_TRANSPORT_PREV:
            airplay_prev();
            break;
        case UI_TRANSPORT_PLAY_PAUSE:
            airplay_toggle_play_pause();
            break;
        case UI_TRANSPORT_NEXT:
            airplay_next();
            break;
        }
        return;
    }
    static const protocol_cmd_t cmds[] = {[UI_TRANSPORT_PREV] = CMD_PREV,
                                          [UI_TRANSPORT_PLAY_PAUSE] = CMD_PLAY_PAUSE,
                                          [UI_TRANSPORT_NEXT] = CMD_NEXT};
    uart_link_send_command(cmds[button]);
}

void now_playing_pick(ui_source_t source) {
    if (audio_source_suspended()) {
        return;
    }
    switch (source) {
    case UI_SOURCE_AIRPLAY:
        audio_source_claim(AUDIO_SOURCE_AIRPLAY);
        break;
    case UI_SOURCE_BLUETOOTH:
        // Stops whichever of this MCU's sources had the DAC.
        audio_source_claim(AUDIO_SOURCE_ESP32);
        break;
    case UI_SOURCE_RADIO:
        // Not offered: playing a station is what brings the radio in front.
        return;
    }
    audio_source_t active = audio_source_active();
    taskENTER_CRITICAL(&s_pick_lock);
    s_pick = source == UI_SOURCE_AIRPLAY ? PICK_AIRPLAY : PICK_BLUETOOTH;
    s_pick_dac = active;
    taskEXIT_CRITICAL(&s_pick_lock);
}
