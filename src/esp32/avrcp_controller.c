#include "avrcp_controller.h"

#include <stdbool.h>

#include "app_state.h"
#include "bt_app_core.h"
#include "esp_log.h"

static const char *TAG = "avrcp_controller";
static bool s_connected;
static bool s_playing_guess;  // no metadata/registration wired up yet, so this
                               // tracks what we last *told* the phone, not
                               // ground truth reported back by it
static uint8_t s_tl;

static void handle_event(uint16_t event, void *param) {
    esp_avrc_ct_cb_param_t *rc = (esp_avrc_ct_cb_param_t *)param;

    switch (event) {
    case ESP_AVRC_CT_CONNECTION_STATE_EVT:
        s_connected = rc->conn_stat.connected;
        s_playing_guess = false;
        ESP_LOGI(TAG, "AVRCP controller %s", s_connected ? "connected" : "disconnected");
        break;

    case ESP_AVRC_CT_PASSTHROUGH_RSP_EVT:
        if (rc->psth_rsp.rsp_code != ESP_AVRC_RSP_ACCEPT && rc->psth_rsp.rsp_code != ESP_AVRC_RSP_INTERIM) {
            ESP_LOGW(TAG, "passthrough 0x%x rejected", rc->psth_rsp.key_code);
        }
        break;

    default:
        break;
    }
}

void avrcp_controller_init(void) {
    s_tl = 0;
}

void avrcp_controller_bt_event(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param) {
    bt_app_work_dispatch((bt_app_cb_t)handle_event, event, param, sizeof(esp_avrc_ct_cb_param_t), NULL, NULL);
}

static void send_passthrough(uint8_t key_code) {
    uint8_t tl = (s_tl++) & 0x0f;
    esp_avrc_ct_send_passthrough_cmd(tl, key_code, ESP_AVRC_PT_CMD_STATE_PRESSED);
    esp_avrc_ct_send_passthrough_cmd(tl, key_code, ESP_AVRC_PT_CMD_STATE_RELEASED);
}

void avrcp_controller_toggle_play_pause(void) {
    if (!s_connected) {
        return;
    }

    uint8_t key_code = s_playing_guess ? ESP_AVRC_PT_CMD_PAUSE : ESP_AVRC_PT_CMD_PLAY;
    s_playing_guess = !s_playing_guess;
    app_state_set_playing(s_playing_guess);
    send_passthrough(key_code);
}

bool avrcp_controller_pause(void) {
    if (!s_connected) {
        return false;
    }

    // app_state's playing flag follows on its own once the phone suspends
    // the stream (ESP_A2D_AUDIO_STATE_EVT).
    s_playing_guess = false;
    send_passthrough(ESP_AVRC_PT_CMD_PAUSE);
    return true;
}
