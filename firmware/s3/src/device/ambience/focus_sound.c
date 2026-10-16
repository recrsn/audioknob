#include "focus_sound.h"

#include "audio_out.h"
#include "audio_source.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define CHUNK_FRAMES 256
// Above the LVGL task (7) on the same core, so a long redraw can't starve
// the 32 ms DMA ring; below AirPlay's player (15), though the two never
// play together: a stream starting stops this first.
#define TASK_PRIO  10
#define TASK_CORE  1
#define TASK_STACK 4096

#define FADE_IN_S    3.0f
#define FADE_OUT_S   2.0f
#define FADE_SWAP_S  0.5f // each way, changing sound
// Per-sample approach of the volume gain: ~10 ms.
#define GAIN_COEF    (1.0f / 441.0f)
#define PREEMPT_WAIT_MS 50
#define STATS_US     (10 * 1000 * 1000)

static const char *TAG = "focus_sound";

static TaskHandle_t s_task;
static ambience_synth_t *s_synth;
// Held around each chunk's render and write, so a preempting stream knows
// when this task has stopped touching audio_out.
static SemaphoreHandle_t s_write_lock;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_want;
static bool s_preempted;
static ambience_kind_t s_kind;
static float s_gain_target;
static volatile float s_cpu_pct;

static float s_buf[CHUNK_FRAMES * 2];
static int16_t s_pcm[CHUNK_FRAMES * 2];

typedef struct {
    bool running; // holding the DAC and writing
    ambience_kind_t kind;
    float fade;   // 0-1, squared when applied
    float gain;
    int64_t busy_us, since_us;
    uint32_t chunks;
} player_t;

// Runs on the task of whichever stream took the DAC. Returns once this
// task is between chunks and will write no more.
static void on_preempted(void) {
    taskENTER_CRITICAL(&s_lock);
    bool was = s_want;
    s_want = false;
    s_preempted = true;
    taskEXIT_CRITICAL(&s_lock);
    if (xSemaphoreTake(s_write_lock, pdMS_TO_TICKS(PREEMPT_WAIT_MS)) == pdTRUE) {
        xSemaphoreGive(s_write_lock);
    }
    xTaskNotifyGive(s_task);
    if (was) {
        ESP_LOGI(TAG, "a stream started: stopping");
    }
}

static void log_stats(player_t *p, int64_t now) {
    if (now - p->since_us < STATS_US || !p->chunks) {
        return;
    }
    int64_t chunk_us = (int64_t)CHUNK_FRAMES * 1000000 / AUDIO_OUT_SAMPLE_RATE;
    s_cpu_pct = 100.0f * (float)p->busy_us / (float)(p->chunks * chunk_us);
    ESP_LOGI(TAG, "kind %d: %lld us to render %d frames (%.1f%% of a core), %lu underruns", p->kind,
             (long long)(p->busy_us / p->chunks), CHUNK_FRAMES, s_cpu_pct, (unsigned long)audio_out_underruns());
    p->busy_us = 0;
    p->chunks = 0;
    p->since_us = now;
}

// One chunk toward fade_target, at `step` per sample.
static void render_chunk(player_t *p, float fade_target, float step, float gain_target) {
    int64_t t0 = esp_timer_get_time();
    ambience_synth_render(s_synth, s_buf, CHUNK_FRAMES);
    for (int i = 0; i < CHUNK_FRAMES; i++) {
        if (p->fade < fade_target) {
            p->fade = p->fade + step > fade_target ? fade_target : p->fade + step;
        } else if (p->fade > fade_target) {
            p->fade = p->fade - step < fade_target ? fade_target : p->fade - step;
        }
        p->gain += (gain_target - p->gain) * GAIN_COEF;
        float g = p->fade * p->fade * p->gain * 32767.0f;
        for (int ch = 0; ch < 2; ch++) {
            float x = s_buf[2 * i + ch] * g;
            x = x > 32767.0f ? 32767.0f : x < -32767.0f ? -32767.0f : x;
            s_pcm[2 * i + ch] = (int16_t)x;
        }
    }
    int64_t t1 = esp_timer_get_time();
    p->busy_us += t1 - t0;
    p->chunks++;
    log_stats(p, t1);
    audio_out_write(s_pcm, CHUNK_FRAMES);
}

