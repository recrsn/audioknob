#include "talk/note/notes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "audio/alarm_sound.h"
#include "audio/source.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "talk/note/audio.h"
#include "talk/note/ogg.h"
#include "talk/note/store.h"
#include "talk/rtsp.h"

// Threads kept on flash; the snapshot adds the discovered knobs with none.
#define STORED_MAX (UI_THREADS_MAX - UI_TALK_MAX)
#define INDEX_MAGIC 0x31544f4eu // "NOT1"
#define INDEX_VERSION 1
// Room kept besides a note being written, for indexes and SPIFFS itself.
#define ROOM_SPARE (16 * 1024)
#define QUEUE_LEN 8
#define WORKER_STACK 6144
#define WORKER_PRIO 3
#define WORKER_IDLE_MS 3000
// How long a note's sender waits for this knob to save it.
#define RECEIVE_WAIT_MS 15000
// Before this the clock wasn't set (2020-01-01).
#define TIME_VALID 1577836800u

typedef struct {
    uint32_t magic, version, size;
    char key[UI_TALK_KEY], name[33];
    uint8_t count;
    ui_note_t items[UI_THREAD_ITEMS];
    uint32_t checksum;
} thread_t;

typedef enum { JOB_MOUNT, JOB_FLUSH, JOB_SEND, JOB_RECEIVE, JOB_RECEIPT, JOB_PLAY, JOB_REMOVE } job_kind_t;

// A received note's sender waits for the save, or gives up: whichever of it
// and the worker is last frees this.
typedef struct {
    SemaphoreHandle_t done;
    int result;
    bool abandoned;
} receive_t;

typedef struct {
    job_kind_t kind;
    char id[17], key[UI_TALK_KEY], name[33];
    uint8_t wave[UI_NOTE_WAVE];
    uint8_t *data;
    size_t len;
    uint32_t ms;
    receive_t *rx;
} job_t;

static const char *TAG = "notes";
static SemaphoreHandle_t s_lock;
static QueueHandle_t s_jobs;
static TaskHandle_t s_task;
static bool s_flush_queued;

// Under s_lock.
static thread_t *s_threads; // [STORED_MAX], PSRAM
static bool s_dirty[STORED_MAX];
static int s_count;
static ui_talk_peer_t s_peers[UI_TALK_MAX];
static int s_peer_count;
static uint32_t s_version;

static bool hex_string(const char *s, size_t len) {
    if (!s || strlen(s) != len) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) {
            return false;
        }
    }
    return true;
}

bool notes_valid_key(const char *key) {
    return hex_string(key, UI_TALK_KEY - 1);
}

bool notes_valid_id(const char *id) {
    return hex_string(id, 16);
}

static void new_id(char id[17]) {
    snprintf(id, 17, "%08lx%08lx", (unsigned long)esp_random(), (unsigned long)esp_random());
}

static uint32_t now(void) {
    time_t t = time(NULL);
    return t >= TIME_VALID ? (uint32_t)t : 0;
}

static void ogg_name(char *out, size_t len, const char *id) {
    snprintf(out, len, "%s.ogg", id);
}

static void index_name(char *out, size_t len, const char *key) {
    snprintf(out, len, "t%s.idx", key);
}

static uint32_t checksum(const thread_t *t) {
    const uint8_t *p = (const uint8_t *)t;
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < offsetof(thread_t, checksum); i++) {
        hash = (hash ^ p[i]) * 16777619u;
    }
    return hash;
}

// ---- The model, under s_lock ----

static void changed(void) {
    s_version++;
}

static uint32_t last_time(const thread_t *t) {
    return t->count ? t->items[t->count - 1].time : 0;
}

static thread_t *thread_of(const char *key, int *index) {
    for (int i = 0; i < s_count; i++) {
        if (!strcmp(s_threads[i].key, key)) {
            if (index) {
                *index = i;
            }
            return &s_threads[i];
        }
    }
    return NULL;
}

