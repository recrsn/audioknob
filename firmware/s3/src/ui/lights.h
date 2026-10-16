#pragma once

#include <stdbool.h>
#include <stdint.h>

// Smart-light backend behind the Lights page. The target is WiZ bulbs
// (local UDP JSON, port 38899); lights_mock.c stands in until that lands,
// so it builds for env:native too.

#define LIGHTS_MAX 6

typedef struct {
    char name[24];
    bool reachable;
    bool on;
    uint8_t brightness; // 1-100 %
} light_t;

// Snapshot of every known light; returns how many were written.
int lights_get(light_t *out, int max);

void lights_set_power(int index, bool on);
void lights_set_brightness(int index, uint8_t pct);

// Wires the backend to the Lights page: registers the UI's handler and
// polls state into it on an LVGL timer. Call under the LVGL lock, after
// ui_init().
void lights_ui_start(void);
