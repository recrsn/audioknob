#include "voice_notes.h"
#include "voice_ogg.h"
#include "message_store.h"
#include "flash_message.h"
#include "talk.h"
#include "audio_source.h"
#include "audio_out.h"
#include "mic.h"
#include "pcm.h"
#include "device_name.h"
#include "encoder/impl/esp_opus_enc.h"
#include "decoder/impl/esp_opus_dec.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static SemaphoreHandle_t s_lock, s_write;
static QueueHandle_t s_audio, s_storage;
static ui_voice_t s_state;
static bool s_stop;
typedef enum { RECORD, PLAY, SAVE, LOAD } action_t;
typedef struct {
    action_t action;
    uint32_t peer;
    char id[17];
    uint8_t *data;
    size_t length;
} job_t;
static bool id_valid(const char *id) {
    if (!id || strlen(id) != 16)
        return false;
    for (int i = 0; i < 16; i++)
        if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f')))
            return false;
    return true;
}
static void state(ui_voice_state_t value, const char *message) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state.state = value;
    s_state.level = 0;
    snprintf(s_state.message, sizeof(s_state.message), "%s", message ? message : "");
    xSemaphoreGive(s_lock);
}
void voice_notes_get_state(ui_voice_t *out) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_state;
    xSemaphoreGive(s_lock);
}
static bool stopped(void) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool stop = s_stop;
    xSemaphoreGive(s_lock);
    return stop || audio_source_suspended() || audio_source_active() != AUDIO_SOURCE_NOTE;
}
void voice_notes_stop(void) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_stop = true;
    xSemaphoreGive(s_lock);
}
void voice_notes_preempt(void) {
    if (!s_lock)
        return;
    voice_notes_stop();
    xSemaphoreTake(s_write, portMAX_DELAY);
    xSemaphoreGive(s_write);
}
static bool begin_job(job_t *job, QueueHandle_t queue, ui_voice_state_t value) {
    if (!message_store_ready())
        return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_state.state != UI_VOICE_IDLE) {
        xSemaphoreGive(s_lock);
        return false;
    }
    bool ok = xQueueSend(queue, job, 0) == pdTRUE;
    if (ok) {
        s_state.state = value;
        s_state.seconds = 0;
        s_state.message[0] = 0;
        s_stop = false;
        snprintf(s_state.id, 17, "%s", job->id);
    }
    xSemaphoreGive(s_lock);
    return ok;
}
bool voice_notes_record(uint32_t peer, char id[17]) {
    job_t job = {.action = RECORD, .peer = peer};
    snprintf(job.id, sizeof(job.id), "%08lx%08lx", (unsigned long)esp_random(), (unsigned long)esp_random());
    bool ok = begin_job(&job, s_audio, UI_VOICE_RECORDING);
    if (ok)
        snprintf(id, 17, "%s", job.id);
    return ok;
}
bool voice_notes_play(const char *id) {
    if (!id_valid(id))
        return false;
    job_t job = {.action = LOAD};
    snprintf(job.id, 17, "%s", id);
    return begin_job(&job, s_storage, UI_VOICE_SAVING);
}
void *voice_notes_read(const char *id, size_t *length) {
    *length = 0;
    if (!id_valid(id))
        return NULL;
    char file[24];
    snprintf(file, sizeof(file), "%s.ogg", id);
    return message_store_read(file, length, VOICE_NOTE_MAX_BYTES);
}
bool voice_notes_receive(uint32_t peer, const char *sender, const char *id, const void *data, size_t length) {
    uint32_t duration;
    if (!id_valid(id) || length > VOICE_NOTE_MAX_BYTES || !voice_ogg_validate(data, length, &duration))
        return false;
    char file[24];
    snprintf(file, sizeof(file), "%s.ogg", id);
    // Identical retransmissions are idempotent; an ID never overwrites a note.
    size_t previous_size;
    void *previous = message_store_read(file, &previous_size, VOICE_NOTE_MAX_BYTES);
    if (previous) {
        bool same = length == previous_size && !memcmp(previous, data, length);
        free(previous);
        return same && flash_message_add_voice(peer, sender, id, duration, false);
    }
    if (!message_store_write(file, data, length))
        return false;
    if (flash_message_add_voice(peer, sender, id, duration, false))
        return true;
    message_store_remove(file);
    return false;
}
static void record_note(job_t *job) {
    void *encoder = NULL;
    bool mic = false;
    uint8_t *data = heap_caps_malloc(VOICE_NOTE_MAX_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    esp_opus_enc_config_t cfg = ESP_OPUS_ENC_CONFIG_DEFAULT();
    cfg.sample_rate = 16000;
    cfg.channel = 1;
    cfg.bitrate = 24000;
    cfg.complexity = 2;
    cfg.enable_vbr = true;
    size_t used = 0, last = 0;
    uint32_t frames = 0, serial = esp_random();
    if (!data || esp_opus_enc_open(&cfg, sizeof(cfg), &encoder) != ESP_AUDIO_ERR_OK ||
        !audio_source_claim(AUDIO_SOURCE_NOTE))
        goto done;
    for (int retry = 0; retry < 10 && !mic && !stopped(); retry++) {
        mic = mic_start(NULL);
        if (!mic)
            vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (!mic)
        goto done;
    used = voice_ogg_headers(data, VOICE_NOTE_MAX_BYTES, serial);
    while (!stopped() && frames < 3000) {
        int16_t pcm[320];
        uint8_t packet[1275];
        if (mic_read(pcm, 320, 60) != 320)
            break;
        esp_audio_enc_in_frame_t in = {.buffer = (uint8_t *)pcm, .len = sizeof(pcm)};
        esp_audio_enc_out_frame_t out = {.buffer = packet, .len = sizeof(packet)};
        if (esp_opus_enc_process(encoder, &in, &out) != ESP_AUDIO_ERR_OK || !out.encoded_bytes)
            break;
        size_t n = voice_ogg_packet(data + used, VOICE_NOTE_MAX_BYTES - used, serial, frames + 2,
                                    (uint64_t)(frames + 1) * 960, false, packet, out.encoded_bytes);
        if (!n)
            break;
        last = used;
        used += n;
        frames++;
        int peak = 0;
        for (int i = 0; i < 320; i++) {
            int v = abs(pcm[i]);
            if (v > peak)
                peak = v;
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_state.seconds = frames / 50;
        s_state.level = peak * 100 / 32768;
        xSemaphoreGive(s_lock);
    }
done:
    if (mic)
        mic_stop();
    if (encoder)
        esp_opus_enc_close(encoder);
    audio_source_release(AUDIO_SOURCE_NOTE);
    if (frames >= 10 && voice_ogg_mark_end(data + last, used - last)) {
        job->action = SAVE;
        job->data = data;
        job->length = used;
        state(UI_VOICE_SAVING, NULL);
        if (xQueueSend(s_storage, job, 0) == pdTRUE)
            return;
    }
    free(data);
    state(UI_VOICE_IDLE, frames < 10 ? "Note too short" : "Could not save voice note");
}
static void play_note(job_t *job) {
    voice_ogg_reader_t reader;
    void *decoder = NULL;
    esp_opus_dec_cfg_t cfg = {.sample_rate = 16000,
                              .channel = 1,
                              .frame_duration = ESP_OPUS_DEC_FRAME_DURATION_20_MS,
                              .self_delimited = false};
    bool ok = voice_ogg_open(&reader, job->data, job->length) &&
              esp_opus_dec_open(&cfg, sizeof(cfg), &decoder) == ESP_AUDIO_ERR_OK &&
              audio_source_claim(AUDIO_SOURCE_NOTE);
    if (ok) {
        state(UI_VOICE_PLAYING, NULL);
        pcm_resampler_t resampler;
        pcm_resampler_init(&resampler, 16000, 44100);
        int32_t gain = 0;
        size_t skip = reader.preskip / 3;
        const uint8_t *packet;
        size_t length;
        uint32_t frames = 0;
        while (!stopped() && voice_ogg_next(&reader, job->data, job->length, &packet, &length) == 1) {
            int16_t pcm[640], output[1800];
            esp_audio_dec_info_t info;
            esp_audio_dec_in_raw_t raw = {.buffer = (uint8_t *)packet, .len = length};
            esp_audio_dec_out_frame_t out = {.buffer = (uint8_t *)pcm, .len = 640};
            if (esp_opus_dec_decode(decoder, &raw, &out, &info) != ESP_AUDIO_ERR_OK || out.decoded_size != 640) {
                ok = false;
                break;
            }
            size_t trim = skip > 320 ? 320 : skip;
            skip -= trim;
            size_t samples = 320 - trim;
            memmove(pcm, pcm + trim, samples * 2);
            pcm_to_stereo(pcm, samples, 1);
            size_t n = pcm_resample(&resampler, pcm, samples, output);
            ui_talk_t talk;
            talk_get_state(&talk);
            pcm_apply_gain(output, n, &gain, pcm_gain_for_volume(talk.volume));
            xSemaphoreTake(s_write, portMAX_DELAY);
            if (!stopped())
                audio_out_write(output, n);
            xSemaphoreGive(s_write);
            frames++;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_state.seconds = frames / 50;
            xSemaphoreGive(s_lock);
        }
    }
    if (decoder)
        esp_opus_dec_close(decoder);
    audio_source_release(AUDIO_SOURCE_NOTE);
    free(job->data);
    state(UI_VOICE_IDLE, ok ? NULL : "Cannot play voice note");
}
static void audio_worker(void *arg) {
    (void)arg;
    job_t job;
    for (;;)
        if (xQueueReceive(s_audio, &job, portMAX_DELAY) == pdTRUE) {
            if (job.action == RECORD)
                record_note(&job);
            else
                play_note(&job);
        }
}
static void storage_worker(void *arg) {
    (void)arg;
    job_t job;
    for (;;) {
        if (xQueueReceive(s_storage, &job, portMAX_DELAY) != pdTRUE)
            continue;
        if (job.action == LOAD) {
            job.data = voice_notes_read(job.id, &job.length);
            uint32_t duration;
            if (job.data && voice_ogg_validate(job.data, job.length, &duration)) {
                job.action = PLAY;
                if (xQueueSend(s_audio, &job, 0) == pdTRUE)
                    continue;
            }
            free(job.data);
            state(UI_VOICE_IDLE, "Cannot read voice note");
        } else {
            char file[24], name[33];
            snprintf(file, sizeof(file), "%s.ogg", job.id);
            device_name_get(name, sizeof(name));
            uint32_t duration;
            bool ok =
                voice_ogg_validate(job.data, job.length, &duration) && message_store_write(file, job.data, job.length);
            free(job.data);
            if (ok) {
                ok = flash_message_add_voice(0, name, job.id, duration, true);
                if (!ok)
                    message_store_remove(file);
            }
            if (ok && job.peer)
                ok = flash_message_send_voice(job.peer, job.id);
            state(UI_VOICE_IDLE,
                  ok ? (job.peer ? "Voice note queued" : "Voice note saved") : "Could not save or send note");
        }
    }
}
void voice_notes_init(void) {
    s_lock = xSemaphoreCreateMutex();
    s_write = xSemaphoreCreateMutex();
    s_audio = xQueueCreate(1, sizeof(job_t));
    s_storage = xQueueCreate(1, sizeof(job_t));
    ESP_ERROR_CHECK(s_lock && s_write && s_audio && s_storage ? ESP_OK : ESP_ERR_NO_MEM);
    s_state.storage_ready = message_store_ready();
    BaseType_t a = xTaskCreatePinnedToCoreWithCaps(audio_worker, "voice_notes", 40960, NULL, 7, NULL, 0,
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    BaseType_t b = xTaskCreate(storage_worker, "note_store", 6144, NULL, 3, NULL);
    ESP_ERROR_CHECK(a == pdPASS && b == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
