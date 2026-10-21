#include "mock_sdk.h"
#include "mock_sha.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "ota.h"

static uint8_t image[8192], expected_digest[32];
static size_t offset, available;
static bool suspended, create_task = true;
static unsigned erases, writes;
static void (*worker)(void *);
static void *worker_arg;
static esp_partition_t partition = {.size = 16384, .label = "ota_1"};

void audio_source_suspend(bool value) { suspended = value; }
void *heap_caps_malloc(size_t size, unsigned caps) {
    (void)caps;
    return malloc(size);
}
const esp_partition_t *esp_ota_get_next_update_partition(const void *p) {
    (void)p;
    return &partition;
}
const esp_partition_t *esp_ota_get_running_partition(void) { return &partition; }
int esp_ota_begin(const esp_partition_t *p, int mode, esp_ota_handle_t *handle) {
    (void)p;
    (void)mode;
    *handle = 1;
    erases++;
    return ESP_OK;
}
int esp_ota_write(esp_ota_handle_t handle, const void *buf, size_t size) {
    (void)handle;
    (void)buf;
    (void)size;
    writes++;
    return ESP_OK;
}
int esp_ota_end(esp_ota_handle_t handle) {
    (void)handle;
    return ESP_OK;
}
int esp_ota_abort(esp_ota_handle_t handle) {
    (void)handle;
    return ESP_OK;
}
int esp_ota_set_boot_partition(const esp_partition_t *p) {
    (void)p;
    return ESP_OK;
}
int esp_ota_get_state_partition(const esp_partition_t *p, esp_ota_img_states_t *state) {
    (void)p;
    *state = 0;
    return ESP_OK;
}
int esp_ota_mark_app_valid_cancel_rollback(void) { return ESP_OK; }
const esp_app_desc_t *esp_app_get_description(void) {
    static esp_app_desc_t d;
    return &d;
}
void esp_restart(void) { assert(!"test must not restart before device confirmation"); }
int xTaskCreate(void (*fn)(void *), const char *name, unsigned stack, void *arg, unsigned priority,
                void *out) {
    (void)name;
    (void)stack;
    (void)priority;
    (void)out;
    if (!create_task) {
        return 0;
    }
    worker = fn;
    worker_arg = arg;
    return pdPASS;
}
void vTaskDelete(void *task) { (void)task; }
const char *esp32_fw_stage_begin(size_t size) {
    (void)size;
    erases++;
    return NULL;
}
bool esp32_fw_stage_write(const void *data, size_t size) {
    (void)data;
    (void)size;
    writes++;
    return true;
}
const char *esp32_fw_stage_end(void) { return NULL; }
void esp32_fw_stage_abort(void) {}
const char *esp32_fw_install(void (*progress)(unsigned)) {
    (void)progress;
    return NULL;
}

static int read_image(void *ctx, char *buf, size_t len) {
    (void)ctx;
    // Short reads exercise read_full(), not only one-shot transfers.
    if (len > 71) {
        len = 71;
    }
    if (len > available - offset) {
        len = available - offset;
    }
    memcpy(buf, image + offset, len);
    offset += len;
    return (int)len;
}
esp_http_client_handle_t https_download_open(const char *url, int *status) {
    (void)url;
    *status = 200;
    return (void *)1;
}
int esp_http_client_read(esp_http_client_handle_t client, char *buf, size_t len) {
    return read_image(client, buf, len);
}
int64_t esp_http_client_get_content_length(esp_http_client_handle_t client) {
    (void)client;
    return available;
}
int esp_http_client_cleanup(esp_http_client_handle_t client) {
    (void)client;
    return ESP_OK;
}

