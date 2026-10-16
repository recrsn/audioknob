/*
 * RTSP/HTTP helpers for the RAOP receiver.
 *
 * Ported from philippe44's AirConnect / squeezelite-esp32 util.c
 * (c) Philippe 2016-2017, philippe_44@outlook.com -- MIT License,
 * https://opensource.org/licenses/MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// NULL-key-terminated header list. Every key/data pair is heap-owned.
typedef struct {
    char *key;
    char *data;
} key_data_t;

// Reads one request (start line + headers + Content-Length body) from sock.
// kd must hold max_headers + 1 entries. *body is heap-allocated (or NULL)
// and NUL-terminated; caller frees it and kd_free()s the headers.
bool http_parse(int sock, char *method, size_t method_size, key_data_t *kd, int max_headers, char **body, int *body_len);

// Sends "<start_line>\r\n<headers>\r\n". Returns false on a short write.
bool http_send(int sock, const char *start_line, const key_data_t *kd);

const char *kd_lookup(const key_data_t *kd, const char *key);
// kd must have room for one more entry plus the NULL terminator.
void kd_add(key_data_t *kd, const char *key, const char *data);
void kd_free(key_data_t *kd);

// Binds a socket of `type` on INADDR_ANY. *port of 0 picks an ephemeral
// port and writes it back. Returns the socket or -1.
int bind_socket(uint16_t *port, int type);
