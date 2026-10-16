#pragma once

#include <stdbool.h>
#include <stdint.h>

// User settings, persisted in NVS namespace "settings". Loading also brings
// up NVS, so it may run before wifi_start().

typedef struct {
    uint8_t brightness; // backlight, 5-100 %
    uint16_t sleep_s;   // display sleep after this long idle; 0 = never
    bool clock_24h;
    bool fahrenheit;
    bool haptics;
    bool art_colors;
    uint8_t timer_sound;  // UI_TIMER_SOUND_*
    uint8_t timer_volume; // UI_TIMER_VOLUME_MIN-100 %
    bool bluetooth;
    uint8_t airplay; // UI_AIRPLAY_* mask; 0 = off
    uint8_t air_show; // ui_air_show_t
    uint8_t ambience_sound;  // ui_ambience_t
    uint8_t ambience_volume; // 0-100
    uint8_t focus_sound;     // ui_ambience_t + 1; 0 = none
    char tz[40]; // manual IANA zone; "" follows the IP-detected one
} settings_t;

// Brings up NVS for anything that reads it before settings_load().
void settings_nvs_init(void);

void settings_load(settings_t *out);
void settings_save(const settings_t *s);
