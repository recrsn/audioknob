#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char url[512];
    char version[32];
    uint8_t sha256[32];
    size_t size;
} release_asset_t;

typedef struct {
    char tag[32];
    release_asset_t s3;
    release_asset_t bluetooth;
} release_manifest_t;

// Only exact asset names and HTTPS downloads from the configured repo.
bool release_manifest_parse(const char *json, const char *repo, release_manifest_t *out);
// Compares stable numeric versions; unknown development versions differ
// from a release unless their strings match (ignoring a leading v).
bool release_version_newer(const char *release, const char *running);
