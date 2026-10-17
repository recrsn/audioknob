#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifndef MESSAGE_DATA_PATH
#define MESSAGE_DATA_PATH "/data"
#endif
#define VOICE_NOTE_MAX_BYTES (300 * 1024)
void message_store_init(void);
bool message_store_ready(void);
void message_store_usage(size_t *total, size_t *used);
// Names are restricted to fixed internal filenames or hex message IDs.
bool message_store_write(const char *name, const void *data, size_t size);
void *message_store_read(const char *name, size_t *size, size_t max_size);
bool message_store_remove(const char *name);
