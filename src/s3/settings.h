#pragma once

#include <stdbool.h>
#include <stdint.h>

// User settings, persisted in NVS namespace "settings". Loading also brings
// up NVS, so it may run before wifi_start().

typedef struct {
    uint8_t brightness; // backlight, 5-100 %
    bool clock_24h;
    bool fahrenheit;
    bool haptics;
    char tz[40]; // manual IANA zone; "" follows the IP-detected one
} settings_t;

void settings_load(settings_t *out);
void settings_save(const settings_t *s);
