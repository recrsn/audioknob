#include "a2dp_sink.h"
#include "app_state.h"
#include "avrcp_controller.h"
#include "avrcp_target.h"
#include "bt_app_core.h"
#include "bt_gap.h"
#include "encoder.h"
#include "esp_avrc_api.h"
#include "esp_log.h"
#include "state_sync.h"

static const char *TAG = "esp32";

static void on_command(protocol_cmd_t cmd) {
    switch (cmd) {
    case CMD_PAUSE:
        // AirPlay took the DAC on the S3. Without an AVRCP link the phone
        // can't be paused, so drop it rather than leave it streaming into
        // a switch that no longer routes to the DAC.
        ESP_LOGI(TAG, "S3 requested pause");
        if (!avrcp_controller_pause()) {
            a2dp_sink_disconnect();
        }
        break;
    case CMD_PLAY_PAUSE:
        avrcp_controller_toggle_play_pause();
        break;
    case CMD_NEXT:
        avrcp_controller_next();
        break;
    case CMD_PREV:
        avrcp_controller_prev();
        break;
    default:
        ESP_LOGW(TAG, "unknown command 0x%02x", cmd);
        break;
    }
}

void app_main(void) {
    app_state_init();
    bt_app_task_start_up();

    ESP_ERROR_CHECK(bt_gap_init());

    esp_avrc_tg_register_callback(avrcp_target_bt_event);
    ESP_ERROR_CHECK(esp_avrc_tg_init());
    avrcp_target_init();

    esp_avrc_ct_register_callback(avrcp_controller_bt_event);
    ESP_ERROR_CHECK(esp_avrc_ct_init());
    avrcp_controller_init();

    esp_a2d_register_callback(a2dp_sink_bt_event);
    a2dp_sink_init();

    encoder_start();
    state_sync_start(on_command);

    bt_gap_set_discoverable(true);
    ESP_LOGI(TAG, "A2DP sink ready, discoverable as \"" AK_BT_DEVICE_NAME "\"");
}
