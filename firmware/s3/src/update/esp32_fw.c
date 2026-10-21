#include "update/esp32_fw.h"

#include <string.h>

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "protocol.h"
#include "drivers/uart_link.h"

#define DIGEST_LEN 32
#define DESC_OFFSET (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t))
#define READ_CHUNK 1024
#define SECTOR 4096

// Ack timeouts. A chunk can wait on a sector erase; MSG_FW_END on
// esp_ota_end() reading the whole image back.
#define BEGIN_TIMEOUT_MS 1000
#define CHUNK_TIMEOUT_MS 1000
#define END_TIMEOUT_MS 5000
// Covers the ESP32 waking from light sleep, which loses the first frame.
#define RETRIES 5
// Past the new image's 60 s probation, so a rollback is seen as one.
#define RESTART_TIMEOUT_MS 90000
#define POLL_MS 200

static const char *TAG = "esp32_fw";

static struct {
    const esp_partition_t *part;
    size_t size;
    size_t written;
    bool ready;
    uint8_t sha256[DIGEST_LEN];
    uint8_t app_sha[sizeof(((hello_t *)0)->app_sha)];
} s_img;

const char *esp32_fw_stage_begin(size_t size) {
    s_img.ready = false;
    s_img.part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "esp32_fw");
    if (!s_img.part) {
        return "No staging partition";
    }
    if (size > s_img.part->size) {
        return "Image too large";
    }
    // Up front: whole 64 KB blocks erase far faster than sector by sector.
    if (esp_partition_erase_range(s_img.part, 0, (size + SECTOR - 1) & ~(SECTOR - 1)) != ESP_OK) {
        return "Couldn't prepare the staging area";
    }
    s_img.size = size;
    s_img.written = 0;
    return NULL;
}

bool esp32_fw_stage_write(const void *buf, size_t len) {
    if (esp_partition_write(s_img.part, s_img.written, buf, len) != ESP_OK) {
        return false;
    }
    s_img.written += len;
    return true;
}

void esp32_fw_stage_abort(void) {
    s_img.ready = false;
}

const char *esp32_fw_stage_end(void) {
    static uint8_t buf[READ_CHUNK];
    esp_image_header_t header;
    esp_app_desc_t desc;
    if (s_img.written != s_img.size || s_img.size < DESC_OFFSET + sizeof(desc) + DIGEST_LEN ||
        esp_partition_read(s_img.part, 0, &header, sizeof(header)) != ESP_OK ||
        esp_partition_read(s_img.part, DESC_OFFSET, &desc, sizeof(desc)) != ESP_OK) {
        return "Image is damaged or incomplete";
    }
    if (!header.hash_appended) {
        return "Image has no checksum";
    }

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    const char *error = NULL;
    size_t body = s_img.size - DIGEST_LEN;
    for (size_t off = 0; off < body && !error; off += READ_CHUNK) {
        size_t n = body - off < READ_CHUNK ? body - off : READ_CHUNK;
        if (esp_partition_read(s_img.part, off, buf, n) != ESP_OK) {
            error = "Staging read failed";
        } else {
            mbedtls_sha256_update(&sha, buf, n);
        }
    }
    uint8_t appended[DIGEST_LEN], digest[DIGEST_LEN];
    if (!error && esp_partition_read(s_img.part, body, appended, DIGEST_LEN) != ESP_OK) {
        error = "Staging read failed";
    }
    if (!error) {
        // The image's digest stops short of itself; the one the ESP32
        // checks covers the whole file.
        mbedtls_sha256_context whole;
        mbedtls_sha256_init(&whole);
        mbedtls_sha256_clone(&whole, &sha);
        mbedtls_sha256_finish(&sha, digest);
        mbedtls_sha256_update(&whole, appended, DIGEST_LEN);
        mbedtls_sha256_finish(&whole, s_img.sha256);
        mbedtls_sha256_free(&whole);
        if (memcmp(digest, appended, DIGEST_LEN)) {
            error = "Image is damaged or incomplete";
        }
    }
    mbedtls_sha256_free(&sha);
    if (error) {
        return error;
    }
    memcpy(s_img.app_sha, desc.app_elf_sha256, sizeof(s_img.app_sha));
    s_img.ready = true;
    return NULL;
}

