#include "talk/note/audio.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio/alarm_sound.h"
#include "audio/mic/mic.h"
#include "audio/pcm.h"
#include "decoder/impl/esp_opus_dec.h"
#include "encoder/impl/esp_opus_enc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "talk/note/notes.h"
#include "talk/note/ogg.h"

#define FRAME_SAMPLES 320 // 20 ms at 16 kHz
#define FRAMES_PER_TENTH 5
#define MAX_FRAMES (UI_NOTE_MAX_TENTHS * FRAMES_PER_TENTH)
#define MIN_FRAMES 25 // shorter is a slip of the finger
#define OPUS_RATIO 3  // Ogg Opus pre-skip counts 48 kHz samples
#define CLIP_WAIT_MS 200
#define DRAIN_POLL_MS 10

typedef enum { REC_IDLE, REC_START, REC_ON, REC_DRAFT } rec_state_t;
typedef enum { REC_KEEP_GOING, REC_SEND, REC_DISCARD, REC_PAUSE } rec_action_t;
typedef enum { PLAY_IDLE, PLAY_LOADING, PLAY_ON, PLAY_DRAIN } play_state_t;

static const char *TAG = "note_audio";
static SemaphoreHandle_t s_lock;

// Fields under s_lock unless marked worker-only.
static struct {
    rec_state_t state;
    rec_action_t action;
    char key[UI_TALK_KEY];
    uint8_t *ogg; // a draft's, else worker-only while recording
    size_t used;
    uint32_t frames;
    uint8_t level;
    uint8_t wave[UI_NOTE_WAVE];
    // Worker-only.
    size_t last_page;
    uint32_t serial;
    uint8_t *levels; // per frame
    void *encoder;
    bool mic, clip;
} r;

static struct {
    play_state_t state;
    bool stop;
    char id[17], next[17];
    uint8_t *ogg;
    size_t len;
    uint32_t frames;
    // Worker-only.
    note_ogg_reader_t reader;
    void *decoder;
    bool clip;
    size_t skip;
    int32_t gain;
} p;

static void lock(void) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock(void) {
    xSemaphoreGive(s_lock);
}

void note_audio_init(void) {
    s_lock = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(s_lock ? ESP_OK : ESP_ERR_NO_MEM);
}

// -60..0 dBFS as 0-100.
static uint8_t level_of(const int16_t *pcm, size_t n) {
    double sum = 0;
    for (size_t i = 0; i < n; i++) {
        sum += (double)pcm[i] * pcm[i];
    }
    float db = 20 * log10f((float)sqrt(sum / n) / 32768.f + 0.000001f);
    int pct = (int)((db + 60) * (100.f / 60));
    return pct < 0 ? 0 : pct > 100 ? 100 : pct;
}

// ---- Recording ----

static void rec_release(void) {
    if (r.mic) {
        mic_stop();
    }
    if (r.encoder) {
        esp_opus_enc_close(r.encoder);
    }
    if (r.clip) {
        alarm_sound_clip_stop();
    }
    free(r.levels);
    r.levels = NULL;
    r.encoder = NULL;
    r.mic = r.clip = false;
}

// Caller holds the lock.
static void rec_clear(void) {
    free(r.ogg);
    r.ogg = NULL;
    r.used = r.frames = 0;
    r.level = 0;
    r.state = REC_IDLE;
    r.action = REC_KEEP_GOING;
}

