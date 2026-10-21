#pragma once
// Reuse the pthread semaphore/FreeRTOS scaffolding from the LAN host checks.
#define vTaskDelete lan_vTaskDelete
#define vTaskDelay lan_vTaskDelay
#include "../lan_voice/runtime.h"
#undef vTaskDelete
#undef vTaskDelay
#include <stdatomic.h>
typedef void *TaskHandle_t;
static atomic_int live_tasks, created_tasks, deleted_tasks;
static atomic_bool fail_create, fail_alloc, pause_start, pause_delete;
static atomic_int at_delete, at_period;
static atomic_bool pause_period;
static void vTaskDelay(uint32_t ms) {
    atomic_fetch_add(&at_period, 1);
    while (atomic_load(&pause_period)) usleep(100);
    atomic_fetch_sub(&at_period, 1);
    usleep(ms * 25 + 25);
}
static void vTaskDelete(void *arg) {
    (void)arg;
    atomic_fetch_add(&at_delete, 1);
    while (atomic_load(&pause_delete)) usleep(100);
    atomic_fetch_sub(&at_delete, 1);
    atomic_fetch_add(&deleted_tasks, 1);
    atomic_fetch_sub(&live_tasks, 1);
    pthread_exit(NULL);
}
static void *sound_thread(void *arg) {
    thread_arg_t task = *(thread_arg_t *)arg;
    free(arg);
    while (atomic_load(&pause_start)) usleep(100);
    task.f(task.arg);
    assert(!"worker must delete itself");
    return NULL;
}
static int xTaskCreatePinnedToCore(TaskFunction_t f, const char *name, unsigned stack,
                                  void *arg, unsigned prio, TaskHandle_t *out, int core) {
    (void)name; (void)stack; (void)prio; (void)core;
    if (atomic_exchange(&fail_create, false)) return 0;
    thread_arg_t *task = malloc(sizeof(*task));
    *task = (thread_arg_t){f, arg};
    *out = (void *)1;
    atomic_fetch_add(&live_tasks, 1);
    atomic_fetch_add(&created_tasks, 1);
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, sound_thread, task));
    pthread_detach(thread);
    return pdPASS;
}
#undef heap_caps_malloc
static void *sound_malloc(size_t size, unsigned caps) {
    (void)caps;
    return atomic_exchange(&fail_alloc, false) ? NULL : malloc(size);
}
#define heap_caps_malloc(n, c) sound_malloc(n, c)
static int64_t esp_timer_get_time(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec * 1000000LL + now.tv_nsec / 1000;
}
uint32_t esp_random(void) { return 12345; }
