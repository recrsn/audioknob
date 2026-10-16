/*
 * Stream handling: SETUP (both phases), RECORD, SETRATEANCHORTIME,
 * FLUSHBUFFERED, FLUSH, TEARDOWN, and the anchor that maps RTP time to
 * the sender's PTP clock. After shairport-sync's rtsp.c (handle_setup_2
 * and friends), rtp.c (the PTP anchor functions) and remote.c
 * (Copyright (c) James Laird 2013, Mike Brady 2014--2026, MIT License).
 * AirPlay 1 sessions (ap1.c) take turns with AirPlay 2 ones here too.
 */

#ifndef AK_NATIVE

#include "session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ap2.h"
#include "buffered.h"
#include "dacp.h"
#include "esp_log.h"
#include "esp_random.h"
#include "event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/inet.h"
#include "ptp.h"
#include "raop_rtp.h"
#include "rtp.h"

#define TYPE_REALTIME 96
#define TYPE_BUFFERED 103
#define TYPE_REMOTE_CONTROL 130

static const char *TAG = "ap2_session";

static struct {
    SemaphoreHandle_t lock;
    ap2_conn_t *principal;
    int type;  // of the running stream, 0 if none
    buffered_t *buffered;
    rtp_t *rtp;
    // An AirPlay 1 stream: NTP-timed, and it emits PLAY/FLUSH itself.
    raop_rtp_t *ap1_rtp;

    // Producers check their epoch against this before queueing audio, so
    // nothing decoded before a flush or pause can play after it.
    SemaphoreHandle_t pcm_lock;
    uint32_t epoch;
    bool play_announced;

    portMUX_TYPE anchor_lock;
    bool anchor_valid;
    uint64_t anchor_clock;
    uint32_t anchor_rtp;
    int64_t anchor_ns;
    uint32_t anchor_epoch;

    volatile bool play_enabled;
} s = {.anchor_lock = portMUX_INITIALIZER_UNLOCKED};

void session_init(void) {
    s.lock = xSemaphoreCreateRecursiveMutex();
    s.pcm_lock = xSemaphoreCreateMutex();
}

/* Timing */

// shairport-sync won't time against a clock that has been master for less.
#define MASTERSHIP_SETTLED_NS 400000000LL

// Re-anchors that move the timeline by more than this are logged.
#define ANCHOR_SHIFT_LOG_NS 1000000LL

void session_set_anchor(uint64_t clock_id, uint32_t rtp, int64_t master_ns) {
    portENTER_CRITICAL(&s.anchor_lock);
    bool had = s.anchor_valid && s.anchor_clock == clock_id;
    int64_t predicted_ns = s.anchor_ns + (int64_t)(int32_t)(rtp - s.anchor_rtp) * 1000000000LL / AP2_RATE;
    portEXIT_CRITICAL(&s.anchor_lock);
    if (had && llabs(master_ns - predicted_ns) > ANCHOR_SHIFT_LOG_NS) {
        ESP_LOGW(TAG, "re-anchor moves the timeline %+.1f ms", (double)(master_ns - predicted_ns) / 1e6);
    }

    portENTER_CRITICAL(&s.anchor_lock);
    s.anchor_valid = true;
    s.anchor_clock = clock_id;
    s.anchor_rtp = rtp;
    s.anchor_ns = master_ns;
    portEXIT_CRITICAL(&s.anchor_lock);
}

void session_clear_anchor(void) {
    portENTER_CRITICAL(&s.anchor_lock);
    s.anchor_valid = false;
    portEXIT_CRITICAL(&s.anchor_lock);
}

