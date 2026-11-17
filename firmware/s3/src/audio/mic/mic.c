#include "audio/mic/mic.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "audio/mic/process.h"

#include "driver/i2s_pdm.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "core/psram_stream.h"
#include "freertos/idf_additions.h"

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

// Processing (AEC + NS, ~11 ms per 20 ms) runs on its own task on core 0,
// mostly idle beside WiFi, rather than in the reader's: inline it left a
// call's worker on core 1 so little slack that the DAC ran dry. Its output
// waits in a stream buffer for mic_read().
#define PROCESS_STACK 8192
#define PROCESS_PRIORITY 10
#define PROCESS_CORE 0
#define OUTPUT_MS 160
static psram_stream_t s_output_buf;
static TaskHandle_t s_process_task;
static SemaphoreHandle_t s_process_done;
static volatile bool s_processing;
static uint32_t s_output_dropped;
static UBaseType_t s_process_stack_left;
static volatile uint32_t s_overflows;
static int64_t s_process_us;
static uint32_t s_process_blocks;

void mic_init(void) {
    s_owner = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(s_owner ? ESP_OK : ESP_ERR_NO_MEM);
}

// The reader fell behind and the driver dropped a DMA buffer of audio.
static bool IRAM_ATTR on_recv_q_ovf(i2s_chan_handle_t handle, i2s_event_data_t *event, void *ctx) {
    (void)handle;
    (void)event;
    (void)ctx;
    s_overflows++;
    return false;
}

// Even a disabled I2S channel holds internal DMA memory. Allocate it only
// for a capture and release it in mic_stop(), while s_owner is held.
static bool create_channel(bool right) {
    // PDM RX exists only on I2S0 on the S3.
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = DMA_DESC_NUM;
    chan_cfg.dma_frame_num = DMA_FRAME_NUM;
    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &s_rx_chan);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "can't create PDM RX: %s", esp_err_to_name(err));
        return false;
    }

    i2s_pdm_rx_config_t cfg = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = MIC_CLK_GPIO,
            .din = MIC_DATA_GPIO,
        },
    };
    cfg.slot_cfg.slot_mask = right ? I2S_PDM_SLOT_RIGHT : I2S_PDM_SLOT_LEFT;
    // 128x oversampling: a 2.048 MHz clock, mid-range for the mic, where
    // the default 64x (1.024 MHz) sits at its floor.
    cfg.clk_cfg.dn_sample_mode = I2S_PDM_DSR_16S;
    err = i2s_channel_init_pdm_rx_mode(s_rx_chan, &cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "can't initialize PDM RX: %s", esp_err_to_name(err));
        i2s_del_channel(s_rx_chan);
        s_rx_chan = NULL;
        return false;
    }
    const i2s_event_callbacks_t callbacks = {.on_recv_q_ovf = on_recv_q_ovf};
    err = i2s_channel_register_event_callback(s_rx_chan, &callbacks, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "can't watch PDM RX: %s", esp_err_to_name(err));
    }
    return true;
}

static void log_internal(const char *step) {
    ESP_LOGI(TAG, "internal RAM %s: %u free, largest %u", step,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}
static size_t read_raw(int16_t *pcm, size_t frames, uint32_t timeout_ms) {
    size_t bytes = 0;
    esp_err_t err = i2s_channel_read(s_rx_chan, pcm, frames * sizeof(int16_t), &bytes, timeout_ms);
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
        ESP_LOGW(TAG, "read failed: %s", esp_err_to_name(err));
    }
    return bytes / sizeof(int16_t);
}

static void process_task(void *arg) {
    (void)arg;
    size_t have = 0;
    while (s_processing) {
        have += read_raw(s_input + have, MIC_PROCESS_FRAMES - have, 50);
        if (have < MIC_PROCESS_FRAMES) {
            continue;
        }
        have = 0;
        // Less any wait for the speaker's reference.
        int64_t start = esp_timer_get_time() - mic_process_reference_wait_us(s_process);
        mic_process_block(s_process, s_input, s_output);
        s_process_us += esp_timer_get_time() - mic_process_reference_wait_us(s_process) - start;
        s_process_blocks++;
        if (xStreamBufferSend(s_output_buf.handle, s_output, sizeof(s_output), 0) != sizeof(s_output)) {
            s_output_dropped++;
        }
    }
    s_process_stack_left = uxTaskGetStackHighWaterMark(NULL);
    xSemaphoreGive(s_process_done);
    vTaskDeleteWithCaps(NULL);
}

static bool process_task_start(void) {
    bool buffered = psram_stream_create(&s_output_buf, MIC_SAMPLE_RATE * OUTPUT_MS / 1000 * sizeof(int16_t), 1);
    s_process_done = xSemaphoreCreateBinary();
    s_processing = true;
    s_output_dropped = 0;
    // Internal stack: the AEC and NS run on it every 10 ms.
    if (buffered && s_process_done &&
        xTaskCreatePinnedToCoreWithCaps(process_task, "mic", PROCESS_STACK, NULL, PROCESS_PRIORITY, &s_process_task,
                                        PROCESS_CORE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) == pdPASS) {
        return true;
    }
    s_processing = false;
    s_process_task = NULL;
    return false;
}

