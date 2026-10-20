#include "alarm_sound.h"

#include <math.h>
#include <stdbool.h>
#include <string.h>

#include "audio_out.h"
#include "audio_source.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// The synthesis runs inside audio_out's mix hook, so it rides on whatever
// writes the DAC: the AirPlay player while it plays (ducked under the
// chime), else this module's task, which tops the DMA queue up with silence
// for the hook to mix into. The hook owns all synth state; the API only
// posts a target that the hook picks up at its next block.

#define SAMPLE_RATE AUDIO_OUT_SAMPLE_RATE

#define LUT_BITS      10
#define LUT_SIZE      (1u << LUT_BITS)
#define LUT_FRAC_BITS (32 - LUT_BITS)

#define MAX_VOICES   6
#define MAX_PARTIALS 7
// A voice is dropped once its partials together fall below this (-40 dB).
#define VOICE_FLOOR  0.01f
#define NOTE_PEAK    0.45f // of full scale, per note at 100 %
#define ATTACK_S     0.004f
#define RELEASE_S    0.08f // on dismissal
// A preview fades out after one period rather than ringing on, since
// AirPlay stays ducked until it ends.
#define PREVIEW_RELEASE_S 0.4f
#define DUCK_GAIN    0.15f // AirPlay under the chime, about -16 dB
#define DUCK_S       0.15f
#define GAIN_SMOOTH  0.001f // one-pole step towards a new volume, per sample

// Silence top-up while nothing else writes: one DMA buffer at a time,
// keeping at least TOPUP_MIN_US queued.
#define TOPUP_FRAMES 240
#define TOPUP_MIN_US 16000
#define TASK_PERIOD_MS 4
#define TASK_STACK 3072
#define TASK_PRIO  10
#define TASK_CORE  1

#define TARGET_KIND  0xFFu
#define TARGET_ONCE  0x100u
// Each preview is a new target, so stepping the volume replays it.
#define TARGET_GEN_SHIFT 9

typedef struct {
    float ratio, amp, tau_s;
} partial_t;

typedef struct {
    const partial_t *partials;
    int count;
} timbre_t;

typedef struct {
    uint16_t at_ms;
    float hz;
} note_t;

typedef struct {
    const note_t *notes;
    int count;
    uint16_t period_ms;
} pattern_t;

typedef struct {
    bool active;
    int count;
    float attack;
    uint32_t phase[MAX_PARTIALS], inc[MAX_PARTIALS];
    float amp[MAX_PARTIALS], decay[MAX_PARTIALS];
    uint32_t born;
} voice_t;

// Vibraphone-like: harmonic, the overtones fading well before the body.
static const partial_t k_chime[] = {
    {1.0f, 1.0f, 1.1f},
    {2.0f, 0.16f, 0.45f},
    {4.0f, 0.05f, 0.18f},
};

// After Risset's bell: inharmonic partials, the hum pair beating slowly.
static const partial_t k_bell[] = {
    {0.56f, 0.55f, 1.6f}, {0.5625f, 0.4f, 1.4f}, {1.0f, 1.0f, 1.2f}, {1.19f, 0.45f, 0.8f},
    {2.0f, 0.35f, 0.6f},  {2.74f, 0.22f, 0.4f},  {4.07f, 0.1f, 0.2f},
};

static const timbre_t k_timbres[] = {
    [ALARM_SOUND_CHIME] = {k_chime, sizeof(k_chime) / sizeof(k_chime[0])},
    [ALARM_SOUND_BELL] = {k_bell, sizeof(k_bell) / sizeof(k_bell[0])},
};

#define C5 523.25f
#define E5 659.26f
#define G5 783.99f
#define C6 1046.50f
#define E6 1318.51f

static const note_t k_timer_notes[] = {{0, E6}, {420, C6}};
static const note_t k_focus_notes[] = {{0, G5}, {320, E5}, {640, C5}};
static const note_t k_break_notes[] = {{0, C5}, {180, G5}, {360, C6}};

static const pattern_t k_patterns[] = {
    [ALARM_SOUND_TIMER] = {k_timer_notes, 2, 2000},
    [ALARM_SOUND_FOCUS_END] = {k_focus_notes, 3, 3000},
    [ALARM_SOUND_BREAK_END] = {k_break_notes, 3, 2400},
};

static const char *TAG = "alarm_sound";

// PSRAM: internal RAM is what AirPlay's buffered stream runs short of.
static float *s_lut;
static TaskHandle_t s_task;

// Posted by the API: 0, or kind + 1, plus TARGET_ONCE and a generation
// for a preview.
static uint32_t s_target;
static uint32_t s_preview_gen;
static volatile alarm_sound_voice_t s_voice = ALARM_SOUND_CHIME;
static volatile float s_gain_target;

