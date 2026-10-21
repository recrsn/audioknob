#include "talk.h"
#include "talk_wire.h"
#include "talk_rtsp.h"
#include "flash_message.h"
#include "voice_notes.h"

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "audio_out.h"
#include "audio_source.h"
#include "device_name.h"
#include "encoder/impl/esp_opus_enc.h"
#include "decoder/impl/esp_opus_dec.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/idf_additions.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "mdns.h"
#include "mic.h"
#include "nvs.h"
#include "pcm.h"
#include "wifi.h"
#include "http_util.h"

#define SESSION_MAX_US (60LL * 1000000)
#define RECEIVE_TIMEOUT_US 500000
#define CONNECT_TIMEOUT_US 2000000
#define PREFILL_US 60000
static const char *TAG = "talk";
static SemaphoreHandle_t s_lock, s_write_lock;
static ui_talk_t s_ui = {.volume = 70};
static uint32_t s_requested_peer;
static bool s_abort, s_network, s_renamed;
static int64_t s_receive_end;
static uint32_t s_request_epoch;
static int64_t s_volume_changed;
static QueueHandle_t s_incoming, s_answer;
typedef struct {
    uint32_t ip;
    uint16_t port, sequence;
    char name[33];
    int64_t deadline;
} incoming_t;

typedef struct {
    ui_talk_state_t state;
    struct sockaddr_in peer;
    uint32_t id, epoch;
    uint16_t sequence;
    uint32_t timestamp;
    int control;
    bool mic;
    int64_t started, last_received, last_open, play_at;
    void *encoder, *decoder;
    int32_t gain;
    pcm_resampler_t resampler;
    talk_jitter_t jitter;
    int16_t pcm[TALK_FRAME_SAMPLES * 2];
    int16_t output[900 * 2];
} session_t;

