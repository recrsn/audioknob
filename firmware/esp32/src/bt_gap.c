#include "bt_gap.h"

#include <stdio.h>
#include <string.h>

#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "app_state.h"
#include "bt_pairing.h"
#include "esp_log.h"
#include "protocol.h"

static const char *TAG = "bt_gap";

static void bt_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
    switch (event) {
    case ESP_BT_GAP_AUTH_CMPL_EVT:
    case ESP_BT_GAP_CFM_REQ_EVT:
    case ESP_BT_GAP_KEY_NOTIF_EVT:
    case ESP_BT_GAP_KEY_REQ_EVT:
    case ESP_BT_GAP_PIN_REQ_EVT:
    case ESP_BT_GAP_REMOVE_BOND_DEV_COMPLETE_EVT:
        bt_pairing_gap_event(event, param);
        break;
    case ESP_BT_GAP_READ_REMOTE_NAME_EVT:
        // Asked for by a2dp_sink once a phone connects.
        if (param->read_rmt_name.stat == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "connected to \"%s\"", (const char *)param->read_rmt_name.rmt_name);
            app_state_set_peer_name((const char *)param->read_rmt_name.rmt_name);
        }
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

    // Display + yes/no (the S3's screen and knob): phones then pair by
    // numeric comparison, which resists a man in the middle, rather than
    // Just Works.
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_IO;
    esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &iocap, sizeof(iocap));
    // Refuses the short keys a KNOB attack negotiates down to.
    esp_bt_gap_set_min_enc_key_size(ESP_BT_ENC_KEY_SIZE_CTRL_MAX);

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