bool session_playtime_us(uint32_t rtp, int64_t *playtime_us) {
    ptp_clock_t clk;
    if (!ptp_clock(&clk) || ptp_local_ns() - clk.mastership_start_ns < MASTERSHIP_SETTLED_NS) {
        return false;
    }

    portENTER_CRITICAL(&s.anchor_lock);
    bool valid = s.anchor_valid;
    uint64_t old_clock = s.anchor_clock;
    // A new grandmaster's time can be stepped from the old one's: carry the
    // anchor across, as shairport-sync does.
    bool moved = valid && s.anchor_clock != clk.clock_id;
    if (moved) {
        s.anchor_ns -= clk.clock_change_ns;
        s.anchor_clock = clk.clock_id;
    }
    uint32_t anchor_rtp = s.anchor_rtp;
    int64_t anchor_ns = s.anchor_ns;
    portEXIT_CRITICAL(&s.anchor_lock);

    if (!valid) {
        return false;
    }
    if (moved) {
        ESP_LOGI(TAG, "anchor moves from clock %016llx to %016llx", (unsigned long long)old_clock,
                 (unsigned long long)clk.clock_id);
    }
    int64_t frames = (int32_t)(rtp - anchor_rtp);
    int64_t master_ns = anchor_ns + frames * 1000000000LL / AP2_RATE;
    *playtime_us = (master_ns - clk.offset_ns) / 1000;
    return true;
}

bool session_play_enabled(void) {
    return s.play_enabled;
}

uint32_t session_epoch(void) {
    xSemaphoreTake(s.pcm_lock, portMAX_DELAY);
    uint32_t e = s.epoch;
    xSemaphoreGive(s.pcm_lock);
    return e;
}

void session_pcm(const int16_t *pcm, size_t frames, int64_t playtime_us, uint32_t epoch) {
    xSemaphoreTake(s.pcm_lock, portMAX_DELAY);
    if (epoch == s.epoch) {
        if (!s.play_announced) {
            s.play_announced = true;
            ap2_emit_type(RAOP_EVENT_PLAY);
        }
        ap2_push_pcm(pcm, frames, playtime_us);
    }
    xSemaphoreGive(s.pcm_lock);
}

// Drops everything queued downstream and anything still being decoded.
static void flush_output(void) {
    xSemaphoreTake(s.pcm_lock, portMAX_DELAY);
    s.epoch++;
    bool was_playing = s.play_announced;
    s.play_announced = false;
    xSemaphoreGive(s.pcm_lock);
    if (was_playing) {
        ap2_emit_type(RAOP_EVENT_FLUSH);
    }
}

/* Streams */

// Caller holds s.lock.
static void stop_streams(void) {
    if (s.type) {
        ptp_play_end();
    }
    if (s.buffered) {
        buffered_stop(s.buffered);
        s.buffered = NULL;
    }
    if (s.rtp) {
        rtp_stop(s.rtp);
        s.rtp = NULL;
    }
    if (s.ap1_rtp) {
        raop_rtp_end(s.ap1_rtp);
        s.ap1_rtp = NULL;
        // Its PLAY went out past the epoch, so FLUSH won't follow from it.
        ap2_emit_type(RAOP_EVENT_FLUSH);
    }
    s.type = 0;
    s.play_enabled = false;
    session_clear_anchor();
    flush_output();
}

// Caller holds s.lock.
static void become_principal(ap2_conn_t *conn) {
    if (s.principal == conn) {
        return;
    }
    if (s.principal) {
        ESP_LOGI(TAG, "conn %d takes over from conn %d", conn->id, s.principal->id);
        stop_streams();
        if (s.principal->airplay1) {
            dacp_stop();
        }
    }
    s.principal = conn;
    if (conn->airplay1) {
        dacp_start(conn->ap1.dacp_id[0] ? conn->ap1.dacp_id : NULL,
                   conn->ap1.active_remote[0] ? conn->ap1.active_remote : NULL);
    }
    ap2_emit(&(raop_event_t){.type = RAOP_EVENT_SESSION_START, .session.client = conn->client_name});
}

// Caller holds s.lock.
static void end_principal(void) {
    stop_streams();
    if (s.principal && s.principal->airplay1) {
        dacp_stop();
    }
    s.principal = NULL;
    ap2_emit_type(RAOP_EVENT_SESSION_END);
}

/* SETUP */

