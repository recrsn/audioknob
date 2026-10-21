#include "runtime.h"
#include "audio/out.h"
#include "audio/source.h"
static pthread_mutex_t output_lock = PTHREAD_MUTEX_INITIALIZER;
static audio_out_mix_t output_mix;
static atomic_bool suspended, held, local, block_topup, force_low_queue;
static atomic_int writes, claims;
static void (*preempt)(void);
void audio_out_set_mix(audio_out_mix_t mix) {
    pthread_mutex_lock(&output_lock);
    output_mix = mix;
    pthread_mutex_unlock(&output_lock);
}
void audio_out_write(const int16_t *pcm, size_t frames) {
    (void)pcm;
    pthread_mutex_lock(&output_lock);
    int16_t buf[512 * 2] = {0};
    assert(frames <= 512);
    if (output_mix) output_mix(buf, frames);
    atomic_fetch_add(&writes, 1);
    pthread_mutex_unlock(&output_lock);
    usleep(75);
}
uint32_t audio_out_queued_us(int64_t *now) {
    if (atomic_load(&force_low_queue)) return 0;
    if (atomic_load(&block_topup)) return 20000;
    static bool queued;
    *now = esp_timer_get_time();
    queued = !queued;
    return queued ? 0 : 20000;
}
uint32_t audio_out_underruns(void) { return 0; }
bool audio_source_suspended(void) { return atomic_load(&suspended); }
void audio_source_hold_s3(bool hold) { atomic_store(&held, hold); }
void audio_source_set_local_preempted(void (*cb)(void)) { preempt = cb; }
bool audio_source_claim_local(void) {
    if (atomic_load(&suspended)) return false;
    atomic_store(&local, true);
    atomic_fetch_add(&claims, 1);
    return true;
}
void audio_source_release_local(void) { atomic_store(&local, false); }
#ifdef FOCUS
#include "ambience/focus_sound.c"
#else
#include "audio/alarm_sound.c"
#endif
#define WAIT_FOR(expr) do { \
    int64_t deadline = esp_timer_get_time() + 5000000; \
    while (!(expr) && esp_timer_get_time() < deadline) usleep(100); \
    assert(expr); \
} while (0)
static bool retired(void) {
    xSemaphoreTake(s_control, portMAX_DELAY);
    bool idle = !s_task;
    xSemaphoreGive(s_control);
    return idle;
}
static void idle(void) {
    WAIT_FOR(retired());
    WAIT_FOR(!atomic_load(&live_tasks));
    assert(!atomic_load(&held));
    assert(!atomic_load(&local));
#ifndef FOCUS
    assert(!atomic_load(&claims)); // the system mix must not claim ambience's source
#endif
#ifdef FOCUS
    assert(!s_synth && !s_pcm && !s_buf);
    ambience_kind_t kind;
    assert(!focus_sound_playing(&kind));
#else
    assert(!s_lut && !output_mix && !s_count && !s_requested);
#endif
}
static void play(void) {
#ifdef FOCUS
    focus_sound_play(AMBIENCE_RAIN);
#else
    alarm_sound_start(ALARM_SOUND_TIMER);
#endif
}
static void stop(void) {
#ifdef FOCUS
    focus_sound_stop();
#else
    alarm_sound_stop();
#endif
}
#ifndef FOCUS
static atomic_bool writer_on;
static void *stream_writer(void *arg) {
    (void)arg;
    int16_t pcm[240 * 2] = {0};
    while (atomic_load(&writer_on)) audio_out_write(pcm, 240);
    return NULL;
}
#endif
static void *storm(void *arg) {
    (void)arg;
    for (int i = 0; i < 300; i++) {
        play();
        if (i % 3) stop();
        if (i % 11 == 0) usleep(50);
    }
    return NULL;
}
int main(void) {
#ifdef FOCUS
    focus_sound_init();
    focus_sound_set_volume(50);
#else
    alarm_sound_init();
    alarm_sound_configure(ALARM_SOUND_CHIME, 50);
#endif
    assert(!atomic_load(&created_tasks));
    stop(); idle();
    atomic_store(&fail_create, true);
    play(); idle();
    atomic_store(&fail_alloc, true);
    play(); idle();
    // Queue/intent changes before the task has even started.
    atomic_store(&pause_start, true);
    play();
    for (int i = 0; i < 100; i++) { stop(); play(); }
#ifndef FOCUS
    assert(s_count == COMMAND_CAPACITY);
    alarm_sound_preview(ALARM_SOUND_BREAK_END); // cannot replace a repeating alarm
    assert(s_requested == ALARM_SOUND_TIMER + 1);
#endif
    stop();
    atomic_store(&pause_start, false);
    idle();
    int before = atomic_load(&writes);
    play(); WAIT_FOR(atomic_load(&writes) > before + 10);
#ifdef FOCUS
    int created = atomic_load(&created_tasks);
    stop();
    focus_sound_play(AMBIENCE_FOREST); // restart during fade, then swap kind
    WAIT_FOR(atomic_load(&writes) > before + 120);
    assert(atomic_load(&created_tasks) == created && !retired());
    atomic_store(&local, false); // incoming stream has already taken ownership
    preempt();
    before = atomic_load(&writes);
    idle();
    assert(atomic_load(&writes) == before);
#else
    assert(atomic_load(&held));
    atomic_store(&suspended, true);
    idle();
    play(); idle();
    atomic_store(&suspended, false);
    // A competing writer/cursor can keep reported depth below the watermark.
    // The queue consumer must still return to control/completion processing.
    atomic_store(&force_low_queue, true);
    alarm_sound_preview(ALARM_SOUND_FOCUS_END);
    idle(); // once ends without any caller issuing stop, even with a low cursor
    before = atomic_load(&writes);
    play();
    WAIT_FOR(atomic_load(&writes) > before + 10);
    stop(); // a repeating sound must also consume cancellation under this load
    idle();
    atomic_store(&force_low_queue, false);
    // Freeze an active worker at its period boundary. Publish completion of
    // the old target, then replace it while no writer advances the mix hook.
    play();
    before = atomic_load(&writes);
    WAIT_FOR(atomic_load(&writes) > before + 10);
    atomic_store(&pause_period, true);
    WAIT_FOR(atomic_load(&at_period));
    atomic_store(&block_topup, true);
    __atomic_store_n(&s_done_target, ALARM_SOUND_TIMER + 1, __ATOMIC_RELAXED);
    alarm_sound_start(ALARM_SOUND_FOCUS_END);
    atomic_store(&pause_period, false);
    WAIT_FOR(__atomic_load_n(&s_target, __ATOMIC_ACQUIRE) == ALARM_SOUND_FOCUS_END + 1);
    usleep(2000);
    assert(atomic_load(&held) && !retired());
    atomic_store(&block_topup, false);
    stop(); idle();
#endif
    // Pause after handle retirement but before actual task deletion. A new
    // request must get a successor and the old task must not touch its state.
    atomic_store(&pause_delete, true);
    play(); stop();
    WAIT_FOR(atomic_load(&at_delete));
    assert(retired());
    before = atomic_load(&writes);
    play(); WAIT_FOR(atomic_load(&writes) > before + 10);
    atomic_store(&pause_delete, false);
    stop(); idle();
#ifndef FOCUS
    // A stream may be inside the mix hook while the worker unhooks/frees its LUT.
    pthread_t writer;
    atomic_store(&writer_on, true);
    assert(!pthread_create(&writer, NULL, stream_writer, NULL));
#endif
    pthread_t threads[4];
    for (int i = 0; i < 4; i++) assert(!pthread_create(&threads[i], NULL, storm, NULL));
    for (int i = 0; i < 4; i++) pthread_join(threads[i], NULL);
    stop(); idle();
#ifndef FOCUS
    atomic_store(&writer_on, false);
    pthread_join(writer, NULL);
#endif
    assert(atomic_load(&created_tasks) == atomic_load(&deleted_tasks));
    puts("sound lifecycle checks passed");
}