static void prepare(unsigned chip) {
    memset(image, 0, sizeof(image));
    esp_image_header_t header = {.magic = ESP_IMAGE_HEADER_MAGIC, .chip_id = chip};
    esp_app_desc_t desc = {.magic_word = ESP_APP_DESC_MAGIC_WORD, .version = "1.2.3"};
    memcpy(image, &header, sizeof(header));
    memcpy(image + sizeof(header) + sizeof(esp_image_segment_header_t), &desc, sizeof(desc));
    offset = 0;
    available = sizeof(image);
    assert(EVP_Digest(image, sizeof(image), expected_digest, NULL, EVP_sha256(), NULL) == 1);
}
static ota_status_t state(void) {
    ota_status_t status;
    ota_get_status(&status);
    return status;
}
int main(void) {
    prepare(CONFIG_IDF_FIRMWARE_CHIP_ID);
    assert(ota_receive(sizeof(image), read_image, NULL));
    assert(state().state == OTA_READY && state().target == OTA_TARGET_S3 && suspended);
    assert(erases == 1 && writes == 2 && state().progress == 100);
    assert(!ota_receive(sizeof(image), read_image, NULL));
    assert(!ota_download_reserve(OTA_TARGET_S3));
    assert(state().state == OTA_READY);
    ota_decline();
    assert(!suspended && state().state == OTA_DECLINED);

    prepare(ESP_CHIP_ID_ESP32);
    assert(ota_download_reserve(OTA_TARGET_S3));
    assert(state().state == OTA_RECEIVING);
    assert(!ota_receive(sizeof(image), read_image, NULL));
    ota_download("https://github.com/fw.bin", sizeof(image), OTA_TARGET_S3, expected_digest);
    assert(state().state == OTA_FAILED && strstr(state().error, "wrong chip"));
    assert(erases == 1 && !suspended); // Wrong-chip release never erases flash.

    prepare(42);
    assert(!ota_receive(sizeof(image), read_image, NULL));
    assert(erases == 1 && state().state == OTA_FAILED && !suspended);
    prepare(CONFIG_IDF_FIRMWARE_CHIP_ID);
    available = 50;
    assert(!ota_receive(sizeof(image), read_image, NULL));
    assert(erases == 1 && !suspended);

    prepare(CONFIG_IDF_FIRMWARE_CHIP_ID);
    assert(ota_download_reserve(OTA_TARGET_S3));
    ota_download("https://github.com/fw.bin", sizeof(image) + 1, OTA_TARGET_S3, expected_digest);
    assert(!worker); // Downloads reuse the caller, without a new task.
    assert(state().state == OTA_FAILED && strstr(state().error, "size changed"));
    assert(erases == 1);

    prepare(ESP_CHIP_ID_ESP32);
    assert(ota_download_reserve(OTA_TARGET_ESP32));
    ota_download("https://github.com/fw.bin", sizeof(image), OTA_TARGET_ESP32, expected_digest);
    assert(!worker); // Downloads reuse the caller, without a new task.
    assert(state().state == OTA_READY && state().target == OTA_TARGET_ESP32);
    assert(erases == 2 && writes == 4);
    ota_decline();
    prepare(CONFIG_IDF_FIRMWARE_CHIP_ID);
    image[400] ^= 1; // Same length and valid header, but different release bytes.
    assert(ota_download_reserve(OTA_TARGET_S3));
    ota_download("https://github.com/fw.bin", sizeof(image), OTA_TARGET_S3, expected_digest);
    assert(!worker); // Downloads reuse the caller, without a new task.
    assert(state().state == OTA_FAILED && strstr(state().error, "checksum mismatch"));
    assert(!suspended);
    // The correct full-file digest is accepted for both image types.
    prepare(CONFIG_IDF_FIRMWARE_CHIP_ID);
    assert(ota_download_reserve(OTA_TARGET_S3));
    ota_download("https://github.com/fw.bin", sizeof(image), OTA_TARGET_S3, expected_digest);
    assert(!worker); // Downloads reuse the caller, without a new task.
    assert(state().state == OTA_READY);
    create_task=false; ota_install();
    assert(state().state==OTA_FAILED && !suspended);
    create_task=true;
    puts("OTA reservation, confirmation, target validation and transfer failures passed");
}
