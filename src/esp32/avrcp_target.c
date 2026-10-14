#include "avrcp_target.h"

#include <stdbool.h>

#include "app_state.h"
#include "bt_app_core.h"
#include "esp_log.h"

#define AVRC_VOLUME_MAX 0x7f

static const char *TAG = "avrcp_target";
static bool s_notify_pending;

static uint8_t avrc_to_pct(uint8_t avrc_vol) {
    return (uint8_t)((avrc_vol * 100 + AVRC_VOLUME_MAX / 2) / AVRC_VOLUME_MAX);
}

static uint8_t pct_to_avrc(uint8_t pct) {
    return (uint8_t)((pct * AVRC_VOLUME_MAX + 50) / 100);
}

static void handle_event(uint16_t event, void *param) {
    esp_avrc_tg_cb_param_t *rc = (esp_avrc_tg_cb_param_t *)param;

    switch (event) {
    case ESP_AVRC_TG_CONNECTION_STATE_EVT:
        ESP_LOGI(TAG, "AVRCP target %s", rc->conn_stat.connected ? "connected" : "disconnected");
        s_notify_pending = false;
        break;

    case ESP_AVRC_TG_SET_ABSOLUTE_VOLUME_CMD_EVT: {
        uint8_t pct = avrc_to_pct(rc->set_abs_vol.volume);
        ESP_LOGI(TAG, "controller set volume: %d%%", pct);
        app_state_set_volume(pct);
        break;
    }

    case ESP_AVRC_TG_REGISTER_NOTIFICATION_EVT:
        if (rc->reg_ntf.event_id == ESP_AVRC_RN_VOLUME_CHANGE) {
            s_notify_pending = true;
            esp_avrc_rn_param_t rn_param = {.volume = pct_to_avrc(app_state_get().volume)};
            esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_VOLUME_CHANGE, ESP_AVRC_RN_RSP_INTERIM, &rn_param);
        }
        break;

    default:
        break;
    }
}

void avrcp_target_init(void) {
    esp_avrc_rn_evt_cap_mask_t evt_set = {0};
    esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &evt_set, ESP_AVRC_RN_VOLUME_CHANGE);
    ESP_ERROR_CHECK(esp_avrc_tg_set_rn_evt_cap(&evt_set));
}

void avrcp_target_bt_event(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param) {
    bt_app_work_dispatch((bt_app_cb_t)handle_event, event, param, sizeof(esp_avrc_tg_cb_param_t), NULL, NULL);
}

void avrcp_target_step_volume(int steps) {
    int volume = (int)app_state_get().volume + steps;
    volume = volume < 0 ? 0 : (volume > 100 ? 100 : volume);
    avrcp_target_set_local_volume((uint8_t)volume);
}

void avrcp_target_set_local_volume(uint8_t volume_pct) {
    app_state_set_volume(volume_pct);

    if (!s_notify_pending) {
        return;
    }
    s_notify_pending = false;
    esp_avrc_rn_param_t rn_param = {.volume = pct_to_avrc(volume_pct)};
    esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_VOLUME_CHANGE, ESP_AVRC_RN_RSP_CHANGED, &rn_param);
}
