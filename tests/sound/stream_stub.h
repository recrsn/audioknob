#pragma once
// A pthread stream buffer, enough for psram_stream.h: bytes in, bytes out,
// Send waiting up to its timeout for room.
#include "runtime.h"

typedef struct {
    pthread_mutex_t lock;
    size_t size, head, used;
    uint8_t *data;
} StaticStreamBuffer_t;
typedef StaticStreamBuffer_t *StreamBufferHandle_t;

static StreamBufferHandle_t xStreamBufferCreateStatic(size_t size, size_t trigger, uint8_t *storage,
                                                      StaticStreamBuffer_t *control) {
    (void)trigger;
    pthread_mutex_init(&control->lock, NULL);
    control->size = size;
    control->data = storage;
    return control;
}
static void vStreamBufferDelete(StreamBufferHandle_t s) { pthread_mutex_destroy(&s->lock); }
static size_t stream_move(StreamBufferHandle_t s, uint8_t *out, const uint8_t *in, size_t n) {
    pthread_mutex_lock(&s->lock);
    size_t moved = 0;
    if (in) {
        for (; moved < n && s->used < s->size; moved++, s->used++)
            s->data[(s->head + s->used) % s->size] = in[moved];
    } else {
        for (; moved < n && s->used; moved++, s->used--, s->head = (s->head + 1) % s->size)
            out[moved] = s->data[s->head];
    }
    pthread_mutex_unlock(&s->lock);
    return moved;
}
static size_t xStreamBufferSend(StreamBufferHandle_t s, const void *data, size_t n, uint32_t wait_ms) {
    size_t sent = stream_move(s, NULL, data, n);
    for (uint32_t waited = 0; sent < n && waited < wait_ms; waited++) {
        usleep(1000);
        sent += stream_move(s, NULL, (const uint8_t *)data + sent, n - sent);
    }
    return sent;
}
static size_t xStreamBufferReceive(StreamBufferHandle_t s, void *out, size_t n, uint32_t wait) {
    (void)wait;
    return stream_move(s, out, NULL, n);
}
static size_t xStreamBufferBytesAvailable(StreamBufferHandle_t s) {
    pthread_mutex_lock(&s->lock);
    size_t used = s->used;
    pthread_mutex_unlock(&s->lock);
    return used;
}
