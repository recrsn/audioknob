#include "a2dp_sink.h"

#include <stdbool.h>
#include <string.h>

#include "app_state.h"
#include "bt_app_core.h"
#include "bt_gap.h"
#include "esp_log.h"
#include "i2s_out.h"

static const char *TAG = "a2dp_sink";

static esp_bd_addr_t s_peer;
static volatile bool s_has_peer;

static void handle_event(uint16_t event, void *param) {
    esp_a2d_cb_param_t *a2d = (esp_a2d_cb_param_t *)param;

    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT: {
        bool connected = a2d->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED;
        ESP_LOGI(TAG, "connection state: %d", a2d->conn_stat.state);
        app_state_set_connected(connected);
        if (connected) {
            memcpy(s_peer, a2d->conn_stat.remote_bda, sizeof(s_peer));
        }
        s_has_peer = connected;
        // Stop advertising once paired so a second phone can't hijack the
        // link; go visible again once this one drops.
        bt_gap_set_discoverable(!connected && a2d->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED);
        if (!connected) {
            i2s_out_stop();
        }
        break;
    }

    case ESP_A2D_AUDIO_STATE_EVT: {
        bool started = a2d->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED;
        ESP_LOGI(TAG, "audio state: %d", a2d->audio_stat.state);
        app_state_set_playing(started);
        if (started) {
            i2s_out_start();
        } else {
            i2s_out_stop();
        }
        break;
    }

    case ESP_A2D_AUDIO_CFG_EVT: {
        esp_a2d_mcc_t *mcc = &a2d->audio_cfg.mcc;
        if (mcc->type != ESP_A2D_MCT_SBC) {
            ESP_LOGW(TAG, "unsupported codec type %d", mcc->type);
            break;
        }
        uint32_t sample_rate = 44100;
        if (mcc->cie.sbc_info.samp_freq & ESP_A2D_SBC_CIE_SF_16K) {
            sample_rate = 16000;
        } else if (mcc->cie.sbc_info.samp_freq & ESP_A2D_SBC_CIE_SF_32K) {
            sample_rate = 32000;
        } else if (mcc->cie.sbc_info.samp_freq & ESP_A2D_SBC_CIE_SF_48K) {
            sample_rate = 48000;
        }
        uint8_t channels = (mcc->cie.sbc_info.ch_mode & ESP_A2D_SBC_CIE_CH_MODE_MONO) ? 1 : 2;
        i2s_out_configure(sample_rate, channels);
        break;
    }

    default:
        break;
    }
}

static void data_cb(const uint8_t *data, uint32_t len) {
    // Runs on the Bluedroid stack's own task; write is a blocking DMA queue
    // push, which is fine here since this is the only consumer of that data.
    i2s_out_write(data, len);
}

void a2dp_sink_init(void) {
    i2s_out_init();
    ESP_ERROR_CHECK(esp_a2d_sink_init());
    esp_a2d_sink_register_data_callback(data_cb);
}

void a2dp_sink_disconnect(void) {
    if (s_has_peer) {
        esp_a2d_sink_disconnect(s_peer);
    }
}

void a2dp_sink_bt_event(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param) {
    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT:
    case ESP_A2D_AUDIO_STATE_EVT:
    case ESP_A2D_AUDIO_CFG_EVT:
        bt_app_work_dispatch((bt_app_cb_t)handle_event, event, param, sizeof(esp_a2d_cb_param_t), NULL, NULL);
        break;
    default:
        break;
    }
}