static bool open_events(ap2_conn_t *conn, bpw_t *w, bpw_obj reply) {
    if (!conn->events) {
        conn->events = event_open(conn, &conn->event_port);
        if (!conn->events) {
            return false;
        }
    }
    bpw_dict_set(w, reply, "eventPort", bpw_uint(w, conn->event_port));
    return true;
}

static int setup_timing(ap2_conn_t *conn, const bplist_t *pl, bp_obj root, bpw_t *w, bpw_obj reply) {
    char timing[16] = "";
    bplist_dict_string(pl, root, "timingProtocol", timing, sizeof(timing));

    if (!strcmp(timing, "PTP")) {
        conn->category = CATEGORY_PTP;
        bplist_dict_string(pl, root, "groupUUID", conn->group_uuid, sizeof(conn->group_uuid));
        bplist_dict_bool(pl, root, "groupContainsGroupLeader", &conn->group_contains_leader);
        if (!conn->shared_secret_len || !open_events(conn, w, reply)) {
            return 500;
        }
        bpw_dict_set(w, reply, "timingPort", bpw_uint(w, 0));
        bpw_obj info = bpw_dict(w);
        bpw_obj addrs = bpw_array(w);
        bpw_array_add(w, addrs, bpw_string(w, conn->self_ip));
        bpw_dict_set(w, info, "Addresses", addrs);
        bpw_dict_set(w, info, "ID", bpw_string(w, conn->self_ip));
        bpw_dict_set(w, reply, "timingPeerInfo", info);

        xSemaphoreTakeRecursive(s.lock, portMAX_DELAY);
        become_principal(conn);
        xSemaphoreGiveRecursive(s.lock);
        ESP_LOGI(TAG, "conn %d: PTP session from \"%s\"", conn->id, conn->client_name);
        return 200;
    }

    bool remote_only = false;
    if (!strcmp(timing, "None") && bplist_dict_bool(pl, root, "isRemoteControlOnly", &remote_only) && remote_only) {
        conn->category = CATEGORY_REMOTE_CONTROL;
        if (!conn->shared_secret_len || !open_events(conn, w, reply)) {
            return 500;
        }
        ESP_LOGI(TAG, "conn %d: remote control session", conn->id);
        return 200;
    }

    ESP_LOGW(TAG, "conn %d: unsupported timing protocol \"%s\"", conn->id, timing);
    return 501;
}

// Caller holds s.lock.
static int setup_stream(ap2_conn_t *conn, const bplist_t *pl, bp_obj stream, bpw_t *w, bpw_obj reply) {
    const uint8_t *key;
    size_t key_len = 0;
    uint64_t type = 0;
    if (!bplist_get_data(pl, bplist_dict_get(pl, stream, "shk"), &key, &key_len) || key_len != 32) {
        ESP_LOGE(TAG, "conn %d: SETUP without a usable session key", conn->id);
        return 400;
    }
    bplist_dict_uint(pl, stream, "type", &type);

    become_principal(conn);
    stop_streams();

    bpw_obj d = bpw_dict(w);
    uint16_t data_port = 0, control_port = 0;
    if (type == TYPE_REALTIME) {
        uint64_t sr = AP2_RATE;
        bplist_dict_uint(pl, stream, "sr", &sr);
        if (sr != AP2_RATE) {
            ESP_LOGW(TAG, "conn %d: %llu Hz realtime stream will play off-pitch", conn->id, (unsigned long long)sr);
        }
        s.rtp = rtp_start(key, true, &data_port, &control_port);
        if (!s.rtp) {
            return 500;
        }
        bpw_dict_set(w, d, "type", bpw_uint(w, TYPE_REALTIME));
        bpw_dict_set(w, d, "dataPort", bpw_uint(w, data_port));
        ESP_LOGI(TAG, "conn %d: realtime stream", conn->id);
    } else if (type == TYPE_BUFFERED) {
        size_t buffer_size;
        s.rtp = rtp_start(key, false, NULL, &control_port);
        s.buffered = s.rtp ? buffered_start(key, &data_port, &buffer_size) : NULL;
        if (!s.buffered) {
            stop_streams();
            return 500;
        }
        bpw_dict_set(w, d, "type", bpw_uint(w, TYPE_BUFFERED));
        bpw_dict_set(w, d, "dataPort", bpw_uint(w, data_port));
        bpw_dict_set(w, d, "audioBufferSize", bpw_uint(w, buffer_size));
        ESP_LOGI(TAG, "conn %d: buffered stream", conn->id);
    } else {
        ESP_LOGW(TAG, "conn %d: unsupported stream type %llu", conn->id, (unsigned long long)type);
        return 501;
    }
    s.type = (int)type;
    ptp_set_timing_peer(conn->client_ip);
    ptp_play_begin();
    bpw_dict_set(w, d, "controlPort", bpw_uint(w, control_port));
    bpw_obj streams = bpw_array(w);
    bpw_array_add(w, streams, d);
    bpw_dict_set(w, reply, "streams", streams);
    return 200;
}

