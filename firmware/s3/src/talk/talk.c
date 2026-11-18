#include "talk/talk.h"
#include "talk/wire.h"
#include "talk/rtsp.h"

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "audio/alarm_sound.h"
#include "audio/out.h"
#include "talk/playout.h"
#include "talk/note/audio.h"
#include "talk/note/notes.h"
#include "esp_mac.h"
#include "audio/source.h"
#include "core/device_name.h"
#include "core/task_stats.h"
#include "decoder/impl/esp_opus_dec.h"
#include "encoder/impl/esp_opus_enc.h"
#include "esp_expression_with_stack.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "mdns.h"
#include "audio/mic/mic.h"
#include "nvs.h"
#include "audio/pcm.h"
#include "net/wifi/wifi.h"
#include "airplay/shared/http_util.h"

// mDNS TXT v: 3 is two-way Opus calls and voice notes, with the knob's key
// in TXT id; knobs on earlier versions aren't listed.
#define TALK_VERSION "3"
#define CALL_LOST_US 3000000
#define DIAL_TIMEOUT_US ((TALK_RING_MS + 5000) * 1000LL)
// Playout delay in frames: from 60 ms, growing by however late a packet
// came (WiFi holds frames back in bursts), to at most 200 ms; after quiet on
// that front it shrinks a frame at a time, dropping a silent frame.
#define DELAY_MIN 3
#define DELAY_MAX 10
#define SHRINK_AFTER_US 10000000
#define QUIET_LEVEL 25 // level() of a frame under -45 dBFS
#define PREFILL_US (DELAY_MIN * 20000)
#define VOLUME_SAVE_US 2000000
#define IDLE_POLL_MS 20
// Opus encoding used ~24 KB of it. Internal RAM the LCD's SPI DMA and the
// mic need must stay free: a 30 KB stack once left the panel unable to draw.
#define CALL_STACK 27648
#define CALL_HEAP_RESERVE 32768
#define STACK_FILL 0xa5 // what esp_execute_shared_stack_function() fills with
static const char *TAG = "talk";
static SemaphoreHandle_t s_stack_lock;
static uint8_t *s_call_stack;
static size_t s_call_stack_used;
// Worker-only.
static int s_sock = -1;
static struct session *s_session;
static SemaphoreHandle_t s_lock, s_write_lock;
static ui_talk_t s_ui = {.volume = 70};
static char s_key[UI_TALK_KEY];
static bool s_abort, s_network, s_renamed;
static esp_timer_handle_t s_save_timer;

// Requests from the UI, consumed by the worker.
static uint32_t s_dial;
static bool s_accept, s_hang_up;

// An incoming call, set up by the RTSP connection task. s_ring_call is its
// id while the call rings or runs, 0 otherwise.
static struct {
    uint32_t ip;
    uint16_t port;
    char key[UI_TALK_KEY], name[33];
} s_ring;
static uint32_t s_ring_call, s_ring_ended, s_last_call;
static talk_answer_t s_ring_answer;
static uint16_t s_answer_sequence;
static uint32_t s_answer_timestamp;

typedef struct session {
    uint32_t call; // the incoming call's id, 0 for one we dialled
    char key[UI_TALK_KEY], name[33]; // the other knob, for the call log
    int outcome; // a ui_call_outcome_t for a call that never connected, -1 unknown
    struct sockaddr_in peer;
    uint32_t ssrc, peer_ssrc;
    uint16_t sequence;
    uint32_t timestamp;
    int control;
    bool in_call, claimed, mic, playout, ringing;
    int64_t started, answered, last_received, play_at;
    void *encoder, *decoder;
    task_stats_t *stats; // CPU per task over the call
    int32_t gain;
    talk_jitter_t jitter;
    int16_t pcm[TALK_FRAME_SAMPLES];
    int16_t far[TALK_FRAME_SAMPLES];
    // For the end-of-call log.
    uint32_t frames, rx, rx_foreign, rx_bad, rx_rejected, concealed;
    // Rejected for arriving after their slot played, by how many frames
    // late (1-3, 4-10, more); the longest silence between arrivals.
    uint32_t late[3];
    int64_t gap_max_us;
    struct sockaddr_in foreign;
    int64_t busy_us, busy_max_us;
    // Playout delay (frames), frames still to insert to reach it, and when
    // a packet last came too late for it.
    int delay, hold, delay_max;
    int64_t last_late;
} session_t;

