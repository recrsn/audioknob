#pragma once

#include <stdbool.h>
#include <stddef.h>

// Minimal DMAP (DAAP tagged) reader for AirPlay's text metadata:
// SET_PARAMETER bodies of type application/x-dmap-tagged, which wrap the
// fields below in an `mlit` container. Every other tag is skipped.
typedef struct {
    char title[64];   // minm
    char artist[64];  // asar
    char album[64];   // asal
} dmap_track_t;

// Fills whatever fields are present (absent ones are left untouched);
// strings are truncated on a UTF-8 boundary. Returns false on malformed input.
bool dmap_parse_track(const char *buf, size_t len, dmap_track_t *out);
