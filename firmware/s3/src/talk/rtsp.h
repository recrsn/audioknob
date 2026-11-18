#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "ui/ui.h"
#ifndef TALK_RTSP_PORT
#define TALK_RTSP_PORT 42424
#endif
#define TALK_RTP_PORT 42425
// How long a call rings before the callee gives up on it.
#define TALK_RING_MS 30000

typedef enum { TALK_ANSWER_PENDING, TALK_ANSWER_ACCEPTED, TALK_ANSWER_DECLINED } talk_answer_t;

// The callee's side, called from the control connection's task. A call is
// identified by the nonzero id ring() returns, so a stale connection can't
// touch a later call.
typedef struct {
    // A RECORD arrived: ring, or return 0 when busy. key is the caller's.
    uint32_t (*ring)(uint32_t ip, uint16_t port, uint16_t sequence, const char *key, const char *name);
    // Polled while ringing; on ACCEPTED, our stream's first sequence and timestamp.
    talk_answer_t (*answer)(uint32_t call, uint16_t *sequence, uint32_t *timestamp);
    // The caller hung up, or the ring timed out.
    void (*stop)(uint32_t call);
    // Whether our side still has the call up.
    bool (*active)(uint32_t call);
    // A voice note arrived (takes ogg; returns the reply's status), or word
    // that one of ours was played.
    int (*note)(const char *key, const char *name, const char *id, const uint8_t wave[UI_NOTE_WAVE], uint8_t *ogg,
                size_t len);
    void (*played)(const char *key, const char *id);
} talk_rtsp_ops_t;
// key: this knob's, sent with every request.
void talk_rtsp_init(const talk_rtsp_ops_t *ops, const char *key);

typedef enum {
    TALK_DIAL_RINGING,
    TALK_DIAL_ANSWERED,
    TALK_DIAL_BUSY,
    TALK_DIAL_DECLINED,
    TALK_DIAL_NO_ANSWER,
    TALK_DIAL_FAILED,
} talk_dial_t;

// Connect, ANNOUNCE, SETUP, then send RECORD, which the callee answers only
// once its user does. Returns the control socket, -1 on failure; the
// callee's media port lands in *peer_port.
int talk_rtsp_dial(uint32_t ip, uint16_t sequence, uint32_t timestamp, uint16_t *peer_port);
// Nonblocking: RECORD's reply, if it has come. On ANSWERED, *sequence is the
// callee's first RTP sequence.
talk_dial_t talk_rtsp_answer(int sock, uint16_t *sequence);
// Closing the connection hangs up, whichever side does it.
void talk_rtsp_end(int sock);
bool talk_rtsp_alive(int sock);

// One request on a connection of its own: SET_PARAMETER /note with the Ogg
// Opus note as the body, or /note/played with none. Returns the reply's
// status, -1 when the knob can't be reached. Blocks until the other knob
// has saved it.
int talk_rtsp_send_note(uint32_t ip, const char *id, const uint8_t wave[UI_NOTE_WAVE], const uint8_t *ogg, size_t len);
int talk_rtsp_send_played(uint32_t ip, const char *id);
