#include "now_playing.h"

#include <string.h>

#include "airplay.h"
#include "audio_source.h"
#include "uart_link.h"

typedef enum { PICK_NONE, PICK_AIRPLAY, PICK_BLUETOOTH } source_pick_t;
static volatile source_pick_t s_pick = PICK_NONE;
static audio_source_t s_pick_dac;

bool now_playing_airplay_in_front(void) {
    if (s_pick != PICK_NONE) {
        if (audio_source_active() == s_pick_dac) {
            return s_pick == PICK_AIRPLAY;
        }
        s_pick = PICK_NONE;
    }
    if (audio_source_active() == AUDIO_SOURCE_AIRPLAY) {
        return true;
    }
    wireless_state_t esp32;
    return airplay_is_connected() && !(uart_link_get_latest(&esp32) && esp32.playing);
}

bool now_playing_get(wireless_state_t *state, track_info_t *track) {
    memset(track, 0, sizeof(*track));
    if (now_playing_airplay_in_front()) {
        airplay_get_state(state, track);
        return true;
    }
    if (!uart_link_get_latest(state)) {
        return false;
    }
    // Nothing has played since boot when this fails; the zeroed track then
    // shows as "Not playing".
    uart_link_get_latest_track(track);
    return true;
}

const uint16_t *now_playing_artwork(uint32_t *version) {
    return now_playing_airplay_in_front() ? airplay_get_artwork(version) : uart_link_get_artwork(version);
}

bool now_playing_adjust_volume(int steps) {
    if (now_playing_airplay_in_front()) {
        airplay_adjust_volume(steps);
        return true;
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
    wireless_state_t state;
    track_info_t track;
    if (!now_playing_get(&state, &track)) {
        return false;
    }
    int steps = (int)(pct > 100 ? 100 : pct) - state.volume;
    return steps == 0 || now_playing_adjust_volume(steps);
}

void now_playing_transport(ui_transport_t button) {
    if (now_playing_airplay_in_front()) {
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
    if (source == UI_SOURCE_AIRPLAY) {
        audio_source_claim_airplay();
        s_pick = PICK_AIRPLAY;
    } else {
        airplay_preempt();
        audio_source_release_airplay();
        s_pick = PICK_BLUETOOTH;
    }
    s_pick_dac = audio_source_active();
}
