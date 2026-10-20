#include "release_manifest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"

static const char *version(const char *s) { return s[0] == 'v' ? s + 1 : s; }

static bool numeric_version(const char *s, unsigned long parts[3]) {
    s = version(s);
    for (unsigned i = 0; i < 3; i++) {
        if (*s < '0' || *s > '9') {
            return false;
        }
        char *end;
        parts[i] = strtoul(s, &end, 10);
        if (parts[i] > 65535) {
            return false;
        }
        if (i == 2) {
            return !*end;
        }
        if (*end != '.') {
            return false;
        }
        s = end + 1;
    }
    return false;
}

bool release_version_newer(const char *release, const char *running) {
    if (!strcmp(version(release), version(running))) {
        return false;
    }
    unsigned long a[3], b[3];
    if (!numeric_version(release, a) || !numeric_version(running, b)) {
        return true;
    }
    for (unsigned i = 0; i < 3; i++) {
        if (a[i] != b[i]) {
            return a[i] > b[i];
        }
    }
    return false;
}

bool release_manifest_parse(const char *text, const char *repo, release_manifest_t *out) {
    memset(out, 0, sizeof(*out));
    cJSON *json = cJSON_Parse(text);
    cJSON *tag = cJSON_GetObjectItemCaseSensitive(json, "tag_name");
    cJSON *assets = cJSON_GetObjectItemCaseSensitive(json, "assets");
    bool ok = cJSON_IsString(tag) && tag->valuestring[0] && strlen(tag->valuestring) < sizeof(out->tag) &&
              cJSON_IsArray(assets) && !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "draft")) &&
              !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "prerelease"));
    if (ok) {
        strcpy(out->tag, tag->valuestring);
        char prefix[256];
        snprintf(prefix, sizeof(prefix), "https://github.com/%s/releases/download/", repo);
        cJSON *asset;
        cJSON_ArrayForEach(asset, assets) {
            cJSON *name = cJSON_GetObjectItemCaseSensitive(asset, "name");
            cJSON *url = cJSON_GetObjectItemCaseSensitive(asset, "browser_download_url");
            cJSON *size = cJSON_GetObjectItemCaseSensitive(asset, "size");
            if (!cJSON_IsString(name) || !cJSON_IsString(url) || !cJSON_IsNumber(size)) {
                continue;
            }
            release_asset_t *dest = NULL;
            size_t limit = 0;
            if (!strcmp(name->valuestring, "firmware-s3.bin")) {
                dest = &out->s3;
                limit = 4 * 1024 * 1024;
            } else if (!strcmp(name->valuestring, "firmware-esp32.bin")) {
                dest = &out->bluetooth;
                limit = 1024 * 1024;
            }
            if (!dest || size->valuedouble < 288 || size->valuedouble > limit ||
                size->valuedouble != (size_t)size->valuedouble ||
                strlen(url->valuestring) >= sizeof(dest->url) ||
                strncmp(url->valuestring, prefix, strlen(prefix))) {
                continue;
            }
            strcpy(dest->url, url->valuestring);
            dest->size = (size_t)size->valuedouble;
        }
        ok = out->s3.size || out->bluetooth.size;
    }
    cJSON_Delete(json);
    if (!ok) {
        memset(out, 0, sizeof(*out));
    }
    return ok;
}