static void log_internal(const char *step) {
    ESP_LOGI(TAG, "internal RAM %s: %u free, largest %u", step,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}
static int64_t now_us(void) {
    return esp_timer_get_time();
}
static void set_state(ui_talk_state_t state, const char *peer, const char *message) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_ui.state = state;
    s_ui.seconds = 0;
    snprintf(s_ui.peer, sizeof(s_ui.peer), "%s", peer ? peer : "");
    snprintf(s_ui.message, sizeof(s_ui.message), "%s", message ? message : "");
    xSemaphoreGive(s_lock);
}
void talk_get_state(ui_talk_t *out) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_ui;
    xSemaphoreGive(s_lock);
    note_audio_get(out);
    out->notes_version = notes_version();
}
void talk_call(uint32_t peer) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!peer)
        s_hang_up = true;
    else if (s_ui.state == UI_TALK_IDLE)
        s_dial = peer;
    xSemaphoreGive(s_lock);
}
void talk_answer(bool accept) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_ui.state == UI_TALK_RINGING) {
        s_accept = accept;
        s_hang_up = !accept;
    }
    xSemaphoreGive(s_lock);
}
void talk_set_volume(uint8_t volume) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_ui.volume = volume > 100 ? 100 : volume;
    xSemaphoreGive(s_lock);
    esp_timer_stop(s_save_timer);
    esp_timer_start_once(s_save_timer, VOLUME_SAVE_US);
}
void talk_preempt(void) {
    if (!s_lock)
        return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_abort = true;
    s_dial = 0;
    xSemaphoreGive(s_lock);
    // The worker checks cancellation under this same lock before each write.
    xSemaphoreTake(s_write_lock, portMAX_DELAY);
    xSemaphoreGive(s_write_lock);
}
void talk_rename(void) {
    if (!s_lock)
        return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_renamed = true;
    xSemaphoreGive(s_lock);
}
static bool cancelled(void) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool abort = s_abort;
    xSemaphoreGive(s_lock);
    return abort || audio_source_suspended() || audio_source_active() != AUDIO_SOURCE_TALK;
}

// The RTSP connection task's side of an incoming call.
// The caller's first sequence goes unused: the jitter buffer starts at the
// first packet that arrives.
static uint32_t ring(uint32_t ip, uint16_t port, uint16_t sequence, const char *key, const char *name) {
    (void)sequence;
    // A note being recorded turns calls away; one playing gives way.
    if (note_audio_recording())
        return 0;
    note_audio_stop();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t call = 0;
    if (s_ui.state == UI_TALK_IDLE && !s_dial && !s_ring_call && !audio_source_suspended()) {
        if (!++s_last_call)
            s_last_call = 1;
        call = s_ring_call = s_last_call;
        s_ring.ip = ip;
        s_ring.port = port;
        snprintf(s_ring.name, sizeof(s_ring.name), "%s", name);
        snprintf(s_ring.key, sizeof(s_ring.key), "%s", key);
        s_ring_answer = TALK_ANSWER_PENDING;
        s_ring_ended = 0;
        s_accept = s_hang_up = false;
        // Set here, not by the worker, so a dial can't slip in meanwhile.
        s_ui.state = UI_TALK_RINGING;
        s_ui.peer_id = ip;
            s_ui.seconds = 0;
        snprintf(s_ui.peer, sizeof(s_ui.peer), "%s", name);
        s_ui.message[0] = '\0';
    }
    xSemaphoreGive(s_lock);
    return call;
}
static talk_answer_t answer(uint32_t call, uint16_t *sequence, uint32_t *timestamp) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    talk_answer_t a = call == s_ring_call ? s_ring_answer : TALK_ANSWER_DECLINED;
    *sequence = s_answer_sequence;
    *timestamp = s_answer_timestamp;
    xSemaphoreGive(s_lock);
    return a;
}
static void ring_stop(uint32_t call) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (call == s_ring_call)
        s_ring_ended = call;
    xSemaphoreGive(s_lock);
}
static bool ring_active(uint32_t call) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool active = call == s_ring_call && s_ring_answer == TALK_ANSWER_ACCEPTED;
    xSemaphoreGive(s_lock);
    return active;
}