static ui_note_t *item_of(const char *id, thread_t **thread, int *index) {
    for (int t = 0; t < s_count; t++) {
        for (int i = 0; i < s_threads[t].count; i++) {
            if (!strcmp(s_threads[t].items[i].id, id)) {
                *thread = &s_threads[t];
                if (index) {
                    *index = i;
                }
                return &s_threads[t].items[i];
            }
        }
    }
    return NULL;
}

static bool is_note(const ui_note_t *n) {
    return n->kind == UI_NOTE_IN || n->kind == UI_NOTE_OUT;
}

static void dirty(const thread_t *t) {
    s_dirty[t - s_threads] = true;
    changed();
}

static const char *peer_name(const char *key) {
    for (int i = 0; i < s_peer_count; i++) {
        if (!strcmp(s_peers[i].key, key)) {
            return s_peers[i].name;
        }
    }
    return NULL;
}

static uint32_t peer_ip(const char *key) {
    for (int i = 0; i < s_peer_count; i++) {
        if (!strcmp(s_peers[i].key, key)) {
            return s_peers[i].id;
        }
    }
    return 0;
}

// The thread for key, made if new. A full store gives up the thread quiet
// longest; its notes' files go into evicted (ids, up to UI_THREAD_ITEMS).
static thread_t *thread_open(const char *key, const char *name, char evicted[][17], int *evicted_count) {
    thread_t *t = thread_of(key, NULL);
    if (t) {
        if (name && name[0] && strcmp(t->name, name)) {
            snprintf(t->name, sizeof(t->name), "%s", name);
            dirty(t);
        }
        return t;
    }
    if (s_count == STORED_MAX) {
        int oldest = 0;
        for (int i = 1; i < s_count; i++) {
            if (last_time(&s_threads[i]) < last_time(&s_threads[oldest])) {
                oldest = i;
            }
        }
        t = &s_threads[oldest];
        for (int i = 0; i < t->count; i++) {
            if (is_note(&t->items[i])) {
                snprintf(evicted[(*evicted_count)++], 17, "%s", t->items[i].id);
            }
        }
        // Its index file goes when the thread's key is next flushed: here,
        // with a different key, it's rewritten under that one instead.
        char old_key[UI_TALK_KEY];
        snprintf(old_key, sizeof(old_key), "%s", t->key);
        snprintf(evicted[(*evicted_count)++], 17, "t%s", old_key);
    } else {
        t = &s_threads[s_count++];
    }
    memset(t, 0, sizeof(*t));
    snprintf(t->key, sizeof(t->key), "%s", key);
    snprintf(t->name, sizeof(t->name), "%s", name ? name : "");
    dirty(t);
    return t;
}

// Appends, dropping the oldest item of a full thread (its id into evicted).
static ui_note_t *thread_add(thread_t *t, char evicted[][17], int *evicted_count) {
    if (t->count == UI_THREAD_ITEMS) {
        if (is_note(&t->items[0])) {
            snprintf(evicted[(*evicted_count)++], 17, "%s", t->items[0].id);
        }
        memmove(t->items, t->items + 1, (UI_THREAD_ITEMS - 1) * sizeof(t->items[0]));
        t->count--;
    }
    ui_note_t *n = &t->items[t->count++];
    memset(n, 0, sizeof(*n));
    new_id(n->id);
    n->time = now();
    dirty(t);
    return n;
}

static void thread_drop(thread_t *t, int index) {
    memmove(&t->items[index], &t->items[index + 1], (t->count - index - 1) * sizeof(t->items[0]));
    t->count--;
    dirty(t);
}

// ---- The worker ----

static void worker(void *arg);

// Caller holds s_lock.
static bool post_locked(const job_t *job) {
    bool ok = xQueueSend(s_jobs, job, 0) == pdTRUE;
    if (ok && !s_task &&
        xTaskCreatePinnedToCore(worker, "notes", WORKER_STACK, NULL, WORKER_PRIO, &s_task, tskNO_AFFINITY) != pdPASS) {
        // Retried by the next post; the job waits in the queue.
        s_task = NULL;
        ESP_LOGW(TAG, "cannot start the worker");
    }
    return ok;
}

