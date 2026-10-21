#include "runtime.h"
#include "../../firmware/s3/src/device/net/network_jobs.c"

static atomic_int calls[NETWORK_JOB_COUNT], active, block_job=-1, entered, resubmit;
static int order[20000], order_count;
static pthread_mutex_t order_lock=PTHREAD_MUTEX_INITIALIZER;
static void job(int kind) {
    assert(!atomic_fetch_add(&active,1)); // All callbacks are strictly serial.
    atomic_fetch_add(&calls[kind],1);
    pthread_mutex_lock(&order_lock); assert(order_count<20000); order[order_count++]=kind;
    pthread_mutex_unlock(&order_lock);
    atomic_fetch_add(&entered,1);
    while (atomic_load(&block_job)==kind) usleep(50);
    if (kind==NETWORK_JOB_LOCATION && atomic_load(&resubmit)>0) {
        atomic_fetch_sub(&resubmit,1); network_jobs_schedule(NETWORK_JOB_LOCATION,0);
    }
    atomic_fetch_sub(&active,1);
}
static void locate(void) { job(NETWORK_JOB_LOCATION); }
static void weather(void) { job(NETWORK_JOB_WEATHER); }
static void release(void) { job(NETWORK_JOB_RELEASE); }
static void save(void) { job(NETWORK_JOB_LOCATION_SAVE); }
static void download(void) { job(NETWORK_JOB_RELEASE_DOWNLOAD); }
static int64_t wall_us(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
    return ts.tv_sec*1000000LL+ts.tv_nsec/1000;
}
#define WAIT(expr) do { \
    int64_t deadline=wall_us()+5000000; \
    while (!(expr) && wall_us()<deadline) usleep(50); \
    assert(expr); \
} while (0)
static bool parked(void) {
    xSemaphoreTake(s_control,portMAX_DELAY); bool blocked=s_worker && s_waiting && next_job()<0;
    xSemaphoreGive(s_control); return blocked;
}
static void idle(void) {
    WAIT(parked() && atomic_load(&waiting)==1 && !atomic_load(&active));
    assert(atomic_load(&live_tasks)==1 && atomic_load(&created_tasks)==1);
}
static void pump(void) { dispatch(NULL); }
static atomic_int producers;
static void *storm(void *arg) {
    unsigned id=(unsigned)(uintptr_t)arg;
    for (int i=0;i<1000;i++) network_jobs_schedule((network_job_t)(id%NETWORK_JOB_COUNT),0);
    atomic_fetch_sub(&producers,1); return NULL;
}
int main(void) {
    atomic_store(&fail_timer_create,true); network_jobs_init(); assert(!s_timer);
    network_jobs_register(NETWORK_JOB_LOCATION,locate,true);
    network_jobs_register(NETWORK_JOB_WEATHER,weather,true);
    network_jobs_register(NETWORK_JOB_RELEASE,release,true);
    network_jobs_register(NETWORK_JOB_LOCATION_SAVE,save,false);
    network_jobs_register(NETWORK_JOB_RELEASE_DOWNLOAD,download,false);
    atomic_store(&fail_timer_start,true);
    network_jobs_schedule(NETWORK_JOB_LOCATION,0); assert(s_timer && !s_timer_started);
    for (int i=0;i<1000;i++) network_jobs_schedule(NETWORK_JOB_LOCATION,0);
    assert(s_timer_started); pump(); assert(!atomic_load(&created_tasks));
    network_jobs_set_network(true); pump(); assert(!atomic_load(&created_tasks));
    // Persistence can run offline/unsynced, without draining network intent.
    atomic_store(&fail_create,true);
    network_jobs_schedule(NETWORK_JOB_LOCATION_SAVE,0); pump();
    assert(!s_worker && atomic_load(&failed_creates)==1 && s_jobs[3].pending);
    pump(); idle();
    assert(atomic_load(&calls[NETWORK_JOB_LOCATION_SAVE])==1 && !atomic_load(&calls[0]));
    atomic_store(&clock_ready,true); pump(); idle(); assert(atomic_load(&calls[0])==1);
    // Timer callback never waits on a busy lifecycle lock.
    xSemaphoreTake(s_control,portMAX_DELAY); pump(); xSemaphoreGive(s_control);

    int before=atomic_load(&created_tasks);
    network_jobs_schedule(NETWORK_JOB_LOCATION,10);
    network_jobs_schedule(NETWORK_JOB_LOCATION,100); // Later retry cannot postpone work.
    atomic_store(&clock_us,9000000); pump(); assert(atomic_load(&created_tasks)==before);
    atomic_store(&clock_us,10000000); pump(); idle(); assert(atomic_load(&calls[0])==2);
    network_jobs_schedule(NETWORK_JOB_LOCATION,100);
    network_jobs_schedule(NETWORK_JOB_LOCATION,0); pump(); idle(); assert(atomic_load(&calls[0])==3);

    network_jobs_schedule(NETWORK_JOB_WEATHER,0); pump(); idle();
    assert(atomic_load(&calls[1])==1 && atomic_load(&created_tasks)==1);

    // Requests arriving during callbacks remain pending. Self-resubmitting
    // location work cannot jump over the other ready kinds.
    atomic_store(&block_job,NETWORK_JOB_LOCATION); int started=atomic_load(&entered);
    network_jobs_schedule(NETWORK_JOB_LOCATION,0); pump(); WAIT(atomic_load(&entered)>started);
    for (int i=0;i<NETWORK_JOB_COUNT;i++) network_jobs_schedule((network_job_t)i,0);
    atomic_store(&resubmit,3); int base=order_count;
    atomic_store(&block_job,-1); idle();
    assert(order[base]==NETWORK_JOB_WEATHER && order[base+1]==NETWORK_JOB_RELEASE && order[base+2]==NETWORK_JOB_LOCATION_SAVE);

    // Disconnect while a callback is running: future network work waits;
    // pending work resumes after reconnect without another user request.
    atomic_store(&block_job,NETWORK_JOB_LOCATION); started=atomic_load(&entered);
    network_jobs_schedule(NETWORK_JOB_LOCATION,0); pump(); WAIT(atomic_load(&entered)>started);
    int weather_before=atomic_load(&calls[1]); network_jobs_schedule(NETWORK_JOB_WEATHER,0);
    network_jobs_set_network(false); atomic_store(&block_job,-1); idle();
    assert(atomic_load(&calls[1])==weather_before && s_jobs[1].pending);
    network_jobs_set_network(true); pump(); idle(); assert(atomic_load(&calls[1])==weather_before+1);

    // Wakeup between publishing idle and actually blocking must be retained.
    atomic_store(&pause_wait,true); network_jobs_schedule(NETWORK_JOB_RELEASE,0);
    pump(); WAIT(atomic_load(&waiting)==0 && parked());
    int before_wake=atomic_load(&calls[1]);
    network_jobs_schedule(NETWORK_JOB_WEATHER,0); pump();
    atomic_store(&pause_wait,false); idle();
    assert(atomic_load(&calls[1])==before_wake+1 && atomic_load(&created_tasks)==1);

    pthread_t threads[8]; atomic_store(&producers,8);
    for (uintptr_t i=0;i<8;i++) assert(!pthread_create(&threads[i],NULL,storm,(void *)i));
    while (atomic_load(&producers)) { pump(); usleep(50); }
    for (int i=0;i<8;i++) pthread_join(threads[i],NULL);
    pump(); idle();
    xSemaphoreTake(s_control,portMAX_DELAY);
    for (int i=0;i<NETWORK_JOB_COUNT;i++) assert(!s_jobs[i].pending);
    xSemaphoreGive(s_control);
    puts("Coalescing, deadlines, readiness, fairness, create/timer failures, concurrent producers, retained static storage and wakeup race passed");
}
