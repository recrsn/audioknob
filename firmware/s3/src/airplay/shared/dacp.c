/*
 * Ported from philippe44's squeezelite-esp32 raop component
 * (c) Philippe 2019-2020, philippe_44@outlook.com -- MIT License,
 * https://opensource.org/licenses/MIT
 */

#ifndef AK_NATIVE

#include "dacp.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "http_util.h"
#include "lwip/sockets.h"
#include "mdns.h"

#define SEARCH_STACK_SIZE 4096
#define SEARCH_TASK_PRIO 3
#define QUERY_TIMEOUT_MS 3000
#define COMMAND_TIMEOUT_S 2

static const char *TAG = "dacp";

static struct {
    char dacp_id[32], active_remote[32];
    struct in_addr host;
    uint16_t port;
    volatile bool searching;
    TaskHandle_t task;
    SemaphoreHandle_t done, lock;
} s;

static void search_task(void *arg) {
    (void)arg;
    bool found = false;

    while (s.searching && !found) {
        mdns_result_t *results = NULL;
        if (mdns_query_ptr("_dacp", "_tcp", QUERY_TIMEOUT_MS, 32, &results) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        for (mdns_result_t *r = results; r && !found; r = r->next) {
            if (!r->instance_name || !strcasestr(r->instance_name, s.dacp_id)) {
                continue;
            }
            for (mdns_ip_addr_t *a = r->addr; a; a = a->next) {
                if (a->addr.type == ESP_IPADDR_TYPE_V4) {
                    xSemaphoreTake(s.lock, portMAX_DELAY);
                    s.host.s_addr = a->addr.u_addr.ip4.addr;
                    s.port = r->port;
                    xSemaphoreGive(s.lock);
                    found = true;
                    ESP_LOGI(TAG, "found remote %s at %s:%u", r->instance_name, inet_ntoa(s.host), r->port);
                    break;
                }
            }
        }
        mdns_query_results_free(results);
    }

    xSemaphoreGive(s.done);
    vTaskDelete(NULL);
}

void dacp_init(void) {
    s.done = xSemaphoreCreateBinary();
    s.lock = xSemaphoreCreateMutex();
}

void dacp_stop(void) {
    if (s.task) {
        s.searching = false;
        xSemaphoreTake(s.done, portMAX_DELAY);
        s.task = NULL;
    }
    xSemaphoreTake(s.lock, portMAX_DELAY);
    s.port = 0;
    s.host.s_addr = 0;
    xSemaphoreGive(s.lock);
}

void dacp_start(const char *dacp_id, const char *active_remote) {
    dacp_stop();
    if (!dacp_id || !active_remote) {
        return;
    }

    xSemaphoreTake(s.lock, portMAX_DELAY);
    snprintf(s.dacp_id, sizeof(s.dacp_id), "%s", dacp_id);
    snprintf(s.active_remote, sizeof(s.active_remote), "%s", active_remote);
    xSemaphoreGive(s.lock);
    s.searching = true;
    if (xTaskCreatePinnedToCore(search_task, "dacp_search", SEARCH_STACK_SIZE, NULL, SEARCH_TASK_PRIO, &s.task,
                                tskNO_AFFINITY) != pdPASS) {
        s.searching = false;
        s.task = NULL;
    }
}

bool dacp_command(const char *command) {
    struct sockaddr_in addr = {.sin_family = AF_INET};
    char active_remote[32];

    xSemaphoreTake(s.lock, portMAX_DELAY);
    addr.sin_addr = s.host;
    addr.sin_port = htons(s.port);
    memcpy(active_remote, s.active_remote, sizeof(active_remote));
    bool known = s.port != 0;
    xSemaphoreGive(s.lock);

    if (!known) {
        return false;
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return false;
    }
    struct timeval timeout = {COMMAND_TIMEOUT_S, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    bool ok = false;
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
        char start_line[128];
        key_data_t headers[3] = {{0}};
        snprintf(start_line, sizeof(start_line), "GET /ctrl-int/1/%s HTTP/1.0", command);
        kd_add(headers, "Active-Remote", active_remote);
        kd_add(headers, "Connection", "close");

        if (http_send(sock, start_line, headers)) {
            char reply[64] = "";
            int n = recv(sock, reply, sizeof(reply) - 1, 0);
            reply[n > 0 ? n : 0] = '\0';
            reply[strcspn(reply, "\r\n")] = '\0';
            ESP_LOGI(TAG, "%s -> %s", command, reply);
            ok = n > 0;
        }
        kd_free(headers);
    } else {
        ESP_LOGW(TAG, "cannot reach remote for %s", command);
    }

    close(sock);
    return ok;
}

bool dacp_volume(float volume) {
    char command[64];
    float db = volume > 0.0f ? (volume - 1.0f) * 30.0f : -144.0f;
    snprintf(command, sizeof(command), "setproperty?dmcp.device-volume=%0.4f", db);
    return dacp_command(command);
}

#endif  // !AK_NATIVE
