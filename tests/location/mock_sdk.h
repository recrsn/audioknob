#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define ESP_OK 0
#define ESP_ERR_NVS_NOT_FOUND 1
#define NVS_READONLY 0
#define NVS_READWRITE 1
#define MALLOC_CAP_SPIRAM 1
#define portMAX_DELAY UINT32_MAX
#define configTICK_RATE_HZ 1000
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_LOGE(tag, ...) ((void)(tag))
#define pdPASS 1
typedef int esp_err_t;
typedef int nvs_handle_t;
typedef int StaticSemaphore_t;
typedef int *SemaphoreHandle_t;
typedef void *TaskHandle_t;
typedef struct { bool valid; char place[32]; char status[64]; } ui_weather_t;
typedef struct { void *(*malloc_fn)(size_t); void (*free_fn)(void *); } cJSON_Hooks;
void cJSON_InitHooks(cJSON_Hooks *hooks);
void *heap_caps_malloc(size_t size, unsigned caps);
SemaphoreHandle_t xSemaphoreCreateMutex(void);
int xSemaphoreTake(SemaphoreHandle_t sem, uint32_t ticks);
int xSemaphoreGive(SemaphoreHandle_t sem);
int xTaskCreate(void (*fn)(void *), const char *name, unsigned stack, void *arg, unsigned priority, TaskHandle_t *out);
void vTaskDelete(void *task);
void settings_nvs_init(void);
int nvs_open(const char *name, int mode, nvs_handle_t *out);
int nvs_get_blob(nvs_handle_t nvs, const char *key, void *out, size_t *len);
int nvs_set_blob(nvs_handle_t nvs, const char *key, const void *value, size_t len);
int nvs_erase_key(nvs_handle_t nvs, const char *key);
int nvs_commit(nvs_handle_t nvs);
void nvs_close(nvs_handle_t nvs);

typedef void *esp_timer_handle_t;
typedef struct { void (*callback)(void *); const char *name; } esp_timer_create_args_t;
int64_t esp_timer_get_time(void);
int esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *out);
int esp_timer_start_periodic(esp_timer_handle_t timer, uint64_t interval);
bool ntp_is_synced(void);
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage);
void vTaskDelay(uint32_t ticks);

typedef uint8_t StackType_t;
typedef int StaticTask_t;
TaskHandle_t xTaskCreateStatic(void (*fn)(void *), const char *name, unsigned stack,
                             void *arg, unsigned priority, StackType_t *buffer, StaticTask_t *tcb);
uint32_t ulTaskNotifyTake(int clear, uint32_t ticks);
void xTaskNotifyGive(TaskHandle_t task);
