#include "stream_player.h"

#include <stdio.h>
#include <string.h>

#include "audio_out.h"
#include "decoder/impl/esp_aac_dec.h"
#include "decoder/impl/esp_mp3_dec.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "pcm.h"
#include "simple_dec/esp_audio_simple_dec.h"

// ~30 s at 128 kbps; what the network gets ahead by is capped by the
// server's own burst, so most of it only fills on a slow DAC clock.
#define RING_BYTES (512 * 1024)
// Buffered before playing (and after an underrun), at the stream's bitrate.
#define PREBUFFER_MS 2000
#define PREBUFFER_MIN (16 * 1024)
#define DEFAULT_KBPS 128

#define IN_CHUNK 2048
#define RS_OUT_FRAMES 4096
// audio_out_write() blocks until its frames are queued; short writes let a
// stop silence output within ~12 ms.
#define WRITE_FRAMES 512
#define DECODE_ERRORS_RESET 20
#define POLL_TICKS pdMS_TO_TICKS(20)

// Audio-path tasks keep internal stacks (TLS handshakes and the AAC decoder
// are the deep ones). Decoding sits where AirPlay 2's AAC decoder does,
// below its player; the two never run at once.
#define NET_STACK 8192
#define NET_PRIO 5
#define DEC_STACK 8192
#define DEC_PRIO 7
#define TASK_CORE 0

#define BACKOFF_MIN_MS 1000
#define BACKOFF_MAX_MS 30000
// A stream that played this long before dropping reconnects at once.
#define HEALTHY_US (20 * 1000 * 1000LL)

#define NO_EPOCH UINT32_MAX

typedef enum {
    NET_CONNECTING,
    NET_STREAMING,
    NET_RETRYING,
    NET_FAILED,
} net_phase_t;

static const char *TAG = "stream_player";

static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_decode_done;
static stream_status_t s_status;
static uint32_t s_version;
static net_phase_t s_net_phase;
static bool s_dec_playing;
static bool s_running; // the network task exists
static TaskHandle_t s_net_task;
static stream_ring_t *s_ring;
// Read without the lock by the tasks.
static volatile bool s_want;
static volatile uint32_t s_gen;
static volatile bool s_decode_quit;
// The ring epoch whose audio may reach audio_out: the current stream's,
// none while switching or stopped.
static volatile uint32_t s_out_epoch = NO_EPOCH;
static volatile int32_t s_gain_target;

// Caller holds s_lock.
static void changed_locked(void) {
    stream_state_t state = STREAM_IDLE;
    if (s_want) {
        static const stream_state_t k_net[] = {[NET_CONNECTING] = STREAM_CONNECTING,
                                               [NET_STREAMING] = STREAM_BUFFERING,
                                               [NET_RETRYING] = STREAM_RETRYING,
                                               [NET_FAILED] = STREAM_FAILED};
        // Buffered audio keeps playing through a reconnect.
        state = s_dec_playing ? STREAM_PLAYING : k_net[s_net_phase];
    }
    s_status.state = state;
    s_version++;
}

static void set_net_phase(net_phase_t phase, const char *error) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_net_phase = phase;
    snprintf(s_status.error, sizeof(s_status.error), "%s", error ? error : "");
    changed_locked();
    xSemaphoreGive(s_lock);
}

static void set_dec_playing(bool playing) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_dec_playing != playing) {
        s_dec_playing = playing;
        changed_locked();
    }
    xSemaphoreGive(s_lock);
}

// -- decoding --

typedef struct {
    stream_ring_t *ring;
    esp_audio_simple_dec_handle_t dec;
    stream_format_t format;
    uint32_t epoch;
    int errors;
    uint8_t in[IN_CHUNK];
    uint8_t *out; // decoder output, then the same frames as stereo
    size_t out_size;
    int16_t rs[RS_OUT_FRAMES * 2];
    pcm_resampler_t resampler;
    int32_t gain;
} decode_t;

static void decoder_close(decode_t *d) {
    if (d->dec) {
        esp_audio_simple_dec_close(d->dec);
        d->dec = NULL;
    }
}

static bool decoder_open(decode_t *d) {
    esp_aac_dec_cfg_t aac = ESP_AAC_DEC_CONFIG_DEFAULT();
    // Most AAC stations are HE-AAC (AAC+).
    aac.aac_plus_enable = true;
    bool is_aac = d->format == STREAM_FORMAT_AAC;
    esp_audio_simple_dec_cfg_t cfg = {
        .dec_type = is_aac ? ESP_AUDIO_SIMPLE_DEC_TYPE_AAC : ESP_AUDIO_SIMPLE_DEC_TYPE_MP3,
        .dec_cfg = is_aac ? &aac : NULL,
        .cfg_size = is_aac ? sizeof(aac) : 0,
    };
    esp_audio_err_t err = esp_audio_simple_dec_open(&cfg, &d->dec);
    if (err != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "decoder open failed: %d", err);
        d->dec = NULL;
        return false;
    }
    d->errors = 0;
    return true;
}

