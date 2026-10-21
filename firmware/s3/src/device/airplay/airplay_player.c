#include "airplay_player.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "audio_out.h"
#include "audio_source.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define SLOT_FRAMES 352  // one AirPlay packet
// ~3.2 s: RAOP's maximum 2.4 s latency plus network jitter.
#define SLOT_COUNT 400

// A frame this far off schedule means a real disturbance (stall, sender
// clock jump), corrected at once: late frames are dropped until back on
// time, an early one gets the silence it is ahead by. Anything smaller is
// left to the trim servo.
#define GATE_US 50000
#define IDLE_AFTER_US 200000

// Trim servo, after rbouteiller/airplay-esp32's audio_timing.c. The error
// is low-passed with a clamped innovation, because a late read (task
// briefly starved) can only ever measure "late": spikes must not drag the
// average. One sample per TRIM_INTERVAL frames is ~710 ppm, far below an
// audible pitch change but well above crystal drift.
#define SERVO_FILTER_DIV 16
#define SERVO_INNOV_CLAMP_US 1500
#define SERVO_ENGAGE_US 5000
#define SERVO_DISENGAGE_US 1500
#define TRIM_INTERVAL 4
// Right after a start the DMA cursor reads a few ms off (the idle ring's
// free descriptors), a constant for the rest of the stream. Measure it once
// the ring has cycled and servo around it instead of trimming it away.
#define CAL_SETTLE_FRAMES 32
#define CAL_FRAMES 32
// Feed-forward: the DAC clock's offset from the sender's is near constant
// (the S3 has no audio PLL, so 44.1 kHz is only approximated), so learn it
// and spread the same correction evenly, leaving the servo only the
// residual. The drift is the net trims plus how far the error moved: trims
// that only remove an offset (a start, a re-align) are matched by the error
// they remove, where counting trims alone read a 5 ms correction as
// hundreds of ppm. Learned per boot, across streams, and per rate: 44.1 and
// 48 kHz come from different clock dividers.
#define FF_MIN_S 10
#define FF_MAX_S 600

#define PLAYER_STACK_SIZE 4096
#define PLAYER_TASK_PRIO 15
#define PLAYER_TASK_CORE 1

#define GAIN_UNITY 32768
// Per-frame exponential approach: ~3 ms time constant.
#define GAIN_RAMP_DIV 256

typedef struct {
    int64_t playtime_us;
    uint32_t rate;
    uint16_t frames;
    int32_t pcm[SLOT_FRAMES * 2];
} slot_t;

typedef struct {
    uint32_t rate;
    // Net samples added (+) or removed (-), how far the error moved
    // meanwhile (early is +), and the frames played.
    int64_t net, err_us, frames;
} ff_t;

typedef struct {
    bool running;
    bool draining;  // inside a late-drop run
    bool servo_on;
    int servo_phase;
    int cal_frames;
    int64_t cal_sum_us, bias_us;
    float ff_acc;
    bool ff_have_err;  // ff_last_err_us is from this run of the stream
    int64_t ff_last_err_us;
    int64_t err_filtered_us;
    int64_t last_audio_us;
    uint32_t trims, drops;
} sync_t;

static const char *TAG = "airplay_player";

static slot_t *s_slots;
static uint16_t s_head, s_count;
static uint32_t s_dropped;
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static volatile bool s_flushed;
static volatile int32_t s_gain_target = GAIN_UNITY;
static int32_t s_gain = GAIN_UNITY;
static ff_t s_ff[] = {{.rate = 44100}, {.rate = 48000}};

// One spare frame for a stretch trim.
static int32_t s_out[(SLOT_FRAMES + 1) * 2];

// Caller holds s_lock.
static void pop_locked(int n) {
    if (n > s_count) {
        n = s_count;
    }
    s_head = (s_head + n) % SLOT_COUNT;
    s_count -= n;
}

static int64_t frames_to_us(int64_t frames, uint32_t rate) {
    return frames * 1000000 / rate;
}

