#pragma once
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#define ESP_OK 0
#define ESP_ERR_NO_MEM 1
#define ESP_ERROR_CHECK(x) assert((x) == ESP_OK)
#define ESP_LOGI(tag, ...)                                                     \
  do {                                                                         \
    (void)(tag);                                                               \
    if (0)                                                                     \
      fprintf(stderr, __VA_ARGS__);                                            \
  } while (0)
#define ESP_LOGW(tag, ...)                                                     \
  do {                                                                         \
    (void)(tag);                                                               \
    if (0)                                                                     \
      fprintf(stderr, __VA_ARGS__);                                            \
  } while (0)
#define ESP_LOGE(tag, ...)                                                     \
  do {                                                                         \
    (void)(tag);                                                               \
    if (0)                                                                     \
      fprintf(stderr, __VA_ARGS__);                                            \
  } while (0)
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_INTERNAL 4
#define heap_caps_malloc(n, c) malloc(n)
#define heap_caps_calloc(n, s, c) calloc(n, s)
#define heap_caps_realloc(p, n, c) realloc(p, n)
#define heap_caps_free free
#define heap_caps_get_free_size(c) 100000
#define heap_caps_get_largest_free_block(c) 100000
#define pdPASS 1
#define pdTRUE 1
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(ms) (ms)
typedef int esp_err_t;
typedef uint32_t TickType_t;
typedef int BaseType_t;
static int64_t esp_timer_get_time(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}
static TickType_t xTaskGetTickCount(void) {
  return (TickType_t)(esp_timer_get_time() / 1000);
}
static void vTaskDelay(uint32_t ms) { usleep(ms * 1000); }
typedef struct {
  pthread_mutex_t m;
  pthread_cond_t c;
  unsigned value;
} *SemaphoreHandle_t;
static SemaphoreHandle_t xSemaphoreCreateBinary(void) {
  SemaphoreHandle_t s = calloc(1, sizeof(*s));
  assert(s);
  pthread_mutex_init(&s->m, NULL);
  pthread_cond_init(&s->c, NULL);
  return s;
}
static SemaphoreHandle_t xSemaphoreCreateMutex(void) {
  SemaphoreHandle_t s = xSemaphoreCreateBinary();
  s->value = 1;
  return s;
}
static int xSemaphoreTake(SemaphoreHandle_t s, uint32_t ms) {
  pthread_mutex_lock(&s->m);
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
      pthread_mutex_unlock(&s->m);
      return 0;
    }
    int rc = ms == portMAX_DELAY ? pthread_cond_wait(&s->c, &s->m)
                                 : pthread_cond_timedwait(&s->c, &s->m, &until);
    if (rc == ETIMEDOUT) {
      pthread_mutex_unlock(&s->m);
      return 0;
    }
    assert(!rc);
  }
  s->value = 0;
  pthread_mutex_unlock(&s->m);
  return 1;
}
static void xSemaphoreGive(SemaphoreHandle_t s) {
  pthread_mutex_lock(&s->m);
  s->value = 1;
  pthread_cond_signal(&s->c);
  pthread_mutex_unlock(&s->m);
}
static void vSemaphoreDelete(SemaphoreHandle_t s) {
  pthread_cond_destroy(&s->c);
  pthread_mutex_destroy(&s->m);
  free(s);
}
typedef struct task {
  void (*fn)(void *);
  void *arg;
  SemaphoreHandle_t notify;
  bool net;
} *TaskHandle_t;
static _Thread_local TaskHandle_t current_task;
static atomic_int tasks_live, codecs_live, writes, net_runs, network_held;
static atomic_bool hold_network, pause_net_start, pause_codec, codec_paused,
    fail_decoder_task;
static void vTaskDeleteWithCaps(void *unused) {
  (void)unused;
  TaskHandle_t t = current_task;
  vSemaphoreDelete(t->notify);
  free(t);
  atomic_fetch_sub(&tasks_live, 1);
  pthread_exit(NULL);
}
static void *task_start(void *arg) {
  current_task = arg;
  while (current_task->net && atomic_load(&pause_net_start))
    usleep(100);
  current_task->fn(current_task->arg);
  abort();
}
static int xTaskCreatePinnedToCoreWithCaps(void (*fn)(void *), const char *name,
                                           unsigned stack, void *arg,
                                           unsigned prio, TaskHandle_t *out,
                                           int core, int caps) {
  (void)stack;
  (void)prio;
  (void)core;
  (void)caps;
  bool net = !strcmp(name, "radio_net");
  if (!net && atomic_exchange(&fail_decoder_task, false))
    return 0;
  TaskHandle_t t = calloc(1, sizeof(*t));
  *t = (struct task){
      .fn = fn, .arg = arg, .notify = xSemaphoreCreateBinary(), .net = net};
  if (out)
    *out = t;
  atomic_fetch_add(&tasks_live, 1);
  pthread_t thread;
  assert(!pthread_create(&thread, NULL, task_start, t));
  pthread_detach(thread);
  return 1;
}
static void xTaskNotifyGive(TaskHandle_t t) { xSemaphoreGive(t->notify); }
static unsigned ulTaskNotifyTake(int clear, uint32_t ms) {
  (void)clear;
  return xSemaphoreTake(current_task->notify, ms);
}
