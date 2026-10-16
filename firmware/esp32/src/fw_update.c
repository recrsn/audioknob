#include "fw_update.h"

#include <string.h>

#include "a2dp_sink.h"
#include "bt_power.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"

// Long enough for the S3 to be restarting too and still answer.
#define PROBATION_US (60 * 1000 * 1000LL)
// For the S3 to get the ack, and the phone the A2DP disconnect.
#define RESTART_DELAY_MS 300

static const char *TAG = "fw_update";

static struct {
    bool active;   // between MSG_FW_BEGIN and the end of the transfer
    bool installed;
    esp_ota_handle_t ota;
    const esp_partition_t *part;
    uint32_t size;
    uint32_t next;
    uint8_t sha256[32];
    mbedtls_sha256_context sha;
} s_fw;

static volatile bool s_probation;
static esp_timer_handle_t s_probation_timer;

static void probation_expired(void *arg) {
    (void)arg;
    ESP_LOGE(TAG, "the S3 never answered; restarting to roll back");
    esp_restart();
}

void fw_update_init(void) {
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) != ESP_OK ||
        state != ESP_OTA_IMG_PENDING_VERIFY) {
        return;
    }
    ESP_LOGI(TAG, "new firmware %s on probation", esp_app_get_description()->version);
    s_probation = true;
    const esp_timer_create_args_t args = {.callback = probation_expired, .name = "fw_probation"};
    ESP_ERROR_CHECK(esp_timer_create(&args, &s_probation_timer));
    ESP_ERROR_CHECK(esp_timer_start_once(s_probation_timer, PROBATION_US));
}

void fw_update_confirm(void) {
    if (!s_probation) {
        return;
    }
    esp_timer_stop(s_probation_timer);
    esp_ota_mark_app_valid_cancel_rollback();
    s_probation = false;
    ESP_LOGI(TAG, "firmware %s kept", esp_app_get_description()->version);
}

bool fw_update_busy(void) {
    return s_fw.active || s_probation;
}

static void abandon(void) {
    if (s_fw.active) {
        esp_ota_abort(s_fw.ota);
        mbedtls_sha256_free(&s_fw.sha);
        s_fw.active = false;
    }
}

static fw_result_t fail(fw_result_t result) {
    ESP_LOGW(TAG, "update failed (%d) at %u of %u", result, (unsigned)s_fw.next, (unsigned)s_fw.size);
    abandon();
    return result;
}

static fw_result_t begin(const fw_begin_t *msg) {
    abandon();
    s_fw.installed = false;
    s_fw.next = 0;
    s_fw.size = msg->size;
    memcpy(s_fw.sha256, msg->sha256, sizeof(s_fw.sha256));
    s_fw.part = esp_ota_get_next_update_partition(NULL);
    if (!s_fw.part || msg->size > s_fw.part->size) {
        return FW_TOO_LARGE;
    }
    // A new image still on probation can't start another update.
    fw_update_confirm();
    // Erases as it goes, so no frame waits on erasing the whole slot.
    if (esp_ota_begin(s_fw.part, OTA_WITH_SEQUENTIAL_WRITES, &s_fw.ota) != ESP_OK) {
        return FW_FLASH_ERROR;
    }
    mbedtls_sha256_init(&s_fw.sha);
    mbedtls_sha256_starts(&s_fw.sha, 0);
    s_fw.active = true;
    ESP_LOGI(TAG, "receiving %u bytes into %s", (unsigned)msg->size, s_fw.part->label);
    return FW_OK;
}

static fw_result_t chunk(const fw_chunk_t *msg, uint8_t len) {
    uint32_t n = len - FW_CHUNK_HEADER;
    if (!s_fw.active) {
        return FW_NOT_STARTED;
    }
    if (msg->offset != s_fw.next) {
        return FW_OUT_OF_ORDER;
    }
    if (s_fw.next + n > s_fw.size) {
        return fail(FW_TOO_LARGE);
    }
    if (esp_ota_write(s_fw.ota, msg->data, n) != ESP_OK) {
        return fail(FW_FLASH_ERROR);
    }
    mbedtls_sha256_update(&s_fw.sha, msg->data, n);
    s_fw.next += n;
    return FW_OK;
}

static fw_result_t end(void) {
    if (s_fw.installed) {
        return FW_OK; // the first ack was lost
    }
    if (!s_fw.active) {
        return FW_NOT_STARTED;
    }
    if (s_fw.next != s_fw.size) {
        return FW_OUT_OF_ORDER;
    }
    uint8_t sha256[32];
    mbedtls_sha256_finish(&s_fw.sha, sha256);
    if (memcmp(sha256, s_fw.sha256, sizeof(sha256))) {
        return fail(FW_BAD_CHECKSUM);
    }
    mbedtls_sha256_free(&s_fw.sha);
    s_fw.active = false;
    if (esp_ota_end(s_fw.ota) != ESP_OK) {
        return FW_BAD_IMAGE;
    }
    if (esp_ota_set_boot_partition(s_fw.part) != ESP_OK) {
        return FW_FLASH_ERROR;
    }
    s_fw.installed = true;
    ESP_LOGI(TAG, "new firmware in %s", s_fw.part->label);
    return FW_OK;
}

bool fw_update_handle(uint8_t type, const uint8_t *payload, uint8_t len, fw_ack_t *ack) {
    fw_result_t result = FW_OUT_OF_ORDER;
    if (type == MSG_FW_BEGIN && len == sizeof(fw_begin_t)) {
        result = begin((const fw_begin_t *)payload);
    } else if (type == MSG_FW_CHUNK && len > FW_CHUNK_HEADER) {
        result = chunk((const fw_chunk_t *)payload, len);
    } else if (type == MSG_FW_END) {
        result = end();
    }
    *ack = (fw_ack_t){.type = type, .result = result, .next = s_fw.next};
    return type == MSG_FW_END && result == FW_OK;
}

void fw_update_restart(void) {
    if (bt_power_enabled()) {
        a2dp_sink_disconnect();
    }
    vTaskDelay(pdMS_TO_TICKS(RESTART_DELAY_MS));
    ESP_LOGI(TAG, "restarting into the new firmware");
    esp_restart();
}