static bool post(const job_t *job) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = post_locked(job);
    xSemaphoreGive(s_lock);
    return ok;
}

// Caller holds s_lock: one flush covers every dirty thread.
static void flush_locked(void) {
    if (!s_flush_queued) {
        s_flush_queued = post_locked(&(job_t){.kind = JOB_FLUSH});
    }
}

static void remove_files(char evicted[][17], int count) {
    for (int i = 0; i < count; i++) {
        job_t job = {.kind = JOB_REMOVE};
        snprintf(job.id, sizeof(job.id), "%s", evicted[i]);
        post(&job);
    }
}

static void do_flush(void) {
    thread_t *copy = heap_caps_malloc(sizeof(*copy), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    for (int i = 0; copy && i < STORED_MAX; i++) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool write = i < s_count && s_dirty[i];
        if (write) {
            s_dirty[i] = false;
            *copy = s_threads[i];
        }
        xSemaphoreGive(s_lock);
        if (!write) {
            continue;
        }
        copy->magic = INDEX_MAGIC;
        copy->version = INDEX_VERSION;
        copy->size = sizeof(*copy);
        copy->checksum = checksum(copy);
        char name[24];
        index_name(name, sizeof(name), copy->key);
        if (!note_store_write(name, copy, sizeof(*copy))) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_dirty[i] = true;
            xSemaphoreGive(s_lock);
        }
    }
    free(copy);
}

static void load_index(const char *file, void *ctx) {
    (void)ctx;
    char name[24];
    snprintf(name, sizeof(name), "%.*s", (int)(strchr(file, '.') - file), file);
    strcat(name, ".idx");
    size_t size;
    thread_t *t = note_store_read(name, &size, sizeof(thread_t));
    bool ok = t && size == sizeof(*t) && t->magic == INDEX_MAGIC && t->version == INDEX_VERSION &&
              t->size == sizeof(*t) && t->checksum == checksum(t) && notes_valid_key(t->key) &&
              t->count <= UI_THREAD_ITEMS && !t->name[32];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (ok && !thread_of(t->key, NULL) && s_count < STORED_MAX) {
        // A restart cut its send short.
        for (int i = 0; i < t->count; i++) {
            if (t->items[i].kind == UI_NOTE_OUT && t->items[i].status == UI_NOTE_SENDING) {
                t->items[i].status = UI_NOTE_FAILED;
            }
        }
        s_threads[s_count++] = *t;
        changed();
    } else if (!ok) {
        ESP_LOGW(TAG, "%s unreadable, skipped", name);
    }
    xSemaphoreGive(s_lock);
    free(t);
}

// A note file no thread lists: left by a power cut mid-save or delete.
static void sweep_orphan(const char *file, void *ctx) {
    (void)ctx;
    char id[17];
    snprintf(id, sizeof(id), "%.16s", file);
    thread_t *t;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool listed = item_of(id, &t, NULL) != NULL;
    xSemaphoreGive(s_lock);
    if (!listed && strlen(file) == 20) {
        note_store_remove(file);
    }
}

static void sweep_temp(const char *file, void *ctx) {
    (void)ctx;
    note_store_remove(file);
}

static bool ends_with(const char *name, const char *suffix) {
    size_t n = strlen(name), tail = strlen(suffix);
    return n > tail && !strcmp(name + n - tail, suffix);
}

static bool ours(const char *name) {
    return ends_with(name, ".idx") || ends_with(name, ".idx.tmp") || ends_with(name, ".ogg") ||
           ends_with(name, ".ogg.tmp");
}

static void do_mount(void) {
    if (!note_store_mount()) {
        return;
    }
    note_store_sweep(ours);
    note_store_list(".idx", load_index, NULL);
    note_store_list(".idx.tmp", load_index, NULL);
    note_store_list(".ogg", sweep_orphan, NULL);
    note_store_list(".ogg.tmp", sweep_temp, NULL);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    ESP_LOGI(TAG, "%d threads loaded", s_count);
    xSemaphoreGive(s_lock);
}

