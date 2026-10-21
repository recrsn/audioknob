#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ota.h"

typedef enum {
    RELEASE_QUIET,
    RELEASE_CHECKING,
    RELEASE_AVAILABLE,
    RELEASE_CURRENT,
    RELEASE_ERROR,
} release_notice_t;

typedef struct {
    bool checking;
    bool checked;
    // Unix seconds; 0 until the first check finishes.
    int64_t last_check;
    int64_t next_check; // 0: no scheduled polling; retained for API compatibility
    bool s3_available;
    bool bluetooth_available;
    release_notice_t notice;
    ota_target_t target;
    char tag[32];
    char s3_version[32];
    char bluetooth_version[32];
    char error[96];
} github_release_status_t;

// Register one startup check after WiFi and clock readiness. No daily polling.
// Manual checks use the same retained worker; results remain available between jobs.
void github_release_start(void);
bool github_release_check(void);
void github_release_get_status(github_release_status_t *out);
bool github_release_download(ota_target_t target);
void github_release_answer(bool download);
