#include "audio/mic/process.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_aec.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_ns.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define DC_POLE 0.995f
// 500 ms of speaker reference: far more than the DAC and mic DMA put
// between a sample written and its echo read back.
#define REF_CAPACITY 8000
// Covers DAC queue, speaker-to-mic path and room reflections.
#define AEC_FILTER_LENGTH 4
// How long a chunk waits for its reference once the speaker has started:
// the canceller pairs samples by count, so filling the gap with silence
// would shift every later pair.
#define REF_WAIT_MS 30

struct mic_process {
    ns_handle_t ns;
    float gain;
    bool primed;
    int32_t dc_x1;
    float dc_y1;
    float dc[MIC_PROCESS_FRAMES];
    int16_t clean[MIC_PROCESS_FRAMES];
    int16_t gained[MIC_PROCESS_FRAMES];

    // The canceller works in chunks of its own size, so input gathers in
    // aec_in and its output queues in pending, primed with a chunk of
    // silence so a block's worth is always there.
    aec_handle_t *aec;
    int chunk;
    int16_t *aec_in, *aec_ref, *aec_out, *pending;
    int aec_in_n, pending_n;
    // Written by the speaker's task, read by the capture's.
    portMUX_TYPE ref_lock;
    int16_t *ref;
    size_t ref_head, ref_count;
    bool ref_started;
    int64_t aec_us, ref_wait_us;
    uint32_t aec_chunks;
};

static int16_t *aligned(size_t samples) {
    return heap_caps_aligned_calloc(16, samples, sizeof(int16_t), MALLOC_CAP_DEFAULT);
}

