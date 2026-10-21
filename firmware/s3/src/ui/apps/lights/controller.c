#include "ui/apps/lights/lights.h"

#include <string.h>

#include "lvgl.h"
#include "ui/ui.h"

#define LIGHTS_POLL_MS 250

static void poll_cb(lv_timer_t *timer) {
    (void)timer;
    light_t lights[LIGHTS_MAX];
    ui_light_t out[UI_LIGHTS_MAX] = {0};
    int n = lights_get(lights, LIGHTS_MAX < UI_LIGHTS_MAX ? LIGHTS_MAX : UI_LIGHTS_MAX);
    for (int i = 0; i < n; i++) {
        memcpy(out[i].name, lights[i].name, sizeof(out[i].name));
        out[i].reachable = lights[i].reachable;
        out[i].on = lights[i].on;
        out[i].brightness = lights[i].brightness;
    }
    ui_update_lights(out, n);
}

static const ui_lights_handler_t s_handler = {
    .set_power = lights_set_power,
    .set_brightness = lights_set_brightness,
};

void lights_ui_start(void) {
    ui_set_lights_handler(&s_handler);
    poll_cb(NULL);
    lv_timer_create(poll_cb, LIGHTS_POLL_MS, NULL);
}