void session_setup(ap2_conn_t *conn, const rtsp_req_t *req, rtsp_resp_t *resp) {
    bplist_t pl;
    if (!rtsp_req_plist(req, &pl)) {
        resp->code = 400;
        return;
    }
    bp_obj root = bplist_root(&pl);
    char name[64];
    if (bplist_dict_string(&pl, root, "name", name, sizeof(name))) {
        snprintf(conn->client_name, sizeof(conn->client_name), "%s", name);
    }

    bpw_t *w = bpw_new();
    if (!w) {
        resp->code = 500;
        return;
    }
    bpw_obj reply = bpw_dict(w);
    bp_obj streams = bplist_dict_get(&pl, root, "streams");

    if (streams == BP_NONE) {
        resp->code = setup_timing(conn, &pl, root, w, reply);
    } else if (conn->category == CATEGORY_PTP) {
        xSemaphoreTakeRecursive(s.lock, portMAX_DELAY);
        resp->code = setup_stream(conn, &pl, bplist_array_get(&pl, streams, 0), w, reply);
        xSemaphoreGiveRecursive(s.lock);
    } else if (conn->category == CATEGORY_REMOTE_CONTROL) {
        // The MediaRemote data stream isn't used; acknowledge it without a port.
        bpw_obj d = bpw_dict(w);
        bpw_dict_set(w, d, "streamID", bpw_uint(w, 1));
        bpw_dict_set(w, d, "type", bpw_uint(w, TYPE_REMOTE_CONTROL));
        bpw_obj arr = bpw_array(w);
        bpw_array_add(w, arr, d);
        bpw_dict_set(w, reply, "streams", arr);
        resp->code = 200;
    } else {
        resp->code = 501;
    }
    if (resp->code == 200) {
        rtsp_resp_plist(resp, w, reply);
    }
    bpw_free(w);
}

/* Play control */

void session_record(ap2_conn_t *conn, const rtsp_req_t *req, rtsp_resp_t *resp) {
    (void)req;
    rtsp_resp_header(resp, "Audio-Latency", "0");
    xSemaphoreTakeRecursive(s.lock, portMAX_DELAY);
    // Realtime audio has no rate to start it: it plays once it arrives.
    if (conn == s.principal && s.type == TYPE_REALTIME) {
        s.play_enabled = true;
    }
    xSemaphoreGiveRecursive(s.lock);
}