static bool rec_open(void) {
    esp_opus_enc_config_t enc = ESP_OPUS_ENC_CONFIG_DEFAULT();
    enc.sample_rate = MIC_SAMPLE_RATE;
    enc.channel = 1;
    enc.bitrate = 24000;
    enc.complexity = 0;
    enc.enable_vbr = true;
    int input, output;
    r.ogg = heap_caps_malloc(NOTE_MAX_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    r.levels = heap_caps_calloc(MAX_FRAMES, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!r.ogg || !r.levels || esp_opus_enc_open(&enc, sizeof(enc), &r.encoder) != ESP_AUDIO_ERR_OK ||
        esp_opus_enc_get_frame_size(r.encoder, &input, &output) != ESP_AUDIO_ERR_OK || input != FRAME_SAMPLES * 2) {
        return false;
    }
    for (int retry = 0; retry < 10 && !r.mic; retry++) {
        r.mic = mic_start(NULL);
        if (!r.mic) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    if (!r.mic) {
        return false;
    }
    r.clip = alarm_sound_clip_start();
    r.serial = esp_random();
    r.used = note_ogg_headers(r.ogg, NOTE_MAX_BYTES, r.serial);
    r.last_page = 0;
    return r.used > 0;
}

// The loudest frame of each stretch.
static void rec_wave(void) {
    memset(r.wave, 0, sizeof(r.wave));
    for (uint32_t i = 0; i < r.frames; i++) {
        uint8_t *bar = &r.wave[i * UI_NOTE_WAVE / r.frames];
        *bar = r.levels[i] > *bar ? r.levels[i] : *bar;
    }
}

// Ends a recording as a draft, or drops it when too short or broken.
static void rec_stop(void) {
    bool keep = r.frames >= MIN_FRAMES && note_ogg_mark_end(r.ogg + r.last_page, r.used - r.last_page);
    if (keep) {
        rec_wave();
        uint8_t *shrunk = heap_caps_realloc(r.ogg, r.used, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (shrunk) {
            r.ogg = shrunk;
        }
    }
    rec_release();
    lock();
    if (keep) {
        r.state = REC_DRAFT;
        r.level = 0;
    } else {
        rec_clear();
    }
    unlock();
    ESP_LOGI(TAG, "recorded %lu frames, %u bytes%s", (unsigned long)r.frames, (unsigned)r.used,
             keep ? "" : ", dropped");
}

// Caller holds the lock; hands the draft to notes.c once unlocked.
static void take_draft(uint8_t **ogg, size_t *len, uint16_t *tenths, uint8_t wave[UI_NOTE_WAVE], char key[UI_TALK_KEY]) {
    *ogg = r.ogg;
    *len = r.used;
    *tenths = (uint16_t)(r.frames / FRAMES_PER_TENTH);
    memcpy(wave, r.wave, UI_NOTE_WAVE);
    snprintf(key, UI_TALK_KEY, "%s", r.key);
    r.ogg = NULL;
    rec_clear();
}

static void send_draft_locked_then_unlock(void) {
    uint8_t *ogg;
    size_t len;
    uint16_t tenths;
    uint8_t wave[UI_NOTE_WAVE];
    char key[UI_TALK_KEY];
    take_draft(&ogg, &len, &tenths, wave, key);
    unlock();
    notes_send(key, ogg, len, tenths, wave);
}

static void rec_frame(void) {
    int16_t pcm[FRAME_SAMPLES];
    uint8_t packet[1275];
    if (mic_read(pcm, FRAME_SAMPLES, 60) != FRAME_SAMPLES) {
        rec_stop();
        return;
    }
    esp_audio_enc_in_frame_t in = {.buffer = (uint8_t *)pcm, .len = sizeof(pcm)};
    esp_audio_enc_out_frame_t out = {.buffer = packet, .len = sizeof(packet)};
    size_t n = 0;
    if (esp_opus_enc_process(r.encoder, &in, &out) == ESP_AUDIO_ERR_OK && out.encoded_bytes) {
        n = note_ogg_packet(r.ogg + r.used, NOTE_MAX_BYTES - r.used, r.serial, r.frames + 2,
                            (uint64_t)(r.frames + 1) * FRAME_SAMPLES * OPUS_RATIO, false, packet, out.encoded_bytes);
    }
    if (!n) {
        rec_stop();
        return;
    }
    uint8_t level = level_of(pcm, FRAME_SAMPLES);
    r.levels[r.frames] = level;
    lock();
    r.last_page = r.used;
    r.used += n;
    r.frames++;
    r.level = level;
    unlock();
    if (r.frames == MAX_FRAMES) {
        rec_stop();
    }
}

static void rec_step(rec_state_t state, rec_action_t action) {
    if (state == REC_START) {
        if (action == REC_KEEP_GOING && rec_open()) {
            lock();
            r.state = REC_ON;
            unlock();
            ESP_LOGI(TAG, "recording");
            return;
        }
        rec_release();
        lock();
        rec_clear();
        unlock();
        return;
    }
    if (action == REC_KEEP_GOING) {
        rec_frame();
        return;
    }
    rec_stop();
    lock();
    if (r.state != REC_DRAFT) {
        unlock();
    } else if (action == REC_SEND) {
        send_draft_locked_then_unlock();
    } else {
        if (action == REC_DISCARD) {
            rec_clear();
        }
        unlock();
    }
}

// ---- Playback ----

static void play_release(void) {
    if (p.clip) {
        alarm_sound_clip_stop();
    }
    if (p.decoder) {
        esp_opus_dec_close(p.decoder);
    }
    p.clip = false;
    p.decoder = NULL;
}

// Ends playback; a tap on another note meanwhile starts that one.
static void play_end(bool played) {
    play_release();
    char id[17], next[17];
    lock();
    snprintf(id, sizeof(id), "%s", p.id);
    snprintf(next, sizeof(next), "%s", p.next);
    free(p.ogg);
    p.ogg = NULL;
    p.next[0] = '\0';
    p.stop = false;
    p.frames = 0;
    p.state = next[0] ? PLAY_LOADING : PLAY_IDLE;
    snprintf(p.id, sizeof(p.id), "%s", next[0] ? next : "");
    unlock();
    if (played) {
        notes_played(id);
    }
    if (next[0]) {
        notes_play(next);
    }
}

static bool play_open(void) {
    esp_opus_dec_cfg_t dec = {.sample_rate = MIC_SAMPLE_RATE,
                              .channel = 1,
                              .frame_duration = ESP_OPUS_DEC_FRAME_DURATION_20_MS,
                              .self_delimited = false};
    if (!note_ogg_open(&p.reader, p.ogg, p.len) || esp_opus_dec_open(&dec, sizeof(dec), &p.decoder) != ESP_AUDIO_ERR_OK) {
        return false;
    }
    p.clip = alarm_sound_clip_start();
    p.skip = p.reader.preskip / OPUS_RATIO;
    p.gain = 0;
    return p.clip;
}

static void play_frame(uint8_t volume) {
    const uint8_t *packet;
    size_t length;
    int rc = note_ogg_next(&p.reader, p.ogg, p.len, &packet, &length);
    if (rc == 0) {
        alarm_sound_clip_finish();
        lock();
        p.state = PLAY_DRAIN;
        unlock();
        return;
    }
    int16_t pcm[FRAME_SAMPLES];
    esp_audio_dec_in_raw_t raw = {.buffer = (uint8_t *)packet, .len = length};
    esp_audio_dec_out_frame_t out = {.buffer = (uint8_t *)pcm, .len = sizeof(pcm)};
    esp_audio_dec_info_t info;
    if (rc < 0 || esp_opus_dec_decode(p.decoder, &raw, &out, &info) != ESP_AUDIO_ERR_OK ||
        out.decoded_size != sizeof(pcm)) {
        ESP_LOGW(TAG, "note %s unplayable", p.id);
        play_end(false);
        return;
    }
    size_t trim = p.skip > FRAME_SAMPLES ? FRAME_SAMPLES : p.skip;
    p.skip -= trim;
    size_t n = FRAME_SAMPLES - trim;
    // -6 dB, as for calls: the sender's mic gain puts speech near full scale.
    pcm_apply_gain(pcm + trim, n, 1, &p.gain, pcm_gain_for_volume(volume) / 2);
    for (size_t done = 0; done < n;) {
        size_t wrote = alarm_sound_clip_write(pcm + trim + done, n - done, CLIP_WAIT_MS);
        if (!wrote) {
            // The clip was taken away (a firmware upload).
            play_end(false);
            return;
        }
        done += wrote;
    }
    lock();
    p.frames++;
    unlock();
}

static void play_step(play_state_t state, bool stop, uint8_t volume) {
    if (stop) {
        play_end(false);
    } else if (state == PLAY_DRAIN) {
        if (alarm_sound_clip_playing()) {
            vTaskDelay(pdMS_TO_TICKS(DRAIN_POLL_MS));
        } else {
            play_end(true);
        }
    } else if (!p.decoder && !play_open()) {
        play_end(false);
    } else {
        play_frame(volume);
    }
}

// ---- The worker's side ----

bool note_audio_busy(void) {
    lock();
    bool busy = r.state == REC_START || r.state == REC_ON || p.state == PLAY_ON || p.state == PLAY_DRAIN;
    unlock();
    return busy;
}

bool note_audio_recording(void) {
    lock();
    bool on = r.state == REC_START || r.state == REC_ON;
    unlock();
    return on;
}

void note_audio_step(uint8_t volume) {
    lock();
    rec_state_t rec = r.state;
    rec_action_t action = r.action;
    play_state_t play = p.state;
    bool stop = p.stop;
    unlock();
    if (rec == REC_START || rec == REC_ON) {
        rec_step(rec, action);
    } else if (play == PLAY_ON || play == PLAY_DRAIN) {
        play_step(play, stop, volume);
    }
}

// ---- Requests ----

void note_audio_record(const char *key) {
    if (!notes_valid_key(key)) {
        return;
    }
    lock();
    if (r.state == REC_DRAFT) {
        rec_clear();
    }
    if (r.state == REC_IDLE) {
        snprintf(r.key, sizeof(r.key), "%s", key);
        r.action = REC_KEEP_GOING;
        r.frames = 0;
        r.state = REC_START;
        // A note playing would share the clip with the duck.
        p.next[0] = '\0';
        if (p.state == PLAY_LOADING) {
            p.state = PLAY_IDLE;
        } else if (p.state != PLAY_IDLE) {
            p.stop = true;
        }
    }
    unlock();
}

static void request(rec_action_t action) {
    lock();
    if (r.state == REC_START || r.state == REC_ON) {
        r.action = action;
        unlock();
    } else if (r.state == REC_DRAFT && action == REC_SEND) {
        send_draft_locked_then_unlock();
    } else {
        if (r.state == REC_DRAFT && action == REC_DISCARD) {
            rec_clear();
        }
        unlock();
    }
}

void note_audio_send(void) {
    request(REC_SEND);
}

void note_audio_discard(void) {
    request(REC_DISCARD);
}

void note_audio_pause(void) {
    request(REC_PAUSE);
}

void note_audio_play_request(const char *id) {
    if (!notes_valid_id(id)) {
        return;
    }
    lock();
    bool load = false;
    if (r.state == REC_START || r.state == REC_ON) {
        // Not over a recording.
    } else if (p.state == PLAY_IDLE || p.state == PLAY_LOADING) {
        snprintf(p.id, sizeof(p.id), "%s", id);
        p.state = PLAY_LOADING;
        load = true;
    } else {
        snprintf(p.next, sizeof(p.next), "%s", id);
        p.stop = true;
    }
    unlock();
    if (load) {
        notes_play(id);
    }
}

void note_audio_play(const char *id, uint8_t *ogg, size_t len) {
    lock();
    bool wanted = p.state == PLAY_LOADING && !strcmp(p.id, id);
    if (wanted) {
        p.ogg = ogg;
        p.len = len;
        p.frames = 0;
        p.stop = false;
        p.state = PLAY_ON;
    }
    unlock();
    if (!wanted) {
        free(ogg);
    }
}

void note_audio_stop(void) {
    lock();
    p.next[0] = '\0';
    if (p.state == PLAY_LOADING) {
        p.state = PLAY_IDLE;
        p.id[0] = '\0';
    } else if (p.state != PLAY_IDLE) {
        p.stop = true;
    }
    unlock();
}

void note_audio_get(ui_talk_t *out) {
    lock();
    out->rec = r.state == REC_ON || r.state == REC_START ? UI_NOTE_REC_RECORDING
               : r.state == REC_DRAFT                    ? UI_NOTE_REC_DRAFT
                                                         : UI_NOTE_REC_IDLE;
    snprintf(out->rec_key, sizeof(out->rec_key), "%s", out->rec ? r.key : "");
    out->rec_tenths = (uint16_t)(r.frames / FRAMES_PER_TENTH);
    out->level = r.level;
    snprintf(out->playing, sizeof(out->playing), "%s", p.state != PLAY_IDLE ? p.id : "");
    out->play_tenths = (uint16_t)(p.frames / FRAMES_PER_TENTH);
    unlock();
}
