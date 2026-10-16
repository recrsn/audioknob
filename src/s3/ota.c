#include "ota.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"

// Internal RAM: flash writes from PSRAM go through a bounce buffer anyway.
#define CHUNK 4096
// The image header, the first segment's header, then the app description.
#define DESC_OFFSET (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t))
#define HEADER_LEN  (DESC_OFFSET + sizeof(esp_app_desc_t))

static const char *TAG = "ota";

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static ota_status_t s_status;
static const esp_partition_t *s_ready;

static void set_status(ota_state_t state, const char *version, const char *error) {
    taskENTER_CRITICAL(&s_lock);
    s_status.state = state;
    if (version) {
        snprintf(s_status.version, sizeof(s_status.version), "%s", version);
    }
    snprintf(s_status.error, sizeof(s_status.error), "%s", error ? error : "");
    taskEXIT_CRITICAL(&s_lock);
}

static bool fail(const char *error) {
    ESP_LOGW(TAG, "update failed: %s", error);
    set_status(OTA_FAILED, NULL, error);
    return false;
}

void ota_get_status(ota_status_t *out) {
    taskENTER_CRITICAL(&s_lock);
    *out = s_status;
    taskEXIT_CRITICAL(&s_lock);
}

static bool read_full(ota_read_fn read, void *ctx, char *buf, size_t len) {
    for (size_t got = 0; got < len;) {
        int n = read(ctx, buf + got, len - got);
        if (n <= 0) {
            return false;
        }
        got += n;
    }
    return true;
}

// Catches the wrong file (the ESP32's image, anything else) before a byte
// of flash is erased; esp_ota_end() checks the rest.
static const char *check_header(const char *buf, char version[32]) {
    const esp_image_header_t *image = (const esp_image_header_t *)buf;
    const esp_app_desc_t *desc = (const esp_app_desc_t *)(buf + DESC_OFFSET);
    if (image->magic != ESP_IMAGE_HEADER_MAGIC || desc->magic_word != ESP_APP_DESC_MAGIC_WORD) {
        return "Not a firmware image";
    }
    if (image->chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID) {
        return "Firmware for a different chip";
    }
    memcpy(version, desc->version, 32);
    version[31] = '\0';
    return NULL;
}

bool ota_receive(size_t size, ota_read_fn read, void *ctx) {
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        return fail("No update slot");
    }
    if (size < HEADER_LEN) {
        return fail("Not a firmware image");
    }
    if (size > part->size) {
        return fail("Image too large");
    }
    char *buf = heap_caps_malloc(CHUNK, MALLOC_CAP_INTERNAL);
    if (!buf) {
        return fail("Out of memory");
    }
    set_status(OTA_RECEIVING, "", NULL);

    const char *error = NULL;
    char version[32];
    esp_ota_handle_t ota = 0;
    size_t len = size < CHUNK ? size : CHUNK;
    if (!read_full(read, ctx, buf, len)) {
        error = "Upload interrupted";
    } else {
        error = check_header(buf, version);
    }
    if (!error && esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &ota) != ESP_OK) {
        ota = 0;
        error = "Couldn't prepare the update slot";
    }
    for (size_t done = 0; !error;) {
        if (esp_ota_write(ota, buf, len) != ESP_OK) {
            error = "Flash write failed";
            break;
        }
        done += len;
        if (done == size) {
            break;
        }
        len = size - done < CHUNK ? size - done : CHUNK;
        if (!read_full(read, ctx, buf, len)) {
            error = "Upload interrupted";
        }
    }
    free(buf);

    if (error) {
        if (ota) {
            esp_ota_abort(ota);
        }
        return fail(error);
    }
    if (esp_ota_end(ota) != ESP_OK) {
        return fail("Image is damaged or incomplete");
    }
    ESP_LOGI(TAG, "version %s written to %s, waiting for confirmation", version, part->label);
    taskENTER_CRITICAL(&s_lock);
    s_ready = part;
    taskEXIT_CRITICAL(&s_lock);
    set_status(OTA_READY, version, NULL);
    return true;
}

void ota_install(void) {
    ota_status_t status;
    taskENTER_CRITICAL(&s_lock);
    status = s_status;
    const esp_partition_t *part = s_ready;
    taskEXIT_CRITICAL(&s_lock);
    if (status.state != OTA_READY) {
        return;
    }
    if (esp_ota_set_boot_partition(part) != ESP_OK) {
        fail("Couldn't switch to the new firmware");
        return;
    }
    ESP_LOGI(TAG, "restarting into %s", status.version);
    esp_restart();
}

void ota_decline(void) {
    ota_status_t status;
    ota_get_status(&status);
    if (status.state == OTA_READY) {
        set_status(OTA_DECLINED, NULL, NULL);
    }
}

void ota_confirm_running(void) {
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "update kept: %s", esp_app_get_description()->version);
    }
}
