#include "bt_power.h"

#include "a2dp_sink.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "fw_update.h"
#include "nvs_flash.h"
#include "state_sync.h"

#define NVS_NAMESPACE "bt"
#define NVS_KEY "enabled"
// Long enough after a wake for the S3's answer to MSG_HELLO.
#define AWAKE_MS 500
// For the phone to hear the A2DP disconnect before the radio goes.
#define DISCONNECT_MS 300

static const char *TAG = "bt_power";

static int8_t s_enabled = -1;

bool bt_power_enabled(void) {
    if (s_enabled >= 0) {
        return s_enabled;
    }
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    uint8_t enabled = 1;
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u8(nvs, NVS_KEY, &enabled);
        nvs_close(nvs);
    }
    s_enabled = enabled != 0;
    return s_enabled;
}

void bt_power_set_enabled(bool enabled) {
    if (enabled == bt_power_enabled()) {
        return;
    }
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        ESP_LOGE(TAG, "can't open NVS");
        return;
    }
    nvs_set_u8(nvs, NVS_KEY, enabled);
    nvs_commit(nvs);
    nvs_close(nvs);

    ESP_LOGI(TAG, "Bluetooth %s, restarting", enabled ? "on" : "off");
    if (!enabled) {
        a2dp_sink_disconnect();
        vTaskDelay(pdMS_TO_TICKS(DISCONNECT_MS));
    }
    esp_restart();
}

void bt_power_sleep_forever(void) {
    ESP_LOGI(TAG, "Bluetooth off, sleeping until the S3 calls");
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(AWAKE_MS));
        if (!fw_update_busy()) {
            state_sync_light_sleep();
        }
    }
}
