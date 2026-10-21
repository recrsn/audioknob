#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#define TAG JOBS_TAG
#include "net/network_jobs.c"
#undef TAG
#include "online/online.c"

static int mutex;
static jmp_buf blocked;
static bool synced = true;
static void (*task_fn)(void *);
static unsigned created_tasks;
static int allocation_failures;
static int64_t clock_ms, stop_ms, action_ms;
static void (*action)(void), (*during_geo)(void), (*during_write)(void), (*during_weather)(void);
static geo_t disk[2], pending[2];
static bool disk_have[2], pending_have[2];
static int open_failures, write_failures, commit_failures;
static int geo_failures, weather_failures, geo_calls, weather_calls, air_calls;
static bool air_fails;
static int64_t geo_times[64], weather_times[512];
static double weather_lats[512], air_lat;
static geo_t fresh;
static const geo_t auto_a = {.city="A", .lat=12, .lon=34, .tz="Asia/Kolkata", .has_offset=true, .tz_offset_s=19800};
static const geo_t auto_b = {.city="B", .lat=56, .lon=78, .tz="Europe/London"};
static const geo_t manual = {.city="Manual", .lat=-12, .lon=-34, .tz="America/New_York"};

void cJSON_InitHooks(cJSON_Hooks *h) { assert(h->malloc_fn && h->free_fn); }
void *heap_caps_malloc(size_t size, unsigned caps) {
    assert(size == BODY_CAP && caps == MALLOC_CAP_SPIRAM);
    if (allocation_failures) { allocation_failures--; return NULL; }
    return malloc(size);
}
SemaphoreHandle_t xSemaphoreCreateMutex(void) { mutex=1; return &mutex; }
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage) { *storage=1; return storage; }
int xSemaphoreTake(SemaphoreHandle_t sem, uint32_t ticks) {
    if (!*sem && !ticks) return 0;
    assert(*sem); *sem=0; return 1;
}
int xSemaphoreGive(SemaphoreHandle_t sem) { assert(!*sem); *sem=1; return 1; }
int64_t esp_timer_get_time(void) { return clock_ms * 1000; }
int esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *out) {
    assert(args->callback==dispatch); *out=&mutex; return ESP_OK;
}
int esp_timer_start_periodic(esp_timer_handle_t timer, uint64_t interval) {
    assert(timer && interval==TIMER_US); return ESP_OK;
}
bool ntp_is_synced(void) { return synced; }
void vTaskDelay(uint32_t ticks) { assert(ticks==1); }
TaskHandle_t xTaskCreateStatic(void (*fn)(void *), const char *name, unsigned stack, void *arg,
                             unsigned priority, StackType_t *buffer, StaticTask_t *tcb) {
    assert(fn==worker && !strcmp(name,"network_jobs") && stack==8192 && !arg && priority==2);
    assert(buffer==s_stack && tcb==&s_task_storage);
    assert(!task_fn); task_fn=fn; created_tasks++; return &mutex;
}
uint32_t ulTaskNotifyTake(int clear, uint32_t ticks) {
    assert(clear==pdTRUE && ticks==portMAX_DELAY && s_waiting);
    longjmp(blocked,1);
}
void xTaskNotifyGive(TaskHandle_t task) { assert(task==s_worker); task_fn=worker; }
void settings_nvs_init(void) {}
static int key_index(const char *key) { assert(!strcmp(key,"manual") || !strcmp(key,"detected")); return !strcmp(key,"manual"); }
int nvs_open(const char *name, int mode, nvs_handle_t *out) {
    assert(!strcmp(name, "location"));
    if (mode == NVS_READWRITE && open_failures) { open_failures--; return 2; }
    memcpy(pending, disk, sizeof(disk)); memcpy(pending_have, disk_have, sizeof(disk_have));
    *out = 1; return ESP_OK;
}
int nvs_get_blob(nvs_handle_t nvs, const char *key, void *out, size_t *len) {
    assert(nvs == 1 && *len == sizeof(geo_t)); int i=key_index(key);
    if (!disk_have[i]) return ESP_ERR_NVS_NOT_FOUND;
    memcpy(out, &disk[i], *len); return ESP_OK;
}
int nvs_set_blob(nvs_handle_t nvs, const char *key, const void *value, size_t len) {
    assert(nvs == 1 && len == sizeof(geo_t));
    if (write_failures) { write_failures--; return 2; }
    int i=key_index(key); memcpy(&pending[i],value,len); pending_have[i]=true;
    if (during_write) { void (*fn)(void)=during_write; during_write=NULL; fn(); }
    return ESP_OK;
}
int nvs_erase_key(nvs_handle_t nvs, const char *key) {
    assert(nvs == 1); int i=key_index(key);
    bool had=pending_have[i]; pending_have[i]=false; return had ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}
