#include <assert.h>
#include <stdio.h>
#include "audio_source.c"
static wireless_state_t peer;
static int pauses, stops[8], local_stops;
bool uart_link_get_latest(wireless_state_t *s) {
    *s = peer;
    return true;
}
void uart_link_send_command(protocol_cmd_t c) {
    assert(c == CMD_PAUSE);
    pauses++;
}
static void preempt(audio_source_t s) {
    stops[s]++;
}
static void local_preempt(void) {
    local_stops++;
}
int main(void) {
    audio_source_init(preempt);
    audio_source_set_local_preempted(local_preempt);
    assert(audio_source_claim_local());
    assert(gpio_level == 1);
    assert(audio_source_claim(AUDIO_SOURCE_TALK));
    assert(local_stops == 1);
    assert(audio_source_claim(AUDIO_SOURCE_NOTE));
    assert(stops[AUDIO_SOURCE_TALK] == 1);
    assert(audio_source_claim(AUDIO_SOURCE_AIRPLAY));
    assert(stops[AUDIO_SOURCE_NOTE] == 1);
    assert(audio_source_claim(AUDIO_SOURCE_TALK));
    assert(stops[AUDIO_SOURCE_AIRPLAY] == 1);
    audio_source_release(AUDIO_SOURCE_NOTE);
    assert(audio_source_active() == AUDIO_SOURCE_TALK);
    peer.connected = peer.playing = 1;
    audio_source_suspend(true);
    assert(gpio_level == 1 && pauses == 1 && stops[AUDIO_SOURCE_TALK] == 2 && stops[AUDIO_SOURCE_NOTE] == 2);
    for (int source = AUDIO_SOURCE_ESP32; source <= AUDIO_SOURCE_TALK; source++)
        assert(!audio_source_claim(source));
    audio_source_release(AUDIO_SOURCE_TALK);
    assert(gpio_level == 1);
    esp32_watch();
    assert(pauses == 2 && gpio_level == 1);
    audio_source_suspend(false);
    assert(gpio_level == 0);
    assert(audio_source_claim(AUDIO_SOURCE_NOTE));
    assert(gpio_level == 1);
    audio_source_release(AUDIO_SOURCE_NOTE);
    assert(gpio_level == 0);
    puts("Talk/voice-note source handovers and OTA suspension passed");
}