static void start(player_t *p, ambience_kind_t kind) {
    if (!audio_source_claim_local()) {
        focus_sound_stop();
        return;
    }
    ambience_synth_reset(s_synth, kind);
    p->running = true;
    p->kind = kind;
    p->fade = 0.0f;
    p->since_us = esp_timer_get_time();
    p->busy_us = 0;
    p->chunks = 0;
    ESP_LOGI(TAG, "playing kind %d", kind);
}

static void player_task(void *arg) {
    (void)arg;
    player_t p = {0};
    const float fs = AUDIO_OUT_SAMPLE_RATE;

    for (;;) {
        if (!p.running) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        }
        taskENTER_CRITICAL(&s_lock);
        bool want = s_want, preempted = s_preempted;
        ambience_kind_t kind = s_kind;
        float gain_target = s_gain_target;
        s_preempted = false;
        taskEXIT_CRITICAL(&s_lock);

        if (preempted) {
            // The DAC is the newcomer's now; nothing to hand back.
            p.running = false;
            continue;
        }
        if (!p.running) {
            if (!want) {
                continue;
            }
            p.gain = gain_target;
            start(&p, kind);
            if (!p.running) {
                continue;
            }
        }

        bool swapping = want && kind != p.kind;
        float target = want && !swapping ? 1.0f : 0.0f;
        float secs = target > 0.0f ? FADE_IN_S : swapping ? FADE_SWAP_S : FADE_OUT_S;

        xSemaphoreTake(s_write_lock, portMAX_DELAY);
        taskENTER_CRITICAL(&s_lock);
        preempted = s_preempted;
        taskEXIT_CRITICAL(&s_lock);
        if (!preempted) {
            render_chunk(&p, target, 1.0f / (secs * fs), gain_target);
        }
        xSemaphoreGive(s_write_lock);
        if (preempted || p.fade > 0.0f || target > 0.0f) {
            continue;
        }

        // Faded all the way out.
        if (swapping) {
            ambience_synth_reset(s_synth, kind);
            p.kind = kind;
        } else {
            p.running = false;
            audio_source_release_local();
            ESP_LOGI(TAG, "stopped");
        }
    }
}

void focus_sound_init(void) {
    s_synth = ambience_synth_create(AUDIO_OUT_SAMPLE_RATE, esp_random());
    s_write_lock = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(s_synth && s_write_lock ? ESP_OK : ESP_ERR_NO_MEM);
    audio_source_set_local_preempted(on_preempted);
    BaseType_t ok = xTaskCreatePinnedToCore(player_task, "focus_sound", TASK_STACK, NULL, TASK_PRIO, &s_task,
                                            TASK_CORE);
    ESP_ERROR_CHECK(ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}

void focus_sound_play(ambience_kind_t kind) {
    taskENTER_CRITICAL(&s_lock);
    s_want = true;
    s_kind = kind;
    taskEXIT_CRITICAL(&s_lock);
    xTaskNotifyGive(s_task);
}

void focus_sound_stop(void) {
    taskENTER_CRITICAL(&s_lock);
    s_want = false;
    taskEXIT_CRITICAL(&s_lock);
    xTaskNotifyGive(s_task);
}

void focus_sound_set_volume(uint8_t pct) {
    float v = (pct > 100 ? 100 : pct) / 100.0f;
    taskENTER_CRITICAL(&s_lock);
    s_gain_target = v * v * v;
    taskEXIT_CRITICAL(&s_lock);
}

float focus_sound_cpu_pct(void) {
    return s_cpu_pct;
}

bool focus_sound_playing(ambience_kind_t *kind) {
    taskENTER_CRITICAL(&s_lock);
    bool want = s_want;
    *kind = s_kind;
    taskEXIT_CRITICAL(&s_lock);
    return want;
}
