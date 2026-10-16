#include "settings.h"

#include <string.h>

#include "esp_log.h"
#include "nvs_flash.h"
#include "ui.h"

#define NVS_NAMESPACE "settings"

static const char *TAG = "settings";

static const settings_t k_defaults = {
    .brightness = CONFIG_AK_BACKLIGHT_DEFAULT_PCT,
    .sleep_s = 300,
    .clock_24h = true,
    .fahrenheit = false,
    .haptics = true,
    .art_colors = true,
    .timer_sound = UI_TIMER_SOUND_CHIME,
    .timer_volume = 60,
    .bluetooth = true,
    .airplay = UI_AIRPLAY_1 | UI_AIRPLAY_2,
    .air_show = UI_AIR_SHOW_HIGH,
    .ambience_sound = UI_AMBIENCE_RAIN,
    .ambience_volume = 30,
    .focus_sound = 0,
    .tz = "",
};

static uint8_t get_u8(nvs_handle_t nvs, const char *key, uint8_t fallback) {
    uint8_t v;
    return nvs_get_u8(nvs, key, &v) == ESP_OK ? v : fallback;
}

void settings_nvs_init(void) {
    // Same recovery wifi.c applies; a second nvs_flash_init() is a no-op.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

void settings_load(settings_t *out) {
    *out = k_defaults;
    settings_nvs_init();

    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    out->brightness = get_u8(nvs, "brightness", k_defaults.brightness);
    if (nvs_get_u16(nvs, "sleep_s", &out->sleep_s) != ESP_OK) {
        out->sleep_s = k_defaults.sleep_s;
    }
    out->clock_24h = get_u8(nvs, "clock_24h", k_defaults.clock_24h);
    out->fahrenheit = get_u8(nvs, "fahrenheit", k_defaults.fahrenheit);
    out->haptics = get_u8(nvs, "haptics", k_defaults.haptics);
    out->art_colors = get_u8(nvs, "art_colors", k_defaults.art_colors);
    out->timer_sound = get_u8(nvs, "timer_sound", k_defaults.timer_sound);
    if (out->timer_sound >= UI_TIMER_SOUND_COUNT) {
        out->timer_sound = k_defaults.timer_sound;
    }
    out->timer_volume = get_u8(nvs, "timer_volume", k_defaults.timer_volume);
    if (out->timer_volume < UI_TIMER_VOLUME_MIN || out->timer_volume > 100) {
        out->timer_volume = k_defaults.timer_volume;
    }
    out->bluetooth = get_u8(nvs, "bluetooth", k_defaults.bluetooth);
    out->airplay = get_u8(nvs, "airplay", k_defaults.airplay);
    out->air_show = get_u8(nvs, "air_show", k_defaults.air_show);
    out->ambience_sound = get_u8(nvs, "amb_sound", k_defaults.ambience_sound);
    out->ambience_volume = get_u8(nvs, "amb_volume", k_defaults.ambience_volume);
    out->focus_sound = get_u8(nvs, "focus_sound", k_defaults.focus_sound);
    if (out->ambience_sound >= UI_AMBIENCE_COUNT) {
        out->ambience_sound = k_defaults.ambience_sound;
    }
    if (out->focus_sound > UI_AMBIENCE_COUNT) {
        out->focus_sound = k_defaults.focus_sound;
    }
    size_t len = sizeof(out->tz);
    if (nvs_get_str(nvs, "tz", out->tz, &len) != ESP_OK) {
        out->tz[0] = '\0';
    }
    nvs_close(nvs);
}

void settings_save(const settings_t *s) {
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        ESP_LOGW(TAG, "can't open NVS");
        return;
    }
    nvs_set_u8(nvs, "brightness", s->brightness);
    nvs_set_u16(nvs, "sleep_s", s->sleep_s);
    nvs_set_u8(nvs, "clock_24h", s->clock_24h);
    nvs_set_u8(nvs, "fahrenheit", s->fahrenheit);
    nvs_set_u8(nvs, "haptics", s->haptics);
    nvs_set_u8(nvs, "art_colors", s->art_colors);
    nvs_set_u8(nvs, "timer_sound", s->timer_sound);
    nvs_set_u8(nvs, "timer_volume", s->timer_volume);
    nvs_set_u8(nvs, "bluetooth", s->bluetooth);
    nvs_set_u8(nvs, "airplay", s->airplay);
    nvs_set_u8(nvs, "air_show", s->air_show);
    nvs_set_u8(nvs, "amb_sound", s->ambience_sound);
    nvs_set_u8(nvs, "amb_volume", s->ambience_volume);
    nvs_set_u8(nvs, "focus_sound", s->focus_sound);
    nvs_set_str(nvs, "tz", s->tz);
    nvs_commit(nvs);
    nvs_close(nvs);
}
