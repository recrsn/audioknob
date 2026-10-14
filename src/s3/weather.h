#pragma once

#include <stdbool.h>

#include "ui.h"

// Blocking Open-Meteo forecast for (lat, lon) over HTTPS: current
// conditions, today's high/low and the next UI_WEATHER_HOURS hours, local to
// the location. Fills everything but `place`. `buf` is scratch for the body.
bool weather_fetch(double lat, double lon, ui_weather_t *out, char *buf, int cap);
