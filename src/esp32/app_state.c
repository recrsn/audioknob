#include "app_state.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static wireless_state_t s_state = {.volume = 50, .source = SOURCE_A2DP};
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

wireless_state_t app_state_get(void) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    wireless_state_t snapshot = s_state;
    xSemaphoreGive(s_lock);
    return snapshot;
}
