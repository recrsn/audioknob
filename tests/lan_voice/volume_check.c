#include "runtime.h"
#include "now_playing.c"
static audio_source_t owner;
static uint8_t volume = 70;
static int old_source_changes;
audio_source_t audio_source_active(void) {
    return owner;
}
void talk_get_state(ui_talk_t *out) {
    memset(out, 0, sizeof(*out));
    out->volume = volume;
}
void talk_set_volume(uint8_t value) {
    volume = value;
}
bool uart_link_get_latest(wireless_state_t *state) {
    memset(state, 0, sizeof(*state));
    return true;
}
void uart_link_send_command(protocol_cmd_t command) {
    (void)command;
    old_source_changes++;
}
void uart_link_send_command_value(protocol_cmd_t command, uint8_t value) {
    (void)command;
    (void)value;
    old_source_changes++;
}
bool airplay_is_connected(void) {
    return false;
}
bool radio_is_loaded(void) {
    return false;
}
void airplay_adjust_volume(int steps) {
    (void)steps;
    old_source_changes++;
}
void radio_adjust_volume(int steps) {
    (void)steps;
    old_source_changes++;
}
void airplay_get_state(wireless_state_t *state, track_info_t *track) {
    memset(state, 0, sizeof(*state));
    memset(track, 0, sizeof(*track));
}
void radio_get_state(wireless_state_t *state, track_info_t *track) {
    airplay_get_state(state, track);
}
int main(void) {
    owner = AUDIO_SOURCE_TALK;
    assert(now_playing_adjust_volume(5) && volume == 75);
    assert(now_playing_adjust_volume(100) && volume == 100);
    owner = AUDIO_SOURCE_NOTE;
    assert(now_playing_set_volume(20) && volume == 20);
    assert(now_playing_adjust_volume(-100) && volume == 0 && old_source_changes == 0);
    owner = AUDIO_SOURCE_ESP32;
    assert(!now_playing_adjust_volume(1) && old_source_changes == 0);
    puts("Background Talk and voice-note knob/web volume routing passed");
}
