#include "mic.h"

#include "driver/i2s_pdm.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// PDM_MIC_SCK/DATA -- see spec.md's pin reference table. Both are strapping
// pins, sampled at reset before this code runs; the channel is only enabled
// while recording, so the clock is idle across restarts.
#define MIC_CLK_GPIO 45
#define MIC_DATA_GPIO 46

#define DMA_FRAME_NUM 320 // 20 ms
#define DMA_DESC_NUM (MIC_BUFFER_MS / 20)

// One-pole DC blocker, pole at 0.995 (~13 Hz at 16 kHz), Q15. The S3's
// PDM filter has no high-pass of its own.
#define DC_POLE_Q15 32604

static const char *TAG = "mic";
static i2s_chan_handle_t s_rx_chan;
static SemaphoreHandle_t s_owner;
static bool s_raw;
static bool s_right;
static int32_t s_dc_x1;
static int32_t s_dc_y1;

void mic_init(void) {
    s_owner = xSemaphoreCreateMutex();
    // PDM RX exists only on I2S0 on the S3.
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = DMA_DESC_NUM;
    chan_cfg.dma_frame_num = DMA_FRAME_NUM;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &s_rx_chan));

    i2s_pdm_rx_config_t cfg = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = MIC_CLK_GPIO,
            .din = MIC_DATA_GPIO,
        },
    };
    // 128x oversampling: a 2.048 MHz clock, mid-range for the mic, where
    // the default 64x (1.024 MHz) sits at its floor.
    cfg.clk_cfg.dn_sample_mode = I2S_PDM_DSR_16S;
    ESP_ERROR_CHECK(i2s_channel_init_pdm_rx_mode(s_rx_chan, &cfg));
    ESP_LOGI(TAG, "PDM RX ready (CLK=%d DATA=%d)", MIC_CLK_GPIO, MIC_DATA_GPIO);
}

static void apply_slot(void) {
    i2s_pdm_rx_slot_config_t slot = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
    slot.slot_mask = s_right ? I2S_PDM_SLOT_RIGHT : I2S_PDM_SLOT_LEFT;
    ESP_ERROR_CHECK(i2s_channel_reconfig_pdm_rx_slot(s_rx_chan, &slot));
}

bool mic_start(void) {
    if (xSemaphoreTake(s_owner, 0) != pdTRUE) {
        return false;
    }
    apply_slot();
    s_dc_x1 = 0;
    s_dc_y1 = 0;
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx_chan));
    return true;
}

void mic_stop(void) {
    i2s_channel_disable(s_rx_chan);
    xSemaphoreGive(s_owner);
}

static void dc_block(int16_t *pcm, size_t frames) {
    for (size_t i = 0; i < frames; i++) {
        int32_t x = pcm[i];
        int32_t y = x - s_dc_x1 + ((s_dc_y1 * DC_POLE_Q15) >> 15);
        s_dc_x1 = x;
        s_dc_y1 = y;
        pcm[i] = y > INT16_MAX ? INT16_MAX : y < INT16_MIN ? INT16_MIN : (int16_t)y;
    }
}

size_t mic_read(int16_t *pcm, size_t frames, uint32_t timeout_ms) {
    size_t bytes = 0;
    esp_err_t err = i2s_channel_read(s_rx_chan, pcm, frames * sizeof(int16_t), &bytes, pdMS_TO_TICKS(timeout_ms));
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
        ESP_LOGW(TAG, "read failed: %s", esp_err_to_name(err));
    }
    size_t got = bytes / sizeof(int16_t);
    if (!s_raw) {
        dc_block(pcm, got);
    }
    return got;
}

void mic_set_raw(bool raw) {
    s_raw = raw;
}

void mic_set_right_slot(bool right) {
    s_right = right;
}
