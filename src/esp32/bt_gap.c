#include "bt_gap.h"

#include <stdio.h>
#include <string.h>

#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_log.h"
#include "protocol.h"

static const char *TAG = "bt_gap";

static void bt_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
    switch (event) {
    case ESP_BT_GAP_AUTH_CMPL_EVT:
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "paired: %s", param->auth_cmpl.device_name);
        } else {
            ESP_LOGW(TAG, "pairing failed, status %d", param->auth_cmpl.stat);
        }
        break;
    case ESP_BT_GAP_CFM_REQ_EVT:
        // Just Works: auto-accept, no keypad/display to confirm a passkey with.
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        break;
    case ESP_BT_GAP_ACL_CONN_CMPL_STAT_EVT:
        ESP_LOGI(TAG, "ACL connected");
        break;
    case ESP_BT_GAP_ACL_DISCONN_CMPL_STAT_EVT:
        ESP_LOGI(TAG, "ACL disconnected, reason 0x%x", param->acl_disconn_cmpl_stat.reason);
        break;
    default:
        break;
    }
}

esp_err_t bt_gap_init(void) {
    esp_err_t err;
    // Classic BT only: BLE is unused here, so reclaim its controller memory.
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    if ((err = esp_bt_controller_init(&bt_cfg)) != ESP_OK) {
        return err;
    }
    if ((err = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT)) != ESP_OK) {
        return err;
    }

    esp_bluedroid_config_t bluedroid_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    if ((err = esp_bluedroid_init_with_cfg(&bluedroid_cfg)) != ESP_OK) {
        return err;
    }
    if ((err = esp_bluedroid_enable()) != ESP_OK) {
        return err;
    }

    esp_bt_gap_register_callback(bt_gap_cb);

    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;
    esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &iocap, sizeof(iocap));

    esp_bt_gap_set_device_name(AK_BT_DEVICE_NAME);

    return ESP_OK;
}

void bt_gap_set_discoverable(bool discoverable) {
    if (discoverable) {
        esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
    } else {
        esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
    }
}

void bt_gap_set_name(const char *name) {
    static char s_name[DEVICE_NAME_MAX + 1];
    // The S3 resends on every link resync; only log real changes.
    if (!strcmp(s_name, name)) {
        return;
    }
    snprintf(s_name, sizeof(s_name), "%s", name);
    esp_bt_gap_set_device_name(s_name);
    ESP_LOGI(TAG, "device name \"%s\"", s_name);
}