static ff_t *ff_for(uint32_t rate) {
    return &s_ff[rate == s_ff[0].rate ? 0 : 1];
}

// Samples the DAC gains on the sender per sample played.
static double ff_drift(const ff_t *ff) {
    return ((double)ff->net + (double)ff->err_us * ff->rate / 1000000.0) / (double)ff->frames;
}

static void write_silence_us(int64_t us) {
    size_t frames = (size_t)(us * audio_out_rate() / 1000000);
    memset(s_out, 0, sizeof(s_out));
    while (frames > 0) {
        size_t chunk = frames < SLOT_FRAMES ? frames : SLOT_FRAMES;
        audio_out_write(s_out, chunk);
        frames -= chunk;
    }
}

// How early (positive) or late (negative) a frame would reach the DAC if
// written now.
static int64_t lead_us(int64_t playtime_us) {
    int64_t now;
    uint32_t queued = audio_out_queued_us(&now);
    return playtime_us - (now + queued);
}

// The quietest point is where dropping or repeating a sample leaves the
// smallest step in the waveform.
static size_t quietest_frame(const int32_t *pcm, size_t frames) {
    size_t best = frames - 1;
    int64_t best_mag = INT64_MAX;
    for (size_t i = 0; i < frames; i++) {
        int64_t mag = llabs((int64_t)pcm[i * 2]) + llabs((int64_t)pcm[i * 2 + 1]);
        if (mag < best_mag) {
            best_mag = mag;
            best = i;
        }
    }
    return best;
}

// Copies pcm into s_out, one frame shorter (adjust < 0) or longer (> 0).
static size_t copy_trimmed(const int32_t *pcm, size_t frames, int adjust) {
    if (adjust == 0) {
        memcpy(s_out, pcm, frames * 2 * sizeof(int32_t));
        return frames;
    }
    size_t at = quietest_frame(pcm, frames);
    if (adjust < 0) {
        memcpy(s_out, pcm, at * 2 * sizeof(int32_t));
        memcpy(s_out + at * 2, pcm + (at + 1) * 2, (frames - at - 1) * 2 * sizeof(int32_t));
        return frames - 1;
    }
    memcpy(s_out, pcm, (at + 1) * 2 * sizeof(int32_t));
    memcpy(s_out + (at + 1) * 2, pcm + at * 2, (frames - at) * 2 * sizeof(int32_t));
    return frames + 1;
}

static void apply_gain(size_t frames) {
    int32_t target = s_gain_target;
    for (size_t i = 0; i < frames; i++) {
        if (s_gain != target) {
            int32_t step = (target - s_gain) / GAIN_RAMP_DIV;
            s_gain += step ? step : (target > s_gain ? 1 : -1);
        }
        s_out[i * 2] = (int32_t)(((int64_t)s_out[i * 2] * s_gain) >> 15);
        s_out[i * 2 + 1] = (int32_t)(((int64_t)s_out[i * 2 + 1] * s_gain) >> 15);
    }
}

static int feed_forward(sync_t *sync, const ff_t *ff, size_t frames) {
    if (ff->frames < (int64_t)FF_MIN_S * ff->rate) {
        return 0;
    }
    sync->ff_acc += (float)(ff_drift(ff) * (double)frames);
    if (sync->ff_acc >= 1.0f) {
        sync->ff_acc -= 1.0f;
        return 1;
    }
    if (sync->ff_acc <= -1.0f) {
        sync->ff_acc += 1.0f;
        return -1;
    }
    return 0;
}

static void ff_record(sync_t *sync, ff_t *ff, int adjust, size_t frames) {
    int64_t err = sync->err_filtered_us;
    if (sync->ff_have_err) {
        ff->err_us += err - sync->ff_last_err_us;
    }
    sync->ff_last_err_us = err;
    sync->ff_have_err = true;
    ff->net += adjust;
    ff->frames += frames;
    if (ff->frames > (int64_t)FF_MAX_S * ff->rate) {
        // Age old history so a changed rate (another sender) is relearned.
        ff->net /= 2;
        ff->err_us /= 2;
        ff->frames /= 2;
    }
}

