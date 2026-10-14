#pragma once

/*
 * The playing session: at most one connection (the principal) streams at a
 * time, and a new stream SETUP takes over from the old one, as in
 * shairport-sync's play lock with interruption allowed.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rtsp.h"

typedef enum {
    MR_PLAY = 0,
    MR_PAUSE = 1,
    MR_TOGGLE_PLAY_PAUSE = 2,
    MR_STOP = 3,
    MR_NEXT_TRACK = 4,
    MR_PREVIOUS_TRACK = 5,
} mr_command_t;

void session_init(void);

void session_setup(ap2_conn_t *conn, const rtsp_req_t *req, rtsp_resp_t *resp);
void session_record(ap2_conn_t *conn, const rtsp_req_t *req, rtsp_resp_t *resp);
void session_setrateanchortime(ap2_conn_t *conn, const rtsp_req_t *req, rtsp_resp_t *resp);
void session_flushbuffered(ap2_conn_t *conn, const rtsp_req_t *req, rtsp_resp_t *resp);
void session_flush(ap2_conn_t *conn, const rtsp_req_t *req, rtsp_resp_t *resp);
void session_teardown(ap2_conn_t *conn, const rtsp_req_t *req, rtsp_resp_t *resp);
void session_conn_closed(ap2_conn_t *conn);

// AirPlay 1: SETUP takes the session over and starts the RTP receiver,
// filling ports with its control, timing and data ports.
bool session_ap1_setup(ap2_conn_t *conn, uint16_t control_port, uint16_t timing_port, uint16_t ports[3]);
void session_ap1_record(ap2_conn_t *conn, uint16_t seqno, uint32_t rtptime);
// False if conn isn't the one playing.
bool session_ap1_flush(ap2_conn_t *conn, uint16_t seqno, uint32_t rtptime);
void session_ap1_teardown(ap2_conn_t *conn);
// Whether the playing sender is AirPlay 1 (remote control goes over DACP).
bool session_is_airplay1(void);

// POST /feedback's stream list; false when nothing is streaming.
bool session_feedback(bpw_t *w, bpw_obj *root);

// Asks the principal sender to act (MediaRemote over its event channel).
bool session_media_command(mr_command_t command);

// The esp_timer time (us) at which this RTP frame should reach the DAC.
bool session_playtime_us(uint32_t rtp, int64_t *playtime_us);

// Audio producers read the epoch before timing a packet and hand it back
// with the PCM; audio from before a flush or pause is then dropped.
uint32_t session_epoch(void);
void session_pcm(const int16_t *pcm, size_t frames, int64_t playtime_us, uint32_t epoch);
void session_set_anchor(uint64_t clock_id, uint32_t rtp, int64_t master_ns);
void session_clear_anchor(void);
bool session_play_enabled(void);
