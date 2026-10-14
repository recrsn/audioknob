#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "geo.h"
#include "ui.h"

// Background task for the internet-backed features: locates the device by IP
// (for the timezone and the forecast), then keeps the weather fresh.
// Producers only: callers poll the getters, per spec.md's decoupling.

void online_start(void);

// From wifi_start()'s got-IP callback; the task waits for the first one.
void online_network_up(void);

// Each getter returns a version that bumps whenever the data changes, so a
// poller can skip unchanged copies; 0 means nothing yet.
uint32_t online_get_location(geo_t *out);
uint32_t online_get_weather(ui_weather_t *out);