static const char *result_text(uint8_t result) {
    switch (result) {
    case FW_NOT_STARTED:
        return "The Bluetooth chip restarted during the update";
    case FW_TOO_LARGE:
        return "Image too large for the Bluetooth chip";
    case FW_FLASH_ERROR:
        return "The Bluetooth chip couldn't write it";
    case FW_BAD_CHECKSUM:
        return "Damaged on the way to the Bluetooth chip";
    case FW_BAD_IMAGE:
        return "The Bluetooth chip refused the image";
    default:
        return "The Bluetooth chip answered oddly";
    }
}

static bool request(uint8_t type, const void *payload, uint8_t len, uint32_t timeout_ms, fw_ack_t *ack) {
    for (int i = 0; i < RETRIES; i++) {
        if (uart_link_fw_request(type, payload, len, timeout_ms, ack)) {
            return true;
        }
        ESP_LOGW(TAG, "no ack for frame 0x%02x, resending", type);
    }
    return false;
}

static const char *send_image(void (*progress)(unsigned percent)) {
    fw_ack_t ack;
    fw_begin_t begin = {.size = s_img.size};
    memcpy(begin.sha256, s_img.sha256, sizeof(begin.sha256));
    if (!request(MSG_FW_BEGIN, &begin, sizeof(begin), BEGIN_TIMEOUT_MS, &ack)) {
        return "The Bluetooth chip isn't answering";
    }
    if (ack.result != FW_OK) {
        return result_text(ack.result);
    }
    fw_chunk_t chunk;
    unsigned shown = 0;
    for (uint32_t offset = 0; offset < s_img.size;) {
        uint32_t n = s_img.size - offset < FW_CHUNK_MAX_DATA ? s_img.size - offset : FW_CHUNK_MAX_DATA;
        chunk.offset = offset;
        if (esp_partition_read(s_img.part, offset, chunk.data, n) != ESP_OK) {
            return "Staging read failed";
        }
        if (!request(MSG_FW_CHUNK, &chunk, FW_CHUNK_HEADER + n, CHUNK_TIMEOUT_MS, &ack)) {
            return "The Bluetooth chip stopped answering";
        }
        if ((ack.result != FW_OK && ack.result != FW_OUT_OF_ORDER) || ack.next > s_img.size) {
            return result_text(ack.result);
        }
        offset = ack.next;
        unsigned percent = (unsigned)((uint64_t)offset * 100 / s_img.size);
        if (percent != shown) {
            shown = percent;
            progress(percent);
        }
    }
    return NULL;
}

const char *esp32_fw_install(void (*progress)(unsigned percent)) {
    if (!s_img.ready) {
        return "Nothing staged";
    }
    ESP_LOGI(TAG, "sending %u bytes", (unsigned)s_img.size);
    const char *error = send_image(progress);
    if (error) {
        return error;
    }
    hello_t hello;
    uint32_t hellos = uart_link_get_esp32_firmware(&hello);
    fw_ack_t ack;
    // Without an ack it may have restarted already; its hello says.
    if (request(MSG_FW_END, NULL, 0, END_TIMEOUT_MS, &ack) && ack.result != FW_OK) {
        return result_text(ack.result);
    }
    ESP_LOGI(TAG, "sent; waiting for the ESP32 to restart");
    for (int waited = 0; waited < RESTART_TIMEOUT_MS; waited += POLL_MS) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        if (uart_link_get_esp32_firmware(&hello) == hellos) {
            continue;
        }
        if (memcmp(hello.app_sha, s_img.app_sha, sizeof(hello.app_sha))) {
            return "The Bluetooth chip went back to its old firmware";
        }
        ESP_LOGI(TAG, "ESP32 runs %s", hello.version);
        return NULL;
    }
    return "The Bluetooth chip didn't come back";
}
