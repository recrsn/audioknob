#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "stream_net.h"

// The radio presets, in the user's order, kept in NVS namespace `radio`.
// Edited from the device web page; the knob's Radio app plays them. Safe
// from any task.

#define RADIO_STATIONS_MAX 24
#define RADIO_NAME_MAX 48 // bytes, UTF-8, with the NUL

typedef struct {
    char name[RADIO_NAME_MAX];
    char url[STREAM_URL_MAX];
} radio_station_t;

// Loads the list; the very first boot gets a few sample stations.
void radio_stations_init(void);

int radio_stations_count(void);
bool radio_stations_get(int index, radio_station_t *out);
// The index of the station with this URL, or -1.
int radio_stations_find(const char *url);

// False when the list is full, or the name is empty or the URL isn't
// http(s)://.
bool radio_stations_add(const char *name, const char *url);
bool radio_stations_remove(int index);
// Replaces one station's name and URL; same rules as add.
bool radio_stations_update(int index, const char *name, const char *url);
// Moves one station to position `to`, shifting the ones between.
bool radio_stations_move(int from, int to);

// Changes whenever the list does.
uint32_t radio_stations_version(void);
