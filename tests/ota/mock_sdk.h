#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#define ESP_OK 0
#define MALLOC_CAP_INTERNAL 0
#define CONFIG_IDF_FIRMWARE_CHIP_ID 9
#define ESP_CHIP_ID_ESP32 0
#define ESP_IMAGE_HEADER_MAGIC 0xe9
#define ESP_APP_DESC_MAGIC_WORD 0xabcd5432
#define OTA_WITH_SEQUENTIAL_WRITES 0
#define ESP_OTA_IMG_PENDING_VERIFY 1
#define pdPASS 1
#define portMUX_INITIALIZER_UNLOCKED 0
#define taskENTER_CRITICAL(lock) ((void)(lock))
#define taskEXIT_CRITICAL(lock) ((void)(lock))
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_LOGW(tag, ...) ((void)(tag))
typedef int portMUX_TYPE;
typedef int esp_ota_handle_t;
typedef int esp_ota_img_states_t;
typedef struct {
    size_t size;
    const char *label;
} esp_partition_t;
typedef struct {
    uint8_t magic;
    uint16_t chip_id;
} esp_image_header_t;
typedef struct {
    uint32_t length;
} esp_image_segment_header_t;
typedef struct {
    uint32_t magic_word;
    char version[32];
} esp_app_desc_t;
typedef void *esp_http_client_handle_t;

void audio_source_suspend(bool suspend);
void *heap_caps_malloc(size_t size, unsigned caps);
const esp_partition_t *esp_ota_get_next_update_partition(const void *);
const esp_partition_t *esp_ota_get_running_partition(void);
int esp_ota_begin(const esp_partition_t *, int, esp_ota_handle_t *);
int esp_ota_write(esp_ota_handle_t, const void *, size_t);
int esp_ota_end(esp_ota_handle_t);
int esp_ota_abort(esp_ota_handle_t);
int esp_ota_set_boot_partition(const esp_partition_t *);
int esp_ota_get_state_partition(const esp_partition_t *, esp_ota_img_states_t *);
int esp_ota_mark_app_valid_cancel_rollback(void);
const esp_app_desc_t *esp_app_get_description(void);
void esp_restart(void);
int xTaskCreate(void (*)(void *), const char *, unsigned, void *, unsigned, void *);
void vTaskDelete(void *);
int esp_http_client_read(esp_http_client_handle_t, char *, size_t);
int64_t esp_http_client_get_content_length(esp_http_client_handle_t);
int esp_http_client_cleanup(esp_http_client_handle_t);

#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_8BIT 0
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
typedef void *TaskHandle_t;
typedef struct {
    char version[32];
} hello_t;
uint32_t ulTaskNotifyTake(int clear, uint32_t ticks);
void xTaskNotifyGive(TaskHandle_t task);
int64_t esp_timer_get_time(void);
uint32_t uart_link_get_esp32_firmware(hello_t *out);
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t client);

typedef int esp_err_t;
#define HTTP_TRANSPORT_OVER_SSL 2
#define HTTP_TRANSPORT_OVER_TCP 1
typedef struct {
    const char *url;
    int timeout_ms;
    int buffer_size_tx;
    int (*crt_bundle_attach)(void *);
    const char *user_agent;
    bool disable_auto_redirect;
} esp_http_client_config_t;
int esp_crt_bundle_attach(void *);
const char *esp_err_to_name(esp_err_t error);
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config);
int esp_http_client_set_header(esp_http_client_handle_t client, const char *name, const char *value);
int esp_http_client_get_transport_type(esp_http_client_handle_t client);
int esp_http_client_open(esp_http_client_handle_t client, int write_len);
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t client);
int esp_http_client_get_status_code(esp_http_client_handle_t client);
int esp_http_client_set_redirection(esp_http_client_handle_t client);
int esp_http_client_close(esp_http_client_handle_t client);
