#pragma once

// CPU use per task over a stretch of time (a call, a stream): take a
// snapshot at the start, then log each task's share of its core since then.
// Needs CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS; without it both are no-ops.

typedef struct task_stats task_stats_t;

task_stats_t *task_stats_begin(void);
// Logs tasks that used at least 1% of a core, busiest first, and frees the snapshot.
void task_stats_end(task_stats_t *stats, const char *what);
