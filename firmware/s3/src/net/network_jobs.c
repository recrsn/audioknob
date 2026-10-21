#include "net/network_jobs.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "net/ntp.h"

#define TASK_STACK 8192
#define TIMER_US 1000000

static const char *TAG = "network_jobs";
static StaticSemaphore_t s_control_storage;
static SemaphoreHandle_t s_control;
static TaskHandle_t s_worker;
// Retain one internal stack/TCB for the service lifetime. Repeated network
// refreshes must not depend on finding another contiguous 8 KiB heap block.
static StackType_t s_stack[TASK_STACK / sizeof(StackType_t)] __attribute__((aligned(16)));
static StaticTask_t s_task_storage;
static bool s_waiting;
static esp_timer_handle_t s_timer;
static bool s_timer_started;
static bool s_network_up;
static unsigned s_cursor;
static struct {
    void (*run)(void);
    int64_t due_us;
    bool pending;
    bool needs_network;
} s_jobs[NETWORK_JOB_COUNT];

// Called under s_control. No client callbacks or blocking network operations.
static int next_job(void) {
    int64_t now = esp_timer_get_time();
    bool ready = s_network_up && ntp_is_synced();
    for (unsigned n = 0; n < NETWORK_JOB_COUNT; n++) {
        unsigned i = (s_cursor + n) % NETWORK_JOB_COUNT;
        if (s_jobs[i].run && s_jobs[i].pending && s_jobs[i].due_us <= now &&
            (!s_jobs[i].needs_network || ready)) {
            return (int)i;
        }
    }
    return -1;
}

static void worker(void *arg) {
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_control, portMAX_DELAY);
        s_waiting = false;
        int job = next_job();
        if (job < 0) {
            // Publish the wait before releasing the lock. A notification sent
            // before ulTaskNotifyTake is retained, so no wakeup is lost.
            s_waiting = true;
            xSemaphoreGive(s_control);
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        s_jobs[job].pending = false;
        s_cursor = ((unsigned)job + 1) % NETWORK_JOB_COUNT;
        void (*run)(void) = s_jobs[job].run;
        xSemaphoreGive(s_control);
        run();
        // A burst must still leave time for lower-priority housekeeping.
        vTaskDelay(1);
    }
}

static void dispatch(void *arg) {
    (void)arg;
    // Never wait behind a producer/task-creation on the timer service task.
    if (xSemaphoreTake(s_control, 0) != pdTRUE) {
        return;
    }
    if (next_job() >= 0) {
        if (!s_worker) {
            s_worker = xTaskCreateStatic(worker, "network_jobs", TASK_STACK, NULL, 2,
                                        s_stack, &s_task_storage);
            if (!s_worker) {
                // Slots remain pending; the next timer tick retries startup.
                ESP_LOGW(TAG, "couldn't start worker; will retry");
            }
        } else if (s_waiting) {
            xTaskNotifyGive(s_worker);
        }
    }
    xSemaphoreGive(s_control);
}

static void ensure_timer(void) {
    if (!s_timer) {
        const esp_timer_create_args_t args = {.callback = dispatch, .name = "network_jobs"};
        if (esp_timer_create(&args, &s_timer) != ESP_OK) {
            s_timer = NULL;
            ESP_LOGW(TAG, "couldn't create scheduler timer; next submission retries");
            return;
        }
    }
    if (!s_timer_started) {
        s_timer_started = esp_timer_start_periodic(s_timer, TIMER_US) == ESP_OK;
        if (!s_timer_started) {
            ESP_LOGW(TAG, "couldn't start scheduler timer; next submission retries");
        }
    }
}

void network_jobs_init(void) {
    s_control = xSemaphoreCreateMutexStatic(&s_control_storage);
    ensure_timer();
}

void network_jobs_register(network_job_t job, void (*run)(void), bool needs_network) {
    if ((unsigned)job >= NETWORK_JOB_COUNT) {
        return;
    }
    xSemaphoreTake(s_control, portMAX_DELAY);
    s_jobs[job].run = run;
    s_jobs[job].needs_network = needs_network;
    xSemaphoreGive(s_control);
}

void network_jobs_schedule(network_job_t job, uint32_t delay_s) {
    if ((unsigned)job >= NETWORK_JOB_COUNT) {
        return;
    }
    int64_t due = esp_timer_get_time() + (int64_t)delay_s * 1000000;
    xSemaphoreTake(s_control, portMAX_DELAY);
    if (!s_jobs[job].pending || due < s_jobs[job].due_us) {
        s_jobs[job].due_us = due;
    }
    s_jobs[job].pending = true;
    ensure_timer();
    xSemaphoreGive(s_control);
}

void network_jobs_set_network(bool up) {
    xSemaphoreTake(s_control, portMAX_DELAY);
    s_network_up = up;
    ensure_timer();
    xSemaphoreGive(s_control);
}
