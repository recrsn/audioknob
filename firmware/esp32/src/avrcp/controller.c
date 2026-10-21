#include "avrcp/controller.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "app_state.h"
#include "bt/app_core.h"
#include "esp_log.h"
#include "track_meta.h"

static const char *TAG = "avrcp_controller";
static bool s_connected;
static bool s_playing_guess;  // tracks what we last *told* the phone, not
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
    track_meta_handle_ct_event(event, rc);
}

// Bluedroid frees metadata text and cover-art data when its callback
// returns, so dispatched copies carry their own (freed by the free_cbs).
static uint8_t *dup_bytes(const uint8_t *src, int len) {
    uint8_t *copy = len > 0 && src ? malloc(len) : NULL;
    if (copy) {
        memcpy(copy, src, len);
    }
    return copy;
}

static void copy_metadata(void *dest, void *src, int len) {
    (void)len;
    esp_avrc_ct_cb_param_t *d = dest, *s = src;
    d->meta_rsp.attr_text = dup_bytes(s->meta_rsp.attr_text, s->meta_rsp.attr_length);
    if (!d->meta_rsp.attr_text) {
        d->meta_rsp.attr_length = 0;
    }
}

static void free_metadata(void *param) {
    free(((esp_avrc_ct_cb_param_t *)param)->meta_rsp.attr_text);
}

static void copy_art_data(void *dest, void *src, int len) {
    (void)len;
    esp_avrc_ct_cb_param_t *d = dest, *s = src;
    d->cover_art_data.p_data = dup_bytes(s->cover_art_data.p_data, s->cover_art_data.data_len);
    if (!d->cover_art_data.p_data && s->cover_art_data.data_len) {
        d->cover_art_data.status = ESP_BT_STATUS_NOMEM;
    }
}

static void free_art_data(void *param) {
    free(((esp_avrc_ct_cb_param_t *)param)->cover_art_data.p_data);
}

void avrcp_controller_init(void) {
    s_tl = 0;
}

void avrcp_controller_bt_event(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param) {
    bt_app_copy_cb_t copy_cb = NULL;
    bt_app_free_cb_t free_cb = NULL;
    if (event == ESP_AVRC_CT_METADATA_RSP_EVT) {
        copy_cb = copy_metadata;
        free_cb = free_metadata;
    } else if (event == ESP_AVRC_CT_COVER_ART_DATA_EVT) {
        copy_cb = copy_art_data;
        free_cb = free_art_data;
    }
    bt_app_work_dispatch((bt_app_cb_t)handle_event, event, param, sizeof(esp_avrc_ct_cb_param_t), copy_cb, free_cb);
}

uint8_t avrcp_controller_alloc_tl(void) {
    return (s_tl++) & ESP_AVRC_TRANS_LABEL_MAX;
}

static void send_passthrough(uint8_t key_code) {
    uint8_t tl = avrcp_controller_alloc_tl();
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

void avrcp_controller_next(void) {
    if (s_connected) {
        send_passthrough(ESP_AVRC_PT_CMD_FORWARD);
    }
}

void avrcp_controller_prev(void) {
    if (s_connected) {
        send_passthrough(ESP_AVRC_PT_CMD_BACKWARD);
    }
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