static int64_t now_us(void) {
    return esp_timer_get_time();
}
static void set_state(ui_talk_state_t state, const char *peer, const char *message) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_ui.state = state;
    s_ui.level = 0;
    snprintf(s_ui.peer, sizeof(s_ui.peer), "%s", peer ? peer : "");
    snprintf(s_ui.message, sizeof(s_ui.message), "%s", message ? message : "");
    xSemaphoreGive(s_lock);
}
void talk_get_state(ui_talk_t *out) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_ui;
    xSemaphoreGive(s_lock);
}
void talk_press(uint32_t peer, bool held) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_requested_peer = held ? peer : 0;
    s_request_epoch++;
    xSemaphoreGive(s_lock);
}
void talk_set_volume(uint8_t volume) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_ui.volume = volume > 100 ? 100 : volume;
    s_volume_changed = now_us();
    xSemaphoreGive(s_lock);
}
void talk_preempt(void) {
    if (!s_lock)
        return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_abort = true;
    s_requested_peer = 0;
    s_request_epoch++;
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
static void finish(session_t **session, const char *message) {
    session_t *s = *session;
    if (s) {
        talk_rtsp_end(s->control);
        if (s->mic)
            mic_stop();
        if (s->encoder)
            esp_opus_enc_close(s->encoder);
        if (s->decoder)
            esp_opus_dec_close(s->decoder);
        audio_source_release(AUDIO_SOURCE_TALK);
        free(s);
        *session = NULL;
    }
    set_state(UI_TALK_IDLE, NULL, message);
}
static session_t *create_session(const struct sockaddr_in *peer, uint32_t id, ui_talk_state_t state, const char *name,
                                 uint32_t epoch) {
    session_t *s = heap_caps_calloc(1, sizeof(*s), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s)
        return NULL;
    s->control = -1;
    s->peer = *peer;
    s->id = id;
    s->state = state;
    s->started = s->last_received = now_us();
    s->epoch = epoch;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_abort = false;
    s_receive_end = 0;
    xSemaphoreGive(s_lock);
    if (!audio_source_claim(AUDIO_SOURCE_TALK) || cancelled()) {
        free(s);
        audio_source_release(AUDIO_SOURCE_TALK);
        return NULL;
    }
    set_state(state, name, NULL);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_ui.peer_id = peer->sin_addr.s_addr;
    xSemaphoreGive(s_lock);
    return s;
}
static void level(const int16_t *pcm, size_t n) {
    double sum = 0;
    for (size_t i = 0; i < n; i++)
        sum += (double)pcm[i] * pcm[i];
    // Show -60..0 dBFS across the level ring.
    float db = 20 * log10f((float)sqrt(sum / n) / 32768.f + 0.000001f);
    int pct = (int)((db + 60) * (100.f / 60));
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_ui.level = pct < 0 ? 0 : pct > 100 ? 100 : pct;
    xSemaphoreGive(s_lock);
}
static bool start_encoder(session_t *s) {
    esp_opus_enc_config_t cfg = ESP_OPUS_ENC_CONFIG_DEFAULT();
    cfg.sample_rate = 16000;
    cfg.channel = 1;
    cfg.bitrate = 24000;
    cfg.complexity = 2;
    cfg.enable_vbr = true;
    if (esp_opus_enc_open(&cfg, sizeof(cfg), &s->encoder) != ESP_AUDIO_ERR_OK)
        return false;
    int input, output;
    if (esp_opus_enc_get_frame_size(s->encoder, &input, &output) != ESP_AUDIO_ERR_OK ||
        input != TALK_FRAME_SAMPLES * 2 || output > TALK_PAYLOAD)
        return false;
    for (int retry = 0; retry < 10 && !s->mic && !cancelled(); retry++) {
        s->mic = mic_start(NULL);
        if (!s->mic)
            vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (!s->mic)
        return false;
    s->state = UI_TALK_SENDING;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_ui.state = UI_TALK_SENDING;
    xSemaphoreGive(s_lock);
    return true;
}
static bool start_decoder(session_t *s) {
    esp_opus_dec_cfg_t cfg = {.sample_rate = 16000,
                              .channel = 1,
                              .frame_duration = ESP_OPUS_DEC_FRAME_DURATION_20_MS,
                              .self_delimited = false};
    if (esp_opus_dec_open(&cfg, sizeof(cfg), &s->decoder) != ESP_AUDIO_ERR_OK)
        return false;
    pcm_resampler_init(&s->resampler, 16000, AUDIO_OUT_SAMPLE_RATE);
    talk_jitter_init(&s->jitter, s->sequence);
    return true;
}
static bool write_audio(session_t *s, size_t frames) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint8_t volume = s_ui.volume;
    xSemaphoreGive(s_lock);
    pcm_apply_gain(s->output, frames, &s->gain, pcm_gain_for_volume(volume));
    xSemaphoreTake(s_write_lock, portMAX_DELAY);
    bool ok = !cancelled();
    if (ok) {
        audio_out_set_rate(AUDIO_OUT_SAMPLE_RATE);
        audio_out_write16(s->output, frames);
    }
    xSemaphoreGive(s_write_lock);
    return ok;
}
static void incoming_chime(session_t *s) {
    // 20 ms, quiet arrival tone, on the same exclusive path as the voice.
    for (size_t i = 0; i < 882; i++) {
        float envelope = sinf((float)i * 3.14159265f / 882);
        int16_t v = (int16_t)(2200 * envelope * sinf((float)i * 2 * 3.14159265f * 880 / AUDIO_OUT_SAMPLE_RATE));
        s->output[2 * i] = s->output[2 * i + 1] = v;
    }
    write_audio(s, 882);
}
void talk_persist_settings(void) {
    if (!s_lock)
        return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int64_t changed = s_volume_changed;
    uint8_t v = s_ui.volume;
    xSemaphoreGive(s_lock);
    if (!changed || now_us() - changed < 2000000)
        return;
    nvs_handle_t nvs;
    if (nvs_open("talk", NVS_READWRITE, &nvs) == ESP_OK) {
        if (nvs_set_u8(nvs, "volume", v) == ESP_OK)
            nvs_commit(nvs);
        nvs_close(nvs);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_volume_changed == changed)
            s_volume_changed = 0;
        xSemaphoreGive(s_lock);
    }
}
static void consume_request(uint32_t epoch) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_request_epoch == epoch)
        s_requested_peer = 0;
    xSemaphoreGive(s_lock);
}
static bool incoming_start(uint32_t ip, uint16_t port, uint16_t sequence, const char *name) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool idle = s_ui.state == UI_TALK_IDLE && !s_requested_peer;
    xSemaphoreGive(s_lock);
    if (!idle || audio_source_suspended())
        return false;
    incoming_t in = {.ip = ip, .port = port, .sequence = sequence, .deadline = now_us() + 1000000};
    snprintf(in.name, sizeof(in.name), "%s", name);
    bool stale;
    while (xQueueReceive(s_answer, &stale, 0) == pdTRUE) {
    }
    if (xQueueSend(s_incoming, &in, 0) != pdTRUE)
        return false;
    bool answer = false;
    if (xQueueReceive(s_answer, &answer, pdMS_TO_TICKS(1500)) != pdTRUE)
        return false;
    return answer;
}
static bool incoming_active(void) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool active = s_ui.state == UI_TALK_RECEIVING;
    xSemaphoreGive(s_lock);
    return active;
}
static void incoming_stop(void) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_ui.state == UI_TALK_RECEIVING)
        s_receive_end = now_us() + 100000;
    xSemaphoreGive(s_lock);
}
static void worker(void *arg) {
    (void)arg;
    int sock = -1;
    session_t *s = NULL;
    for (;;) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        uint32_t requested = s_requested_peer, epoch = s_request_epoch;
        bool network = s_network;
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
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (s && (cancelled() || now_us() - s->started > SESSION_MAX_US ||
                  (s->state != UI_TALK_RECEIVING &&
                   (!requested || epoch != s->epoch || (s->control >= 0 && !talk_rtsp_alive(s->control)))))) {
            consume_request(s->epoch);
            requested = 0;
            finish(&s, NULL);
        }
        incoming_t incoming;
        if (xQueueReceive(s_incoming, &incoming, 0) == pdTRUE) {
            bool accepted = false;
            if (!s && !requested && now_us() < incoming.deadline && !audio_source_suspended()) {
                struct sockaddr_in peer = {
                    .sin_family = AF_INET, .sin_port = htons(incoming.port), .sin_addr.s_addr = incoming.ip};
                s = create_session(&peer, 0, UI_TALK_RECEIVING, incoming.name, epoch);
                if (s) {
                    s->sequence = incoming.sequence;
                    accepted = start_decoder(s);
                }
                if (accepted)
                    incoming_chime(s);
                else
                    finish(&s, "Voice unavailable");
            }
            xQueueOverwrite(s_answer, &accepted);
        }
        if (!s && requested && !audio_source_suspended()) {
            char name[33] = "";
            xSemaphoreTake(s_lock, portMAX_DELAY);
            for (int i = 0; i < s_ui.count; i++)
                if (s_ui.peers[i].id == requested)
                    snprintf(name, sizeof(name), "%s", s_ui.peers[i].name);
            xSemaphoreGive(s_lock);
            struct sockaddr_in peer = {.sin_family = AF_INET, .sin_addr.s_addr = requested};
            if (name[0])
                s = create_session(&peer, esp_random(), UI_TALK_CONNECTING, name, epoch);
            if (s) {
                s->sequence = (uint16_t)esp_random();
                s->timestamp = esp_random();
                uint16_t peer_port;
                s->control = talk_rtsp_begin(requested, s->sequence, s->timestamp, &peer_port);
                xSemaphoreTake(s_lock, portMAX_DELAY);
                bool held = s_requested_peer == requested && s_request_epoch == epoch;
                xSemaphoreGive(s_lock);
                if (s->control >= 0 && held && !cancelled()) {
                    s->peer.sin_port = htons(peer_port);
                    if (!start_encoder(s)) {
                        consume_request(epoch);
                        finish(&s, "Microphone unavailable");
                    }
                } else {
                    consume_request(epoch);
                    finish(&s, held ? "Peer unavailable or busy" : NULL);
                }
            } else {
                consume_request(epoch);
                set_state(UI_TALK_IDLE, NULL, "Peer unavailable");
            }
        }
        // RTP reordering is bounded; control messages never enter this socket.
        for (int i = 0; i < 16; i++) {
            uint8_t wire[TALK_RTP_HEADER + TALK_PAYLOAD + 64];
            struct sockaddr_in from;
            socklen_t fromlen = sizeof(from);
            int n = recvfrom(sock, wire, sizeof(wire), 0, (struct sockaddr *)&from, &fromlen);
            if (n < 0)
                break;
            if (!s || s->state != UI_TALK_RECEIVING || from.sin_addr.s_addr != s->peer.sin_addr.s_addr ||
                from.sin_port != s->peer.sin_port)
                continue;
            talk_packet_t p;
            if (!talk_rtp_decode(&p, wire, n))
                continue;
            if (!s->play_at)
                s->id = p.ssrc;
            if (p.ssrc != s->id)
                continue;
            if (talk_jitter_push(&s->jitter, &p)) {
                s->last_received = now_us();
                if (!s->play_at)
                    s->play_at = now_us() + PREFILL_US;
            }
        }
        if (s && s->state == UI_TALK_SENDING) {
            size_t frames = mic_read(s->pcm, TALK_FRAME_SAMPLES, 60);
            if (frames != TALK_FRAME_SAMPLES || cancelled()) {
                consume_request(s->epoch);
                finish(&s, "Microphone stopped");
            } else {
                level(s->pcm, frames);
                talk_packet_t p = {.ssrc = s->id, .sequence = s->sequence++, .timestamp = s->timestamp};
                s->timestamp += 960;
                esp_audio_enc_in_frame_t in = {.buffer = (uint8_t *)s->pcm, .len = frames * 2};
                esp_audio_enc_out_frame_t out = {.buffer = p.payload, .len = sizeof(p.payload)};
                uint8_t wire[TALK_RTP_HEADER + TALK_PAYLOAD];
                bool ok = esp_opus_enc_process(s->encoder, &in, &out) == ESP_AUDIO_ERR_OK && out.encoded_bytes;
                if (ok) {
                    p.length = out.encoded_bytes;
                    size_t n = talk_rtp_encode(wire, &p);
                    ok = n && sendto(sock, wire, n, 0, (struct sockaddr *)&s->peer, sizeof(s->peer)) == (int)n;
                }
                if (!ok) {
                    consume_request(s->epoch);
                    finish(&s, "Voice send failed");
                }
            }
        }
        if (s && s->state == UI_TALK_RECEIVING) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            int64_t end = s_receive_end;
            xSemaphoreGive(s_lock);
            if ((end && now_us() >= end) || now_us() - s->last_received > RECEIVE_TIMEOUT_US)
                finish(&s, NULL);
            else if (s->play_at && now_us() >= s->play_at) {
                uint8_t encoded[TALK_PAYLOAD];
                size_t n = talk_jitter_pop(&s->jitter, encoded);
                esp_audio_dec_in_raw_t raw = {.buffer = encoded,
                                              .len = n,
                                              .frame_recover =
                                                  n ? ESP_AUDIO_DEC_RECOVERY_NONE : ESP_AUDIO_DEC_RECOVERY_PLC};
                esp_audio_dec_out_frame_t out = {.buffer = (uint8_t *)s->pcm, .len = TALK_FRAME_SAMPLES * 2};
                esp_audio_dec_info_t info;
                if (esp_opus_dec_decode(s->decoder, &raw, &out, &info) != ESP_AUDIO_ERR_OK ||
                    out.decoded_size != TALK_FRAME_SAMPLES * 2)
                    memset(s->pcm, 0, TALK_FRAME_SAMPLES * 2);
                level(s->pcm, TALK_FRAME_SAMPLES);
                pcm_to_stereo(s->pcm, TALK_FRAME_SAMPLES, 1);
                write_audio(s, pcm_resample(&s->resampler, s->pcm, TALK_FRAME_SAMPLES, s->output));
                s->play_at += 20000;
                if (now_us() - s->play_at > 100000)
                    finish(&s, "Voice interrupted");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(s ? 2 : 10));
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
            mdns_txt_item_t txt[] = {{"v", "1"}, {"name", name}};
            // AirPlay owns mdns_init(). Retry until that task has initialized it;
            // add our service once, well after its initial probing settles.
            if (mdns_service_add(NULL, "_audioknob", "_tcp", TALK_RTSP_PORT, txt, 2) != ESP_OK)
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
            const char *name = r->instance_name;
            bool version = false;
            for (size_t i = 0; i < r->txt_count; i++) {
                if (!strcmp(r->txt[i].key, "name"))
                    name = r->txt[i].value;
                if (!strcmp(r->txt[i].key, "v") && !strcmp(r->txt[i].value, "1"))
                    version = true;
            }
            if (!version || !name || r->port != TALK_RTSP_PORT)
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
    }
}
static bool on_message(uint32_t ip, const char *name, const char *path, const char *type, const void *body,
                       size_t length) {
    if (!strncmp(path, "/messages/voice?id=", 19))
        return !strcmp(type, "audio/ogg") && voice_notes_receive(ip, name, path + 19, body, length);
    return !strcmp(type, "application/json") && length <= 1024 && flash_message_receive(ip, path, body);
}
void talk_init(void) {
    s_lock = xSemaphoreCreateMutex();
    s_write_lock = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(s_lock && s_write_lock ? ESP_OK : ESP_ERR_NO_MEM);
    nvs_handle_t nvs;
    if (nvs_open("talk", NVS_READONLY, &nvs) == ESP_OK) {
        uint8_t volume;
        if (nvs_get_u8(nvs, "volume", &volume) == ESP_OK && volume <= 100)
            s_ui.volume = volume;
        nvs_close(nvs);
    }
    s_incoming = xQueueCreate(1, sizeof(incoming_t));
    s_answer = xQueueCreate(1, sizeof(bool));
    ESP_ERROR_CHECK(s_incoming && s_answer ? ESP_OK : ESP_ERR_NO_MEM);
    // Espressif documents up to 40 KB codec stack. Keep it in PSRAM; this
    // task never writes flash (volume persistence runs on the messages task).
    BaseType_t w =
        xTaskCreatePinnedToCoreWithCaps(worker, "talk", 40960, NULL, 8, NULL, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
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
        talk_rtsp_init(&(talk_rtsp_ops_t){
            .start = incoming_start, .stop = incoming_stop, .active = incoming_active, .message = on_message});
        ESP_LOGI(TAG, "LAN voice ready, RTSP %d / RTP %d", TALK_RTSP_PORT, TALK_RTP_PORT);
    }
}
