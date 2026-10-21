#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#ifndef TALK_RTSP_PORT
#define TALK_RTSP_PORT 42424
#endif
#define TALK_RTP_PORT 42425

typedef struct {
    bool (*start)(uint32_t ip, uint16_t port, uint16_t sequence, const char *name);
    void (*stop)(void);
    bool (*active)(void);
    // Fire-and-forget message/ack requests; body is UTF-8 JSON.
    bool (*message)(uint32_t ip, const char *name, const char *path, const char *type, const void *body, size_t length);
} talk_rtsp_ops_t;
void talk_rtsp_init(const talk_rtsp_ops_t *ops);
// Connect / ANNOUNCE / SETUP / RECORD. Caller closes via end on failure too.
// Returns control socket, -1 on failure. Media peer port is negotiated.
int talk_rtsp_begin(uint32_t ip, uint16_t sequence, uint32_t timestamp, uint16_t *peer_port);
void talk_rtsp_end(int sock);
bool talk_rtsp_alive(int sock);
bool talk_rtsp_message(uint32_t ip, const char *path, const char *json);

bool talk_rtsp_message_bytes(uint32_t ip, const char *path, const char *type, const void *data, size_t length);
