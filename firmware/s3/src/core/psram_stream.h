#pragma once

#include <stdbool.h>
#include <stdlib.h>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"

// A stream buffer in PSRAM. Not xStreamBufferCreateWithCaps(): IDF 5.5's
// vStreamBufferDeleteWithCaps() deletes it as a queue (vSemaphoreDelete())
// and then frees its memory again, a double free.

typedef struct {
    StreamBufferHandle_t handle;
    StaticStreamBuffer_t *control;
    uint8_t *storage;
} psram_stream_t;

static inline bool psram_stream_create(psram_stream_t *s, size_t bytes, size_t trigger) {
    s->control = heap_caps_calloc(1, sizeof(*s->control), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    // Static storage needs one byte past the capacity.
    s->storage = heap_caps_malloc(bytes + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s->handle = s->control && s->storage ? xStreamBufferCreateStatic(bytes, trigger, s->storage, s->control) : NULL;
    return s->handle != NULL;
}

static inline void psram_stream_delete(psram_stream_t *s) {
    if (s->handle) {
        vStreamBufferDelete(s->handle);
    }
    free(s->control);
    free(s->storage);
    *s = (psram_stream_t){0};
}