// Room for `need` decoder bytes, plus as many again for mono to stereo.
static bool out_reserve(decode_t *d, size_t need) {
    if (need * 2 <= d->out_size) {
        return true;
    }
    uint8_t *buf = heap_caps_realloc(d->out, need * 2, MALLOC_CAP_SPIRAM);
    if (!buf) {
        return false;
    }
    d->out = buf;
    d->out_size = need * 2;
    return true;
}

static void update_format(uint32_t rate, uint8_t channels) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_status.sample_rate != rate || s_status.channels != channels) {
        s_status.sample_rate = rate;
        s_status.channels = channels;
        changed_locked();
    }
    xSemaphoreGive(s_lock);
}

static void play_pcm(decode_t *d, size_t bytes) {
    esp_audio_simple_dec_info_t info = {0};
    esp_audio_simple_dec_get_info(d->dec, &info);
    if (info.bits_per_sample != 16 || info.channel == 0 || info.sample_rate == 0) {
        return;
    }
    update_format(info.sample_rate, info.channel);
    if (d->resampler.in_rate != info.sample_rate) {
        pcm_resampler_init(&d->resampler, info.sample_rate, AUDIO_OUT_SAMPLE_RATE);
    }

    int16_t *pcm = (int16_t *)d->out;
    size_t frames = bytes / (info.channel * sizeof(int16_t));
    pcm_to_stereo(pcm, frames, info.channel);

    // Pieces small enough that their output fits d->rs.
    size_t piece = (size_t)((uint64_t)(RS_OUT_FRAMES - 2) * info.sample_rate / AUDIO_OUT_SAMPLE_RATE) - 1;
    for (size_t at = 0; at < frames; at += piece) {
        size_t n = frames - at < piece ? frames - at : piece;
        size_t out = pcm_resample(&d->resampler, pcm + at * 2, n, d->rs);
        pcm_apply_gain(d->rs, out, &d->gain, s_gain_target);
        for (size_t w = 0; w < out; w += WRITE_FRAMES) {
            if (s_out_epoch != d->epoch) {
                return;
            }
            audio_out_write(d->rs + w * 2, out - w < WRITE_FRAMES ? out - w : WRITE_FRAMES);
        }
    }
    if (s_out_epoch == d->epoch) {
        set_dec_playing(true);
    }
}

static void decode_chunk(decode_t *d, size_t len) {
    if (!d->dec && !decoder_open(d)) {
        return;
    }
    esp_audio_simple_dec_raw_t raw = {.buffer = d->in, .len = (uint32_t)len};
    while (raw.len && !s_decode_quit) {
        esp_audio_simple_dec_out_t frame = {.buffer = d->out, .len = (uint32_t)(d->out_size / 2)};
        esp_audio_err_t err = esp_audio_simple_dec_process(d->dec, &raw, &frame);
        if (err == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            if (!out_reserve(d, frame.needed_size)) {
                return;
            }
            continue;
        }
        if (err != ESP_AUDIO_ERR_OK) {
            // A reconnect can join mid-frame; the parser finds the next sync.
            if (++d->errors >= DECODE_ERRORS_RESET) {
                ESP_LOGW(TAG, "decode errors (%d), reopening decoder", err);
                decoder_close(d);
            }
            return;
        }
        d->errors = 0;
        if (frame.decoded_size) {
            play_pcm(d, frame.decoded_size);
        }
        if (!raw.consumed && !frame.decoded_size) {
            break;
        }
        raw.len -= raw.consumed;
        raw.buffer += raw.consumed;
    }
}

static size_t prebuffer_bytes(const decode_t *d) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int kbps = s_status.bitrate_kbps ? s_status.bitrate_kbps : DEFAULT_KBPS;
    xSemaphoreGive(s_lock);
    size_t bytes = (size_t)kbps * 1000 / 8 * PREBUFFER_MS / 1000;
    size_t max = stream_ring_size(d->ring) / 2;
    return bytes < PREBUFFER_MIN ? PREBUFFER_MIN : bytes > max ? max : bytes;
}