// Frees space for `need` more bytes by dropping the oldest notes (any
// thread's), never `keep`.
static bool make_room(size_t need, const char *keep) {
    while (note_store_room() < need + ROOM_SPARE) {
        char id[17] = "";
        xSemaphoreTake(s_lock, portMAX_DELAY);
        thread_t *oldest_thread = NULL;
        int oldest = -1;
        for (int t = 0; t < s_count; t++) {
            for (int i = 0; i < s_threads[t].count; i++) {
                ui_note_t *n = &s_threads[t].items[i];
                if (is_note(n) && strcmp(n->id, keep) &&
                    (!oldest_thread || n->time < oldest_thread->items[oldest].time)) {
                    oldest_thread = &s_threads[t];
                    oldest = i;
                }
            }
        }
        if (oldest_thread) {
            snprintf(id, sizeof(id), "%s", oldest_thread->items[oldest].id);
            thread_drop(oldest_thread, oldest);
        }
        xSemaphoreGive(s_lock);
        if (!id[0]) {
            return false;
        }
        char file[24];
        ogg_name(file, sizeof(file), id);
        note_store_remove(file);
        ESP_LOGI(TAG, "dropped note %s for room", id);
    }
    return true;
}

static void set_status(const char *id, ui_note_status_t status) {
    thread_t *t;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    ui_note_t *n = item_of(id, &t, NULL);
    if (n && n->status != status) {
        n->status = status;
        dirty(t);
    }
    xSemaphoreGive(s_lock);
}

static void do_send(job_t *job) {
    char file[24];
    ogg_name(file, sizeof(file), job->id);
    if (job->data) {
        if (!make_room(job->len, job->id) || !note_store_write(file, job->data, job->len)) {
            ESP_LOGW(TAG, "note %s not saved", job->id);
        }
    } else {
        job->data = note_store_read(file, &job->len, NOTE_MAX_BYTES);
    }
    thread_t *t;
    uint8_t wave[UI_NOTE_WAVE] = {0};
    uint32_t ip = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    ui_note_t *n = item_of(job->id, &t, NULL);
    if (n) {
        memcpy(wave, n->wave, sizeof(wave));
        ip = peer_ip(t->key);
    }
    xSemaphoreGive(s_lock);
    int code = n && ip && job->data ? talk_rtsp_send_note(ip, job->id, wave, job->data, job->len) : -1;
    ESP_LOGI(TAG, "note %s sent: %d", job->id, code);
    set_status(job->id, code == 200 ? UI_NOTE_SENT : UI_NOTE_FAILED);
}

static int do_receive(job_t *job) {
    char file[24];
    ogg_name(file, sizeof(file), job->id);
    if (!note_store_ready()) {
        return 503;
    }
    if (!make_room(job->len, job->id) || !note_store_write(file, job->data, job->len)) {
        return 507;
    }
    char evicted[UI_THREAD_ITEMS + 2][17];
    int evicted_count = 0;
    thread_t *t;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!item_of(job->id, &t, NULL)) {
        t = thread_open(job->key, job->name, evicted, &evicted_count);
        ui_note_t *n = thread_add(t, evicted, &evicted_count);
        snprintf(n->id, sizeof(n->id), "%s", job->id);
        n->kind = UI_NOTE_IN;
        n->unread = true;
        n->tenths = (uint16_t)(job->ms / 100);
        memcpy(n->wave, job->wave, UI_NOTE_WAVE);
    }
    xSemaphoreGive(s_lock);
    remove_files(evicted, evicted_count);
    ESP_LOGI(TAG, "note %s from %s, %lu ms", job->id, job->name, (unsigned long)job->ms);
    // Never over a call: the far end is talking.
    if (audio_source_active() != AUDIO_SOURCE_TALK) {
        alarm_sound_notify(ALARM_SOUND_NOTE);
    }
    return 200;
}

