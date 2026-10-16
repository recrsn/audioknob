#include "ota.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp32_fw.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Internal RAM: flash writes from PSRAM go through a bounce buffer anyway.
#define CHUNK 4096
// The image header, the first segment's header, then the app description.
#define DESC_OFFSET (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t))
#define HEADER_LEN  (DESC_OFFSET + sizeof(esp_app_desc_t))
#define INSTALL_TASK_STACK 4096

static const char *TAG = "ota";

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static ota_status_t s_status;

// Where an image goes, per chip. The functions return NULL or why they
// failed.
typedef struct {
    const char *(*begin)(size_t size);
    bool (*write)(const void *buf, size_t len);
    const char *(*end)(void);
    void (*abort)(void);
} ota_sink_t;

static struct {
    const esp_partition_t *part;
    esp_ota_handle_t handle;
} s_s3;

static const char *s3_begin(size_t size) {
    s_s3.part = esp_ota_get_next_update_partition(NULL);
    if (!s_s3.part) {
        return "No update slot";
    }
    if (size > s_s3.part->size) {
        return "Image too large";
    }
    if (esp_ota_begin(s_s3.part, OTA_WITH_SEQUENTIAL_WRITES, &s_s3.handle) != ESP_OK) {
        return "Couldn't prepare the update slot";
    }
    return NULL;
}

static bool s3_write(const void *buf, size_t len) {
    return esp_ota_write(s_s3.handle, buf, len) == ESP_OK;
}

static const char *s3_end(void) {
    return esp_ota_end(s_s3.handle) == ESP_OK ? NULL : "Image is damaged or incomplete";
}

static void s3_abort(void) {
    esp_ota_abort(s_s3.handle);
}

static const ota_sink_t k_sinks[] = {
    [OTA_TARGET_S3] = {s3_begin, s3_write, s3_end, s3_abort},
    [OTA_TARGET_ESP32] = {esp32_fw_stage_begin, esp32_fw_stage_write, esp32_fw_stage_end, esp32_fw_stage_abort},
};

static void set_status(ota_state_t state, const char *error) {
    taskENTER_CRITICAL(&s_lock);
    s_status.state = state;
    snprintf(s_status.error, sizeof(s_status.error), "%s", error ? error : "");
    taskEXIT_CRITICAL(&s_lock);
}

static bool fail(const char *error) {
    ESP_LOGW(TAG, "update failed: %s", error);
    set_status(OTA_FAILED, error);
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

// Catches the wrong file before a byte of flash is erased; the rest is
// checked once it's all written.
static const char *check_header(const char *buf, ota_target_t *target, char version[32]) {
    const esp_image_header_t *image = (const esp_image_header_t *)buf;
    const esp_app_desc_t *desc = (const esp_app_desc_t *)(buf + DESC_OFFSET);
    if (image->magic != ESP_IMAGE_HEADER_MAGIC || desc->magic_word != ESP_APP_DESC_MAGIC_WORD) {
        return "Not a firmware image";
    }
    if (image->chip_id == CONFIG_IDF_FIRMWARE_CHIP_ID) {
        *target = OTA_TARGET_S3;
    } else if (image->chip_id == ESP_CHIP_ID_ESP32) {
        *target = OTA_TARGET_ESP32;
    } else {
        return "Firmware for a different chip";
    }
    memcpy(version, desc->version, 32);
    version[31] = '\0';
    return NULL;
}

// One step under the lock, so an install accepted on the device and an
// upload arriving on the web server can't both proceed.
static bool start_receiving(void) {
    taskENTER_CRITICAL(&s_lock);
    bool ok = s_status.state != OTA_INSTALLING;
    if (ok) {
        s_status.state = OTA_RECEIVING;
        s_status.error[0] = '\0';
    }
    taskEXIT_CRITICAL(&s_lock);
    return ok;
}

bool ota_receive(size_t size, ota_read_fn read, void *ctx) {
    // The ESP32's image is read from staging while it's sent. Not fail():
    // the install's own status stays up.
    if (!start_receiving()) {
        return false;
    }
    if (size < HEADER_LEN) {
        return fail("Not a firmware image");
    }
    char *buf = heap_caps_malloc(CHUNK, MALLOC_CAP_INTERNAL);
    if (!buf) {
        return fail("Out of memory");
    }

    ota_target_t target = OTA_TARGET_S3;
    char version[32];
    const ota_sink_t *sink = NULL;
    size_t len = size < CHUNK ? size : CHUNK;
    const char *error = read_full(read, ctx, buf, len) ? check_header(buf, &target, version) : "Upload interrupted";
    if (!error) {
        error = k_sinks[target].begin(size);
        sink = error ? NULL : &k_sinks[target];
    }
    for (size_t done = 0; !error;) {
        if (!sink->write(buf, len)) {
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
        if (sink) {
            sink->abort();
        }
        return fail(error);
    }
    error = sink->end();
    if (error) {
        return fail(error);
    }
    ESP_LOGI(TAG, "%s version %s received, waiting for confirmation", target == OTA_TARGET_S3 ? "S3" : "ESP32",
             version);
    taskENTER_CRITICAL(&s_lock);
    s_status.target = target;
    snprintf(s_status.version, sizeof(s_status.version), "%s", version);
    taskEXIT_CRITICAL(&s_lock);
    set_status(OTA_READY, NULL);
    return true;
}

static void set_progress(unsigned percent) {
    taskENTER_CRITICAL(&s_lock);
    s_status.progress = percent;
    taskEXIT_CRITICAL(&s_lock);
}

static void esp32_install_task(void *arg) {
    (void)arg;
    const char *error = esp32_fw_install(set_progress);
    if (error) {
        fail(error);
    } else {
        set_status(OTA_INSTALLED, NULL);
    }
    vTaskDelete(NULL);
}

void ota_install(void) {
    taskENTER_CRITICAL(&s_lock);
    ota_status_t status = s_status;
    if (status.state == OTA_READY) {
        s_status.state = OTA_INSTALLING;
        s_status.progress = 0;
    }
    taskEXIT_CRITICAL(&s_lock);
    if (status.state != OTA_READY) {
        return;
    }
    if (status.target == OTA_TARGET_ESP32) {
        if (xTaskCreate(esp32_install_task, "esp32_fw", INSTALL_TASK_STACK, NULL, 3, NULL) != pdPASS) {
            fail("Out of memory");
        }
        return;
    }
    if (esp_ota_set_boot_partition(s_s3.part) != ESP_OK) {
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
        set_status(OTA_DECLINED, NULL);
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
