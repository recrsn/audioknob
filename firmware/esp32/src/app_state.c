#include "app_state.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static wireless_state_t s_state = {.volume = 50, .source = SOURCE_A2DP};
static char s_peer_name[PEER_NAME_MAX + 1];
static SemaphoreHandle_t s_lock;

void app_state_init(void) {
    s_lock = xSemaphoreCreateMutex();
}

void app_state_set_connected(bool connected) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state.connected = connected;
    if (!connected) {
        s_state.playing = false;
    }
    xSemaphoreGive(s_lock);
}

void app_state_set_playing(bool playing) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state.playing = playing;
    xSemaphoreGive(s_lock);
}

void app_state_set_volume(uint8_t volume) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state.volume = volume;
    xSemaphoreGive(s_lock);
}

void app_state_set_muted(bool muted) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state.muted = muted;
    xSemaphoreGive(s_lock);
}

void app_state_set_peer_name(const char *name) {
    size_t n = strnlen(name, PEER_NAME_MAX + 1);
    if (n > PEER_NAME_MAX) {
        n = PEER_NAME_MAX;
        // Back off to the start of the character the cut fell inside.
        while (n && ((uint8_t)name[n] & 0xC0) == 0x80) {
            n--;
        }
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(s_peer_name, name, n);
    s_peer_name[n] = '\0';
    xSemaphoreGive(s_lock);
}

void app_state_get_peer_name(char out[PEER_NAME_MAX + 1]) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(out, s_peer_name, PEER_NAME_MAX + 1);
    xSemaphoreGive(s_lock);
}

wireless_state_t app_state_get(void) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    wireless_state_t snapshot = s_state;
    xSemaphoreGive(s_lock);
    return snapshot;
}