static void log_call(const session_t *s) {
    if (s->in_call)
        notes_log_call(s->key, s->name, s->call != 0, UI_CALL_ANSWERED, (uint32_t)((now_us() - s->answered) / 1000000));
    else if (s->outcome >= 0)
        notes_log_call(s->key, s->name, s->call != 0, (ui_call_outcome_t)s->outcome, 0);
}
static void finish(session_t **session, const char *message) {
    session_t *s = *session;
    if (!s)
        return;
    log_call(s);
    if (s->in_call) {
        log_internal("at the end of the call");
        task_stats_end(s->stats, "call");
    }
    if (s->ringing)
        alarm_sound_stop();
    talk_rtsp_end(s->control);
    // Before the mic: it feeds the echo canceller.
    if (s->playout)
        talk_playout_stop();
    if (s->mic)
        mic_stop();
    if (s->encoder)
        esp_opus_enc_close(s->encoder);
    if (s->decoder)
        esp_opus_dec_close(s->decoder);
    if (s->claimed)
        audio_source_release(AUDIO_SOURCE_TALK);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s->call && s->call == s_ring_call) {
        if (s_ring_answer == TALK_ANSWER_PENDING)
            s_ring_answer = TALK_ANSWER_DECLINED;
        s_ring_call = 0;
    }
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "call over: %s", message ? message : "hung up");
    if (s->frames) {
        ESP_LOGI(TAG, "%lu frames, %lld us busy each (max %lld); rx %lu ok, %lu from %s:%u (want :%u), %lu bad, "
                      "%lu rejected; %lu concealed",
                 (unsigned long)s->frames, s->busy_us / s->frames, s->busy_max_us, (unsigned long)s->rx,
                 (unsigned long)s->rx_foreign, inet_ntoa(s->foreign.sin_addr), ntohs(s->foreign.sin_port),
                 ntohs(s->peer.sin_port), (unsigned long)s->rx_bad, (unsigned long)s->rx_rejected,
                 (unsigned long)s->concealed);
        ESP_LOGI(TAG, "late %lu/%lu/%lu (1-3/4-10/more frames), longest gap %lld ms; delay %d ms, at most %d",
                 (unsigned long)s->late[0], (unsigned long)s->late[1], (unsigned long)s->late[2],
                 s->gap_max_us / 1000, s->delay * 20, s->delay_max * 20);
    }
    free(s);
    *session = NULL;
    set_state(UI_TALK_IDLE, NULL, message);
}
static session_t *create_session(uint32_t ip, uint16_t port, uint32_t call) {
    session_t *s = heap_caps_calloc(1, sizeof(*s), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s)
        return NULL;
    s->control = -1;
    s->outcome = -1;
    s->peer = (struct sockaddr_in){.sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = ip};
    s->call = call;
    s->started = now_us();
    s->ssrc = esp_random();
    s->sequence = (uint16_t)esp_random();
    s->timestamp = esp_random();
    return s;
}
// -60..0 dBFS as 0-100.
static uint8_t level(const int16_t *pcm, size_t n) {
    double sum = 0;
    for (size_t i = 0; i < n; i++)
        sum += (double)pcm[i] * pcm[i];
    float db = 20 * log10f((float)sqrt(sum / n) / 32768.f + 0.000001f);
    int pct = (int)((db + 60) * (100.f / 60));
    return pct < 0 ? 0 : pct > 100 ? 100 : pct;
}
static bool open_codecs(session_t *s) {
    esp_opus_enc_config_t enc = ESP_OPUS_ENC_CONFIG_DEFAULT();
    enc.sample_rate = 16000;
    enc.channel = 1;
    enc.bitrate = 24000;
    enc.complexity = 0; // ~4 ms a frame on an internal stack; 2 took ~7
    enc.enable_vbr = true;
    int input, output;
    if (esp_opus_enc_open(&enc, sizeof(enc), &s->encoder) != ESP_AUDIO_ERR_OK ||
        esp_opus_enc_get_frame_size(s->encoder, &input, &output) != ESP_AUDIO_ERR_OK ||
        input != TALK_FRAME_SAMPLES * 2 || output > TALK_PAYLOAD)
        return false;
    esp_opus_dec_cfg_t dec = {.sample_rate = 16000,
                              .channel = 1,
                              .frame_duration = ESP_OPUS_DEC_FRAME_DURATION_20_MS,
                              .self_delimited = false};
    return esp_opus_dec_open(&dec, sizeof(dec), &s->decoder) == ESP_AUDIO_ERR_OK;
}
// The playout task's writer; talk_preempt() takes the same lock, so once it
// returns nothing more reaches the DAC.
static void write_audio(const int16_t *pcm, size_t frames) {
    xSemaphoreTake(s_write_lock, portMAX_DELAY);
    if (!cancelled()) {
        audio_out_set_rate(AUDIO_OUT_SAMPLE_RATE);
        audio_out_write16(pcm, frames);
    }
    xSemaphoreGive(s_write_lock);
}