void session_setrateanchortime(ap2_conn_t *conn, const rtsp_req_t *req, rtsp_resp_t *resp) {
    (void)resp;
    bplist_t pl;
    if (!rtsp_req_plist(req, &pl)) {
        return;
    }
    bp_obj root = bplist_root(&pl);
    xSemaphoreTakeRecursive(s.lock, portMAX_DELAY);
    if (conn != s.principal) {
        xSemaphoreGiveRecursive(s.lock);
        return;
    }

    uint64_t secs, frac = 0, clock = 0, rtp = 0;
    if (bplist_dict_uint(&pl, root, "networkTimeSecs", &secs)) {
        bplist_dict_uint(&pl, root, "networkTimeFrac", &frac);
        bplist_dict_uint(&pl, root, "networkTimeTimelineID", &clock);
        bplist_dict_uint(&pl, root, "rtpTime", &rtp);
        // frac is a binary fraction of a second; keep ~0.25 ns of it.
        int64_t ns = (int64_t)secs * 1000000000LL + (int64_t)(((frac >> 32) * 1000000000ULL) >> 32);
        session_set_anchor(clock, (uint32_t)rtp, ns);
    }

    uint64_t rate;
    if (bplist_dict_uint(&pl, root, "rate", &rate)) {
        if (rate & 1) {
            ESP_LOGI(TAG, "play");
            ptp_play_begin();
            s.play_enabled = true;
        } else {
            ESP_LOGI(TAG, "pause");
            s.play_enabled = false;
            session_clear_anchor();
            flush_output();
        }
    }
    xSemaphoreGiveRecursive(s.lock);
}

void session_flushbuffered(ap2_conn_t *conn, const rtsp_req_t *req, rtsp_resp_t *resp) {
    (void)resp;
    bplist_t pl;
    if (!rtsp_req_plist(req, &pl)) {
        return;
    }
    bp_obj root = bplist_root(&pl);
    uint64_t from_seq = 0, until_seq = 0;
    bool from_valid = bplist_dict_uint(&pl, root, "flushFromSeq", &from_seq);
    bplist_dict_uint(&pl, root, "flushUntilSeq", &until_seq);

    xSemaphoreTakeRecursive(s.lock, portMAX_DELAY);
    if (conn == s.principal && s.buffered) {
        buffered_flush(s.buffered, from_valid, (uint32_t)from_seq, (uint32_t)until_seq);
        if (!from_valid) {
            // Stop the clock until SETRATEANCHORTIME restarts it.
            s.play_enabled = false;
            session_clear_anchor();
            flush_output();
        }
    }
    xSemaphoreGiveRecursive(s.lock);
}

void session_flush(ap2_conn_t *conn, const rtsp_req_t *req, rtsp_resp_t *resp) {
    (void)req;
    (void)resp;
    xSemaphoreTakeRecursive(s.lock, portMAX_DELAY);
    if (conn == s.principal) {
        flush_output();
    }
    xSemaphoreGiveRecursive(s.lock);
}

void session_teardown(ap2_conn_t *conn, const rtsp_req_t *req, rtsp_resp_t *resp) {
    bplist_t pl;
    bool streams_only =
        rtsp_req_plist(req, &pl) && bplist_dict_get(&pl, bplist_root(&pl), "streams") != BP_NONE;

    xSemaphoreTakeRecursive(s.lock, portMAX_DELAY);
    if (conn == s.principal) {
        if (streams_only) {
            stop_streams();
        } else {
            end_principal();
        }
    }
    xSemaphoreGiveRecursive(s.lock);

    if (!streams_only) {
        rtsp_resp_header(resp, "Connection", "close");
        conn->stop = true;
    }
}

void session_conn_closed(ap2_conn_t *conn) {
    xSemaphoreTakeRecursive(s.lock, portMAX_DELAY);
    if (conn == s.principal) {
        end_principal();
    }
    xSemaphoreGiveRecursive(s.lock);
}

/* AirPlay 1 */

bool session_ap1_setup(ap2_conn_t *conn, uint16_t control_port, uint16_t timing_port, uint16_t ports[3]) {
    struct in_addr host;
    if (!inet_aton(conn->client_ip, &host)) {
        return false;
    }
    xSemaphoreTakeRecursive(s.lock, portMAX_DELAY);
    become_principal(conn);
    stop_streams();
    raop_rtp_resp_t r = raop_rtp_init(host, conn->ap1.encrypted ? conn->ap1.aeskey : NULL,
                                      conn->ap1.encrypted ? conn->ap1.aesiv : NULL, conn->ap1.rtpmap, conn->ap1.fmtp,
                                      control_port,
                                      timing_port, ap2_event_cb, ap2_push_pcm);
    s.ap1_rtp = r.ctx;
    xSemaphoreGiveRecursive(s.lock);
    if (!r.ctx) {
        return false;
    }
    ports[0] = r.cport;
    ports[1] = r.tport;
    ports[2] = r.aport;
    ESP_LOGI(TAG, "conn %d: AirPlay 1 stream%s", conn->id, conn->ap1.encrypted ? ", encrypted" : "");
    return true;
}