// Returns the one-sample adjustment the servo wants for the frame about to
// play, given its current lead.
static int servo_update(sync_t *sync, const ff_t *ff, int64_t lead) {
    if (sync->cal_frames < CAL_SETTLE_FRAMES + CAL_FRAMES) {
        if (sync->cal_frames++ >= CAL_SETTLE_FRAMES) {
            sync->cal_sum_us += lead;
        }
        if (sync->cal_frames == CAL_SETTLE_FRAMES + CAL_FRAMES) {
            sync->bias_us = sync->cal_sum_us / CAL_FRAMES;
            ESP_LOGI(TAG, "output offset %lld us", (long long)sync->bias_us);
        }
        return 0;
    }
    lead -= sync->bias_us;

    int64_t innov = lead - sync->err_filtered_us;
    if (innov > SERVO_INNOV_CLAMP_US) {
        innov = SERVO_INNOV_CLAMP_US;
    } else if (innov < -SERVO_INNOV_CLAMP_US) {
        innov = -SERVO_INNOV_CLAMP_US;
    }
    sync->err_filtered_us += innov / SERVO_FILTER_DIV;

    int64_t mag = llabs(sync->err_filtered_us);
    if (!sync->servo_on && mag > SERVO_ENGAGE_US) {
        sync->servo_on = true;
        sync->servo_phase = 0;
        ESP_LOGI(TAG, "trimming: %lld us %s", (long long)mag, sync->err_filtered_us > 0 ? "early" : "late");
    } else if (sync->servo_on && mag < SERVO_DISENGAGE_US) {
        sync->servo_on = false;
        ESP_LOGI(TAG, "in sync (%lu trims so far, clock %+ld ppm, underruns %lu)", (unsigned long)sync->trims,
                 ff->frames ? (long)(ff_drift(ff) * 1000000.0) : 0L, (unsigned long)audio_out_underruns());
    }

    if (!sync->servo_on || ++sync->servo_phase < TRIM_INTERVAL) {
        return 0;
    }
    sync->servo_phase = 0;
    sync->trims++;
    // Early: stretch so playout slows down; late: shrink to catch up.
    return sync->err_filtered_us > 0 ? 1 : -1;
}

// Caller holds s_lock; releases it.
static void play_head_locked(sync_t *sync) {
    const slot_t *slot = &s_slots[s_head];
    const int64_t frame_us = frames_to_us(slot->frames, slot->rate);

    if (slot->rate != audio_out_rate()) {
        // Reclocking waits for the old rate's audio to play out, then the
        // stream re-aligns at the new one like a fresh start.
        uint32_t rate = slot->rate;
        xSemaphoreGive(s_lock);
        audio_out_set_rate(rate);
        sync->running = false;
        if (audio_out_rate() != rate) {
            // The clock refused it: drop the audio rather than retry forever.
            airplay_player_flush();
        }
        return;
    }
    int64_t lead = lead_us(slot->playtime_us);

    if (!sync->running) {
        if (lead < -frame_us / 2) {
            pop_locked(1);
            xSemaphoreGive(s_lock);
            return;
        }
        if (lead > GATE_US) {
            xSemaphoreGive(s_lock);
            // Woken early by every push/flush; the loop just re-evaluates.
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS((lead - GATE_US / 2) / 1000));
            return;
        }
        // Pad up to the exact start sample so the stream begins in sync,
        // and start at the target gain rather than ramping into it.
        xSemaphoreGive(s_lock);
        if (lead > 0) {
            write_silence_us(lead);
        }
        memset(sync, 0, sizeof(*sync));
        sync->running = true;
        s_gain = s_gain_target;
        return;
    }

    int64_t late_limit = sync->draining ? -frame_us / 2 : -GATE_US;
    if (lead < late_limit) {
        // Drop until back on schedule, not merely back inside the gate, or
        // the session would sit at -GATE_US for good.
        sync->draining = true;
        sync->drops++;
        pop_locked(1);
        xSemaphoreGive(s_lock);
        return;
    }
    if (sync->draining) {
        ESP_LOGW(TAG, "behind sender, dropped %lu packet(s)", (unsigned long)sync->drops);
        sync->draining = false;
        sync->drops = 0;
        sync->err_filtered_us = lead - sync->bias_us;
        // The drops moved the error, not the clock.
        sync->ff_have_err = false;
    }
    if (lead > GATE_US) {
        // The schedule jumped ahead: restart, which waits and re-aligns.
        xSemaphoreGive(s_lock);
        ESP_LOGW(TAG, "%lld ms ahead of sender, re-aligning", (long long)(lead / 1000));
        sync->running = false;
        return;
    }

    ff_t *ff = ff_for(slot->rate);
    int adjust = servo_update(sync, ff, lead);
    if (sync->cal_frames >= CAL_SETTLE_FRAMES + CAL_FRAMES) {
        adjust += feed_forward(sync, ff, slot->frames);
        adjust = adjust > 1 ? 1 : (adjust < -1 ? -1 : adjust);
        ff_record(sync, ff, adjust, slot->frames);
    }
    size_t frames = copy_trimmed(slot->pcm, slot->frames, adjust);
    pop_locked(1);
    xSemaphoreGive(s_lock);

    apply_gain(frames);
    audio_out_write(s_out, frames);
    sync->last_audio_us = esp_timer_get_time();
}