static bool echo_create(mic_process_t *p) {
    aec_config_t cfg = {
        .mic_num = 1,
        .ref_num = 1,
        .out_num = 1,
        .filter_length = AEC_FILTER_LENGTH,
        .sample_rate = 16000,
        .caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
        .mode = AEC_MODE_VOIP_LOW_COST,
        .nlp_level = AEC_NLP_LEVEL_NORMAL,
    };
    // PSRAM: internal RAM measured no faster, and the caller's stack needs it.
    // caps covers only some of ESP-SR's buffers; the rest are plain mallocs
    // under CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL, ~28 KB of internal RAM
    // (~7% slower in PSRAM). Lowering the limit meanwhile is safe for other
    // tasks: with SPIRAM_USE_MALLOC any plain malloc may already be PSRAM.
    heap_caps_malloc_extmem_enable(0);
    p->aec = aec_create_from_config(&cfg);
    heap_caps_malloc_extmem_enable(CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL);
    if (!p->aec) {
        return false;
    }
    p->chunk = aec_get_chunksize(p->aec);
    p->aec_in = aligned(p->chunk);
    p->aec_ref = aligned(p->chunk);
    p->aec_out = aligned(p->chunk);
    p->pending = calloc(p->chunk + MIC_PROCESS_FRAMES, sizeof(int16_t));
    p->ref = heap_caps_calloc(REF_CAPACITY, sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    p->pending_n = p->chunk;
    return p->chunk > 0 && p->aec_in && p->aec_ref && p->aec_out && p->pending && p->ref;
}

static size_t internal_free(void) {
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

mic_process_t *mic_process_create(uint8_t gain_db, uint8_t noise_level, bool echo_cancel) {
    size_t start = internal_free();
    mic_process_t *p = calloc(1, sizeof(*p));
    if (!p) {
        return NULL;
    }
    p->ref_lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    p->gain = powf(10.0f, gain_db / 20.0f);
    size_t before_ns = internal_free();
    bool ns_ok = !noise_level || (p->ns = ns_pro_create(10, noise_level - 1, 16000));
    size_t before_aec = internal_free();
    bool aec_ok = ns_ok && (!echo_cancel || echo_create(p));
    ESP_LOGI("mic", "processing internal RAM: struct %u, NS %u, AEC %u bytes", (unsigned)(start - before_ns),
             (unsigned)(before_ns - before_aec), (unsigned)(before_aec - internal_free()));
    if (!ns_ok || !aec_ok) {
        mic_process_destroy(p);
        return NULL;
    }
    return p;
}

void mic_process_reference(mic_process_t *p, const int16_t *pcm, size_t frames) {
    if (!p || !p->aec) {
        return;
    }
    taskENTER_CRITICAL(&p->ref_lock);
    for (size_t i = 0; i < frames; i++) {
        if (p->ref_count == REF_CAPACITY) {
            p->ref_head = (p->ref_head + 1) % REF_CAPACITY;
            p->ref_count--;
        }
        p->ref[(p->ref_head + p->ref_count++) % REF_CAPACITY] = pcm[i];
    }
    p->ref_started = true;
    taskEXIT_CRITICAL(&p->ref_lock);
}

static size_t reference_count(mic_process_t *p) {
    taskENTER_CRITICAL(&p->ref_lock);
    size_t n = p->ref_started ? p->ref_count : SIZE_MAX;
    taskEXIT_CRITICAL(&p->ref_lock);
    return n;
}

// Silence while the reference runs short: nothing was played then.
static void take_reference(mic_process_t *p) {
    int64_t start = esp_timer_get_time();
    for (int waited = 0; reference_count(p) < (size_t)p->chunk && waited < REF_WAIT_MS; waited++) {
        vTaskDelay(1);
    }
    p->ref_wait_us += esp_timer_get_time() - start;
    taskENTER_CRITICAL(&p->ref_lock);
    for (int i = 0; i < p->chunk; i++) {
        if (p->ref_count) {
            p->aec_ref[i] = p->ref[p->ref_head];
            p->ref_head = (p->ref_head + 1) % REF_CAPACITY;
            p->ref_count--;
        } else {
            p->aec_ref[i] = 0;
        }
    }
    taskEXIT_CRITICAL(&p->ref_lock);
}

// Cancels the echo in p->clean in place, a chunk behind.
static void echo_cancel(mic_process_t *p) {
    for (int done = 0; done < MIC_PROCESS_FRAMES;) {
        int n = MIC_PROCESS_FRAMES - done < p->chunk - p->aec_in_n ? MIC_PROCESS_FRAMES - done : p->chunk - p->aec_in_n;
        memcpy(p->aec_in + p->aec_in_n, p->clean + done, n * sizeof(int16_t));
        p->aec_in_n += n;
        done += n;
        if (p->aec_in_n == p->chunk) {
            take_reference(p);
            int64_t start = esp_timer_get_time();
            aec_process(p->aec, p->aec_in, p->aec_ref, p->aec_out);
            p->aec_us += esp_timer_get_time() - start;
            p->aec_chunks++;
            memcpy(p->pending + p->pending_n, p->aec_out, p->chunk * sizeof(int16_t));
            p->pending_n += p->chunk;
            p->aec_in_n = 0;
        }
    }
    memcpy(p->clean, p->pending, MIC_PROCESS_FRAMES * sizeof(int16_t));
    p->pending_n -= MIC_PROCESS_FRAMES;
    memmove(p->pending, p->pending + MIC_PROCESS_FRAMES, p->pending_n * sizeof(int16_t));
}

static int16_t saturate(float v) {
    return v > INT16_MAX ? INT16_MAX : v < INT16_MIN ? INT16_MIN : (int16_t)v;
}

void mic_process_block(mic_process_t *p, const int16_t *in, int16_t *out) {
    if (!p->primed) {
        // Starting from zero turns the mic's DC bias into an artificial pop.
        p->dc_x1 = in[0];
        p->primed = true;
    }
    for (int i = 0; i < MIC_PROCESS_FRAMES; i++) {
        int32_t x = in[i];
        float y = (float)(x - p->dc_x1) + p->dc_y1 * DC_POLE;
        p->dc_x1 = x;
        p->dc_y1 = y;
        p->dc[i] = y;
    }
    // Before the gain: amplified, the speaker's echo would clip, and a
    // clipped echo no longer matches the reference.
    if (p->aec) {
        for (int i = 0; i < MIC_PROCESS_FRAMES; i++) {
            p->clean[i] = saturate(p->dc[i]);
        }
        echo_cancel(p);
        for (int i = 0; i < MIC_PROCESS_FRAMES; i++) {
            p->dc[i] = p->clean[i];
        }
    }
    for (int i = 0; i < MIC_PROCESS_FRAMES; i++) {
        p->gained[i] = saturate(p->dc[i] * p->gain);
    }
    if (p->ns) {
        ns_process(p->ns, p->gained, out);
    } else {
        memcpy(out, p->gained, sizeof(p->gained));
    }
}

int64_t mic_process_reference_wait_us(const mic_process_t *p) {
    return p ? p->ref_wait_us : 0;
}

void mic_process_destroy(mic_process_t *p) {
    if (!p) {
        return;
    }
    if (p->ns) {
        ns_destroy(p->ns);
    }
    if (p->aec) {
        if (p->aec_chunks) {
            ESP_LOGI("mic", "AEC %lld us per %d-sample chunk, %lld us waiting for its reference", p->aec_us / p->aec_chunks,
                     p->chunk, p->ref_wait_us / p->aec_chunks);
        }
        aec_destroy(p->aec);
    }
    heap_caps_free(p->aec_in);
    heap_caps_free(p->aec_ref);
    heap_caps_free(p->aec_out);
    free(p->pending);
    heap_caps_free(p->ref);
    free(p);
}
