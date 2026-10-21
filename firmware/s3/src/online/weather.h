#pragma once

#include <stdbool.h>

#include "ui/ui.h"

// Blocking Open-Meteo forecast for (lat, lon) over HTTPS: current
// conditions and UV, today's high/low and UV peak, and the next UI_WEATHER_HOURS hours, local to
// the location. Fills everything but `place`. `buf` is scratch for the body.
bool weather_fetch(double lat, double lon, ui_weather_t *out, char *buf, int cap);

// Blocking Open-Meteo air quality for (lat, lon): fills `has_aqi`, `aqi` (US
// scale) and `pm2_5`, leaving the rest. A separate host, so a second TLS
// handshake: call it after weather_fetch(), never alongside it.
bool air_quality_fetch(double lat, double lon, ui_weather_t *out, char *buf, int cap);
