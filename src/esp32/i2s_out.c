#include "i2s_out.h"

#include <inttypes.h>

#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

// ESP32_I2S_DAC_BCK/DIN/LRCK-WS — see repo spec's pin reference table.
#define I2S_BCK_GPIO 25
#define I2S_DIN_GPIO 26
#define I2S_WS_GPIO 27

static const char *TAG = "i2s_out";
static i2s_chan_handle_t s_tx_chan;

void i2s_out_init(void) {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx_chan, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCK_GPIO,
            .ws = I2S_WS_GPIO,
            .dout = I2S_DIN_GPIO,
            .din = I2S_GPIO_UNUSED,
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx_chan, &std_cfg));
    ESP_LOGI(TAG, "I2S TX ready (BCK=%d WS=%d DOUT=%d)", I2S_BCK_GPIO, I2S_WS_GPIO, I2S_DIN_GPIO);
}

void i2s_out_configure(uint32_t sample_rate_hz, uint8_t channels) {
    i2s_channel_disable(s_tx_chan);

    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate_hz);
    i2s_std_slot_config_t slot_cfg =
        I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, channels == 1 ? I2S_SLOT_MODE_MONO : I2S_SLOT_MODE_STEREO);
    ESP_ERROR_CHECK(i2s_channel_reconfig_std_clock(s_tx_chan, &clk_cfg));
    ESP_ERROR_CHECK(i2s_channel_reconfig_std_slot(s_tx_chan, &slot_cfg));

    ESP_LOGI(TAG, "reconfigured: %" PRIu32 " Hz, %d ch", sample_rate_hz, channels);
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx_chan));
}

void i2s_out_start(void) {
    i2s_channel_enable(s_tx_chan);
}

void i2s_out_stop(void) {
    i2s_channel_disable(s_tx_chan);
}

void i2s_out_write(const uint8_t *data, size_t len) {
    size_t written = 0;
    while (written < len) {
        size_t bytes_written = 0;
        esp_err_t err = i2s_channel_write(s_tx_chan, data + written, len - written, &bytes_written, portMAX_DELAY);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "i2s write failed: %s", esp_err_to_name(err));
            break;
        }
        written += bytes_written;
    }
}