void session_ap1_record(ap2_conn_t *conn, uint16_t seqno, uint32_t rtptime) {
    xSemaphoreTakeRecursive(s.lock, portMAX_DELAY);
    if (conn == s.principal && s.ap1_rtp) {
        raop_rtp_record(s.ap1_rtp, seqno, rtptime);
    }
    xSemaphoreGiveRecursive(s.lock);
}

bool session_ap1_flush(ap2_conn_t *conn, uint16_t seqno, uint32_t rtptime) {
    xSemaphoreTakeRecursive(s.lock, portMAX_DELAY);
    bool playing = conn == s.principal && s.ap1_rtp;
    if (playing) {
        raop_rtp_flush(s.ap1_rtp, seqno, rtptime);
    }
    xSemaphoreGiveRecursive(s.lock);
    return playing;
}

void session_ap1_teardown(ap2_conn_t *conn) {
    xSemaphoreTakeRecursive(s.lock, portMAX_DELAY);
    if (conn == s.principal) {
        end_principal();
    }
    xSemaphoreGiveRecursive(s.lock);
}

bool session_is_airplay1(void) {
    xSemaphoreTakeRecursive(s.lock, portMAX_DELAY);
    bool ap1 = s.principal && s.principal->airplay1;
    xSemaphoreGiveRecursive(s.lock);
    return ap1;
}

bool session_feedback(bpw_t *w, bpw_obj *root) {
    xSemaphoreTakeRecursive(s.lock, portMAX_DELAY);
    int type = s.type;
    xSemaphoreGiveRecursive(s.lock);
    if (!type) {
        return false;
    }
    bpw_obj stream = bpw_dict(w);
    bpw_dict_set(w, stream, "type", bpw_uint(w, (uint64_t)type));
    bpw_dict_set(w, stream, "sr", bpw_real(w, AP2_RATE));
    bpw_obj streams = bpw_array(w);
    bpw_array_add(w, streams, stream);
    *root = bpw_dict(w);
    bpw_dict_set(w, *root, "streams", streams);
    return true;
}

/* Remote control */