static void player_task(void *arg) {
    (void)arg;
    sync_t sync = {0};

    for (;;) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_flushed) {
            // Pause or seek: whatever comes next starts a new stream.
            s_flushed = false;
            sync.running = false;
        }
        if (s_count == 0) {
            xSemaphoreGive(s_lock);
            if (sync.running && esp_timer_get_time() - sync.last_audio_us > IDLE_AFTER_US) {
                sync.running = false;
            }
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
            continue;
        }
        play_head_locked(&sync);
    }
}

void airplay_player_init(void) {
    s_slots = heap_caps_calloc(SLOT_COUNT, sizeof(slot_t), MALLOC_CAP_SPIRAM);
    s_lock = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(s_slots && s_lock ? ESP_OK : ESP_ERR_NO_MEM);

    BaseType_t ok = xTaskCreatePinnedToCore(player_task, "airplay_play", PLAYER_STACK_SIZE, NULL, PLAYER_TASK_PRIO,
                                            &s_task, PLAYER_TASK_CORE);
    ESP_ERROR_CHECK(ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}

void airplay_player_push(const int32_t *pcm, size_t frames, int64_t playtime_us, uint32_t rate) {
    size_t offset = 0;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (audio_source_suspended()) {
        xSemaphoreGive(s_lock);
        return;
    }
    while (offset < frames) {
        if (s_count == SLOT_COUNT) {
            if (s_dropped++ % 100 == 0) {
                ESP_LOGW(TAG, "queue full, dropping audio (%lu dropped)", (unsigned long)s_dropped);
            }
            break;
        }

        size_t chunk = frames - offset < SLOT_FRAMES ? frames - offset : SLOT_FRAMES;
        slot_t *slot = &s_slots[(s_head + s_count) % SLOT_COUNT];
        slot->playtime_us = playtime_us + frames_to_us((int64_t)offset, rate);
        slot->rate = rate;
        slot->frames = chunk;
        if (pcm) {
            memcpy(slot->pcm, pcm + offset * 2, chunk * 2 * sizeof(int32_t));
        } else {
            memset(slot->pcm, 0, chunk * 2 * sizeof(int32_t));
        }
        s_count++;
        offset += chunk;
    }
    xSemaphoreGive(s_lock);

    xTaskNotifyGive(s_task);
}

void airplay_player_flush(void) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    pop_locked(s_count);
    s_flushed = true;
    xSemaphoreGive(s_lock);
    xTaskNotifyGive(s_task);
}

void airplay_player_set_volume(uint8_t volume_pct) {
    float v = volume_pct / 100.0f;
    s_gain_target = (int32_t)(v * v * v * GAIN_UNITY);
}
