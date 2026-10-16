#pragma once

/*
 * Buffered audio (stream type 103): encrypted AAC blocks the sender pushes
 * over TCP well ahead of time, decoded as each nears its playtime.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct buffered buffered_t;

// Listens for the sender's audio connection; *port and *buffer_size go
// in the SETUP reply.
buffered_t *buffered_start(const uint8_t key[32], uint16_t *port, size_t *buffer_size);
void buffered_stop(buffered_t *b);

// FLUSHBUFFERED: with from_valid, drop blocks from_seq..until_seq when they
// come up; otherwise drop everything up to until_seq now.
void buffered_flush(buffered_t *b, bool from_valid, uint32_t from_seq, uint32_t until_seq);
