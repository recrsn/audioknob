#pragma once

/*
 * Apple binary property lists ("bplist00"), the body format of most
 * AirPlay 2 RTSP requests and responses.
 *
 * The reader works in place over the received bytes; objects are addressed
 * by index and decoded on access. The writer builds a small object graph
 * and serializes it in one go.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint32_t bp_obj;
#define BP_NONE UINT32_MAX

typedef struct {
    const uint8_t *data;
    size_t len;
    uint8_t offset_size;
    uint8_t ref_size;
    uint64_t num_objects;
    uint64_t root;
    uint64_t offset_table;
} bplist_t;

// False if data is not a well-formed bplist00 trailer and offset table.
bool bplist_open(bplist_t *pl, const uint8_t *data, size_t len);
bp_obj bplist_root(const bplist_t *pl);

// BP_NONE if obj is not a dict or has no such key.
bp_obj bplist_dict_get(const bplist_t *pl, bp_obj dict, const char *key);
bp_obj bplist_array_get(const bplist_t *pl, bp_obj array, size_t index);

// Each is false if obj is missing or of another type.
bool bplist_get_uint(const bplist_t *pl, bp_obj obj, uint64_t *out);
bool bplist_get_bool(const bplist_t *pl, bp_obj obj, bool *out);
// Points into the plist's buffer.
bool bplist_get_data(const bplist_t *pl, bp_obj obj, const uint8_t **data, size_t *len);
// ASCII or UTF-16 string as NUL-terminated UTF-8, truncated to cap.
bool bplist_get_string(const bplist_t *pl, bp_obj obj, char *out, size_t cap);

// Shorthands for dict members.
bool bplist_dict_uint(const bplist_t *pl, bp_obj dict, const char *key, uint64_t *out);
bool bplist_dict_bool(const bplist_t *pl, bp_obj dict, const char *key, bool *out);
bool bplist_dict_string(const bplist_t *pl, bp_obj dict, const char *key, char *out, size_t cap);

typedef struct bpw bpw_t;
typedef int bpw_obj;

bpw_t *bpw_new(void);
void bpw_free(bpw_t *w);

bpw_obj bpw_dict(bpw_t *w);
bpw_obj bpw_array(bpw_t *w);
bpw_obj bpw_string(bpw_t *w, const char *utf8);
bpw_obj bpw_uint(bpw_t *w, uint64_t value);
bpw_obj bpw_bool(bpw_t *w, bool value);
bpw_obj bpw_real(bpw_t *w, double value);
// Copies data.
bpw_obj bpw_data(bpw_t *w, const void *data, size_t len);

void bpw_dict_set(bpw_t *w, bpw_obj dict, const char *key, bpw_obj value);
void bpw_array_add(bpw_t *w, bpw_obj array, bpw_obj value);

// Returns a malloc'd buffer, or NULL if any allocation along the way failed.
uint8_t *bpw_serialize(bpw_t *w, bpw_obj root, size_t *len);
