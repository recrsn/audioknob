#include "airplay_player.h"

#include <stdbool.h>
#include <string.h>

#include "audio_out.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "raop.h"

#define SLOT_FRAMES 352  // one AirPlay packet
// ~3.2 s: RAOP's maximum 2.4 s latency plus network jitter.
#define SLOT_COUNT 400
#define FRAME_MS ((SLOT_FRAMES * 1000) / RAOP_SAMPLE_RATE)

// Average the scheduling error over ~250 ms before correcting it, so DMA
// and scheduler jitter don't trigger corrections on their own.
#define SYNC_WINDOW 32
#define SYNC_TOLERANCE_MS 10
#define IDLE_AFTER_MS 200

#define PLAYER_STACK_SIZE 4096
#define PLAYER_TASK_PRIO 15
#define PLAYER_TASK_CORE 1

#define GAIN_UNITY 32768

typedef struct {
    uint32_t playtime_ms;
    uint16_t frames;
    int16_t pcm[SLOT_FRAMES * 2];
} slot_t;

static const char *TAG = "airplay_player";

static slot_t *s_slots;
static uint16_t s_head, s_count;
static uint32_t s_dropped;
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static volatile int32_t s_gain = GAIN_UNITY;

static int16_t s_out[SLOT_FRAMES * 2];
static const int16_t s_silence[SLOT_FRAMES * 2];

// Caller holds s_lock.
static void pop_locked(int n) {
    if (n > s_count) {
        n = s_count;
    }
    s_head = (s_head + n) % SLOT_COUNT;
    s_count -= n;
}

static void write_silence_ms(int32_t ms) {
    size_t frames = (size_t)ms * RAOP_SAMPLE_RATE / 1000;
    while (frames > 0) {
        size_t chunk = frames < SLOT_FRAMES ? frames : SLOT_FRAMES;
        audio_out_write(s_silence, chunk);
        frames -= chunk;
    }
}

// Caller holds s_lock. Copies the head slot into s_out with gain applied.
static size_t take_head_locked(void) {
    const slot_t *slot = &s_slots[s_head];
    int32_t gain = s_gain;
    size_t frames = slot->frames;

    for (size_t i = 0; i < frames * 2; i++) {
        s_out[i] = (int16_t)((slot->pcm[i] * gain) >> 15);
    }
    pop_locked(1);
    return frames;
}

static void player_task(void *arg) {
    (void)arg;
    bool running = false;
    int32_t error_sum = 0;
    int error_count = 0;
    uint32_t last_audio_ms = 0;

    for (;;) {
        xSemaphoreTake(s_lock, portMAX_DELAY);

        if (s_count == 0) {
            xSemaphoreGive(s_lock);
            if (running && raop_time_ms() - last_audio_ms > IDLE_AFTER_MS) {
                running = false;
            }
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
            continue;
        }

        uint32_t now = raop_time_ms();
        // How early (positive) or late (negative) the head frame would reach
        // the DAC if written right now.
        int32_t lead = (int32_t)(s_slots[s_head].playtime_ms - (now + audio_out_latency_ms()));

        if (!running) {
            if (lead > 1) {
                xSemaphoreGive(s_lock);
                // Woken early by every push/flush; the loop just re-evaluates.
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(lead));
                continue;
            }
            if (lead < -FRAME_MS) {
                pop_locked(1);
                xSemaphoreGive(s_lock);
                continue;
            }
            running = true;
            error_sum = error_count = 0;
        } else {
            error_sum += lead;
            if (++error_count == SYNC_WINDOW) {
                int32_t avg = error_sum / error_count;
                error_sum = error_count = 0;

                if (avg < -SYNC_TOLERANCE_MS) {
                    int drop = (-avg + FRAME_MS / 2) / FRAME_MS;
                    pop_locked(drop > 0 ? drop : 1);
                    xSemaphoreGive(s_lock);
                    ESP_LOGI(TAG, "%ld ms behind sender, skipped %d packet(s)", (long)-avg, drop);
                    continue;
                }
                if (avg > SYNC_TOLERANCE_MS) {
                    xSemaphoreGive(s_lock);
                    ESP_LOGI(TAG, "%ld ms ahead of sender, padding silence", (long)avg);
                    write_silence_ms(avg);
                    continue;
                }
            }
        }

        size_t frames = take_head_locked();
        xSemaphoreGive(s_lock);

        last_audio_ms = now;
        audio_out_write(s_out, frames);
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

void airplay_player_push(const int16_t *pcm, size_t frames, uint32_t playtime_ms) {
    size_t offset = 0;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    while (offset < frames) {
        if (s_count == SLOT_COUNT) {
            if (s_dropped++ % 100 == 0) {
                ESP_LOGW(TAG, "queue full, dropping audio (%lu dropped)", (unsigned long)s_dropped);
            }
            break;
        }

        size_t chunk = frames - offset < SLOT_FRAMES ? frames - offset : SLOT_FRAMES;
        slot_t *slot = &s_slots[(s_head + s_count) % SLOT_COUNT];
        slot->playtime_ms = playtime_ms + (uint32_t)((offset * 1000) / RAOP_SAMPLE_RATE);
        slot->frames = chunk;
        memcpy(slot->pcm, pcm + offset * 2, chunk * 2 * sizeof(int16_t));
        s_count++;
        offset += chunk;
    }
    xSemaphoreGive(s_lock);

    xTaskNotifyGive(s_task);
}

void airplay_player_flush(void) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    pop_locked(s_count);
    xSemaphoreGive(s_lock);
    xTaskNotifyGive(s_task);
}

void airplay_player_set_volume(uint8_t volume_pct) {
    float v = volume_pct / 100.0f;
    s_gain = (int32_t)(v * v * v * GAIN_UNITY);
}
