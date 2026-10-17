#include "message_store.h"
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "message_store";
static bool s_ready;
static SemaphoreHandle_t s_lock;
static bool filename(const char *name) {
    if (!name || !*name || strlen(name) > 24)
        return false;
    for (const char *p = name; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '.' || *p == '_'))
            return false;
    return strcmp(name, ".") && strcmp(name, "..");
}
void message_store_init(void) {
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock)
        return;
    const esp_partition_t *part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "user_data");
    if (!part) {
        ESP_LOGE(TAG, "Missing user_data partition; install the full flash image over USB");
        return;
    }
    esp_vfs_spiffs_conf_t cfg = {.base_path = MESSAGE_DATA_PATH,
                                 .partition_label = "user_data",
                                 .max_files = 5,
                                 .format_if_mount_failed = false};
    esp_err_t err = esp_vfs_spiffs_register(&cfg);
    if (err != ESP_OK) {
        // Format only a completely erased, unused region. Never silently
        // reformat a damaged filesystem containing the user's recordings.
        uint8_t block[512];
        bool erased = true;
        for (size_t off = 0; off < part->size && erased; off += sizeof(block)) {
            if (esp_partition_read(part, off, block, sizeof(block)) != ESP_OK) {
                erased = false;
                break;
            }
            for (size_t i = 0; i < sizeof(block); i++)
                if (block[i] != 0xff) {
                    erased = false;
                    break;
                }
        }
        if (!erased) {
            ESP_LOGE(TAG, "Data area unavailable; preserved without formatting");
            return;
        }
        cfg.format_if_mount_failed = true;
        err = esp_vfs_spiffs_register(&cfg);
    }
    s_ready = err == ESP_OK;
    if (s_ready)
        ESP_LOGI(TAG, "Saved-message partition mounted (%u bytes)", (unsigned)part->size);
}
bool message_store_ready(void) {
    return s_ready;
}
void message_store_usage(size_t *total, size_t *used) {
    *total = *used = 0;
    if (s_ready && esp_spiffs_info("user_data", total, used) != ESP_OK)
        *total = *used = 0;
}
bool message_store_write(const char *name, const void *data, size_t size) {
    if (!s_ready || !filename(name))
        return false;
    char path[sizeof(MESSAGE_DATA_PATH) + 32], temp[sizeof(MESSAGE_DATA_PATH) + 36];
    snprintf(path, sizeof(path), MESSAGE_DATA_PATH "/%s", name);
    snprintf(temp, sizeof(temp), MESSAGE_DATA_PATH "/%s.tmp", name);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t total, used;
    message_store_usage(&total, &used);
    // SPIFFS needs free pages for GC; keep the last quarter free.
    bool ok = total && size + used + 4096 < total * 3 / 4;
    FILE *file = ok ? fopen(temp, "wb") : NULL;
    ok = file && fwrite(data, 1, size, file) == size;
    if (file && fclose(file) != 0)
        ok = false;
    // SPIFFS rename does not replace a destination. Metadata uses two
    // generations, so the other committed file survives this replacement.
    if (ok && unlink(path) != 0 && errno != ENOENT)
        ok = false;
    if (ok)
        ok = rename(temp, path) == 0;
    if (!ok)
        unlink(temp);
    xSemaphoreGive(s_lock);
    return ok;
}
void *message_store_read(const char *name, size_t *size, size_t max_size) {
    *size = 0;
    if (!s_ready || !filename(name))
        return NULL;
    char path[sizeof(MESSAGE_DATA_PATH) + 32];
    snprintf(path, sizeof(path), MESSAGE_DATA_PATH "/%s", name);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    FILE *f = fopen(path, "rb");
    void *data = NULL;
    if (f && fseek(f, 0, SEEK_END) == 0) {
        long len = ftell(f);
        if (len > 0 && (size_t)len <= max_size && fseek(f, 0, SEEK_SET) == 0) {
            data = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (data && fread(data, 1, len, f) == (size_t)len)
                *size = len;
            else {
                free(data);
                data = NULL;
            }
        }
    }
    if (f)
        fclose(f);
    xSemaphoreGive(s_lock);
    return data;
}
bool message_store_remove(const char *name) {
    if (!s_ready || !filename(name))
        return false;
    char path[sizeof(MESSAGE_DATA_PATH) + 32];
    snprintf(path, sizeof(path), MESSAGE_DATA_PATH "/%s", name);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = unlink(path) == 0;
    xSemaphoreGive(s_lock);
    return ok;
}
