#include "mic.h"

#include <string.h>

#include "esp_timer.h"
#include "mic_process.h"

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

static const char *TAG = "mic";
static i2s_chan_handle_t s_rx_chan;
static SemaphoreHandle_t s_owner;
static bool s_raw;
static mic_process_t *s_process;
static int16_t s_input[MIC_PROCESS_FRAMES], s_output[MIC_PROCESS_FRAMES];
static size_t s_input_frames, s_output_pos, s_output_frames;
static int64_t s_process_us;
static uint32_t s_process_blocks;

void mic_init(void) {
    s_owner = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(s_owner ? ESP_OK : ESP_ERR_NO_MEM);
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

static void apply_slot(bool right) {
    i2s_pdm_rx_slot_config_t slot = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
    slot.slot_mask = right ? I2S_PDM_SLOT_RIGHT : I2S_PDM_SLOT_LEFT;
    ESP_ERROR_CHECK(i2s_channel_reconfig_pdm_rx_slot(s_rx_chan, &slot));
}

bool mic_start(const mic_config_t *config) {
    static const mic_config_t defaults = {.gain_db = MIC_DEFAULT_GAIN_DB, .noise_level = MIC_DEFAULT_NOISE_LEVEL};
    const mic_config_t *cfg = config ? config : &defaults;
    if (cfg->gain_db > MIC_MAX_GAIN_DB || cfg->noise_level > MIC_MAX_NOISE_LEVEL || xSemaphoreTake(s_owner, 0) != pdTRUE) {
        return false;
    }
    s_raw = cfg->raw;
    if (!s_raw) {
        s_process = mic_process_create(cfg->gain_db, cfg->noise_level);
        if (!s_process) {
            ESP_LOGW(TAG, "no memory for microphone processing");
            xSemaphoreGive(s_owner);
            return false;
        }
    }
    apply_slot(cfg->right_slot);
    s_input_frames = s_output_pos = s_output_frames = 0;
    s_process_us = s_process_blocks = 0;
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx_chan));
    if (!s_raw) {
        // Let the PDM decimator settle before exposing the first audio block.
        for (int i = 0; i < 5; i++) {
            size_t bytes = 0;
            i2s_channel_read(s_rx_chan, s_input, sizeof(s_input), &bytes, 30);
            if (bytes == sizeof(s_input)) {
                mic_process_block(s_process, s_input, s_output);
            }
        }
    }
    ESP_LOGI(TAG, "capture: gain %u dB, NS %s, raw %d", cfg->gain_db,
             cfg->noise_level && !s_raw ? "ESP-SR" : "off", s_raw);
    return true;
}

void mic_stop(void) {
    i2s_channel_disable(s_rx_chan);
    mic_process_destroy(s_process);
    s_process = NULL;
    if (s_process_blocks) {
        ESP_LOGI(TAG, "processing %lld us per 10 ms block (%.1f%% of a core)",
                 s_process_us / s_process_blocks, (double)s_process_us / (s_process_blocks * 100.0));
    }
    xSemaphoreGive(s_owner);
}

static size_t read_raw(int16_t *pcm, size_t frames, uint32_t timeout_ms) {
    size_t bytes = 0;
    esp_err_t err = i2s_channel_read(s_rx_chan, pcm, frames * sizeof(int16_t), &bytes, timeout_ms);
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
        ESP_LOGW(TAG, "read failed: %s", esp_err_to_name(err));
    }
    return bytes / sizeof(int16_t);
}

size_t mic_read(int16_t *pcm, size_t frames, uint32_t timeout_ms) {
    if (s_raw) {
        return read_raw(pcm, frames, timeout_ms);
    }
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    size_t got = 0;
    while (got < frames) {
        size_t pending = s_output_frames - s_output_pos;
        if (pending) {
            size_t n = frames - got < pending ? frames - got : pending;
            memcpy(pcm + got, s_output + s_output_pos, n * sizeof(int16_t));
            s_output_pos += n;
            got += n;
            continue;
        }
        int64_t remaining = deadline - esp_timer_get_time();
        if (remaining <= 0) {
            break;
        }
        size_t n = read_raw(s_input + s_input_frames, MIC_PROCESS_FRAMES - s_input_frames,
                            (uint32_t)((remaining + 999) / 1000));
        if (!n) {
            break;
        }
        s_input_frames += n;
        if (s_input_frames == MIC_PROCESS_FRAMES) {
            int64_t start = esp_timer_get_time();
            mic_process_block(s_process, s_input, s_output);
            s_process_us += esp_timer_get_time() - start;
            s_process_blocks++;
            s_input_frames = s_output_pos = 0;
            s_output_frames = MIC_PROCESS_FRAMES;
        }
    }
    return got;
}

float mic_processing_cpu_pct(void) {
    return s_process_blocks ? (float)s_process_us / (s_process_blocks * 100.0f) : 0;
}
