#ifndef AK_NATIVE

#include "audio_out.h"

#include "driver/i2s_std.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

// S3_I2S_DAC_BCK/LRCK-WS/DIN -- see spec.md's pin reference table.
#define I2S_BCK_GPIO 39
#define I2S_WS_GPIO 40
#define I2S_DOUT_GPIO 41

#define DMA_DESC_NUM 6
#define DMA_FRAME_NUM 240
#define FRAME_BYTES (2 * sizeof(int16_t))

static const char *TAG = "audio_out";
static i2s_chan_handle_t s_tx_chan;

// Output cursor: submitted advances on each write, sent from the DMA
// completion ISR. Their difference is what is queued ahead of the next
// write. With auto_clear the DMA keeps clocking out silence when the writer
// stalls, so sent can overtake submitted; that excess is folded into lost
// so the queue depth never goes negative.
static uint64_t s_submitted;
static uint64_t s_sent;
static uint64_t s_lost;
static uint32_t s_underruns;
static bool s_had_queue;

static bool IRAM_ATTR on_sent(i2s_chan_handle_t handle, i2s_event_data_t *event, void *ctx) {
    (void)handle;
    (void)ctx;
    __atomic_add_fetch(&s_sent, event->size / FRAME_BYTES, __ATOMIC_RELAXED);
    return false;
}

void audio_out_init(void) {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = DMA_DESC_NUM;
    chan_cfg.dma_frame_num = DMA_FRAME_NUM;
    chan_cfg.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx_chan, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_OUT_SAMPLE_RATE),
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
    const i2s_event_callbacks_t callbacks = {.on_sent = on_sent};
    ESP_ERROR_CHECK(i2s_channel_register_event_callback(s_tx_chan, &callbacks, NULL));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx_chan));
    ESP_LOGI(TAG, "I2S TX ready (BCK=%d WS=%d DOUT=%d)", I2S_BCK_GPIO, I2S_WS_GPIO, I2S_DOUT_GPIO);
}

void audio_out_write(const int16_t *pcm, size_t frames) {
    const uint8_t *data = (const uint8_t *)pcm;
    size_t len = frames * FRAME_BYTES;
    size_t written = 0;

    while (written < len) {
        size_t n = 0;
        esp_err_t err = i2s_channel_write(s_tx_chan, data + written, len - written, &n, portMAX_DELAY);
        __atomic_add_fetch(&s_submitted, n / FRAME_BYTES, __ATOMIC_RELAXED);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "i2s write failed: %s", esp_err_to_name(err));
            return;
        }
        written += n;
    }
}

uint32_t audio_out_queued_us(int64_t *now_us) {
    uint64_t submitted = __atomic_load_n(&s_submitted, __ATOMIC_RELAXED);
    uint64_t sent = __atomic_load_n(&s_sent, __ATOMIC_RELAXED);
    *now_us = esp_timer_get_time();

    uint64_t queued = 0;
    if (sent > submitted + s_lost) {
        s_lost = sent - submitted;
        // Only a dry ring while audio was queued is an underrun; idling
        // between streams runs it dry by design.
        if (s_had_queue) {
            s_underruns++;
        }
    } else {
        queued = submitted + s_lost - sent;
    }
    s_had_queue = queued > 0;

    // sent only advances a whole descriptor at a time, so on average half of
    // the one playing has already been heard.
    queued = queued > DMA_FRAME_NUM / 2 ? queued - DMA_FRAME_NUM / 2 : 0;
    return (uint32_t)(queued * 1000000 / AUDIO_OUT_SAMPLE_RATE);
}

uint32_t audio_out_underruns(void) {
    return s_underruns;
}

#endif  // !AK_NATIVE