static void random_uuid(char out[37]) {
    uint8_t b[16];
    esp_fill_random(b, sizeof(b));
    b[6] = (b[6] & 0x0F) | 0x40;
    b[8] = (b[8] & 0x3F) | 0x80;
    snprintf(out, 37, "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X", b[0], b[1], b[2], b[3],
             b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

// An NSKeyedArchiver-encoded NSMutableArray holding the group UUID, as
// shairport-sync's destinationArchive() builds it.
static bpw_obj destination_archive(bpw_t *w, const char *group_uuid) {
    bpw_t *a = bpw_new();
    if (!a) {
        return -1;
    }
    bpw_obj root = bpw_dict(a);
    bpw_dict_set(a, root, "$version", bpw_uint(a, 100000));
    bpw_dict_set(a, root, "$archiver", bpw_string(a, "NSKeyedArchiver"));
    bpw_obj top_uid = bpw_dict(a);
    bpw_dict_set(a, top_uid, "CF$UID", bpw_uint(a, 1));
    bpw_obj top = bpw_dict(a);
    bpw_dict_set(a, top, "root", top_uid);
    bpw_dict_set(a, root, "$top", top);

    bpw_obj objects = bpw_array(a);
    bpw_array_add(a, objects, bpw_string(a, "$null"));
    bpw_obj elem_uid = bpw_dict(a);
    bpw_dict_set(a, elem_uid, "CF$UID", bpw_uint(a, 2));
    bpw_obj ns_objects = bpw_array(a);
    bpw_array_add(a, ns_objects, elem_uid);
    bpw_obj class_uid = bpw_dict(a);
    bpw_dict_set(a, class_uid, "CF$UID", bpw_uint(a, 3));
    bpw_obj array_dict = bpw_dict(a);
    bpw_dict_set(a, array_dict, "NS.objects", ns_objects);
    bpw_dict_set(a, array_dict, "$class", class_uid);
    bpw_array_add(a, objects, array_dict);
    bpw_array_add(a, objects, bpw_string(a, group_uuid));
    bpw_obj classes = bpw_array(a);
    bpw_array_add(a, classes, bpw_string(a, "NSMutableArray"));
    bpw_array_add(a, classes, bpw_string(a, "NSArray"));
    bpw_array_add(a, classes, bpw_string(a, "NSObject"));
    bpw_obj classdef = bpw_dict(a);
    bpw_dict_set(a, classdef, "$classname", bpw_string(a, "NSMutableArray"));
    bpw_dict_set(a, classdef, "$classes", classes);
    bpw_array_add(a, objects, classdef);
    bpw_dict_set(a, root, "$objects", objects);

    size_t len;
    uint8_t *bin = bpw_serialize(a, root, &len);
    bpw_free(a);
    if (!bin) {
        return -1;
    }
    bpw_obj data = bpw_data(w, bin, len);
    free(bin);
    return data;
}

bool session_media_command(mr_command_t command) {
    bool ok = false;
    xSemaphoreTakeRecursive(s.lock, portMAX_DELAY);
    ap2_conn_t *conn = s.principal;
    if (conn && conn->events) {
        bpw_t *w = bpw_new();
        if (w) {
            char number[8], uuid[37];
            snprintf(number, sizeof(number), "%d", (int)command);
            bpw_obj d = bpw_dict(w);
            bpw_dict_set(w, d, "type", bpw_string(w, "sendMediaRemoteCommand"));
            bpw_dict_set(w, d, "modernMediaRemoteCommand", bpw_string(w, number));
            if (conn->group_uuid[0]) {
                bpw_obj params = bpw_dict(w);
                random_uuid(uuid);
                bpw_dict_set(w, params, "kMRMediaRemoteOptionSendOptionsNumber", bpw_uint(w, 0));
                bpw_dict_set(w, params, "kMRMediaRemoteOptionCommandID", bpw_string(w, uuid));
                bpw_dict_set(w, params, "kMRMediaRemoteOptionIsRedirectingCommand", bpw_bool(w, true));
                bpw_dict_set(w, params, "kMRMediaRemoteOptionDestinationDeviceUIDs",
                             destination_archive(w, conn->group_uuid));
                bpw_dict_set(w, d, "params", params);
            }
            ok = event_post_command(conn->events, w, d);
            bpw_free(w);
        }
    }
    xSemaphoreGiveRecursive(s.lock);
    ESP_LOGI(TAG, "MediaRemote command %d %s", (int)command, ok ? "sent" : "failed");
    return ok;
}

// shairport-sync's ap2_event_send_unit_volume_notification(): "dvlc", the
// device volume, with the unit volume both at the top level and in params.
bool session_volume(float volume) {
    bool ok = false;
    xSemaphoreTakeRecursive(s.lock, portMAX_DELAY);
    ap2_conn_t *conn = s.principal;
    if (conn && conn->events) {
        bpw_t *w = bpw_new();
        if (w) {
            bpw_obj params = bpw_dict(w);
            bpw_dict_set(w, params, "volume", bpw_real(w, volume));
            bpw_obj d = bpw_dict(w);
            bpw_dict_set(w, d, "type", bpw_string(w, "sendMediaRemoteCommand"));
            bpw_dict_set(w, d, "value", bpw_string(w, "dvlc"));
            bpw_dict_set(w, d, "volume", bpw_real(w, volume));
            bpw_dict_set(w, d, "params", params);
            ok = event_post_command(conn->events, w, d);
            bpw_free(w);
        }
    }
    xSemaphoreGiveRecursive(s.lock);
    if (!ok) {
        ESP_LOGW(TAG, "volume %.3f not sent", volume);
    }
    return ok;
}

#endif  // !AK_NATIVE
