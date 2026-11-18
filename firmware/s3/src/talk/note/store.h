#pragma once
#include <stdbool.h>
#include <stddef.h>

// Voice notes and their threads' indexes on the user_data SPIFFS partition.
// Every call reads or writes flash, so it must come from a task with an
// internal stack (notes.c's worker), never LVGL's or a PSRAM-stack task.

#ifndef NOTE_STORE_PATH
#define NOTE_STORE_PATH "/notes"
#endif

// Mounts, formatting a partition that holds no filesystem. Slow the first
// time (the format erases ~7 MB).
bool note_store_mount(void);
bool note_store_ready(void);
// Bytes a write may still use, keeping the quarter SPIFFS needs to collect
// garbage free.
size_t note_store_room(void);
// Names are fixed-format: lowercase hex, '.', '_' only, at most 24 chars.
// A write replaces the file whole: readers see the old one or the new one.
bool note_store_write(const char *name, const void *data, size_t size);
// PSRAM; NULL when missing or over max.
void *note_store_read(const char *name, size_t *size, size_t max);
bool note_store_remove(const char *name);
// Every file whose name ends in suffix.
void note_store_list(const char *suffix, void (*each)(const char *name, void *ctx), void *ctx);
// Removes every file `ours` doesn't claim, such as an older firmware's.
void note_store_sweep(bool (*ours)(const char *name));
