#include "net_runtime.h"
#include "stream_net.c"
size_t stream_ring_write(stream_ring_t *r, const uint8_t *b, size_t n,
                         TickType_t w) {
  (void)r;
  (void)b;
  (void)w;
  return n;
}
static bool stop(void *ctx) {
  (void)ctx;
  return stop_connect;
}
static bool connected(void *ctx, int socket) {
  (void)ctx;
  assert(socket == 42 && net_order == 1);
  net_order = 2;
  if (net_mode == 1)
    stop_connect = true;
  return !stop_connect;
}
static void disconnected(void *ctx) {
  (void)ctx;
  assert(net_order == 2);
  net_order = 3;
}
int main(void) {
  conn_t connection = {0};
  strcpy(connection.url, "https://test/aac");
  char error[64];
  stream_net_ops_t ops = {.should_stop = stop,
                          .on_connected = connected,
                          .on_disconnected = disconnected};
  for (net_mode = 0; net_mode < 3; net_mode++) {
    net_order = net_fetches = net_cleanups = 0;
    stop_connect = false;
    stream_net_result_t result =
        hop(&connection, NULL, &ops, error, sizeof(error));
    assert(net_cleanups == 1 && net_order == 4);
    assert(net_fetches == (net_mode == 0));
    assert(result == (net_mode == 0   ? STREAM_NET_FAILED
                      : net_mode == 1 ? STREAM_NET_STOPPED
                                      : STREAM_NET_RETRY));
  }
  puts("radio HTTP: publish-after-open, reject cancelled connection, "
       "unpublish-before-cleanup and open failure passed");
}
