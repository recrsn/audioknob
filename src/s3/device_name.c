#include "device_name.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#define NVS_NAMESPACE "device"
#define NVS_KEY_NAME "name"

#define DEFAULT_PREFIX "AudioKnob-"
#define DEFAULT_SUFFIX_LEN 8
#define HOSTNAME_FALLBACK "audioknob"
#define HOSTNAME_MAX 63

static const char *TAG = "device_name";

static SemaphoreHandle_t s_lock;
static char s_name[DEVICE_NAME_MAX + 1];
static device_name_cb_t s_on_change;

static void make_default(char *out, size_t len) {
    static const char alphabet[] = "ABCDEFGHJKMNPQRSTUVWXYZ23456789"; // no 0/O, 1/I/L
    char suffix[DEFAULT_SUFFIX_LEN + 1];
    for (int i = 0; i < DEFAULT_SUFFIX_LEN; i++) {
        suffix[i] = alphabet[esp_random() % (sizeof(alphabet) - 1)];
    }
    suffix[DEFAULT_SUFFIX_LEN] = '\0';
    snprintf(out, len, DEFAULT_PREFIX "%s", suffix);
}

static void save(const char *name) {
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        ESP_LOGW(TAG, "can't open NVS");
        return;
    }
    nvs_set_str(nvs, NVS_KEY_NAME, name);
    nvs_commit(nvs);
    nvs_close(nvs);
}

void device_name_init(device_name_cb_t on_change) {
    s_lock = xSemaphoreCreateMutex();
    s_on_change = on_change;

    nvs_handle_t nvs;
    size_t len = sizeof(s_name);
    bool loaded = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK;
    if (loaded) {
        loaded = nvs_get_str(nvs, NVS_KEY_NAME, s_name, &len) == ESP_OK && s_name[0];
        nvs_close(nvs);
    }
    if (!loaded) {
        make_default(s_name, sizeof(s_name));
        save(s_name);
    }
    ESP_LOGI(TAG, "\"%s\"", s_name);
    on_change(s_name);
}

void device_name_get(char *out, size_t len) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(out, len, "%s", s_name);
    xSemaphoreGive(s_lock);
}

void device_name_hostname(char *out, size_t len) {
    char name[DEVICE_NAME_MAX + 1];
    device_name_get(name, sizeof(name));

    size_t max = len - 1 < HOSTNAME_MAX ? len - 1 : HOSTNAME_MAX;
    size_t n = 0;
    bool dash = false;
    for (const char *c = name; *c && n < max; c++) {
        if (*c == '\'') {
            continue; // "Alex's" -> "alexs", not "alex-s"
        }
        if (isalnum((unsigned char)*c) && !(*c & 0x80)) {
            if (dash && n && n < max - 1) {
                out[n++] = '-';
            }
            out[n++] = (char)tolower((unsigned char)*c);
            dash = false;
        } else {
            dash = true;
        }
    }
    out[n] = '\0';
    if (!n) {
        snprintf(out, len, HOSTNAME_FALLBACK);
    }
}

bool device_name_set(const char *name) {
    while (isspace((unsigned char)*name)) {
        name++;
    }
    size_t len = strlen(name);
    while (len && isspace((unsigned char)name[len - 1])) {
        len--;
    }
    if (!len || len > DEVICE_NAME_MAX) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20 || c == 0x7F) {
            return false;
        }
    }

    char clean[DEVICE_NAME_MAX + 1];
    memcpy(clean, name, len);
    clean[len] = '\0';

    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool changed = strcmp(s_name, clean) != 0;
    memcpy(s_name, clean, len + 1);
    xSemaphoreGive(s_lock);
    if (changed) {
        save(clean);
        ESP_LOGI(TAG, "renamed to \"%s\"", clean);
        s_on_change(clean);
    }
    return true;
}
