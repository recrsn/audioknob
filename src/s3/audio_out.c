#include "audio_out.h"

#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

// S3_I2S_DAC_BCK/LRCK-WS/DIN -- see spec.md's pin reference table.
#define I2S_BCK_GPIO 39
#define I2S_WS_GPIO 40
#define I2S_DOUT_GPIO 41

#define SAMPLE_RATE 44100
#define DMA_DESC_NUM 6
#define DMA_FRAME_NUM 240

static const char *TAG = "audio_out";
static i2s_chan_handle_t s_tx_chan;

void audio_out_init(void) {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = DMA_DESC_NUM;
    chan_cfg.dma_frame_num = DMA_FRAME_NUM;
    chan_cfg.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx_chan, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCK_GPIO,
            .ws = I2S_WS_GPIO,
            .dout = I2S_DOUT_GPIO,
            .din = I2S_GPIO_UNUSED,
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx_chan));
    ESP_LOGI(TAG, "I2S TX ready (BCK=%d WS=%d DOUT=%d)", I2S_BCK_GPIO, I2S_WS_GPIO, I2S_DOUT_GPIO);
}

void audio_out_write(const int16_t *pcm, size_t frames) {
    const uint8_t *data = (const uint8_t *)pcm;
    size_t len = frames * 2 * sizeof(int16_t);
    size_t written = 0;

    while (written < len) {
        size_t n = 0;
        esp_err_t err = i2s_channel_write(s_tx_chan, data + written, len - written, &n, portMAX_DELAY);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "i2s write failed: %s", esp_err_to_name(err));
            return;
        }
        written += n;
    }
}

uint32_t audio_out_latency_ms(void) {
    return (DMA_DESC_NUM * DMA_FRAME_NUM * 1000) / SAMPLE_RATE;
}