int nvs_commit(nvs_handle_t nvs) {
    assert(nvs == 1);
    if (commit_failures) { commit_failures--; return 2; }
    memcpy(disk,pending,sizeof(disk)); memcpy(disk_have,pending_have,sizeof(disk_have)); return ESP_OK;
}
void nvs_close(nvs_handle_t nvs) { assert(nvs == 1); }
bool geo_lookup(geo_t *out, char *buf, int cap) {
    assert(buf && cap == BODY_CAP && geo_calls < 64);
    geo_times[geo_calls++] = clock_ms/1000;
    if (during_geo) { void (*fn)(void)=during_geo; during_geo=NULL; fn(); }
    if (geo_failures) { geo_failures--; return false; }
    *out = fresh; return true;
}
bool weather_fetch(double lat, double lon, ui_weather_t *out, char *buf, int cap) {
    (void)lon; assert(out->valid && buf && cap==BODY_CAP && weather_calls < 512);
    weather_times[weather_calls]=clock_ms/1000; weather_lats[weather_calls++]=lat;
    if (during_weather) { void (*fn)(void)=during_weather; during_weather=NULL; fn(); }
    if (weather_failures) { weather_failures--; return false; }
    return true;
}
bool air_quality_fetch(double lat, double lon, ui_weather_t *out, char *buf, int cap) {
    (void)lon; assert(out->valid && buf && cap==BODY_CAP); air_calls++; air_lat=lat; return !air_fails;
}
static void boot(void) {
    memset(&s_loc,0,sizeof(s_loc)); s_loc.version=1;
    memset(&s_weather,0,sizeof(s_weather)); s_weather_version=0;
    clock_ms=0; geo_calls=weather_calls=air_calls=0;
    created_tasks=0; task_fn=NULL; allocation_failures=0; synced=true;
    memset(s_jobs,0,sizeof(s_jobs)); s_cursor=0; s_worker=NULL; s_waiting=false;
    s_timer=NULL; s_timer_started=false; s_network_up=false;
    s_last_geo_request=s_weather_for=0; s_geo_retry_s=s_weather_retry_s=RETRY_MIN_S;
    action=NULL; during_geo=NULL; during_write=NULL; during_weather=NULL;
    geo_failures=weather_failures=open_failures=write_failures=commit_failures=0;
    air_fails=false; fresh=auto_a; online_start(); online_network_up();
}
static void reset(void) { memset(disk_have,0,sizeof(disk_have)); memset(disk,0,sizeof(disk)); boot(); }
static void run(int seconds) {
    stop_ms=(int64_t)seconds*1000;
    while (clock_ms < stop_ms) {
        if (action && clock_ms >= action_ms) {
            void (*fn)(void)=action; action=NULL; fn();
        }
        dispatch(NULL);
        if (task_fn) {
            void (*fn)(void *)=task_fn; task_fn=NULL;
            if (!setjmp(blocked)) fn(NULL);
        }
        assert(mutex==1 && (!s_worker || s_waiting));
        int64_t next=clock_ms+1000;
        if (action && action_ms<next) next=action_ms;
        clock_ms=next;
    }
}
static void select_manual(void) { online_set_location(&manual); }
static void select_auto(void) { online_set_location(NULL); }
static void detect(void) { online_detect_location(); }
static void select_b(void) { online_set_location(&auto_b); }
static void expect_location(const geo_t *expected, bool is_manual) {
    geo_t geo; bool m; assert(online_get_location(&geo,&m));
    assert(m==is_manual && !memcmp(&geo,expected,sizeof(geo)));
}
int main(void) {
    // Saved auto survives boot, network-up and multiple old 12-hour deadlines.
    reset(); disk[0]=auto_a; disk_have[0]=true; boot(); run(24*3600+1);
    assert(geo_calls==0 && weather_calls==97 && air_calls==97);
    for (int i=0;i<weather_calls;i++) assert(weather_times[i]==i*900 && weather_lats[i]==auto_a.lat);
    expect_location(&auto_a,false);

    // Missing data retries 30,60,120,...600 seconds until success, then stops.
    reset(); geo_failures=7; run(3000);
    int64_t expected[]={0,30,90,210,450,930,1530,2130};
    assert(geo_calls==8 && !memcmp(expected,geo_times,sizeof(expected)));
    assert(disk_have[0] && !memcmp(&disk[0],&auto_a,sizeof(geo_t)));
    boot(); run(1300); assert(geo_calls==0 && weather_calls==2);

    // Manual wins at boot and throughout an explicit cache refresh.
    reset(); disk[0]=auto_a; disk_have[0]=true; disk[1]=manual; disk_have[1]=true;
    boot(); action=detect; action_ms=10000; fresh=auto_b; run(1000);
    assert(geo_calls==1 && geo_times[0]==10 && weather_calls==2 && air_lat==manual.lat);
    expect_location(&manual,true); assert(!memcmp(&disk[0],&auto_b,sizeof(geo_t)));
    boot(); action=select_auto; action_ms=10000; run(1000);
    assert(geo_calls==0 && weather_lats[1]==auto_b.lat && !disk_have[1]);
    expect_location(&auto_b,false);

    // Manual only: no background detection until auto is explicitly selected.
    reset(); disk[1]=manual; disk_have[1]=true; boot(); run(1000); assert(geo_calls==0);
    boot(); action=select_auto; action_ms=10000; run(100);
    assert(geo_calls==1 && geo_times[0]==10); expect_location(&auto_a,false);

    // Explicit detection preserves cached coordinates through failures, while
    // weather continues every 15 minutes and geo backoff reaches its cap.
    reset(); disk[0]=auto_a; disk_have[0]=true; boot(); fresh=auto_b;
    action=detect; action_ms=10000; geo_failures=7; run(2200);
    assert(geo_calls==8 && geo_times[7]==2140);
    assert(weather_calls==4 && weather_times[1]==900 && weather_times[2]==1800);
    assert(weather_lats[1]==auto_a.lat && weather_lats[3]==auto_b.lat);
    expect_location(&auto_b,false);

    // A fresh explicit request bypasses the old retry deadline.
    reset(); online_detect_location(); geo_failures=1;
    action=detect; action_ms=10000; run(100);
    assert(geo_calls==2 && geo_times[0]==0 && geo_times[1]==10);

    // A new request during an in-flight lookup is not lost.
    reset(); online_detect_location(); during_geo=detect; run(100);
    assert(geo_calls==2 && s_loc.detect_completed==s_loc.detect_requested);
    // A manual selection during a lookup still determines forecast/timezone.
    reset(); during_geo=select_manual; run(100);
    assert(geo_calls==1 && weather_calls==1 && weather_lats[0]==manual.lat);
    expect_location(&manual,true);

    // Successful HTTP detection is not repeated when NVS open/set/commit fail.
    reset(); open_failures=1; write_failures=1; commit_failures=1; run(100);
    assert(geo_calls==1 && disk_have[0] && !s_loc.detected_dirty);
    boot(); run(100); assert(geo_calls==0);
    // A manual change during NVS write must persist on the next iteration.
    reset(); online_set_location(&manual); during_write=select_b; run(100);
    assert(disk_have[1] && !memcmp(&disk[1],&auto_b,sizeof(geo_t)) && !s_loc.manual_dirty);

    // Failed deletion of a manual pin is retried and survives the next reboot.
    reset(); disk[0]=auto_a; disk_have[0]=true; disk[1]=manual; disk_have[1]=true;
    boot(); online_set_location(NULL); commit_failures=1; run(100);
    assert(!disk_have[1] && !s_loc.manual_dirty && geo_calls==0);
    boot(); run(100); expect_location(&auto_a,false);

    // Optional air-quality failure does not change the 15-minute cadence.
    reset(); disk[0]=auto_a; disk_have[0]=true; boot(); air_fails=true; run(1801);
    assert(geo_calls==0 && air_calls==3 && weather_calls==3 && s_weather.valid);
    assert(weather_times[1]==900 && weather_times[2]==1800);

    // Selection changes during weather HTTP discard the obsolete forecast.
    reset(); disk[0]=auto_a; disk_have[0]=true; boot(); during_weather=select_manual; run(100);
    assert(weather_calls==2 && weather_lats[0]==auto_a.lat && weather_lats[1]==manual.lat);
    assert(!strcmp(s_weather.place,manual.city) && disk_have[1]);

    // No workers while WiFi/clock are unavailable; storage still runs offline.
    reset(); network_jobs_set_network(false); synced=false;
    online_set_location(&manual); run(100);
    assert(created_tasks==1 && disk_have[1] && !geo_calls && !weather_calls);
    clock_ms=0; online_network_up(); run(100);
    assert(created_tasks==1 && !weather_calls);
    clock_ms=0; synced=true; run(100);
    assert(weather_calls==1 && weather_lats[0]==manual.lat && !geo_calls);

    // Response-allocation failures retain detection intent and retry.
    reset(); allocation_failures=1; run(100);
    assert(geo_calls==1 && geo_times[0]==30 && disk_have[0]);

    // Weather keeps its own existing retry sequence, without triggering geo.
    reset(); disk[0]=auto_a; disk_have[0]=true; boot(); weather_failures=3; run(1200);
    assert(geo_calls==0 && weather_calls==5 && air_calls==2);
    assert(weather_times[0]==0 && weather_times[1]==30 && weather_times[2]==90 && weather_times[3]==210 && weather_times[4]==1110);
    puts("Location cache, reboot, overrides, explicit detection, backoff, NVS retries and weather cadence passed");
}
