#pragma once

#include <stdbool.h>

// Where this device is, from its public IP address.
typedef struct {
    char city[32];
    double lat;
    double lon;
    char tz[40];       // IANA zone name
    bool has_offset;   // whether the provider reported tz_offset_s
    int tz_offset_s;   // current UTC offset, east-positive; for zones tz_db lacks
} geo_t;

// Blocking lookup over HTTPS (ipwho.is, falling back to ipinfo.io). `buf` is
// scratch for the response body.
bool geo_lookup(geo_t *out, char *buf, int cap);
