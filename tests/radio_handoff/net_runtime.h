#pragma once
#include "runtime.h"
#define HTTP_EVENT_ON_HEADER 1
#define ESP_ERR_HTTP_EAGAIN 123
static int esp_crt_bundle_attach(void *p) {
  (void)p;
  return 0;
}
typedef struct {
  int event_id;
  void *user_data;
  const char *header_key, *header_value;
} esp_http_client_event_t;
typedef struct {
  const char *url;
  int timeout_ms;
  int (*crt_bundle_attach)(void *);
  const char *user_agent;
  int (*event_handler)(esp_http_client_event_t *);
  void *user_data;
  int buffer_size, buffer_size_tx;
  bool disable_auto_redirect;
} esp_http_client_config_t;
typedef void *esp_http_client_handle_t;
static int net_order, net_mode, net_fetches, net_cleanups;
static bool stop_connect;
static esp_http_client_handle_t
esp_http_client_init(const esp_http_client_config_t *c) {
  (void)c;
  return (void *)1;
}
static int esp_http_client_set_header(void *c, const char *k, const char *v) {
  (void)c;
  (void)k;
  (void)v;
  return 0;
}
static int esp_http_client_open(void *c, int len) {
  (void)c;
  (void)len;
  assert(net_order == 0);
  net_order = 1;
  return net_mode == 2 ? -1 : 0;
}
static int esp_http_client_get_socket(void *c) {
  (void)c;
  return 42;
}
static int esp_http_client_fetch_headers(void *c) {
  (void)c;
  assert(net_order == 2);
  net_fetches++;
  return 0;
}
static int esp_http_client_get_status_code(void *c) {
  (void)c;
  return 404;
}
static int esp_http_client_set_timeout_ms(void *c, int ms) {
  (void)c;
  (void)ms;
  return 0;
}
static int esp_http_client_read(void *c, char *buf, int len) {
  (void)c;
  (void)buf;
  (void)len;
  return 0;
}
static int esp_http_client_cleanup(void *c) {
  (void)c;
  assert(net_order == (net_mode == 2 ? 1 : 3));
  net_cleanups++;
  net_order = 4;
  return 0;
}