static void decode_task(void *arg) {
    decode_t *d = heap_caps_calloc(1, sizeof(*d), MALLOC_CAP_SPIRAM);
    if (d && !out_reserve(d, 4096 * 2 * sizeof(int16_t))) {
        heap_caps_free(d);
        d = NULL;
    }
    if (!d) {
        ESP_LOGE(TAG, "no memory for the decoder");
        while (!s_decode_quit) {
            vTaskDelay(POLL_TICKS);
        }
    } else {
        d->ring = arg;
        d->epoch = NO_EPOCH;
    }
    bool waiting = true;
    while (d && !s_decode_quit) {
        if (waiting) {
            if (stream_ring_level(d->ring) < prebuffer_bytes(d)) {
                vTaskDelay(POLL_TICKS);
                continue;
            }
            waiting = false;
        }
        uint32_t epoch, format;
        size_t n = stream_ring_read(d->ring, d->in, sizeof(d->in), POLL_TICKS, &epoch, &format);
        if (epoch != d->epoch) {
            // A new stream: its first bytes may have come with this read, but
            // it gets its prebuffer like any other. Losing a chunk at the
            // start is harmless; the parser syncs on the next frame.
            decoder_close(d);
            d->epoch = epoch;
            d->format = (stream_format_t)format;
            d->gain = 0; // fades in
            pcm_resampler_init(&d->resampler, AUDIO_OUT_SAMPLE_RATE, AUDIO_OUT_SAMPLE_RATE);
            waiting = true;
            set_dec_playing(false);
            continue;
        }
        if (n == 0) {
            if (s_out_epoch == d->epoch) {
                ESP_LOGW(TAG, "buffer ran dry, rebuffering");
            }
            waiting = true;
            set_dec_playing(false);
            continue;
        }
        if (d->format == STREAM_FORMAT_MP3 || d->format == STREAM_FORMAT_AAC) {
            decode_chunk(d, n);
        }
    }
    if (d) {
        decoder_close(d);
        heap_caps_free(d->out);
        heap_caps_free(d);
    }
    set_dec_playing(false);
    xSemaphoreGive(s_decode_done);
    vTaskDeleteWithCaps(NULL);
}

// -- network --

typedef struct {
    stream_ring_t *ring;
    uint32_t gen;
    stream_format_t epoch_format;
    int64_t audio_since;
} net_t;

static bool should_stop(void *ctx) {
    const net_t *net = ctx;
    return !s_want || s_gen != net->gen;
}

static void on_audio(void *ctx, const stream_info_t *info) {
    net_t *net = ctx;
    net->audio_since = esp_timer_get_time();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_net_phase = NET_STREAMING;
    s_status.error[0] = '\0';
    s_status.format = info->format;
    s_status.bitrate_kbps = (uint16_t)info->bitrate_kbps;
    snprintf(s_status.name, sizeof(s_status.name), "%s", info->name);
    changed_locked();
    xSemaphoreGive(s_lock);
    // A reconnect to the same format carries on in the same epoch, so what's
    // buffered keeps playing.
    if (info->format != net->epoch_format) {
        net->epoch_format = info->format;
        uint32_t epoch = stream_ring_reset(net->ring, info->format);
        if (!should_stop(net)) {
            s_out_epoch = epoch;
        }
    }
}

static void on_title(void *ctx, const char *title) {
    (void)ctx;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (strcmp(s_status.title, title)) {
        snprintf(s_status.title, sizeof(s_status.title), "%s", title);
        ESP_LOGI(TAG, "title: %s", title);
        changed_locked();
    }
    xSemaphoreGive(s_lock);
}

// Waits for `ms`, or until play/stop changes what's wanted.
static void wait_for_change(const net_t *net, uint32_t ms) {
    int64_t until = esp_timer_get_time() + (int64_t)ms * 1000;
    while (!should_stop(net)) {
        int64_t left = until - esp_timer_get_time();
        if (left <= 0) {
            return;
        }
        int64_t ms = left / 1000 + 1;
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ms > 1000 ? 1000 : ms));
    }
}

