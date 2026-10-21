#include "ui/apps/lights/lights.h"

#include <string.h>

// In-memory stand-in for the WiZ backend (requested as a mock for now):
// three bulbs that take every change instantly.

static light_t s_lights[] = {
    {.name = "Desk lamp", .reachable = true, .on = true, .brightness = 70},
    {.name = "Ceiling", .reachable = true, .on = false, .brightness = 100},
    {.name = "Floor lamp", .reachable = true, .on = true, .brightness = 35},
};

#define LIGHT_COUNT ((int)(sizeof(s_lights) / sizeof(s_lights[0])))

int lights_get(light_t *out, int max) {
    int n = max < LIGHT_COUNT ? max : LIGHT_COUNT;
    memcpy(out, s_lights, sizeof(s_lights[0]) * n);
    return n;
}

void lights_set_power(int index, bool on) {
    if (index >= 0 && index < LIGHT_COUNT) {
        s_lights[index].on = on;
    }
}

void lights_set_brightness(int index, uint8_t pct) {
    if (index >= 0 && index < LIGHT_COUNT) {
        s_lights[index].brightness = pct;
        s_lights[index].on = true;
    }
}
