#include "radio/stream/ring.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "freertos/semphr.h"

struct stream_ring {
    uint8_t *buf;
    size_t size;
    size_t head; // next read
    size_t count;
    uint32_t epoch;
    uint32_t format;
    SemaphoreHandle_t lock;
    SemaphoreHandle_t data;  // given when bytes arrive
    SemaphoreHandle_t space; // given when bytes leave
};

stream_ring_t *stream_ring_create(size_t size) {
    stream_ring_t *ring = heap_caps_calloc(1, sizeof(*ring), MALLOC_CAP_SPIRAM);
    if (!ring) {
        return NULL;
    }
    ring->buf = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    ring->size = size;
    ring->lock = xSemaphoreCreateMutex();
    ring->data = xSemaphoreCreateBinary();
    ring->space = xSemaphoreCreateBinary();
    if (!ring->buf || !ring->lock || !ring->data || !ring->space) {
        stream_ring_destroy(ring);
        return NULL;
    }
    return ring;
}

void stream_ring_destroy(stream_ring_t *ring) {
    if (!ring) {
        return;
    }
    if (ring->lock) {
        vSemaphoreDelete(ring->lock);
    }
    if (ring->data) {
        vSemaphoreDelete(ring->data);
    }
    if (ring->space) {
        vSemaphoreDelete(ring->space);
    }
    heap_caps_free(ring->buf);
    heap_caps_free(ring);
}

uint32_t stream_ring_reset(stream_ring_t *ring, uint32_t format) {
    xSemaphoreTake(ring->lock, portMAX_DELAY);
    ring->head = 0;
    ring->count = 0;
    uint32_t epoch = ++ring->epoch;
    ring->format = format;
    xSemaphoreGive(ring->lock);
    stream_ring_wake(ring);
    return epoch;
}

size_t stream_ring_write(stream_ring_t *ring, const uint8_t *data, size_t len, TickType_t wait) {
    size_t done = 0;
    TickType_t start = xTaskGetTickCount();
    while (done < len) {
        xSemaphoreTake(ring->lock, portMAX_DELAY);
        size_t room = ring->size - ring->count;
        size_t n = len - done < room ? len - done : room;
        size_t tail = (ring->head + ring->count) % ring->size;
        size_t first = n < ring->size - tail ? n : ring->size - tail;
        memcpy(ring->buf + tail, data + done, first);
        memcpy(ring->buf, data + done + first, n - first);
        ring->count += n;
        xSemaphoreGive(ring->lock);

        if (n) {
            done += n;
            xSemaphoreGive(ring->data);
            continue;
        }
        TickType_t elapsed = xTaskGetTickCount() - start;
        if (elapsed >= wait || xSemaphoreTake(ring->space, wait - elapsed) != pdTRUE) {
            break;
        }
    }
    return done;
}

size_t stream_ring_read(stream_ring_t *ring, uint8_t *out, size_t max, TickType_t wait, uint32_t *epoch,
                        uint32_t *format) {
    for (int attempt = 0; attempt < 2; attempt++) {
        xSemaphoreTake(ring->lock, portMAX_DELAY);
        size_t n = max < ring->count ? max : ring->count;
        size_t first = n < ring->size - ring->head ? n : ring->size - ring->head;
        memcpy(out, ring->buf + ring->head, first);
        memcpy(out + first, ring->buf, n - first);
        ring->head = (ring->head + n) % ring->size;
        ring->count -= n;
        *epoch = ring->epoch;
        *format = ring->format;
        xSemaphoreGive(ring->lock);

        if (n) {
            xSemaphoreGive(ring->space);
            return n;
        }
        if (attempt == 0 && xSemaphoreTake(ring->data, wait) != pdTRUE) {
            break;
        }
    }
    return 0;
}

size_t stream_ring_level(stream_ring_t *ring) {
    xSemaphoreTake(ring->lock, portMAX_DELAY);
    size_t level = ring->count;
    xSemaphoreGive(ring->lock);
    return level;
}

size_t stream_ring_size(const stream_ring_t *ring) {
    return ring->size;
}

void stream_ring_wake(stream_ring_t *ring) {
    xSemaphoreGive(ring->data);
    xSemaphoreGive(ring->space);
}
