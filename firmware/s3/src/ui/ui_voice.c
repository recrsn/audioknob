#include "ui_internal.h"
#include <string.h>
static ui_voice_t s_voice;
static void (*s_record)(uint32_t, bool), (*s_play)(const char *), (*s_stop)(void);
void ui_set_voice_handler(void (*record)(uint32_t, bool), void (*play)(const char *), void (*stop)(void)) {
    s_record = record;
    s_play = play;
    s_stop = stop;
}
const ui_voice_t *ui_voice_current(void) {
    return &s_voice;
}
void ui_voice_record(uint32_t peer, bool held) {
    if (s_record)
        s_record(peer, held);
}
void ui_voice_play(const char *id) {
    if (s_play)
        s_play(id);
}
void ui_voice_stop(void) {
    if (s_stop)
        s_stop();
}
void ui_update_voice(const ui_voice_t *voice) {
    s_voice = *voice;
    ui_inbox_voice_changed();
    ui_talk_voice_changed();
}
