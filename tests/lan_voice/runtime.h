#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <string.h>
#define ESP_OK 0
#define ESP_ERR_NO_MEM 1
#define ESP_ERROR_CHECK(x) assert((x) == ESP_OK)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define heap_caps_malloc(n, c) malloc(n)
#define heap_caps_calloc(n, s, c) calloc(n, s)
#define pdPASS 1
#define pdTRUE 1
#define pdFALSE 0
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(ms) (ms)
typedef int BaseType_t;
typedef pthread_mutex_t portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
#define taskENTER_CRITICAL(p) pthread_mutex_lock(p)
#define taskEXIT_CRITICAL(p) pthread_mutex_unlock(p)
typedef void (*TaskFunction_t)(void *);
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    unsigned value, limit;
} *SemaphoreHandle_t;
static inline SemaphoreHandle_t xSemaphoreCreateCounting(unsigned limit, unsigned value) {
    SemaphoreHandle_t s = calloc(1, sizeof(*s));
    assert(s);
    pthread_mutex_init(&s->mutex, NULL);
    pthread_cond_init(&s->cond, NULL);
    s->limit = limit;
    s->value = value;
    return s;
}
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void) {
    return xSemaphoreCreateCounting(1, 1);
}
static inline int xSemaphoreTake(SemaphoreHandle_t s, uint32_t ms) {
    pthread_mutex_lock(&s->mutex);
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += ms / 1000;
    until.tv_nsec += (ms % 1000) * 1000000LL;
    if (until.tv_nsec >= 1000000000) {
        until.tv_sec++;
        until.tv_nsec -= 1000000000;
    }
    while (!s->value) {
        if (!ms) {
            pthread_mutex_unlock(&s->mutex);
            return 0;
        }
        int rc = ms == portMAX_DELAY ? pthread_cond_wait(&s->cond, &s->mutex)
                                     : pthread_cond_timedwait(&s->cond, &s->mutex, &until);
        if (rc == ETIMEDOUT) {
            pthread_mutex_unlock(&s->mutex);
            return 0;
        }
    }
    s->value--;
    pthread_mutex_unlock(&s->mutex);
    return 1;
}
static inline void xSemaphoreGive(SemaphoreHandle_t s) {
    pthread_mutex_lock(&s->mutex);
    assert(s->value < s->limit);
    s->value++;
    pthread_cond_signal(&s->cond);
    pthread_mutex_unlock(&s->mutex);
}
static inline void vTaskDelay(uint32_t ms) {
    usleep(ms * 1000);
}
static inline void vTaskDelete(void *unused) {
    (void)unused;
    pthread_exit(NULL);
}
typedef struct {
    TaskFunction_t f;
    void *arg;
} thread_arg_t;
static void *thread_main(void *arg) {
    thread_arg_t task = *(thread_arg_t *)arg;
    free(arg);
    task.f(task.arg);
    return NULL;
}
static inline int xTaskCreate(TaskFunction_t f, const char *name, unsigned stack, void *arg, unsigned priority,
                              void *out) {
#ifdef HOST_NO_TASKS
    (void)f;
    (void)name;
    (void)stack;
    (void)arg;
    (void)priority;
    (void)out;
    return 1;
#else
    (void)name;
    (void)stack;
    (void)priority;
    (void)out;
    pthread_t thread;
    thread_arg_t *task = malloc(sizeof(*task));
    *task = (thread_arg_t){f, arg};
    int rc = pthread_create(&thread, NULL, thread_main, task);
    if (rc) {
        free(task);
        return 0;
    }
    pthread_detach(thread);
    return 1;
#endif
}
uint32_t esp_random(void);
typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    size_t capacity, item_size, count, head;
    uint8_t *data;
} *QueueHandle_t;
static inline QueueHandle_t xQueueCreate(size_t count, size_t item_size) {
    QueueHandle_t q = calloc(1, sizeof(*q));
    assert(q);
    q->capacity = count;
    q->item_size = item_size;
    q->data = calloc(count, item_size);
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->cond, NULL);
    return q;
}
static inline int xQueueSend(QueueHandle_t q, const void *item, uint32_t ms) {
    (void)ms;
    pthread_mutex_lock(&q->mutex);
    if (q->count == q->capacity) {
        pthread_mutex_unlock(&q->mutex);
        return 0;
    }
    memcpy(q->data + ((q->head + q->count) % q->capacity) * q->item_size, item, q->item_size);
    q->count++;
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->mutex);
    return 1;
}
static inline int xQueueReceive(QueueHandle_t q, void *item, uint32_t ms) {
    pthread_mutex_lock(&q->mutex);
    while (!q->count) {
        if (!ms) {
            pthread_mutex_unlock(&q->mutex);
            return 0;
        }
        pthread_cond_wait(&q->cond, &q->mutex);
    }
    memcpy(item, q->data + q->head * q->item_size, q->item_size);
    q->head = (q->head + 1) % q->capacity;
    q->count--;
    pthread_mutex_unlock(&q->mutex);
    return 1;
}

#define xTaskCreateWithCaps(f, n, s, a, p, o, c) xTaskCreate(f, n, s, a, p, o)

#define vTaskDeleteWithCaps(p) vTaskDelete(p)

#define xTaskCreatePinnedToCoreWithCaps(f,n,s,a,p,o,core,c) xTaskCreate(f,n,s,a,p,o)