static void receive_done(receive_t *rx, int result) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool abandoned = rx->abandoned;
    rx->result = result;
    if (!abandoned) {
        xSemaphoreGive(rx->done);
    }
    xSemaphoreGive(s_lock);
    if (abandoned) {
        vSemaphoreDelete(rx->done);
        free(rx);
    }
}

static void do_play(job_t *job) {
    char file[24];
    ogg_name(file, sizeof(file), job->id);
    size_t len;
    uint8_t *data = note_store_read(file, &len, NOTE_MAX_BYTES);
    if (data) {
        note_audio_play(job->id, data, len);
    } else {
        ESP_LOGW(TAG, "note %s missing", job->id);
    }
}

static void do_receipt(job_t *job) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t ip = peer_ip(job->key);
    xSemaphoreGive(s_lock);
    // Told once: a knob that's away misses it, as it would a call.
    if (ip) {
        talk_rtsp_send_played(ip, job->id);
    }
}

static void do_remove(job_t *job) {
    char file[24];
    if (job->id[0] == 't') {
        // An evicted thread's index, unless its key came back meanwhile.
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool back = thread_of(job->id + 1, NULL) != NULL;
        xSemaphoreGive(s_lock);
        index_name(file, sizeof(file), job->id + 1);
        if (!back) {
            note_store_remove(file);
        }
        return;
    }
    ogg_name(file, sizeof(file), job->id);
    note_store_remove(file);
}

static void worker(void *arg) {
    (void)arg;
    job_t job;
    for (;;) {
        if (xQueueReceive(s_jobs, &job, pdMS_TO_TICKS(WORKER_IDLE_MS)) != pdTRUE) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            if (!uxQueueMessagesWaiting(s_jobs)) {
                s_task = NULL;
                xSemaphoreGive(s_lock);
                vTaskDelete(NULL);
            }
            xSemaphoreGive(s_lock);
            continue;
        }
        switch (job.kind) {
        case JOB_MOUNT:
            do_mount();
            break;
        case JOB_FLUSH:
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_flush_queued = false;
            xSemaphoreGive(s_lock);
            break;
        case JOB_SEND:
            do_send(&job);
            break;
        case JOB_RECEIVE:
            receive_done(job.rx, do_receive(&job));
            break;
        case JOB_PLAY:
            do_play(&job);
            break;
        case JOB_RECEIPT:
            do_receipt(&job);
            break;
        case JOB_REMOVE:
            do_remove(&job);
            break;
        }
        free(job.data);
        do_flush();
    }
}

// ---- The API ----

