#pragma once

#include <stddef.h>

// Blocking HTTP(S) GET into `buf`, NUL-terminated. HTTPS is verified against
// the ESP-IDF certificate bundle. Returns the body length, or -1 on any
// failure (transport, non-200 status, or a body that doesn't fit).
int http_get(const char *url, char *buf, size_t cap);
