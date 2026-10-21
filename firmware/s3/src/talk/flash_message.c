#include "talk/flash_message.h"
#include "talk/rtsp.h"
#include "talk/message_store.h"
#include "talk/talk.h"
#include "core/device_name.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "nvs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OUTBOX_MAX 8
#define QUEUE_MAX 8
#define STORE_VERSION 2
static const char *TAG = "flash_message";
static SemaphoreHandle_t s_lock;
static QueueHandle_t s_jobs;
typedef struct {
    char id[17], name[33], text[UI_MESSAGE_TEXT_MAX + 1];
    uint32_t peer;
    uint8_t delivered, acknowledged, pending, failed, voice, local;
    uint32_t duration_ms;
} message_t;
typedef struct {
    uint32_t version, generation, checksum;
    uint8_t count;
    message_t items[UI_INBOX_MAX];
} store_t;
static store_t *s_inbox_store, *s_outbox_store;
#define s_inbox (*s_inbox_store)
#define s_out_store (*s_outbox_store)
#define s_outbox s_out_store.items
#define s_out_count s_out_store.count
typedef struct {
    bool ack, remove;
    message_t message;
} job_t;

static bool valid_id(const char *id) {
    if (!id || strlen(id) != 16)
        return false;
    for (int i = 0; i < 16; i++)
        if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f')))
            return false;
    return true;
}
static bool valid_text(const char *text) {
    size_t n = text ? strlen(text) : 0;
    if (!n || n > UI_MESSAGE_TEXT_MAX)
        return false;
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)text[i] < 32 && text[i] != '\n')
            return false;
    return true;
}
// Caller holds s_lock. Persist before accepting an inbox mutation, so a
// successful delivery means it survives a restart. Receipts are persisted too.
static uint32_t checksum(const store_t *store) {
    const uint8_t *p = (const uint8_t *)store;
    uint32_t hash = 2166136261;
    for (size_t i = 0; i < sizeof(*store); i++)
        if (i < 8 || i >= 12)
            hash = (hash ^ p[i]) * 16777619;
    return hash;
}
static bool save_store(store_t *store, const char *prefix) {
    uint32_t previous = store->generation;
    store->generation++;
    store->checksum = checksum(store);
    bool ok = false;
    if (message_store_ready()) {
        char file[24];
        snprintf(file, sizeof(file), "%s.%c", prefix, store->generation & 1 ? 'a' : 'b');
        ok = message_store_write(file, store, sizeof(*store));
    } else {
        nvs_handle_t nvs;
        if (nvs_open("inbox", NVS_READWRITE, &nvs) == ESP_OK) {
            ok = nvs_set_blob(nvs, prefix, store, sizeof(*store)) == ESP_OK && nvs_commit(nvs) == ESP_OK;
            nvs_close(nvs);
        }
    }
    if (!ok)
        store->generation = previous;
    return ok;
}
static bool save_inbox(void) {
    return save_store(&s_inbox, "inbox");
}
static bool save_outbox(void) {
    return save_store(&s_out_store, "outbox");
}
static bool loaded_store_valid(const store_t *store) {
    if (store->version != STORE_VERSION || store->count > UI_INBOX_MAX || store->checksum != checksum(store))
        return false;
    for (int i = 0; i < store->count; i++) {
        const message_t *m = &store->items[i];
        if (m->id[16] || m->name[32] || m->text[UI_MESSAGE_TEXT_MAX] || !valid_id(m->id))
            return false;
    }
    return true;
}
// Caller holds s_lock. Only a durably queued item can reach the sender task.
static bool enqueue_outgoing(job_t *job) {
    for (int i = 0; i < s_out_count; i++) {
        message_t *m = &s_outbox[i];
        if (!strcmp(m->id, job->message.id) && m->peer == job->message.peer && (m->pending || m->delivered))
            return true;
    }
    store_t *before = heap_caps_malloc(sizeof(*before), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!before)
        return false;
    *before = s_out_store;
    int index = s_out_count;
    for (int i = 0; i < s_out_count; i++)
        if (!strcmp(s_outbox[i].id, job->message.id) && s_outbox[i].peer == job->message.peer)
            index = i;
    if (index == OUTBOX_MAX) {
        if (s_outbox[0].pending) {
            free(before);
            return false;
        }
        memmove(s_outbox, s_outbox + 1, (OUTBOX_MAX - 1) * sizeof(*s_outbox));
        index = --s_out_count;
    }
    s_outbox[index] = job->message;
    if (index == s_out_count)
        s_out_count++;
    bool saved = save_outbox();
    bool ok = saved && xQueueSend(s_jobs, job, 0) == pdTRUE;
    if (!ok) {
        s_out_store = *before;
        if (saved)
            save_outbox();
    }
    free(before);
    return ok;
}
bool flash_message_send(uint32_t peer, const char *text, char id[17]) {
    if (!peer || !valid_text(text))
        return false;
    job_t job = {0};
    job.message.peer = peer;
    snprintf(job.message.id, sizeof(job.message.id), "%08lx%08lx", (unsigned long)esp_random(),
             (unsigned long)esp_random());
    device_name_get(job.message.name, sizeof(job.message.name));
    snprintf(job.message.text, sizeof(job.message.text), "%s", text);
    job.message.pending = 1;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = enqueue_outgoing(&job);
    if (ok)
        snprintf(id, 17, "%s", job.message.id);
    xSemaphoreGive(s_lock);
    return ok;
}
// The worker resolves the ID: it holds s_lock through flash writes, which the
// UI and web callers must not wait on.
static bool enqueue_by_id(const char *id, bool ack) {
    if (!valid_id(id))
        return false;
    job_t job = {.ack = ack, .remove = !ack};
    snprintf(job.message.id, sizeof(job.message.id), "%s", id);
    return xQueueSend(s_jobs, &job, 0) == pdTRUE;
}
bool flash_message_ack(const char *id) {
    return enqueue_by_id(id, true);
}
bool flash_message_delete(const char *id) {
    return enqueue_by_id(id, false);
}
bool flash_message_add_voice(uint32_t peer, const char *sender, const char *id, uint32_t duration_ms, bool local) {
    if (!valid_id(id) || !sender || strlen(sender) > 32)
        return false;
    bool ok = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_inbox.count; i++)
        if (!strcmp(s_inbox.items[i].id, id)) {
            ok = s_inbox.items[i].peer == peer && s_inbox.items[i].voice;
            xSemaphoreGive(s_lock);
            return ok;
        }
    if (s_inbox.count < UI_INBOX_MAX) {
        message_t *m = &s_inbox.items[s_inbox.count++];
        memset(m, 0, sizeof(*m));
        snprintf(m->id, 17, "%s", id);
        snprintf(m->name, 33, "%s", sender);
        snprintf(m->text, sizeof(m->text), "Voice note (%lu s)", (unsigned long)((duration_ms + 999) / 1000));
        m->peer = peer;
        m->delivered = 1;
        m->voice = 1;
        m->local = local;
        m->duration_ms = duration_ms;
        ok = save_inbox();
        if (!ok) {
            memset(m, 0, sizeof(*m));
            s_inbox.count--;
        }
    }
    xSemaphoreGive(s_lock);
    return ok;
}
bool flash_message_send_voice(uint32_t peer, const char *id) {
    if (!peer || !valid_id(id))
        return false;
    job_t job = {0};
    bool found = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_inbox.count; i++)
        if (!strcmp(s_inbox.items[i].id, id) && s_inbox.items[i].voice) {
            job.message = s_inbox.items[i];
            found = true;
            break;
        }
    if (found) {
        job.message.peer = peer;
        job.message.pending = 1;
        job.message.acknowledged = job.message.delivered = job.message.failed = 0;
        device_name_get(job.message.name, sizeof(job.message.name));
        found = enqueue_outgoing(&job);
    }
    xSemaphoreGive(s_lock);
    return found;
}
bool flash_message_receive(uint32_t peer, const char *path, const char *json) {
    if (strcmp(path, "/messages") && strcmp(path, "/messages/ack"))
        return false;
    cJSON *root = cJSON_Parse(json);
    if (!root)
        return false;
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "id"));
    const char *text = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "text"));
    const char *name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(root, "sender"));
    bool ack = !strcmp(path, "/messages/ack"), ok = false;
    if (!valid_id(id) || (!ack && (!valid_text(text) || !name || !name[0] || strlen(name) > 32))) {
        cJSON_Delete(root);
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (ack) {
        for (int i = 0; i < s_out_count; i++)
            if (!strcmp(s_outbox[i].id, id) && s_outbox[i].peer == peer) {
                uint8_t before = s_outbox[i].acknowledged;
                s_outbox[i].acknowledged = 1;
                ok = save_outbox();
                if (!ok)
                    s_outbox[i].acknowledged = before;
                break;
            }
    } else {
        for (int i = 0; i < s_inbox.count; i++)
            if (s_inbox.items[i].peer == peer && !strcmp(s_inbox.items[i].id, id)) {
                ok = true;
                break;
            }
        if (!ok && s_inbox.count < UI_INBOX_MAX) {
            message_t *m = &s_inbox.items[s_inbox.count++];
            memset(m, 0, sizeof(*m));
            m->peer = peer;
            m->delivered = 1;
            snprintf(m->id, 17, "%s", id);
            snprintf(m->name, 33, "%s", name);
            snprintf(m->text, sizeof(m->text), "%s", text);
            ok = save_inbox();
            if (!ok) {
                memset(m, 0, sizeof(*m));
                s_inbox.count--;
            }
        }
    }
    xSemaphoreGive(s_lock);
    cJSON_Delete(root);
    return ok;
}
void flash_message_snapshot(ui_inbox_t *out) {
    if (xSemaphoreTake(s_lock, 0) != pdTRUE)
        return;
    memset(out, 0, sizeof(*out));
    out->count = s_inbox.count;
    for (int i = 0; i < s_inbox.count; i++) {
        const message_t *m = &s_inbox.items[i];
        ui_message_t *d = &out->messages[i];
        snprintf(d->id, sizeof(d->id), "%s", m->id);
        snprintf(d->sender, sizeof(d->sender), "%s", m->name);
        snprintf(d->text, sizeof(d->text), "%s", m->text);
        d->voice = m->voice;
        d->local = m->local;
        d->duration_ms = m->duration_ms;
        d->acknowledged = m->acknowledged;
        d->pending = m->pending;
        d->failed = m->failed;
    }
    xSemaphoreGive(s_lock);
}
cJSON *flash_message_json(void) {
    cJSON *root = cJSON_CreateObject(), *in = cJSON_AddArrayToObject(root, "inbox"),
          *out = cJSON_AddArrayToObject(root, "outbox");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int list = 0; list < 2; list++) {
        int count = list ? s_out_count : s_inbox.count;
        for (int i = 0; i < count; i++) {
            message_t *m = list ? &s_outbox[i] : &s_inbox.items[i];
            cJSON *item = cJSON_CreateObject();
            cJSON_AddItemToArray(list ? out : in, item);
            struct in_addr addr = {.s_addr = m->peer};
            cJSON_AddStringToObject(item, "id", m->id);
            cJSON_AddStringToObject(item, "peer", inet_ntoa(addr));
            cJSON_AddStringToObject(item, "sender", m->name);
            cJSON_AddStringToObject(item, "text", m->text);
            cJSON_AddBoolToObject(item, "voice", m->voice);
            cJSON_AddBoolToObject(item, "local", m->local);
            cJSON_AddNumberToObject(item, "durationMs", m->duration_ms);
            cJSON_AddBoolToObject(item, "pending", m->pending);
            cJSON_AddBoolToObject(item, "delivered", m->delivered);
            cJSON_AddBoolToObject(item, "acknowledged", m->acknowledged);
            cJSON_AddBoolToObject(item, "failed", m->failed);
        }
    }
    xSemaphoreGive(s_lock);
    return root;
}
// Fills in the received item to acknowledge, once: repeat requests while one
// is in flight or done are dropped.
static bool claim_ack(message_t *out) {
    bool ok = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_inbox.count; i++) {
        message_t *m = &s_inbox.items[i];
        if (strcmp(m->id, out->id))
            continue;
        ok = !m->local && m->peer && !m->acknowledged && !m->pending;
        if (ok) {
            m->pending = 1;
            m->failed = 0;
            *out = *m;
        }
        break;
    }
    xSemaphoreGive(s_lock);
    return ok;
}
static void worker(void *arg) {
    (void)arg;
    job_t job;
    for (;;) {
        if (xQueueReceive(s_jobs, &job, pdMS_TO_TICKS(1000)) != pdTRUE) {
            talk_persist_settings();
            continue;
        }
        if (job.remove) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            for (int i = 0; i < s_inbox.count; i++)
                if (!strcmp(s_inbox.items[i].id, job.message.id)) {
                    store_t before = s_inbox;
                    memmove(&s_inbox.items[i], &s_inbox.items[i + 1], (s_inbox.count - i - 1) * sizeof(message_t));
                    memset(&s_inbox.items[--s_inbox.count], 0, sizeof(message_t));
                    if (!save_inbox()) {
                        // Keep the item; its flags describe the acknowledgement, not this.
                        s_inbox = before;
                        ESP_LOGW(TAG, "Couldn't delete %s", job.message.id);
                    } else if (before.items[i].voice) {
                        char file[24];
                        snprintf(file, sizeof(file), "%s.ogg", job.message.id);
                        message_store_remove(file);
                    }
                    break;
                }
            xSemaphoreGive(s_lock);
            continue;
        }
        if (job.ack && !claim_ack(&job.message))
            continue;
        cJSON *json = cJSON_CreateObject();
        cJSON_AddStringToObject(json, "id", job.message.id);
        if (!job.ack) {
            cJSON_AddStringToObject(json, "text", job.message.text);
            cJSON_AddStringToObject(json, "sender", job.message.name);
        }
        char *body = cJSON_PrintUnformatted(json);
        cJSON_Delete(json);
        bool ok;
        if (job.message.voice && !job.ack) {
            char file[24], path[64];
            snprintf(file, sizeof(file), "%s.ogg", job.message.id);
            snprintf(path, sizeof(path), "/messages/voice?id=%s", job.message.id);
            size_t size;
            void *data = message_store_read(file, &size, VOICE_NOTE_MAX_BYTES);
            ok = data && talk_rtsp_message_bytes(job.message.peer, path, "audio/ogg", data, size);
            free(data);
        } else
            ok = body && talk_rtsp_message(job.message.peer, job.ack ? "/messages/ack" : "/messages", body);
        cJSON_free(body);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        int count = job.ack ? s_inbox.count : s_out_count;
        for (int i = 0; i < count; i++) {
            message_t *m = job.ack ? &s_inbox.items[i] : &s_outbox[i];
            if (strcmp(m->id, job.message.id) || m->peer != job.message.peer)
                continue;
            m->pending = 0;
            m->failed = !ok;
            if (job.ack) {
                m->acknowledged = ok;
                save_inbox();
            } else {
                m->delivered = ok;
                save_outbox();
            }
            break;
        }
        xSemaphoreGive(s_lock);
    }
}
void flash_message_init(void) {
    s_inbox_store = heap_caps_calloc(1, sizeof(store_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_outbox_store = heap_caps_calloc(1, sizeof(store_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_ERROR_CHECK(s_inbox_store && s_outbox_store ? ESP_OK : ESP_ERR_NO_MEM);
    s_inbox.version = s_out_store.version = STORE_VERSION;
    s_lock = xSemaphoreCreateMutex();
    s_jobs = xQueueCreate(QUEUE_MAX, sizeof(job_t));
    ESP_ERROR_CHECK(s_lock && s_jobs ? ESP_OK : ESP_ERR_NO_MEM);
    for (int list = 0; list < 2; list++) {
        store_t *target = list ? &s_out_store : &s_inbox;
        const char *prefix = list ? "outbox" : "inbox";
        bool loaded = false;
        for (int slot = 0; slot < 2; slot++) {
            char file[24];
            snprintf(file, sizeof(file), "%s.%c", prefix, slot ? 'b' : 'a');
            size_t size;
            store_t *store = message_store_read(file, &size, sizeof(store_t));
            if (store && size == sizeof(*store) && loaded_store_valid(store) &&
                (!loaded || store->generation > target->generation)) {
                *target = *store;
                loaded = true;
            }
            free(store);
        }
        if (!loaded) {
            nvs_handle_t nvs;
            if (nvs_open("inbox", NVS_READONLY, &nvs) == ESP_OK) {
                size_t size = sizeof(*target);
                store_t *store = malloc(sizeof(*store));
                if (store && nvs_get_blob(nvs, prefix, store, &size) == ESP_OK && size == sizeof(*store) &&
                    loaded_store_valid(store))
                    *target = *store;
                free(store);
                nvs_close(nvs);
            }
        }
        for (int i = 0; i < target->count; i++) {
            if (list && target->items[i].pending)
                target->items[i].failed = 1;
            target->items[i].pending = 0;
        }
    }
    ESP_ERROR_CHECK(xTaskCreate(worker, "messages", 6144, NULL, 3, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