void notes_init(const char *own_key) {
    (void)own_key;
    s_lock = xSemaphoreCreateMutex();
    s_jobs = xQueueCreateWithCaps(QUEUE_LEN, sizeof(job_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_threads = heap_caps_calloc(STORED_MAX, sizeof(thread_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_ERROR_CHECK(s_lock && s_jobs && s_threads ? ESP_OK : ESP_ERR_NO_MEM);
    post(&(job_t){.kind = JOB_MOUNT});
}

void notes_set_peers(const ui_talk_peer_t *peers, int count) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (count != s_peer_count || memcmp(peers, s_peers, count * sizeof(*peers))) {
        memcpy(s_peers, peers, count * sizeof(*peers));
        s_peer_count = count;
        // Names follow renames, kept for when the knob is away.
        for (int i = 0; i < count; i++) {
            thread_t *t = thread_of(peers[i].key, NULL);
            if (t && strcmp(t->name, peers[i].name)) {
                snprintf(t->name, sizeof(t->name), "%s", peers[i].name);
                s_dirty[t - s_threads] = true;
            }
        }
        changed();
    }
    xSemaphoreGive(s_lock);
}

uint32_t notes_version(void) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t v = s_version;
    xSemaphoreGive(s_lock);
    return v;
}

static int by_activity(const void *a, const void *b) {
    const ui_thread_t *x = a, *y = b;
    if (!x->count != !y->count) {
        return x->count ? -1 : 1;
    }
    if (x->count) {
        uint32_t tx = x->items[x->count - 1].time, ty = y->items[y->count - 1].time;
        if (tx != ty) {
            return tx > ty ? -1 : 1;
        }
    }
    return strcmp(x->name, y->name);
}

void notes_snapshot(ui_notes_t *out) {
    memset(out, 0, sizeof(*out));
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_count && out->count < UI_THREADS_MAX; i++) {
        const thread_t *t = &s_threads[i];
        ui_thread_t *u = &out->threads[out->count++];
        snprintf(u->key, sizeof(u->key), "%s", t->key);
        const char *name = peer_name(t->key);
        snprintf(u->name, sizeof(u->name), "%s", name ? name : t->name);
        u->peer = peer_ip(t->key);
        u->count = t->count;
        memcpy(u->items, t->items, t->count * sizeof(t->items[0]));
        for (int n = 0; n < t->count; n++) {
            u->unread += t->items[n].unread;
        }
    }
    for (int i = 0; i < s_peer_count && out->count < UI_THREADS_MAX; i++) {
        if (thread_of(s_peers[i].key, NULL)) {
            continue;
        }
        ui_thread_t *u = &out->threads[out->count++];
        snprintf(u->key, sizeof(u->key), "%s", s_peers[i].key);
        snprintf(u->name, sizeof(u->name), "%s", s_peers[i].name);
        u->peer = s_peers[i].id;
    }
    xSemaphoreGive(s_lock);
    qsort(out->threads, out->count, sizeof(out->threads[0]), by_activity);
}

void notes_send(const char *key, uint8_t *ogg, size_t len, uint16_t tenths, const uint8_t wave[UI_NOTE_WAVE]) {
    char evicted[UI_THREAD_ITEMS + 2][17];
    int evicted_count = 0;
    job_t job = {.kind = JOB_SEND, .data = ogg, .len = len};
    xSemaphoreTake(s_lock, portMAX_DELAY);
    thread_t *t = thread_open(key, peer_name(key), evicted, &evicted_count);
    ui_note_t *n = thread_add(t, evicted, &evicted_count);
    n->kind = UI_NOTE_OUT;
    n->status = UI_NOTE_SENDING;
    n->tenths = tenths;
    memcpy(n->wave, wave, UI_NOTE_WAVE);
    snprintf(job.id, sizeof(job.id), "%s", n->id);
    xSemaphoreGive(s_lock);
    remove_files(evicted, evicted_count);
    if (!post(&job)) {
        free(ogg);
        set_status(job.id, UI_NOTE_FAILED);
    }
}

void notes_resend(const char *id) {
    thread_t *t;
    job_t job = {.kind = JOB_SEND};
    xSemaphoreTake(s_lock, portMAX_DELAY);
    ui_note_t *n = notes_valid_id(id) ? item_of(id, &t, NULL) : NULL;
    bool ok = n && n->kind == UI_NOTE_OUT && n->status == UI_NOTE_FAILED;
    if (ok) {
        n->status = UI_NOTE_SENDING;
        dirty(t);
    }
    xSemaphoreGive(s_lock);
    snprintf(job.id, sizeof(job.id), "%s", id);
    if (ok && !post(&job)) {
        set_status(id, UI_NOTE_FAILED);
    }
}

void notes_play(const char *id) {
    if (notes_valid_id(id)) {
        job_t job = {.kind = JOB_PLAY};
        snprintf(job.id, sizeof(job.id), "%s", id);
        post(&job);
    }
}

void notes_played(const char *id) {
    thread_t *t;
    job_t job = {.kind = JOB_RECEIPT};
    xSemaphoreTake(s_lock, portMAX_DELAY);
    ui_note_t *n = item_of(id, &t, NULL);
    bool tell = n && n->kind == UI_NOTE_IN && n->unread;
    if (tell) {
        n->unread = false;
        dirty(t);
        flush_locked();
        snprintf(job.key, sizeof(job.key), "%s", t->key);
    }
    xSemaphoreGive(s_lock);
    if (tell) {
        snprintf(job.id, sizeof(job.id), "%s", id);
        post(&job);
    }
}

void notes_remove(const char *id) {
    thread_t *t;
    int index;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    ui_note_t *n = notes_valid_id(id) ? item_of(id, &t, &index) : NULL;
    bool file = n && is_note(n);
    if (n) {
        thread_drop(t, index);
        flush_locked();
    }
    xSemaphoreGive(s_lock);
    if (file) {
        job_t job = {.kind = JOB_REMOVE};
        snprintf(job.id, sizeof(job.id), "%s", id);
        post(&job);
    }
}

void notes_seen(const char *key) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    thread_t *t = thread_of(key, NULL);
    for (int i = 0; t && i < t->count; i++) {
        if (!is_note(&t->items[i]) && t->items[i].unread) {
            t->items[i].unread = false;
            dirty(t);
            flush_locked();
        }
    }
    xSemaphoreGive(s_lock);
}

