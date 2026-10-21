#include "a2dp_sink.h"
#include "app_state.h"
#include "avrcp/controller.h"
#include "avrcp/target.h"
#include "bt/app_core.h"
#include "bt/gap.h"
#include "bt/pairing.h"
#include "bt/power.h"
#include "driver/gpio.h"
#include "encoder.h"
#include "esp_avrc_api.h"
#include "esp_log.h"
#include "fw_update.h"
#include "state_sync.h"

static const char *TAG = "esp32";

// PCM5100A XSMT: low soft-mutes the DAC for both sources, since the S3's
// I2S reaches it through the same CH445P. Nothing on the board pulls it
// high; the factory firmware drove it.
#define DAC_XSMT_GPIO GPIO_NUM_32

static void on_command(protocol_cmd_t cmd, uint8_t value) {
    if (cmd == CMD_IDENTIFY) {
        state_sync_send_hello();
        return;
    }
    if (cmd == CMD_BT_ENABLE || cmd == CMD_BT_DISABLE) {
        bt_power_set_enabled(cmd == CMD_BT_ENABLE);
        return;
    }
    if (!bt_power_enabled()) {
        return;
    }
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
    case CMD_VOLUME_UP:
        avrcp_target_step_volume(1);
        break;
    case CMD_VOLUME_DOWN:
        avrcp_target_step_volume(-1);
        break;
    case CMD_VOLUME_SET:
        avrcp_target_set_local_volume(value > 100 ? 100 : value);
        break;
    case CMD_PAIRING_OPEN:
    case CMD_PAIRING_CLOSE:
    case CMD_PAIRING_ACCEPT:
    case CMD_PAIRING_REJECT:
    case CMD_FORGET_DEVICES:
        bt_pairing_command(cmd);
        break;
    default:
        ESP_LOGW(TAG, "unknown command 0x%02x", cmd);
        break;
    }
}

static void dac_unmute(void) {
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << DAC_XSMT_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    ESP_ERROR_CHECK(gpio_set_level(DAC_XSMT_GPIO, 1));
    // Through light sleep and the restart a Bluetooth toggle takes, so
    // AirPlay on the S3 keeps playing.
    ESP_ERROR_CHECK(gpio_hold_en(DAC_XSMT_GPIO));
}

// Only the device name, which this MCU can't use until Bluetooth is on.
static void ignore_name(const char *name) {
    (void)name;
}

void app_main(void) {
    dac_unmute();
    fw_update_init();
    app_state_init();
    if (!bt_power_enabled()) {
        state_sync_start(on_command, ignore_name);
        bt_power_sleep_forever();
    }
    bt_app_task_start_up();

    ESP_ERROR_CHECK(bt_gap_init());
    bt_pairing_init();

    esp_avrc_tg_register_callback(avrcp_target_bt_event);
    ESP_ERROR_CHECK(esp_avrc_tg_init());
    avrcp_target_init();

    esp_avrc_ct_register_callback(avrcp_controller_bt_event);
    ESP_ERROR_CHECK(esp_avrc_ct_init());
    avrcp_controller_init();

    esp_a2d_register_callback(a2dp_sink_bt_event);
    a2dp_sink_init();

    encoder_start();
    state_sync_start(on_command, bt_gap_set_name);

    ESP_LOGI(TAG, "A2DP sink ready");
}
