#include "talk/playout.h"

#include <stdlib.h>
#include <string.h>

#include "audio/mic/mic.h"
#include "audio/out.h"
#include "audio/pcm.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "core/psram_stream.h"
#include "freertos/task.h"
#include "talk/wire.h"

static const char *TAG = "talk_playout";

#define RING_MS 100
// Held back before playing, at the start and after the ring runs empty:
// two frames, so one is always waiting and the worker may run a frame late
// without a gap.
#define PREFILL_MS 40
#define SAMPLES_PER_MS 16
// Above the call worker (8): it only copies and waits.
#define TASK_PRIORITY 9
#define TASK_CORE 1
#define TASK_STACK 4096
#define SETTLE_FRAMES 5

static psram_stream_t s_ring;
static SemaphoreHandle_t s_done;
static volatile bool s_running;
static talk_playout_write_t s_write;
static uint32_t s_rebuffers, s_dropped, s_written, s_dry_at_start, s_dry_last;

static struct {
    pcm_resampler_t resampler;
    int16_t pcm[TALK_FRAME_SAMPLES * 2];
    int16_t far[TALK_FRAME_SAMPLES];
    int16_t output[900 * 2];
} *s_buf;

static bool prefill(void) {
    const size_t need = PREFILL_MS * SAMPLES_PER_MS * sizeof(int16_t);
    while (s_running && xStreamBufferBytesAvailable(s_ring.handle) < need) {
        vTaskDelay(1);
    }
    return s_running;
}

static bool take_frame(void) {
    size_t got = 0;
    while (s_running && got < sizeof(s_buf->far)) {
        got += xStreamBufferReceive(s_ring.handle, (uint8_t *)s_buf->far + got, sizeof(s_buf->far) - got, pdMS_TO_TICKS(20));
    }
    return got == sizeof(s_buf->far);
}

static void playout_task(void *arg) {
    (void)arg;
    bool filled = false;
    while (s_running) {
        if (!filled && !(filled = prefill())) {
            break;
        }
        if (!take_frame()) {
            break;
        }
        // The canceller's reference goes in as the frame goes to the DAC, so it
        // leads the echo by just the DAC's queue.
        mic_reference(s_buf->far, TALK_FRAME_SAMPLES);
        memcpy(s_buf->pcm, s_buf->far, sizeof(s_buf->far));
        pcm_to_stereo(s_buf->pcm, TALK_FRAME_SAMPLES, 1);
        // Counted as of before each write: the last one, as the call ends,
        // finds the DAC draining.
        s_dry_last = audio_out_dry_buffers();
        s_write(s_buf->output, pcm_resample(&s_buf->resampler, s_buf->pcm, TALK_FRAME_SAMPLES, s_buf->output));
        // Counted once the DAC's queue has filled: the first writes go into
        // an idle one, which reads as dry until they catch up.
        if (++s_written == SETTLE_FRAMES) {
            s_dry_at_start = s_dry_last;
        }
        if (!xStreamBufferBytesAvailable(s_ring.handle)) {
            s_rebuffers++;
            filled = false;
        }
    }
    xSemaphoreGive(s_done);
    vTaskDeleteWithCaps(NULL);
}

bool talk_playout_start(talk_playout_write_t write) {
    s_write = write;
    s_rebuffers = s_dropped = s_written = 0;
    s_buf = heap_caps_calloc(1, sizeof(*s_buf), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    bool ring = psram_stream_create(&s_ring, RING_MS * SAMPLES_PER_MS * sizeof(int16_t), 1);
    s_done = xSemaphoreCreateBinary();
    if (s_buf && ring && s_done) {
        pcm_resampler_init(&s_buf->resampler, 16000, AUDIO_OUT_SAMPLE_RATE);
        s_running = true;
        // Never touches flash, so its stack can be PSRAM.
        if (xTaskCreatePinnedToCoreWithCaps(playout_task, "talk_out", TASK_STACK, NULL, TASK_PRIORITY, NULL, TASK_CORE,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS) {
            return true;
        }
        s_running = false;
        xSemaphoreGive(s_done);
    }
    talk_playout_stop();
    return false;
}

void talk_playout_push(const int16_t *pcm, size_t frames) {
    if (s_ring.handle && xStreamBufferSend(s_ring.handle, pcm, frames * sizeof(int16_t), 0) != frames * sizeof(int16_t)) {
        s_dropped++;
    }
}

void talk_playout_stop(void) {
    if (s_done) {
        s_running = false;
        xSemaphoreTake(s_done, portMAX_DELAY);
        vSemaphoreDelete(s_done);
        s_done = NULL;
    }
    psram_stream_delete(&s_ring);
    free(s_buf);
    s_buf = NULL;
    uint32_t dry = s_written >= SETTLE_FRAMES ? s_dry_last - s_dry_at_start : 0;
    ESP_LOGI(TAG, "%lu frames played; ring ran empty %lu times, %lu frames dropped as full; DAC ran dry %lu times",
             (unsigned long)s_written, (unsigned long)s_rebuffers, (unsigned long)s_dropped, (unsigned long)dry);
}