void notes_log_call(const char *key, const char *name, bool incoming, ui_call_outcome_t outcome, uint32_t seconds) {
    if (!notes_valid_key(key)) {
        return;
    }
    char evicted[UI_THREAD_ITEMS + 2][17];
    int evicted_count = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    thread_t *t = thread_open(key, name, evicted, &evicted_count);
    ui_note_t *n = thread_add(t, evicted, &evicted_count);
    n->kind = incoming ? UI_CALL_IN : UI_CALL_OUT;
    n->status = outcome;
    n->unread = outcome == UI_CALL_MISSED;
    n->tenths = seconds > 6000 ? 60000 : (uint16_t)(seconds * 10);
    flush_locked();
    xSemaphoreGive(s_lock);
    remove_files(evicted, evicted_count);
}

int notes_receive(const char *key, const char *name, const char *id, const uint8_t wave[UI_NOTE_WAVE], uint8_t *ogg,
                  size_t len) {
    uint32_t ms;
    if (!notes_valid_key(key) || !notes_valid_id(id) || len > NOTE_MAX_BYTES || !note_ogg_validate(ogg, len, &ms)) {
        free(ogg);
        return 400;
    }
    thread_t *t;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool known = item_of(id, &t, NULL) != NULL;
    xSemaphoreGive(s_lock);
    if (known) {
        // A resend after a lost reply.
        free(ogg);
        return 200;
    }
    receive_t *rx = calloc(1, sizeof(*rx));
    job_t job = {.kind = JOB_RECEIVE, .data = ogg, .len = len, .ms = ms, .rx = rx};
    snprintf(job.id, sizeof(job.id), "%s", id);
    snprintf(job.key, sizeof(job.key), "%s", key);
    snprintf(job.name, sizeof(job.name), "%s", name);
    memcpy(job.wave, wave, UI_NOTE_WAVE);
    if (rx) {
        rx->done = xSemaphoreCreateBinary();
    }
    if (!rx || !rx->done || !post(&job)) {
        if (rx && rx->done) {
            vSemaphoreDelete(rx->done);
        }
        free(rx);
        free(ogg);
        return 503;
    }
    // A sender that gave up waiting still gets the note filed.
    bool saved = xSemaphoreTake(rx->done, pdMS_TO_TICKS(RECEIVE_WAIT_MS)) == pdTRUE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    // The worker may have finished between the timeout and here.
    saved = saved || xSemaphoreTake(rx->done, 0) == pdTRUE;
    rx->abandoned = !saved;
    int result = rx->result;
    xSemaphoreGive(s_lock);
    if (!saved) {
        ESP_LOGW(TAG, "note %s: save still pending", id);
        return 503;
    }
    vSemaphoreDelete(rx->done);
    free(rx);
    return result;
}

void notes_receipt(const char *key, const char *id) {
    thread_t *t;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    ui_note_t *n = notes_valid_id(id) ? item_of(id, &t, NULL) : NULL;
    if (n && n->kind == UI_NOTE_OUT && !strcmp(t->key, key) && n->status != UI_NOTE_PLAYED) {
        n->status = UI_NOTE_PLAYED;
        dirty(t);
        flush_locked();
    }
    xSemaphoreGive(s_lock);
}