// Set by the hook when what it last saw has finished and AirPlay is back
// at full level; s_done_target is that target.
static volatile bool s_done;
static volatile uint32_t s_done_target;

// Hook state, under audio_out's lock.
static struct {
    uint32_t seen;
    const pattern_t *pattern;
    bool seq_on, once, releasing;
    int next;
    uint32_t t, next_at, period;
    float release, release_step, duck, gain;
    uint32_t clock;
    voice_t voices[MAX_VOICES];
} h;

static uint32_t ms_to_samples(uint32_t ms) {
    return ms * (SAMPLE_RATE / 1000) + ms * (SAMPLE_RATE % 1000) / 1000;
}

static void trigger(float hz) {
    voice_t *v = NULL;
    for (int i = 0; i < MAX_VOICES; i++) {
        if (!h.voices[i].active) {
            v = &h.voices[i];
            break;
        }
    }
    if (!v) {
        v = &h.voices[0];
        for (int i = 1; i < MAX_VOICES; i++) {
            if (h.voices[i].born < v->born) {
                v = &h.voices[i];
            }
        }
    }

    const timbre_t *timbre = &k_timbres[s_voice];
    float sum = 0;
    for (int p = 0; p < timbre->count; p++) {
        sum += timbre->partials[p].amp;
    }
    v->count = timbre->count;
    for (int p = 0; p < timbre->count; p++) {
        const partial_t *pt = &timbre->partials[p];
        v->phase[p] = 0;
        v->inc[p] = (uint32_t)(hz * pt->ratio / SAMPLE_RATE * 4294967296.0f);
        v->amp[p] = NOTE_PEAK * pt->amp / sum;
        v->decay[p] = expf(-1.0f / (pt->tau_s * SAMPLE_RATE));
    }
    v->attack = 0;
    v->born = h.clock++;
    v->active = true;
}

static float voice_sample(voice_t *v) {
    float s = 0;
    for (int p = 0; p < v->count; p++) {
        uint32_t ph = v->phase[p];
        uint32_t i = ph >> LUT_FRAC_BITS;
        float frac = (float)(ph & ((1u << LUT_FRAC_BITS) - 1)) * (1.0f / (1u << LUT_FRAC_BITS));
        s += (s_lut[i] + (s_lut[i + 1] - s_lut[i]) * frac) * v->amp[p];
        v->amp[p] *= v->decay[p];
        v->phase[p] = ph + v->inc[p];
    }
    if (v->attack < 1.0f) {
        s *= v->attack;
        v->attack += 1.0f / (ATTACK_S * SAMPLE_RATE);
    }
    return s;
}

static void cull_voices(void) {
    for (int i = 0; i < MAX_VOICES; i++) {
        voice_t *v = &h.voices[i];
        if (!v->active) {
            continue;
        }
        float level = 0;
        for (int p = 0; p < v->count; p++) {
            level += v->amp[p];
        }
        v->active = level >= VOICE_FLOOR * NOTE_PEAK;
    }
}

static void seq_start(uint32_t target) {
    h.pattern = &k_patterns[(target & TARGET_KIND) - 1];
    h.once = target & TARGET_ONCE;
    h.seq_on = true;
    h.releasing = false;
    h.release = 1.0f;
    h.next = 0;
    h.t = 0;
    h.next_at = ms_to_samples(h.pattern->notes[0].at_ms);
    h.period = ms_to_samples(h.pattern->period_ms);
}

static void seq_step(void) {
    if (!h.seq_on) {
        return;
    }
    if (h.next < h.pattern->count && h.t == h.next_at) {
        trigger(h.pattern->notes[h.next].hz);
        if (++h.next < h.pattern->count) {
            h.next_at = ms_to_samples(h.pattern->notes[h.next].at_ms);
        }
    }
    if (++h.t >= h.period) {
        if (h.once) {
            h.seq_on = false;
            h.releasing = true;
            h.release_step = 1.0f / (PREVIEW_RELEASE_S * SAMPLE_RATE);
            return;
        }
        h.t = 0;
        h.next = 0;
        h.next_at = ms_to_samples(h.pattern->notes[0].at_ms);
    }
}

static int16_t saturate(float x) {
    return x > 32767.0f ? 32767 : x < -32768.0f ? -32768 : (int16_t)x;
}

