#include "core/task_stats.h"

#include <stdlib.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"

#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS

static const char *TAG = "task_stats";
// Room for tasks created after the snapshot.
#define SPARE_TASKS 8

struct task_stats {
    UBaseType_t count;
    configRUN_TIME_COUNTER_TYPE total;
    TaskStatus_t tasks[];
};

static task_stats_t *snapshot(void) {
    UBaseType_t capacity = uxTaskGetNumberOfTasks() + SPARE_TASKS;
    task_stats_t *s =
        heap_caps_malloc(sizeof(*s) + capacity * sizeof(TaskStatus_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s)
        s->count = uxTaskGetSystemState(s->tasks, capacity, &s->total);
    return s;
}

task_stats_t *task_stats_begin(void) {
    return snapshot();
}

static configRUN_TIME_COUNTER_TYPE before(const task_stats_t *start, const TaskStatus_t *t) {
    for (UBaseType_t i = 0; i < start->count; i++)
        if (start->tasks[i].xHandle == t->xHandle)
            return start->tasks[i].ulRunTimeCounter;
    return 0;
}

void task_stats_end(task_stats_t *start, const char *what) {
    task_stats_t *end = start ? snapshot() : NULL;
    // Unsigned differences stay right across one counter wrap (~71 min).
    configRUN_TIME_COUNTER_TYPE elapsed = end ? end->total - start->total : 0;
    if (elapsed) {
        // Reuse the counters as each task's use since the start.
        for (UBaseType_t i = 0; i < end->count; i++)
            end->tasks[i].ulRunTimeCounter -= before(start, &end->tasks[i]);
        ESP_LOGI(TAG, "%s, %lu ms; share of its core:", what, (unsigned long)(elapsed / 1000));
        for (;;) {
            TaskStatus_t *busiest = NULL;
            for (UBaseType_t i = 0; i < end->count; i++)
                if (!busiest || end->tasks[i].ulRunTimeCounter > busiest->ulRunTimeCounter)
                    busiest = &end->tasks[i];
            unsigned pct = busiest ? (unsigned)(100ULL * busiest->ulRunTimeCounter / elapsed) : 0;
            if (!pct)
                break;
            BaseType_t affinity = xTaskGetCoreID(busiest->xHandle);
            int core = affinity == tskNO_AFFINITY ? -1 : (int)affinity;
            ESP_LOGI(TAG, "  %3u%%  %-16s core %2d  prio %u", pct, busiest->pcTaskName, core,
                     (unsigned)busiest->uxBasePriority);
            busiest->ulRunTimeCounter = 0;
        }
    }
    free(end);
    free(start);
}

#else

task_stats_t *task_stats_begin(void) {
    return NULL;
}
void task_stats_end(task_stats_t *stats, const char *what) {
    (void)stats;
    (void)what;
}

#endif