// Both sides answered: take the DAC and open the mic. NULL, or why not.
static const char *start_call(session_t *s) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_abort = false;
    xSemaphoreGive(s_lock);
    s->claimed = audio_source_claim(AUDIO_SOURCE_TALK);
    if (!s->claimed || cancelled())
        return "Speaker busy";
    log_internal("with the DAC claimed");
    if (!open_codecs(s))
        return "Call failed";
    log_internal("with Opus open");
    static const mic_config_t mic = {
        .gain_db = MIC_DEFAULT_GAIN_DB, .noise_level = MIC_DEFAULT_NOISE_LEVEL, .echo_cancel = true};
    for (int retry = 0; retry < 10 && !s->mic; retry++) {
        s->mic = mic_start(&mic);
        if (!s->mic)
            vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (!s->mic)
        return "Microphone unavailable";
    log_internal("in the call");
    s->playout = talk_playout_start(write_audio);
    if (!s->playout)
        return "Call failed";
    s->in_call = true;
    s->stats = task_stats_begin();
    s->answered = s->last_received = now_us();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_ui.state = UI_TALK_IN_CALL;
    s_ui.message[0] = '\0';
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "in call with %s", inet_ntoa(s->peer.sin_addr));
    return NULL;
}

// Our side of an incoming call, once the connection task has rung.
static session_t *answer_ringing(void) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t call = s_ring_call, ip = s_ring.ip;
    uint16_t port = s_ring.port;
    char key[UI_TALK_KEY], name[33];
    snprintf(key, sizeof(key), "%s", s_ring.key);
    snprintf(name, sizeof(name), "%s", s_ring.name);
    xSemaphoreGive(s_lock);
    session_t *s = create_session(ip, port, call);
    if (!s) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_ring_answer = TALK_ANSWER_DECLINED;
        s_ring_call = 0;
        xSemaphoreGive(s_lock);
        set_state(UI_TALK_IDLE, NULL, "Missed call");
        return NULL;
    }
    snprintf(s->key, sizeof(s->key), "%s", key);
    snprintf(s->name, sizeof(s->name), "%s", name);
    // Over whatever plays, which ducks under it; the DAC is only taken on
    // answering.
    alarm_sound_start(ALARM_SOUND_CALL);
    s->ringing = true;
    return s;
}
static void accept_ringing(session_t **session) {
    session_t *s = *session;
    alarm_sound_stop();
    s->ringing = false;
    const char *failure = start_call(s);
    if (failure) {
        finish(session, failure);
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_answer_sequence = s->sequence;
    s_answer_timestamp = s->timestamp;
    s_ring_answer = TALK_ANSWER_ACCEPTED;
    xSemaphoreGive(s_lock);
}
static session_t *dial(uint32_t ip) {
    char name[33] = "", key[UI_TALK_KEY] = "";
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_ui.count; i++)
        if (s_ui.peers[i].id == ip) {
            snprintf(name, sizeof(name), "%s", s_ui.peers[i].name);
            snprintf(key, sizeof(key), "%s", s_ui.peers[i].key);
        }
    snprintf(s_ui.peer, sizeof(s_ui.peer), "%s", name);
    s_ui.peer_id = ip;
    xSemaphoreGive(s_lock);
    session_t *s = name[0] ? create_session(ip, 0, 0) : NULL;
    uint16_t port = 0;
    if (s) {
        snprintf(s->key, sizeof(s->key), "%s", key);
        snprintf(s->name, sizeof(s->name), "%s", name);
        s->control = talk_rtsp_dial(ip, s->sequence, s->timestamp, &port);
    }
    if (!s || s->control < 0) {
        if (s)
            finish(&s, "Knob unavailable");
        else
            set_state(UI_TALK_IDLE, NULL, "Knob unavailable");
        return NULL;
    }
    s->peer.sin_port = htons(port);
    return s;
}
static void dialing(session_t **session) {
    session_t *s = *session;
    static const char *const k_outcome[] = {
        [TALK_DIAL_BUSY] = "Busy",
        [TALK_DIAL_DECLINED] = "Declined",
        [TALK_DIAL_NO_ANSWER] = "No answer",
        [TALK_DIAL_FAILED] = "Call failed",
    };
    static const int k_logged[] = {
        [TALK_DIAL_BUSY] = UI_CALL_BUSY,
        [TALK_DIAL_DECLINED] = UI_CALL_DECLINED,
        [TALK_DIAL_NO_ANSWER] = UI_CALL_NO_ANSWER,
        [TALK_DIAL_FAILED] = -1,
    };
    uint16_t first;
    talk_dial_t outcome = talk_rtsp_answer(s->control, &first);
    if (outcome == TALK_DIAL_ANSWERED) {
        const char *failure = start_call(s);
        if (failure)
            finish(session, failure);
    } else if (outcome != TALK_DIAL_RINGING) {
        s->outcome = k_logged[outcome];
        finish(session, k_outcome[outcome]);
    } else if (now_us() - s->started > DIAL_TIMEOUT_US) {
        s->outcome = UI_CALL_NO_ANSWER;
        finish(session, "No answer");
    }
}