static void process_task_stop(void) {
    if (s_process_task) {
        s_processing = false;
        xSemaphoreTake(s_process_done, portMAX_DELAY);
        s_process_task = NULL;
        ESP_LOGI(TAG, "processing task stack: %u bytes left of %u", (unsigned)s_process_stack_left, PROCESS_STACK);
    }
    psram_stream_delete(&s_output_buf);
    if (s_process_done) {
        vSemaphoreDelete(s_process_done);
        s_process_done = NULL;
    }
    if (s_output_dropped) {
        ESP_LOGW(TAG, "%lu processed blocks dropped: reads fell behind", (unsigned long)s_output_dropped);
    }
}

bool mic_start(const mic_config_t *config) {
    static const mic_config_t defaults = {.gain_db = MIC_DEFAULT_GAIN_DB, .noise_level = MIC_DEFAULT_NOISE_LEVEL};
    const mic_config_t *cfg = config ? config : &defaults;
    if (cfg->gain_db > MIC_MAX_GAIN_DB || cfg->noise_level > MIC_MAX_NOISE_LEVEL || xSemaphoreTake(s_owner, 0) != pdTRUE) {
        return false;
    }
    s_raw = cfg->raw;
    log_internal("before the mic");
    if (!create_channel(cfg->right_slot)) {
        xSemaphoreGive(s_owner);
        return false;
    }
    log_internal("with the I2S channel");
    if (!s_raw) {
        s_process = mic_process_create(cfg->gain_db, cfg->noise_level, cfg->echo_cancel);
        if (!s_process) {
            ESP_LOGW(TAG, "no memory for microphone processing");
            i2s_del_channel(s_rx_chan);
            s_rx_chan = NULL;
            xSemaphoreGive(s_owner);
            return false;
        }
    }
    log_internal("with processing");
    s_process_us = s_process_blocks = 0;
    s_overflows = 0;
    esp_err_t err = i2s_channel_enable(s_rx_chan);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "can't start PDM RX: %s", esp_err_to_name(err));
        mic_process_destroy(s_process);
        s_process = NULL;
        i2s_del_channel(s_rx_chan);
        s_rx_chan = NULL;
        xSemaphoreGive(s_owner);
        return false;
    }
    if (!s_raw) {
        // Let the PDM decimator settle before exposing the first audio block.
        for (int i = 0; i < 5; i++) {
            size_t bytes = 0;
            i2s_channel_read(s_rx_chan, s_input, sizeof(s_input), &bytes, 30);
            if (bytes == sizeof(s_input)) {
                mic_process_block(s_process, s_input, s_output);
            }
        }
        if (!process_task_start()) {
            ESP_LOGW(TAG, "can't start microphone processing");
            process_task_stop();
            i2s_channel_disable(s_rx_chan);
            i2s_del_channel(s_rx_chan);
            s_rx_chan = NULL;
            mic_process_destroy(s_process);
            s_process = NULL;
            xSemaphoreGive(s_owner);
            return false;
        }
        log_internal("with the processing task");
    }
    ESP_LOGI(TAG, "capture: gain %u dB, NS %s, AEC %d, raw %d", cfg->gain_db,
             cfg->noise_level && !s_raw ? "ESP-SR" : "off", cfg->echo_cancel && !s_raw, s_raw);
    return true;
}

void mic_stop(void) {
    process_task_stop();
    i2s_channel_disable(s_rx_chan);
    i2s_del_channel(s_rx_chan);
    s_rx_chan = NULL;
    mic_process_destroy(s_process);
    s_process = NULL;
    if (s_process_blocks) {
        ESP_LOGI(TAG, "processing %lld us per 10 ms block (%.1f%% of a core)",
                 s_process_us / s_process_blocks, (double)s_process_us / (s_process_blocks * 100.0));
    }
    if (s_overflows) {
        ESP_LOGW(TAG, "%lu DMA buffers dropped: reads fell behind", (unsigned long)s_overflows);
    }
    xSemaphoreGive(s_owner);
}

size_t mic_read(int16_t *pcm, size_t frames, uint32_t timeout_ms) {
    if (s_raw) {
        return read_raw(pcm, frames, timeout_ms);
    }
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    size_t got = 0;
    while (got < frames) {
        int64_t remaining = deadline - esp_timer_get_time();
        if (remaining <= 0) {
            break;
        }
        size_t n = xStreamBufferReceive(s_output_buf.handle, pcm + got, (frames - got) * sizeof(int16_t),
                                        pdMS_TO_TICKS((remaining + 999) / 1000));
        got += n / sizeof(int16_t);
    }
    return got;
}

void mic_reference(const int16_t *pcm, size_t frames) {
    mic_process_reference(s_process, pcm, frames);
}

float mic_processing_cpu_pct(void) {
    return s_process_blocks ? (float)s_process_us / (s_process_blocks * 100.0f) : 0;
}
