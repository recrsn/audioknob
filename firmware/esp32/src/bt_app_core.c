#include "bt_app_core.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "bt_app_core";

typedef struct {
    uint16_t event;
    bt_app_cb_t cb;
    bt_app_free_cb_t free_cb;
    void *param;
} bt_app_msg_t;

static QueueHandle_t s_task_queue;

static void bt_app_task_handler(void *arg) {
    bt_app_msg_t msg;
    for (;;) {
        if (xQueueReceive(s_task_queue, &msg, portMAX_DELAY) == pdTRUE) {
            if (msg.cb) {
                msg.cb(msg.event, msg.param);
            }
            if (msg.param) {
                if (msg.free_cb) {
                    msg.free_cb(msg.param);
                }
                free(msg.param);
            }
        }
    }
}

bool bt_app_work_dispatch(bt_app_cb_t cb, uint16_t event, void *params, int param_len,
                           bt_app_copy_cb_t copy_cb, bt_app_free_cb_t free_cb) {
    bt_app_msg_t msg = {.event = event, .cb = cb, .free_cb = free_cb};

    if (param_len > 0 && params) {
        msg.param = malloc(param_len);
        if (!msg.param) {
            return false;
        }
        memcpy(msg.param, params, param_len);
        if (copy_cb) {
            copy_cb(msg.param, params, param_len);
        }
    }

    if (xQueueSend(s_task_queue, &msg, pdMS_TO_TICKS(10)) != pdTRUE) {
        ESP_LOGE(TAG, "work queue full, dropping event 0x%x", event);
        if (msg.param) {
            if (free_cb) {
                free_cb(msg.param);
            }
            free(msg.param);
        }
        return false;
    }
    return true;
}

void bt_app_task_start_up(void) {
    s_task_queue = xQueueCreate(10, sizeof(bt_app_msg_t));
    xTaskCreate(bt_app_task_handler, "bt_app_task", 4096, NULL, 10, NULL);
}
