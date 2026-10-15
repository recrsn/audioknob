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
// poller can skip unchanged copies; 0 means nothing yet. The location is
// the manual one if set, else the IP-detected one, cached across boots;
// `manual` may be NULL.
uint32_t online_get_location(geo_t *out, bool *manual);
uint32_t online_get_weather(ui_weather_t *out);

// Pins the location (e.g. picked on the web page), or with NULL goes back to
// the IP-detected one. Persisted, and the forecast follows at once.
void online_set_location(const geo_t *manual);