// Queued datagrams into the call's jitter buffer; with no call, discarded.
static void receive(int sock, session_t *s) {
    // RTP reordering is bounded; control messages never enter this socket.
    for (int i = 0; i < 16; i++) {
        uint8_t wire[TALK_RTP_HEADER + TALK_PAYLOAD + 64];
        struct sockaddr_in from;
        socklen_t fromlen = sizeof(from);
        int n = recvfrom(sock, wire, sizeof(wire), 0, (struct sockaddr *)&from, &fromlen);
        if (n < 0)
            return;
        if (!s || !s->in_call)
            continue;
        if (from.sin_addr.s_addr != s->peer.sin_addr.s_addr || from.sin_port != s->peer.sin_port) {
            s->rx_foreign++;
            s->foreign = from;
            continue;
        }
        talk_packet_t p;
        if (!talk_rtp_decode(&p, wire, n)) {
            s->rx_bad++;
            continue;
        }
        // Starts at the first packet that arrives: any sent before this side
        // took the call were dropped, and waiting for them would add delay.
        if (!s->play_at) {
            s->peer_ssrc = p.ssrc;
            talk_jitter_init(&s->jitter, p.sequence);
        }
        int16_t ahead = (int16_t)(p.sequence - s->jitter.next);
        if (p.ssrc != s->peer_ssrc || !talk_jitter_push(&s->jitter, &p)) {
            s->rx_rejected++;
            if (ahead < 0 && p.ssrc == s->peer_ssrc) {
                s->late[-ahead <= 3 ? 0 : -ahead <= 10 ? 1 : 2]++;
                s->last_late = now_us();
                int grow = -ahead < DELAY_MAX - s->delay ? -ahead : DELAY_MAX - s->delay;
                s->delay += grow;
                s->hold += grow;
                s->delay_max = s->delay > s->delay_max ? s->delay : s->delay_max;
            }
            continue;
        }
        s->rx++;
        int64_t t = now_us();
        if (s->play_at && t - s->last_received > s->gap_max_us)
            s->gap_max_us = t - s->last_received;
        s->last_received = t;
        if (!s->play_at) {
            s->play_at = s->last_received + PREFILL_US;
            s->delay = s->delay_max = DELAY_MIN;
            s->last_late = s->last_received;
        }
    }
}
static bool send_frame(int sock, session_t *s) {
    talk_packet_t p = {.ssrc = s->ssrc, .sequence = s->sequence++, .timestamp = s->timestamp};
    s->timestamp += TALK_RTP_TICKS;
    esp_audio_enc_in_frame_t in = {.buffer = (uint8_t *)s->pcm, .len = TALK_FRAME_SAMPLES * 2};
    esp_audio_enc_out_frame_t out = {.buffer = p.payload, .len = sizeof(p.payload)};
    if (esp_opus_enc_process(s->encoder, &in, &out) != ESP_AUDIO_ERR_OK || !out.encoded_bytes)
        return false;
    p.length = out.encoded_bytes;
    uint8_t wire[TALK_RTP_HEADER + TALK_PAYLOAD];
    size_t n = talk_rtp_encode(wire, &p);
    // A datagram WiFi drops is the far end's loss to conceal, not a reason
    // to hang up.
    if (n)
        sendto(sock, wire, n, 0, (struct sockaddr *)&s->peer, sizeof(s->peer));
    return n > 0;
}
// A packet into s->far; with none (n 0), Opus's concealment.
static bool decode(session_t *s, uint8_t *raw, size_t n) {
    esp_audio_dec_in_raw_t in = {
        .buffer = raw, .len = n, .frame_recover = n ? ESP_AUDIO_DEC_RECOVERY_NONE : ESP_AUDIO_DEC_RECOVERY_PLC};
    esp_audio_dec_out_frame_t out = {.buffer = (uint8_t *)s->far, .len = sizeof(s->far)};
    esp_audio_dec_info_t info;
    return esp_opus_dec_decode(s->decoder, &in, &out, &info) == ESP_AUDIO_ERR_OK &&
           out.decoded_size == sizeof(s->far);
}
// Back to a shorter delay: a quiet frame just played gives way to the next.
static void shrink_delay(session_t *s, uint8_t *raw) {
    if (s->delay <= DELAY_MIN || s->hold || now_us() - s->last_late < SHRINK_AFTER_US ||
        level(s->far, TALK_FRAME_SAMPLES) >= QUIET_LEVEL || !talk_jitter_ready(&s->jitter) ||
        !decode(s, raw, talk_jitter_pop(&s->jitter, raw)))
        return;
    s->delay--;
    s->last_late = now_us();
}
static void conceal(session_t *s) {
    // esp_opus_dec refuses a NULL buffer even with nothing in it.
    uint8_t none;
    if (!decode(s, &none, 0))
        memset(s->far, 0, sizeof(s->far));
}
// The far end's next 20 ms into s->far: a packet, Opus's concealment for one
// missing or inserted to lengthen the delay, or silence before playback
// starts.
static void decode_frame(session_t *s) {
    if (!s->play_at || now_us() < s->play_at) {
        memset(s->far, 0, sizeof(s->far));
        return;
    }
    if (s->hold) {
        s->hold--;
        conceal(s);
        return;
    }
    uint8_t raw[TALK_PAYLOAD];
    size_t n = talk_jitter_pop(&s->jitter, raw);
    if (n && decode(s, raw, n)) {
        shrink_delay(s, raw);
        return;
    }
    s->concealed++;
    conceal(s);
}
// One 20 ms frame each way. The mic paces the call; the DAC runs off the
// same crystal, so its queue stays as deep as it started and the echo
// canceller's reference keeps a fixed lead over the echo.
static void call_frame(int sock, session_t **session) {
    session_t *s = *session;
    if (mic_read(s->pcm, TALK_FRAME_SAMPLES, 60) != TALK_FRAME_SAMPLES) {
        finish(session, "Microphone stopped");
        return;
    }
    int64_t started = now_us();
    s->frames++;
    if (!send_frame(sock, s)) {
        finish(session, "Call failed");
        return;
    }
    receive(sock, s);
    decode_frame(s);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    // -6 dB: the far end's mic gain makes speech near full scale, far
    // louder than music at the same volume.
    int32_t target = pcm_gain_for_volume(s_ui.volume) / 2;
    s_ui.seconds = (uint32_t)((now_us() - s->answered) / 1000000);
    xSemaphoreGive(s_lock);
    // Volume goes on before playout takes the canceller's reference, so it
    // hears what the speaker plays.
    pcm_apply_gain(s->far, TALK_FRAME_SAMPLES, 1, &s->gain, target);
    talk_playout_push(s->far, TALK_FRAME_SAMPLES);
    // Not counting the wait for the mic, whose processing mic_stop() logs.
    int64_t busy = now_us() - started;
    s->busy_us += busy;
    s->busy_max_us = busy > s->busy_max_us ? busy : s->busy_max_us;
}