static void stream_session(stream_ring_t *ring) {
    net_t net = {.ring = ring, .gen = s_gen - 1};
    const stream_net_ops_t ops = {.ctx = &net, .should_stop = should_stop, .on_audio = on_audio, .on_title = on_title};
    char url[STREAM_URL_MAX], error[64];
    uint32_t backoff = BACKOFF_MIN_MS;

    for (;;) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool want = s_want;
        uint32_t gen = s_gen;
        snprintf(url, sizeof(url), "%s", s_status.url);
        xSemaphoreGive(s_lock);
        if (!want) {
            return;
        }
        if (gen != net.gen) {
            // Whatever is buffered belongs to the last station.
            net.gen = gen;
            net.epoch_format = STREAM_FORMAT_UNKNOWN;
            stream_ring_reset(ring, STREAM_FORMAT_UNKNOWN);
            backoff = BACKOFF_MIN_MS;
        }

        stream_net_result_t result = stream_net_run(url, ring, &ops, error, sizeof(error));
        if (should_stop(&net)) {
            continue;
        }
        if (result == STREAM_NET_FAILED) {
            ESP_LOGW(TAG, "%s: %s", url, error);
            set_net_phase(NET_FAILED, error);
            wait_for_change(&net, UINT32_MAX);
            continue;
        }
        if (net.audio_since && esp_timer_get_time() - net.audio_since > HEALTHY_US) {
            backoff = BACKOFF_MIN_MS;
        }
        net.audio_since = 0;
        ESP_LOGW(TAG, "%s: %s, retrying in %lu ms", url, error, (unsigned long)backoff);
        set_net_phase(NET_RETRYING, error);
        wait_for_change(&net, backoff);
        backoff = backoff * 2 > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : backoff * 2;
        if (!should_stop(&net)) {
            set_net_phase(NET_CONNECTING, NULL);
        }
    }
}

static void net_task(void *arg) {
    (void)arg;
    for (;;) {
        stream_ring_t *ring = stream_ring_create(RING_BYTES);
        TaskHandle_t dec = NULL;
        s_decode_quit = false;
        if (ring && xTaskCreatePinnedToCoreWithCaps(decode_task, "radio_dec", DEC_STACK, ring, DEC_PRIO, &dec,
                                                    TASK_CORE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) == pdPASS) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_ring = ring;
            xSemaphoreGive(s_lock);
            stream_session(ring);
            s_decode_quit = true;
            stream_ring_wake(ring);
            xSemaphoreTake(s_decode_done, portMAX_DELAY);
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_ring = NULL;
            xSemaphoreGive(s_lock);
        } else {
            ESP_LOGE(TAG, "no memory to start the stream");
            set_net_phase(NET_FAILED, "Out of memory");
            net_t idle = {.gen = s_gen};
            wait_for_change(&idle, UINT32_MAX);
        }
        stream_ring_destroy(ring);

        // A play() that came in while this wound down still finds it running.
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (!s_want) {
            s_running = false;
            s_net_task = NULL;
            xSemaphoreGive(s_lock);
            break;
        }
        xSemaphoreGive(s_lock);
    }
    vTaskDeleteWithCaps(NULL);
}

// -- API --

void stream_player_init(void) {
    s_lock = xSemaphoreCreateMutex();
    s_decode_done = xSemaphoreCreateBinary();
    ESP_ERROR_CHECK(s_lock && s_decode_done ? ESP_OK : ESP_ERR_NO_MEM);
    esp_mp3_dec_register();
    esp_aac_dec_register();
    stream_player_set_volume(50);
}

static void kick_locked(void) {
    if (s_net_task) {
        xTaskNotifyGive(s_net_task);
    }
    if (s_ring) {
        stream_ring_wake(s_ring);
    }
}

void stream_player_play(const char *url) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_out_epoch = NO_EPOCH;
    s_want = true;
    s_gen++;
    snprintf(s_status.url, sizeof(s_status.url), "%s", url);
    s_status.title[0] = s_status.name[0] = s_status.error[0] = '\0';
    s_status.format = STREAM_FORMAT_UNKNOWN;
    s_status.sample_rate = 0;
    s_status.channels = 0;
    s_status.bitrate_kbps = 0;
    s_net_phase = NET_CONNECTING;
    s_dec_playing = false;
    changed_locked();
    if (!s_running) {
        if (xTaskCreatePinnedToCoreWithCaps(net_task, "radio_net", NET_STACK, NULL, NET_PRIO, &s_net_task, TASK_CORE,
                                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) == pdPASS) {
            s_running = true;
        } else {
            s_net_phase = NET_FAILED;
            snprintf(s_status.error, sizeof(s_status.error), "Out of memory");
            changed_locked();
        }
    } else {
        kick_locked();
    }
    xSemaphoreGive(s_lock);
}

void stream_player_stop(void) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_out_epoch = NO_EPOCH;
    if (s_want) {
        s_want = false;
        s_gen++;
        s_dec_playing = false;
        s_status.title[0] = '\0';
        changed_locked();
        kick_locked();
    }
    xSemaphoreGive(s_lock);
}

void stream_player_set_volume(uint8_t volume_pct) {
    s_gain_target = pcm_gain_for_volume(volume_pct);
}

uint32_t stream_player_status(stream_status_t *out) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_status;
    uint32_t version = s_version;
    xSemaphoreGive(s_lock);
    return version;
}
