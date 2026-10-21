#pragma once
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#define ESP_OK 0
#define ESP_LOGW(tag, ...) ((void)(tag))
#define pdPASS 1
#define pdTRUE 1
#define portMAX_DELAY UINT32_MAX
typedef pthread_mutex_t StaticSemaphore_t;
typedef pthread_mutex_t *SemaphoreHandle_t;
typedef uint8_t StackType_t;
typedef struct { pthread_mutex_t mutex; pthread_cond_t cond; unsigned notifications; } StaticTask_t;
typedef StaticTask_t *TaskHandle_t;
typedef void *esp_timer_handle_t;
typedef struct { void (*callback)(void *); const char *name; } esp_timer_create_args_t;
static atomic_bool clock_ready, fail_create, fail_timer_create, fail_timer_start;
static atomic_bool pause_start, pause_wait;
static atomic_int live_tasks, created_tasks, at_wait, waiting, failed_creates;
static atomic_llong clock_us;
static SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *s) {
    assert(!pthread_mutex_init(s,NULL)); return s;
}
static int xSemaphoreTake(SemaphoreHandle_t s, uint32_t ticks) {
    return !(ticks ? pthread_mutex_lock(s) : pthread_mutex_trylock(s));
}
static void xSemaphoreGive(SemaphoreHandle_t s) { assert(!pthread_mutex_unlock(s)); }
static int64_t esp_timer_get_time(void) { return atomic_load(&clock_us); }
static bool ntp_is_synced(void) { return atomic_load(&clock_ready); }
static int esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *out) {
    assert(args->callback && !strcmp(args->name,"network_jobs"));
    if (atomic_exchange(&fail_timer_create,false)) return -1;
    *out=(void *)1; return ESP_OK;
}
static int esp_timer_start_periodic(esp_timer_handle_t timer, uint64_t period) {
    assert(timer && period==1000000);
    return atomic_exchange(&fail_timer_start,false) ? -1 : ESP_OK;
}
static void vTaskDelay(uint32_t ticks) { assert(ticks==1); usleep(50); }
static TaskHandle_t retained_task;
static uint32_t ulTaskNotifyTake(int clear, uint32_t ticks) {
    assert(clear==pdTRUE && ticks==portMAX_DELAY);
    atomic_fetch_add(&at_wait,1);
    while (atomic_load(&pause_wait)) usleep(50);
    pthread_mutex_lock(&retained_task->mutex);
    while (!retained_task->notifications) {
        atomic_fetch_add(&waiting,1);
        pthread_cond_wait(&retained_task->cond,&retained_task->mutex);
        atomic_fetch_sub(&waiting,1);
    }
    unsigned count=retained_task->notifications; retained_task->notifications=0;
    pthread_mutex_unlock(&retained_task->mutex);
    atomic_fetch_sub(&at_wait,1); return count;
}
static void xTaskNotifyGive(TaskHandle_t task) {
    assert(task==retained_task);
    pthread_mutex_lock(&task->mutex); task->notifications++;
    pthread_cond_signal(&task->cond); pthread_mutex_unlock(&task->mutex);
}
static void *thread_start(void *arg) {
    void (*fn)(void *)=*(void (**)(void *))arg; free(arg);
    while (atomic_load(&pause_start)) usleep(50);
    fn(NULL); assert(false); return NULL;
}
static TaskHandle_t xTaskCreateStatic(void (*fn)(void *), const char *name, unsigned stack,
                                     void *arg, unsigned priority, StackType_t *buffer, StaticTask_t *tcb) {
    assert(!strcmp(name,"network_jobs") && stack==8192 && !arg && priority==2 && buffer && tcb);
    if (atomic_exchange(&fail_create,false)) { atomic_fetch_add(&failed_creates,1); return NULL; }
    assert(!atomic_load(&created_tasks)); // Stack/TCB are reused, never recreated.
    assert(!pthread_mutex_init(&tcb->mutex,NULL)); assert(!pthread_cond_init(&tcb->cond,NULL));
    tcb->notifications=0; retained_task=tcb;
    void (**stored)(void *)=malloc(sizeof(fn)); assert(stored); *stored=fn;
    atomic_fetch_add(&created_tasks,1); atomic_fetch_add(&live_tasks,1);
    pthread_t thread; assert(!pthread_create(&thread,NULL,thread_start,stored));
    pthread_detach(thread); return tcb;
}