// Ends the session when either side has hung up or the call can't go on.
static void check_ended(session_t **session, bool hang_up, uint32_t ended) {
    session_t *s = *session;
    bool ringing = s->call && !s->in_call;
    if (ringing)
        s->outcome = hang_up ? UI_CALL_DECLINED : UI_CALL_MISSED;
    if (hang_up)
        finish(session, NULL);
    else if (s->call && ended == s->call)
        finish(session, s->in_call ? "Call ended" : "Missed call");
    else if (!s->in_call && audio_source_suspended())
        finish(session, s->call ? "Missed call" : NULL);
    else if (s->in_call && cancelled())
        finish(session, "Call ended");
    else if (s->in_call && !s->call && !talk_rtsp_alive(s->control))
        finish(session, "Call ended");
    else if (s->in_call && now_us() - s->last_received > CALL_LOST_US)
        finish(session, "Call lost");
}
// The stack grows down: count fill bytes from its low end.
static size_t stack_untouched(const uint8_t *stack) {
    size_t n = 0;
    while (n < CALL_STACK && stack[n] == STACK_FILL)
        n++;
    return n;
}
// One pass of the worker: a call frame in a call, else a short poll.
static void step(void) {
    int sock = s_sock;
    session_t *s = s_session;
    {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool network = s_network, hang_up = s_hang_up, accept = s_accept;
        uint32_t ringing = s_ring_call, ended = s_ring_ended, peer = 0;
        s_hang_up = s_accept = false;
        if (!s && !ringing && s_dial) {
            peer = s_dial;
            // In the same hold as taking the request, so ring() turns
            // callers away from here on.
            s_ui.state = UI_TALK_CALLING;
            s_ui.message[0] = '\0';
        }
        s_dial = 0;
        xSemaphoreGive(s_lock);
        if (sock < 0 && network) {
            uint16_t port = TALK_RTP_PORT;
            sock = bind_socket(&port, SOCK_DGRAM);
            if (sock >= 0) {
                int flags = fcntl(sock, F_GETFL, 0);
                fcntl(sock, F_SETFL, flags | O_NONBLOCK);
            }
        }
        if (sock < 0) {
            if (peer)
                set_state(UI_TALK_IDLE, NULL, "Not on WiFi");
            vTaskDelay(pdMS_TO_TICKS(100));
            s_sock = sock;
            return;
        }

        if (!s && ringing)
            s = answer_ringing();
        else if (!s && peer)
            s = dial(peer);
        // Also right after answer_ringing(): Decline may have come first.
        if (s)
            check_ended(&s, hang_up, ended);

        if (s && !s->in_call) {
            if (s->call && accept)
                accept_ringing(&s);
            else if (!s->call)
                dialing(&s);
        }
        if (s && s->in_call) {
            call_frame(sock, &s);
        } else {
            receive(sock, NULL);
            vTaskDelay(pdMS_TO_TICKS(IDLE_POLL_MS));
        }
    }
    s_sock = sock;
    s_session = s;
}
// A call or a voice note being recorded or played: frames to run.
static bool busy(void) {
    return s_session || note_audio_busy();
}
static void pass(void) {
    if (s_session || !note_audio_busy()) {
        step();
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool call = s_dial || s_ring_call;
    uint8_t volume = s_ui.volume;
    xSemaphoreGive(s_lock);
    // A note playing gives way to a call; recording turns calls away.
    if (call)
        note_audio_stop();
    note_audio_step(volume);
}
static void call_passes(void) {
    while (busy())
        pass();
    if (s_call_stack)
        s_call_stack_used = CALL_STACK - stack_untouched(s_call_stack);
}
static void worker(void *arg) {
    (void)arg;
    for (;;) {
        if (!busy()) {
            step();
            continue;
        }
        // Opus, the echo canceller and noise suppressor keep their work on
        // the stack; on the task's PSRAM stack a frame took longer than 20 ms.
        // Internal RAM for it is only taken while a call rings or runs, before
        // the mic and codecs split the heap.
        log_internal("before the call stack");
        size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        s_call_stack = free_internal >= CALL_STACK + CALL_HEAP_RESERVE
                           ? heap_caps_malloc(CALL_STACK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
                           : NULL;
        if (!s_call_stack) {
            ESP_LOGW(TAG, "no internal RAM for the call stack (%u free, largest %u), using PSRAM",
                     (unsigned)free_internal,
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
            call_passes();
            ESP_LOGI(TAG, "PSRAM stack: %u bytes never used", (unsigned)uxTaskGetStackHighWaterMark(NULL));
            continue;
        }
        log_internal("with the call stack");
        esp_execute_shared_stack_function(s_stack_lock, s_call_stack, CALL_STACK, call_passes);
        ESP_LOGI(TAG, "call stack: %u of %u bytes used", (unsigned)s_call_stack_used, (unsigned)CALL_STACK);
        free(s_call_stack);
        s_call_stack = NULL;
    }
}

// Runs on the esp_timer task, whose internal stack may write flash.
static void save_volume(void *arg) {
    (void)arg;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint8_t v = s_ui.volume;
    xSemaphoreGive(s_lock);
    nvs_handle_t nvs;
    if (nvs_open("talk", NVS_READWRITE, &nvs) == ESP_OK) {
        if (nvs_set_u8(nvs, "volume", v) == ESP_OK)
            nvs_commit(nvs);
        nvs_close(nvs);
    }
}
static void discovery(void *arg) {
    (void)arg;
    bool advertised = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool network = s_network, rename = s_renamed;
        xSemaphoreGive(s_lock);
        char own_ip[16];
        char ssid[33];
        if (!network || !wifi_get_station(ssid, sizeof(ssid), own_ip, sizeof(own_ip)))
            continue;
        if (!advertised) {
            char name[33];
            device_name_get(name, sizeof(name));
            mdns_txt_item_t txt[] = {{"v", TALK_VERSION}, {"name", name}, {"id", s_key}};
            // AirPlay owns mdns_init(). Retry until that task has initialized it;
            // add our service once, well after its initial probing settles.
            if (mdns_service_add(NULL, "_audioknob", "_tcp", TALK_RTSP_PORT, txt, 3) != ESP_OK)
                continue;
            advertised = true;
        } else if (rename) {
            char name[33];
            device_name_get(name, sizeof(name));
            mdns_service_txt_item_set("_audioknob", "_tcp", "name", name);
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_renamed = false;
            xSemaphoreGive(s_lock);
        }
        mdns_result_t *results = NULL;
        if (mdns_query_ptr("_audioknob", "_tcp", 1000, UI_TALK_MAX + 1, &results) != ESP_OK)
            continue;
        ui_talk_peer_t peers[UI_TALK_MAX] = {0};
        int count = 0;
        for (mdns_result_t *r = results; r && count < UI_TALK_MAX; r = r->next) {
            const char *name = r->instance_name, *key = NULL;
            bool version = false;
            for (size_t i = 0; i < r->txt_count; i++) {
                if (!r->txt[i].value)
                    continue;
                if (!strcmp(r->txt[i].key, "name"))
                    name = r->txt[i].value;
                if (!strcmp(r->txt[i].key, "id"))
                    key = r->txt[i].value;
                if (!strcmp(r->txt[i].key, "v") && !strcmp(r->txt[i].value, TALK_VERSION))
                    version = true;
            }
            if (!version || !name || !notes_valid_key(key) || !strcmp(key, s_key) || r->port != TALK_RTSP_PORT)
                continue;
            for (mdns_ip_addr_t *a = r->addr; a; a = a->next) {
                if (a->addr.type != ESP_IPADDR_TYPE_V4 || a->addr.u_addr.ip4.addr == inet_addr(own_ip))
                    continue;
                uint32_t id = a->addr.u_addr.ip4.addr;
                bool duplicate = false;
                for (int i = 0; i < count; i++)
                    if (peers[i].id == id)
                        duplicate = true;
                if (!duplicate) {
                    peers[count].id = id;
                    snprintf(peers[count].key, sizeof(peers[count].key), "%s", key);
                    snprintf(peers[count++].name, 33, "%s", name);
                }
                break;
            }
        }
        mdns_query_results_free(results);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_ui.count = count;
        memcpy(s_ui.peers, peers, sizeof(peers));
        xSemaphoreGive(s_lock);
        notes_set_peers(peers, count);
    }
}
void talk_init(void) {
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
    snprintf(s_key, sizeof(s_key), "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    note_audio_init();
    notes_init(s_key);
    s_lock = xSemaphoreCreateMutex();
    s_write_lock = xSemaphoreCreateMutex();
    s_stack_lock = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(s_lock && s_write_lock && s_stack_lock ? ESP_OK : ESP_ERR_NO_MEM);
    nvs_handle_t nvs;
    if (nvs_open("talk", NVS_READONLY, &nvs) == ESP_OK) {
        uint8_t volume;
        if (nvs_get_u8(nvs, "volume", &volume) == ESP_OK && volume <= 100)
            s_ui.volume = volume;
        nvs_close(nvs);
    }
    const esp_timer_create_args_t timer = {.callback = save_volume, .name = "talk_volume"};
    ESP_ERROR_CHECK(esp_timer_create(&timer, &s_save_timer));
    // Espressif documents up to 40 KB codec stack. Keep it in PSRAM; this
    // task never writes flash (volume persistence runs on the esp_timer task).
    // Core 1, above LVGL: on core 0 WiFi took enough that a 20 ms frame
    // overran; a call has preempted AirPlay's core 1 tasks.
    BaseType_t w =
        xTaskCreatePinnedToCoreWithCaps(worker, "talk", 40960, NULL, 8, NULL, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    BaseType_t d =
        xTaskCreateWithCaps(discovery, "talk_discovery", 4096, NULL, 3, NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_ERROR_CHECK(w == pdPASS && d == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
void talk_network_up(void) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool first = !s_network;
    s_network = true;
    xSemaphoreGive(s_lock);
    // WiFi brings lwIP up after talk_init(). Opening a socket before then
    // asserts in tcpip_send_msg_wait_sem. Keep one listener across reconnects.
    if (first) {
        talk_rtsp_init(&(talk_rtsp_ops_t){.ring = ring,
                                          .answer = answer,
                                          .stop = ring_stop,
                                          .active = ring_active,
                                          .note = notes_receive,
                                          .played = notes_receipt},
                       s_key);
        ESP_LOGI(TAG, "LAN voice ready, RTSP %d / RTP %d", TALK_RTSP_PORT, TALK_RTP_PORT);
    }
}
