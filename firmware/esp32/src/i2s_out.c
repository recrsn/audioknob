#include "i2s_out.h"

#include <inttypes.h>

#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

// ESP32_I2S_DAC_BCK/DIN/LRCK-WS — see repo spec's pin reference table.
#define I2S_BCK_GPIO 25
#define I2S_DIN_GPIO 26
#define I2S_WS_GPIO 27

// Decouples Bluedroid's data callback from the I2S DMA, like Espressif's
// a2dp_sink example (and Waveshare's stock firmware): the BT stack never
// blocks on audio, and radio jitter is absorbed here. 32 KB is ~185 ms of
// 44.1 kHz stereo; playback starts once PREFETCH is buffered (~115 ms).
#define RB_SIZE (32 * 1024)
#define RB_PREFETCH (20 * 1024)
// On overflow, incoming packets are dropped until the backlog drains to this.
#define RB_DROP_RESUME (RB_SIZE / 5)
// One DMA descriptor's worth (240 frames x 4 bytes) per write.
#define WRITE_CHUNK 960
// An empty buffer for this long means the stream starved; refill first.
#define UNDERRUN_MS 20
// Bounded so a write against a disabled channel can't wedge the task.
#define WRITE_TIMEOUT_MS 100

#define I2S_TASK_STACK 3072
#define I2S_TASK_PRIO (configMAX_PRIORITIES - 3)
#define I2S_TASK_CORE 1 // Bluedroid is pinned to core 0

// Q15, as in the S3's airplay_player. Phones using AVRCP absolute volume
// send full-scale PCM and leave the volume to us.
#define GAIN_UNITY 32768
// Each sample closes 1/GAIN_RAMP_DIV of the gap, so steps don't click.
#define GAIN_RAMP_DIV 256

typedef enum {
    MODE_STOPPED,
    MODE_PREFETCHING,
    MODE_PLAYING,
    MODE_DROPPING,
} rb_mode_t;

static const char *TAG = "i2s_out";
static i2s_chan_handle_t s_tx_chan;
static RingbufHandle_t s_rb;
static SemaphoreHandle_t s_wake;
static volatile rb_mode_t s_mode = MODE_STOPPED;
static volatile int32_t s_gain_target = GAIN_UNITY;
static int32_t s_gain = GAIN_UNITY;

static size_t rb_filled(void) {
    return RB_SIZE - xRingbufferGetCurFreeSize(s_rb);
}

static void drain(void) {
    size_t n;
    void *item;
    while ((item = xRingbufferReceiveUpTo(s_rb, &n, 0, RB_SIZE)) != NULL) {
        vRingbufferReturnItem(s_rb, item);
    }
}

static void apply_gain(int16_t *samples, size_t count) {
    int32_t target = s_gain_target;
    if (s_gain == GAIN_UNITY && target == GAIN_UNITY) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        if (s_gain != target) {
            int32_t step = (target - s_gain) / GAIN_RAMP_DIV;
            s_gain += step ? step : (target > s_gain ? 1 : -1);
        }
        samples[i] = (int16_t)((samples[i] * s_gain) >> 15);
    }
}

static void play_until_starved(void) {
    while (s_mode == MODE_PLAYING || s_mode == MODE_DROPPING) {
        size_t n = 0;
        uint8_t *item = xRingbufferReceiveUpTo(s_rb, &n, pdMS_TO_TICKS(UNDERRUN_MS), WRITE_CHUNK);
        if (!item) {
            if (s_mode == MODE_PLAYING) {
                s_mode = MODE_PREFETCHING;
                ESP_LOGW(TAG, "underrun, refilling");
            }
            return;
        }
        apply_gain((int16_t *)item, n / sizeof(int16_t));
        size_t written = 0;
        while (written < n) {
            size_t chunk = 0;
            if (i2s_channel_write(s_tx_chan, item + written, n - written, &chunk, WRITE_TIMEOUT_MS) != ESP_OK) {
                break;
            }
            written += chunk;
        }
        vRingbufferReturnItem(s_rb, item);
    }
}

static void i2s_task(void *arg) {
    (void)arg;
    while (1) {
        xSemaphoreTake(s_wake, portMAX_DELAY);
        if (s_mode == MODE_STOPPED) {
            drain();
        } else {
            play_until_starved();
        }
    }
}

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

    s_rb = xRingbufferCreate(RB_SIZE, RINGBUF_TYPE_BYTEBUF);
    s_wake = xSemaphoreCreateBinary();
    ESP_ERROR_CHECK(s_rb && s_wake ? ESP_OK : ESP_ERR_NO_MEM);
    xTaskCreatePinnedToCore(i2s_task, "i2s_out", I2S_TASK_STACK, NULL, I2S_TASK_PRIO, NULL, I2S_TASK_CORE);

    ESP_LOGI(TAG, "I2S TX ready (BCK=%d WS=%d DOUT=%d, %d KB buffer)", I2S_BCK_GPIO, I2S_WS_GPIO, I2S_DIN_GPIO,
             RB_SIZE / 1024);
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
    s_mode = MODE_PREFETCHING;
}

void i2s_out_stop(void) {
    s_mode = MODE_STOPPED;
    i2s_channel_disable(s_tx_chan);
    // Wake the task to discard the backlog, so the next stream doesn't open
    // with the tail of this one.
    xSemaphoreGive(s_wake);
}

void i2s_out_set_volume(uint8_t volume_pct) {
    // Cubic, like AirPlay: roughly even loudness steps across the range.
    float v = (volume_pct > 100 ? 100 : volume_pct) / 100.0f;
    s_gain_target = (int32_t)(v * v * v * GAIN_UNITY);
}

void i2s_out_write(const uint8_t *data, size_t len) {
    switch (s_mode) {
    case MODE_STOPPED:
        return;
    case MODE_DROPPING:
        if (rb_filled() > RB_DROP_RESUME) {
            return;
        }
        s_mode = MODE_PLAYING;
        break;
    default:
        break;
    }

    if (xRingbufferSend(s_rb, data, len, 0) != pdTRUE) {
        ESP_LOGW(TAG, "buffer full, dropping until it drains");
        s_mode = MODE_DROPPING;
        return;
    }

    if (s_mode == MODE_PREFETCHING && rb_filled() >= RB_PREFETCH) {
        s_mode = MODE_PLAYING;
        xSemaphoreGive(s_wake);
    }
}
