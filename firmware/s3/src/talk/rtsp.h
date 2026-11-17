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
} talk_rtsp_ops_t;
void talk_rtsp_init(const talk_rtsp_ops_t *ops);
// Connect / ANNOUNCE / SETUP / RECORD. Caller closes via end on failure too.
// Returns control socket, -1 on failure. Media peer port is negotiated.
int talk_rtsp_begin(uint32_t ip, uint16_t sequence, uint32_t timestamp, uint16_t *peer_port);
void talk_rtsp_end(int sock);
bool talk_rtsp_alive(int sock);
