#pragma once

#include <stdbool.h>
#include <stdint.h>

// Fixed coalescing slots: one pending invocation per kind, plus the running
// invocation. Earliest deadline wins, so retries never postpone user requests.
typedef enum {
    NETWORK_JOB_LOCATION,
    NETWORK_JOB_WEATHER,
    NETWORK_JOB_RELEASE,
    NETWORK_JOB_LOCATION_SAVE,
    NETWORK_JOB_RELEASE_DOWNLOAD, // reserved OTA download; attempts even offline
    NETWORK_JOB_COUNT,
} network_job_t;

// Boot initialization, before registering jobs or starting WiFi. The shared
// timer only dispatches; callbacks run serially on one retained static 8 KiB
// internal stack (including NVS writes), blocking between jobs. Jobs must finish,
// or schedule a future retry.
void network_jobs_init(void);
void network_jobs_register(network_job_t job, void (*run)(void), bool needs_network);
void network_jobs_schedule(network_job_t job, uint32_t delay_s);

// Network jobs wait for both a station IP and a synced/restored clock. Local
// persistence jobs remain runnable offline. Pending jobs survive disconnects.
void network_jobs_set_network(bool up);