static void mix(int16_t *pcm, size_t frames) {
    uint32_t target = __atomic_load_n(&s_target, __ATOMIC_ACQUIRE);
    if (target != h.seen) {
        h.seen = target;
        if (target) {
            seq_start(target);
        } else {
            h.seq_on = false;
            h.releasing = true;
            h.release_step = 1.0f / (RELEASE_S * SAMPLE_RATE);
        }
    }

    bool finished = h.releasing && h.release <= 0;
    float duck_to = finished ? 1.0f : DUCK_GAIN;
    float duck_step = (1.0f - DUCK_GAIN) / (DUCK_S * SAMPLE_RATE);
    float gain_target = s_gain_target;

    for (size_t n = 0; n < frames; n++) {
        seq_step();
        float out = 0;
        if (!finished) {
            for (int i = 0; i < MAX_VOICES; i++) {
                if (h.voices[i].active) {
                    out += voice_sample(&h.voices[i]);
                }
            }
            if (h.releasing) {
                out *= h.release;
                h.release -= h.release_step;
            }
        }
        h.gain += (gain_target - h.gain) * GAIN_SMOOTH;
        h.duck = h.duck < duck_to ? fminf(h.duck + duck_step, duck_to) : fmaxf(h.duck - duck_step, duck_to);

        float chime = out * h.gain * 32767.0f;
        pcm[n * 2] = saturate(pcm[n * 2] * h.duck + chime);
        pcm[n * 2 + 1] = saturate(pcm[n * 2 + 1] * h.duck + chime);
    }

    if (h.releasing && h.release <= 0) {
        memset(h.voices, 0, sizeof(h.voices));
    } else {
        cull_voices();
    }
    s_done_target = h.seen;
    s_done = finished && h.duck >= 1.0f;
}

static void topup(void) {
    static const int16_t silence[TOPUP_FRAMES * 2];
    int64_t now;
    while (!audio_source_suspended() && audio_out_queued_us(&now) < TOPUP_MIN_US) {
        audio_out_write(silence, TOPUP_FRAMES);
    }
}

static void begin(void) {
    memset(&h, 0, sizeof(h));
    h.duck = 1.0f;
    h.gain = s_gain_target;
    s_done = false;
    audio_source_hold_s3(true);
    audio_out_set_mix(mix);
}

// Unhooking takes audio_out's lock, so no mix() is running past it. A
// finished preview's target is cleared unless a new request replaced it.
static void end(void) {
    audio_out_set_mix(NULL);
    audio_source_hold_s3(false);
    uint32_t done = s_done_target;
    __atomic_compare_exchange_n(&s_target, &done, 0, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

static void alarm_task(void *arg) {
    (void)arg;
    bool active = false;
    for (;;) {
        if (!active) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            if (!__atomic_load_n(&s_target, __ATOMIC_ACQUIRE)) {
                continue;
            }
            begin();
            active = true;
        }
        if (audio_source_suspended()) {
            __atomic_store_n(&s_target, 0, __ATOMIC_RELEASE);
            end();
            active = false;
            continue;
        }
        topup();
        if (s_done) {
            end();
            active = false;
            if (__atomic_load_n(&s_target, __ATOMIC_ACQUIRE)) {
                xTaskNotifyGive(s_task);
            }
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(TASK_PERIOD_MS));
    }
}

void alarm_sound_init(void) {
    s_lut = heap_caps_malloc((LUT_SIZE + 1) * sizeof(*s_lut), MALLOC_CAP_SPIRAM);
    ESP_ERROR_CHECK(s_lut ? ESP_OK : ESP_ERR_NO_MEM);
    for (uint32_t i = 0; i <= LUT_SIZE; i++) {
        s_lut[i] = sinf(2.0f * (float)M_PI * i / LUT_SIZE);
    }
    BaseType_t ok = xTaskCreatePinnedToCore(alarm_task, "alarm_sound", TASK_STACK, NULL, TASK_PRIO, &s_task, TASK_CORE);
    ESP_ERROR_CHECK(ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}

void alarm_sound_configure(alarm_sound_voice_t voice, uint8_t volume_pct) {
    float v = volume_pct / 100.0f;
    s_voice = voice;
    s_gain_target = v * v * v;
}

static void post(uint32_t target) {
    if (audio_source_suspended()) {
        target = 0;
    }
    __atomic_store_n(&s_target, target, __ATOMIC_RELEASE);
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

void alarm_sound_start(alarm_sound_kind_t kind) {
    ESP_LOGI(TAG, "ringing (%d)", kind);
    post((uint32_t)kind + 1);
}

void alarm_sound_stop(void) {
    post(0);
}

void alarm_sound_preview(alarm_sound_kind_t kind) {
    uint32_t cur = __atomic_load_n(&s_target, __ATOMIC_ACQUIRE);
    if (cur && !(cur & TARGET_ONCE)) {
        return;
    }
    s_preview_gen++;
    post(((uint32_t)kind + 1) | TARGET_ONCE | (s_preview_gen << TARGET_GEN_SHIFT));
}
