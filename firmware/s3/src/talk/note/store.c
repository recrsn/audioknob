#include "talk/note/store.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define LABEL "user_data"
// Written a piece at a time with a tick between: a flash write stalls
// everything running from flash or PSRAM, and short stalls let the DAC's
// queue ride them out.
#define WRITE_CHUNK 4096
#define SUFFIX_TMP ".tmp"

static const char *TAG = "note_store";
static bool s_ready;

static bool valid_name(const char *name) {
    size_t n = name ? strlen(name) : 0;
    if (!n || n > 24) {
        return false;
    }
    for (const char *p = name; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '.' || *p == '_')) {
            return false;
        }
    }
    return name[0] != '.';
}

static void path_of(char *out, size_t len, const char *name, const char *suffix) {
    snprintf(out, len, NOTE_STORE_PATH "/%s%s", name, suffix);
}

bool note_store_mount(void) {
    esp_vfs_spiffs_conf_t cfg = {
        .base_path = NOTE_STORE_PATH, .partition_label = LABEL, .max_files = 4, .format_if_mount_failed = true};
    esp_err_t err = esp_vfs_spiffs_register(&cfg);
    s_ready = err == ESP_OK;
    size_t total = 0, used = 0;
    if (s_ready) {
        esp_spiffs_info(LABEL, &total, &used);
    }
    ESP_LOGI(TAG, "%s: %u of %u bytes used", s_ready ? "mounted" : esp_err_to_name(err), (unsigned)used,
             (unsigned)total);
    return s_ready;
}

bool note_store_ready(void) {
    return s_ready;
}

size_t note_store_room(void) {
    size_t total = 0, used = 0;
    if (!s_ready || esp_spiffs_info(LABEL, &total, &used) != ESP_OK) {
        return 0;
    }
    size_t limit = total / 4 * 3;
    return used < limit ? limit - used : 0;
}

static bool write_file(const char *path, const uint8_t *data, size_t size) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        return false;
    }
    bool ok = true;
    for (size_t at = 0; ok && at < size; at += WRITE_CHUNK) {
        size_t n = size - at < WRITE_CHUNK ? size - at : WRITE_CHUNK;
        ok = fwrite(data + at, 1, n, f) == n;
        vTaskDelay(1);
    }
    return fclose(f) == 0 && ok;
}

bool note_store_write(const char *name, const void *data, size_t size) {
    if (!s_ready || !valid_name(name)) {
        return false;
    }
    char path[48], temp[52];
    path_of(path, sizeof(path), name, "");
    path_of(temp, sizeof(temp), name, SUFFIX_TMP);
    // SPIFFS's rename won't replace a file: the old one goes first, and a
    // reader finding only the .tmp takes that (note_store_read).
    bool ok = write_file(temp, data, size) && (unlink(path) == 0 || errno == ENOENT) && rename(temp, path) == 0;
    if (!ok) {
        unlink(temp);
        ESP_LOGW(TAG, "write %s (%u bytes) failed", name, (unsigned)size);
    }
    return ok;
}

static void *read_file(const char *path, size_t *size, size_t max) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    void *data = NULL;
    long len = fseek(f, 0, SEEK_END) == 0 ? ftell(f) : -1;
    if (len > 0 && (size_t)len <= max && fseek(f, 0, SEEK_SET) == 0) {
        data = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (data && fread(data, 1, len, f) == (size_t)len) {
            *size = len;
        } else {
            free(data);
            data = NULL;
        }
    }
    fclose(f);
    return data;
}

void *note_store_read(const char *name, size_t *size, size_t max) {
    *size = 0;
    if (!s_ready || !valid_name(name)) {
        return NULL;
    }
    char path[48], temp[52];
    path_of(path, sizeof(path), name, "");
    void *data = read_file(path, size, max);
    if (!data) {
        // Power lost between a write's unlink and rename.
        path_of(temp, sizeof(temp), name, SUFFIX_TMP);
        data = read_file(temp, size, max);
        if (data) {
            rename(temp, path);
        }
    }
    return data;
}

bool note_store_remove(const char *name) {
    if (!s_ready || !valid_name(name)) {
        return false;
    }
    char path[48];
    path_of(path, sizeof(path), name, "");
    return unlink(path) == 0;
}

void note_store_list(const char *suffix, void (*each)(const char *name, void *ctx), void *ctx) {
    DIR *dir = s_ready ? opendir(NOTE_STORE_PATH) : NULL;
    if (!dir) {
        return;
    }
    size_t tail = strlen(suffix);
    struct dirent *e;
    while ((e = readdir(dir))) {
        size_t n = strlen(e->d_name);
        if (n > tail && !strcmp(e->d_name + n - tail, suffix) && valid_name(e->d_name)) {
            each(e->d_name, ctx);
        }
    }
    closedir(dir);
}
