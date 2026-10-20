#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "release_manifest.h"
#include "cJSON.h"

static const char *repo = "recrsn/audioknob";
static const char *valid = "{\"tag_name\":\"v1.2.3\",\"assets\":["
                           "{\"name\":\"firmware-s3.bin\",\"digest\":\"sha256:"
                           "0101010101010101010101010101010101010101010101010101010101010101\",\"size\":4096,"
                           "\"browser_download_url\":\"https://"
                           "github.com/recrsn/audioknob/releases/download/v1.2.3/firmware-s3.bin\"},"
                           "{\"name\":\"firmware-esp32.bin\",\"digest\":\"sha256:"
                           "0101010101010101010101010101010101010101010101010101010101010101\",\"size\":1024,"
                           "\"browser_download_url\":\"https:/"
                           "/github.com/recrsn/audioknob/releases/download/v1.2.3/firmware-esp32.bin\"}]}";

static void rejected(const char *json) {
    release_manifest_t manifest;
    memset(&manifest, 0x7f, sizeof(manifest));
    assert(!release_manifest_parse(json, repo, &manifest));
    assert(!manifest.tag[0] && !manifest.s3.size && !manifest.bluetooth.size);
}

int main(void) {
    release_manifest_t manifest;
    assert(release_manifest_parse(valid, repo, &manifest));
    assert(!strcmp(manifest.tag, "v1.2.3"));
    assert(manifest.s3.size == 4096 && manifest.bluetooth.size == 1024);
    assert(strstr(manifest.s3.url, "/firmware-s3.bin"));
    assert(!release_manifest_parse(valid, "other/repo", &manifest));
    rejected("not json");
    rejected("null");
    rejected("{}");
    rejected("{\"tag_name\":\"v1.0.0\",\"assets\":[]}");
    rejected("{\"tag_name\":\"v1.0.0\",\"assets\":{}}");
    rejected("{\"tag_name\":\"1234567890123456789012345678901234\",\"assets\":[]}");
    char text[2048];
    const char *urls[] = {
        "http://github.com/recrsn/audioknob/releases/download/v1/fw.bin",
        "https://github.com.evil/recrsn/audioknob/releases/download/v1/fw.bin",
        "https://github.com/other/repo/releases/download/v1/fw.bin",
    };
    for (size_t i = 0; i < sizeof(urls) / sizeof(urls[0]); i++) {
        snprintf(text, sizeof(text),
                 "{\"tag_name\":\"v1\",\"assets\":[{\"name\":\"firmware-s3.bin\",\"digest\":\"sha256:"
                 "0101010101010101010101010101010101010101010101010101010101010101\",\"size\":4096,\"browser_"
                 "download_url\":\"%s\"}]}",
                 urls[i]);
        rejected(text);
    }
    const char *names[] = {"firmware.bin", "firmware-s3.bin", "firmware-esp32.bin"};
    const char *sizes[] = {"4096", "4194305", "1048577", "0", "-1", "4096.5", "1e999", "\"4096\""};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        snprintf(text, sizeof(text),
                 "{\"tag_name\":\"v1\",\"assets\":[{\"name\":\"%s\",\"digest\":\"sha256:"
                 "0101010101010101010101010101010101010101010101010101010101010101\",\"size\":%s,\"browser_"
                 "download_url\":"
                 "\"https://github.com/recrsn/audioknob/releases/download/v1/fw.bin\"}]}",
                 names[i < 3 ? i : 1], sizes[i]);
        rejected(text);
    }
    for (int i = 0; i < 2; i++) {
        snprintf(text, sizeof(text), "{\"%s\":true,%s", i ? "draft" : "prerelease", valid + 1);
        rejected(text);
    }
    cJSON *json = cJSON_Parse(valid);
    cJSON *s3 = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(json, "assets"), 0);
    cJSON *bluetooth = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(json, "assets"), 1);
    cJSON_AddStringToObject(bluetooth, "label", "version:v1.0.0");
    char *encoded = cJSON_PrintUnformatted(json);
    assert(release_manifest_parse(encoded, repo, &manifest));
    assert(!strcmp(manifest.s3.version, "v1.2.3") && !strcmp(manifest.bluetooth.version, "v1.0.0"));
    assert(manifest.s3.sha256[0] == 1 && manifest.bluetooth.sha256[31] == 1);
    cJSON_free(encoded);
    const char *digests[] = {"", "sha256:01", "sha1:0101010101010101010101010101010101010101",
                             "sha256:010101010101010101010101010101010101010101010101010101010101010g"};
    for (size_t i = 0; i < sizeof(digests) / sizeof(digests[0]); i++) {
        cJSON_ReplaceItemInObjectCaseSensitive(s3, "digest", cJSON_CreateString(digests[i]));
        encoded = cJSON_PrintUnformatted(json);
        rejected(encoded);
        cJSON_free(encoded);
    }
    cJSON_DeleteItemFromObjectCaseSensitive(s3, "digest");
    encoded = cJSON_PrintUnformatted(json);
    rejected(encoded);
    cJSON_free(encoded);
    cJSON_Delete(json);
    assert(!release_version_newer("v1.2.3", "1.2.3"));
    assert(!release_version_newer("1.2.3", "v1.2.3"));
    assert(release_version_newer("v1.10.0", "1.9.9"));
    assert(!release_version_newer("v1.9.9", "1.10.0"));
    assert(release_version_newer("v2.0.0", "1.99.99"));
    assert(!release_version_newer("v1.0.0", "2.0.0"));
    assert(release_version_newer("v1.2.3", "abc1234-dirty"));
    assert(!release_version_newer("abc1234", "abc1234"));
    puts("Release manifest validation and version comparisons passed");
}
