#include "runtime.h"
#include "stream_player.c"
static void until(bool (*predicate)(void)) {
  int64_t end = esp_timer_get_time() + 3000000;
  while (!predicate()) {
    assert(esp_timer_get_time() < end);
    usleep(1000);
  }
}
static bool is_playing(void) {
  stream_status_t s;
  stream_player_status(&s);
  return s.state == STREAM_PLAYING;
}
static bool net_blocked(void) { return atomic_load(&network_held) > 0; }
static bool no_codec(void) { return atomic_load(&codecs_live) == 0; }
static bool all_gone(void) { return atomic_load(&tasks_live) == 0; }
static bool codec_blocked(void) { return atomic_load(&codec_paused); }
static void finish(void) {
  atomic_store(&hold_network, false);
  atomic_store(&pause_codec, false);
  atomic_store(&pause_net_start, false);
  stream_player_stop();
  assert(stream_player_wait_stopped(3000));
  until(all_gone);
  assert(!atomic_load(&codecs_live));
}

esp_audio_err_t esp_mp3_dec_register(void) { return ESP_AUDIO_ERR_OK; }
esp_audio_err_t esp_aac_dec_register(void) { return ESP_AUDIO_ERR_OK; }
esp_audio_err_t esp_audio_simple_dec_open(esp_audio_simple_dec_cfg_t *cfg,
                                          esp_audio_simple_dec_handle_t *out) {
  (void)cfg;
  *out = malloc(1);
  atomic_fetch_add(&codecs_live, 1);
  return ESP_AUDIO_ERR_OK;
}
void esp_audio_simple_dec_close(esp_audio_simple_dec_handle_t dec) {
  free(dec);
  atomic_fetch_sub(&codecs_live, 1);
}
esp_audio_err_t
esp_audio_simple_dec_get_info(esp_audio_simple_dec_handle_t dec,
                              esp_audio_simple_dec_info_t *info) {
  (void)dec;
  *info = (esp_audio_simple_dec_info_t){
      .sample_rate = 44100, .channel = 2, .bits_per_sample = 16};
  return ESP_AUDIO_ERR_OK;
}
esp_audio_err_t
esp_audio_simple_dec_process(esp_audio_simple_dec_handle_t dec,
                             esp_audio_simple_dec_raw_t *raw,
                             esp_audio_simple_dec_out_t *frame) {
  (void)dec;
  if (atomic_load(&pause_codec)) {
    atomic_store(&codec_paused, true);
    while (atomic_load(&pause_codec))
      usleep(100);
    atomic_store(&codec_paused, false);
  }
  assert(frame->len >= 4096);
  memset(frame->buffer, raw->buffer[0], 4096);
  frame->decoded_size = 4096;
  raw->consumed = raw->len;
  usleep(1000);
  return ESP_AUDIO_ERR_OK;
}
void audio_out_set_rate(uint32_t hz) { (void)hz; }
void audio_out_write16(const int16_t *pcm, size_t frames) {
  (void)pcm;
  (void)frames;
  atomic_fetch_add(&writes, 1);
  usleep(500);
}
stream_net_result_t stream_net_run(const char *url, stream_ring_t *ring,
                                   const stream_net_ops_t *ops, char *error,
                                   size_t len) {
  (void)error;
  (void)len;
  int sockets[2];
  assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, sockets));
  atomic_fetch_add(&net_runs, 1);
  bool accepted = ops->on_connected(ops->ctx, sockets[0]);
  if (accepted) {
    stream_info_t info = {.format = strstr(url, "aac") ? STREAM_FORMAT_AAC
                                                       : STREAM_FORMAT_MP3,
                          .bitrate_kbps = 128};
    ops->on_audio(ops->ctx, &info);
    uint8_t data[65536];
    memset(data, 1, sizeof(data));
    size_t done = 0;
    while (done < sizeof(data) && !ops->should_stop(ops->ctx))
      done += stream_ring_write(ring, data + done, sizeof(data) - done, 20);
    // Model a blocked HTTP/TLS read: socket shutdown must wake it.
    char byte;
    assert(recv(sockets[0], &byte, 1, 0) == 0);
    atomic_fetch_add(&network_held, 1);
    while (atomic_load(&hold_network))
      usleep(100);
    atomic_fetch_sub(&network_held, 1);
  }
  ops->on_disconnected(ops->ctx);
  close(sockets[0]);
  close(sockets[1]);
  return STREAM_NET_STOPPED;
}
int main(void) {
  stream_player_init();
  // Cancellation reaches the decoder even while lower-priority network
  // cleanup is deliberately held. The blocking socket read must wake too.
  atomic_store(&hold_network, true);
  stream_player_play("https://test/mp3");
  until(is_playing);
  stream_player_stop();
  until(net_blocked);
  until(no_codec);
  assert(!stream_player_wait_stopped(10));
  // A stale audio/title callback cannot reactivate a stopped epoch.
  net_t old = {.ring = s_ring, .gen = s_gen - 1};
  stream_info_t info = {.format = STREAM_FORMAT_AAC};
  on_audio(&old, &info);
  on_title(&old, "stale");
  assert(s_out_epoch == NO_EPOCH && !s_dec_playing);
  // Restart while the old network worker is still unwinding.
  int runs = atomic_load(&net_runs);
  stream_player_play("https://test/aac");
  atomic_store(&hold_network, false);
  until(is_playing);
  assert(atomic_load(&net_runs) > runs);
  finish();
  // Stop before task startup must survive lifecycle initialization.
  atomic_store(&pause_net_start, true);
  stream_player_play("https://test/mp3");
  stream_player_stop();
  atomic_store(&pause_net_start, false);
  assert(stream_player_wait_stopped(3000));
  until(all_gone);
  // Stop during an in-flight codec call: its returned PCM must not write.
  atomic_store(&pause_codec, true);
  stream_player_play("https://test/aac");
  until(codec_blocked);
  int before = atomic_load(&writes);
  stream_player_stop();
  atomic_store(&pause_codec, false);
  assert(stream_player_wait_stopped(3000));
  until(all_gone);
  assert(atomic_load(&writes) == before);
  // Station switches wake an old read and preserve a functioning decoder.
  stream_player_play("https://test/mp3");
  until(is_playing);
  runs = atomic_load(&net_runs);
  stream_player_play("https://test/aac");
  until(is_playing);
  assert(atomic_load(&net_runs) > runs);
  finish();
  // Decoder task creation failure can still be stopped and restarted.
  atomic_store(&fail_decoder_task, true);
  stream_player_play("https://test/aac");
  usleep(50000);
  finish();
  stream_player_play("https://test/aac");
  until(is_playing);
  finish();
  vSemaphoreDelete(s_lock);
  vSemaphoreDelete(s_decode_done);
  puts("radio handoff: blocked-read shutdown, independent decoder stop, stale "
       "callbacks, rapid restart, startup cancellation and create failure "
       "passed");
}
