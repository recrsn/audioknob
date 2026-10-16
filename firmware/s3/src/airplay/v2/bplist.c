#ifndef AK_NATIVE

#include "bplist.h"

#include <stdlib.h>
#include <string.h>

#define MAGIC "bplist00"
#define MAGIC_LEN 8
#define TRAILER_LEN 32

enum {
    T_SIMPLE = 0x0,
    T_INT = 0x1,
    T_REAL = 0x2,
    T_DATA = 0x4,
    T_ASCII = 0x5,
    T_UTF16 = 0x6,
    T_ARRAY = 0xA,
    T_DICT = 0xD,
};

#define SIMPLE_FALSE 0x08
#define SIMPLE_TRUE 0x09

static uint64_t read_be(const uint8_t *p, size_t n) {
    uint64_t v = 0;
    for (size_t i = 0; i < n; i++) {
        v = (v << 8) | p[i];
    }
    return v;
}

static bool object_offset(const bplist_t *pl, bp_obj obj, size_t *offset) {
    if (obj == BP_NONE || obj >= pl->num_objects) {
        return false;
    }
    uint64_t off = read_be(pl->data + pl->offset_table + (size_t)obj * pl->offset_size, pl->offset_size);
    if (off < MAGIC_LEN || off >= pl->offset_table) {
        return false;
    }
    *offset = (size_t)off;
    return true;
}

// Decodes an object's marker into its type and element count, and points
// *body at what follows the (possibly extended) count.
static bool object_header(const bplist_t *pl, bp_obj obj, uint8_t *type, uint64_t *count, size_t *body) {
    size_t off;
    if (!object_offset(pl, obj, &off)) {
        return false;
    }
    uint8_t marker = pl->data[off];
    *type = marker >> 4;
    *count = marker & 0x0F;
    *body = off + 1;
    bool counted = *type == T_DATA || *type == T_ASCII || *type == T_UTF16 || *type == T_ARRAY || *type == T_DICT;
    if (counted && *count == 0x0F) {
        if (*body >= pl->offset_table || pl->data[*body] >> 4 != T_INT) {
            return false;
        }
        size_t n = (size_t)1 << (pl->data[*body] & 0x0F);
        if (n > 8 || *body + 1 + n > pl->offset_table) {
            return false;
        }
        *count = read_be(pl->data + *body + 1, n);
        *body += 1 + n;
    }
    return true;
}

static bool span_ok(const bplist_t *pl, size_t start, uint64_t len) {
    return start <= pl->offset_table && len <= pl->offset_table - start;
}

bool bplist_open(bplist_t *pl, const uint8_t *data, size_t len) {
    memset(pl, 0, sizeof(*pl));
    if (len < MAGIC_LEN + TRAILER_LEN || memcmp(data, MAGIC, MAGIC_LEN) != 0) {
        return false;
    }
    const uint8_t *t = data + len - TRAILER_LEN;
    pl->data = data;
    pl->len = len;
    pl->offset_size = t[6];
    pl->ref_size = t[7];
    pl->num_objects = read_be(t + 8, 8);
    pl->root = read_be(t + 16, 8);
    pl->offset_table = read_be(t + 24, 8);

    if (pl->offset_size < 1 || pl->offset_size > 8 || pl->ref_size < 1 || pl->ref_size > 8) {
        return false;
    }
    uint64_t table_end = len - TRAILER_LEN;
    if (pl->offset_table < MAGIC_LEN || pl->offset_table > table_end || pl->num_objects == 0 ||
        pl->num_objects > (table_end - pl->offset_table) / pl->offset_size || pl->root >= pl->num_objects) {
        return false;
    }
    return true;
}

bp_obj bplist_root(const bplist_t *pl) {
    return (bp_obj)pl->root;
}

static bp_obj read_ref(const bplist_t *pl, size_t at) {
    uint64_t ref = read_be(pl->data + at, pl->ref_size);
    return ref < pl->num_objects ? (bp_obj)ref : BP_NONE;
}

static bool key_equals(const bplist_t *pl, bp_obj obj, const char *key) {
    uint8_t type;
    uint64_t count;
    size_t body;
    if (!object_header(pl, obj, &type, &count, &body)) {
        return false;
    }
    size_t klen = strlen(key);
    if (type == T_ASCII) {
        return count == klen && span_ok(pl, body, count) && memcmp(pl->data + body, key, klen) == 0;
    }
    if (type == T_UTF16) {
        if (count != klen || !span_ok(pl, body, count * 2)) {
            return false;
        }
        for (size_t i = 0; i < klen; i++) {
            if (read_be(pl->data + body + i * 2, 2) != (uint8_t)key[i]) {
                return false;
            }
        }
        return true;
    }
    return false;
}

bp_obj bplist_dict_get(const bplist_t *pl, bp_obj dict, const char *key) {
    uint8_t type;
    uint64_t count;
    size_t body;
    if (!object_header(pl, dict, &type, &count, &body) || type != T_DICT ||
        !span_ok(pl, body, count * 2 * pl->ref_size)) {
        return BP_NONE;
    }
    for (uint64_t i = 0; i < count; i++) {
        if (key_equals(pl, read_ref(pl, body + i * pl->ref_size), key)) {
            return read_ref(pl, body + (count + i) * pl->ref_size);
        }
    }
    return BP_NONE;
}

bp_obj bplist_array_get(const bplist_t *pl, bp_obj array, size_t index) {
    uint8_t type;
    uint64_t count;
    size_t body;
    if (!object_header(pl, array, &type, &count, &body) || type != T_ARRAY || index >= count ||
        !span_ok(pl, body, count * pl->ref_size)) {
        return BP_NONE;
    }
    return read_ref(pl, body + index * pl->ref_size);
}

bool bplist_get_uint(const bplist_t *pl, bp_obj obj, uint64_t *out) {
    uint8_t type;
    uint64_t count;
    size_t body;
    if (!object_header(pl, obj, &type, &count, &body) || type != T_INT) {
        return false;
    }
    size_t n = (size_t)1 << count;
    if (n > 16 || !span_ok(pl, body, n)) {
        return false;
    }
    // 16-byte integers hold unsigned 64-bit values in their low half.
    *out = n == 16 ? read_be(pl->data + body + 8, 8) : read_be(pl->data + body, n);
    return true;
}

bool bplist_get_bool(const bplist_t *pl, bp_obj obj, bool *out) {
    size_t off;
    if (!object_offset(pl, obj, &off)) {
        return false;
    }
    uint8_t marker = pl->data[off];
    if (marker != SIMPLE_FALSE && marker != SIMPLE_TRUE) {
        return false;
    }
    *out = marker == SIMPLE_TRUE;
    return true;
}

bool bplist_get_data(const bplist_t *pl, bp_obj obj, const uint8_t **data, size_t *len) {
    uint8_t type;
    uint64_t count;
    size_t body;
    if (!object_header(pl, obj, &type, &count, &body) || type != T_DATA || !span_ok(pl, body, count)) {
        return false;
    }
    *data = pl->data + body;
    *len = (size_t)count;
    return true;
}

static size_t utf8_put(char *out, size_t cap, size_t pos, uint32_t cp) {
    char buf[4];
    size_t n;
    if (cp < 0x80) {
        buf[0] = (char)cp;
        n = 1;
    } else if (cp < 0x800) {
        buf[0] = (char)(0xC0 | (cp >> 6));
        buf[1] = (char)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        buf[0] = (char)(0xE0 | (cp >> 12));
        buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        buf[0] = (char)(0xF0 | (cp >> 18));
        buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[3] = (char)(0x80 | (cp & 0x3F));
        n = 4;
    }
    // Never split a character: stop at the last one that fits.
    if (pos + n >= cap) {
        return 0;
    }
    memcpy(out + pos, buf, n);
    return n;
}

bool bplist_get_string(const bplist_t *pl, bp_obj obj, char *out, size_t cap) {
    uint8_t type;
    uint64_t count;
    size_t body;
    if (cap == 0 || !object_header(pl, obj, &type, &count, &body)) {
        return false;
    }
    size_t pos = 0;
    if (type == T_ASCII) {
        if (!span_ok(pl, body, count)) {
            return false;
        }
        pos = count < cap - 1 ? (size_t)count : cap - 1;
        memcpy(out, pl->data + body, pos);
    } else if (type == T_UTF16) {
        if (!span_ok(pl, body, count * 2)) {
            return false;
        }
        for (uint64_t i = 0; i < count; i++) {
            uint32_t cp = (uint32_t)read_be(pl->data + body + i * 2, 2);
            if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < count) {
                uint32_t lo = (uint32_t)read_be(pl->data + body + (i + 1) * 2, 2);
                if (lo >= 0xDC00 && lo < 0xE000) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    i++;
                }
            }
            size_t n = utf8_put(out, cap, pos, cp);
            if (n == 0) {
                break;
            }
            pos += n;
        }
    } else {
        return false;
    }
    out[pos] = '\0';
    return true;
}

bool bplist_dict_uint(const bplist_t *pl, bp_obj dict, const char *key, uint64_t *out) {
    return bplist_get_uint(pl, bplist_dict_get(pl, dict, key), out);
}

bool bplist_dict_bool(const bplist_t *pl, bp_obj dict, const char *key, bool *out) {
    return bplist_get_bool(pl, bplist_dict_get(pl, dict, key), out);
}

bool bplist_dict_string(const bplist_t *pl, bp_obj dict, const char *key, char *out, size_t cap) {
    return bplist_get_string(pl, bplist_dict_get(pl, dict, key), out, cap);
}

/* Writer */

typedef struct {
    uint8_t type;
    union {
        uint64_t u;
        double r;
        bool b;
        struct {
            uint8_t *p;
            size_t len;  // bytes; UTF-16 strings hold big-endian code units
        } buf;
        struct {
            bpw_obj *items;  // dicts: key, value, key, value...
            size_t n, cap;
        } list;
    };
} wobj_t;

struct bpw {
    wobj_t *objs;
    size_t n, cap;
    bool failed;
};

bpw_t *bpw_new(void) {
    return calloc(1, sizeof(bpw_t));
}

void bpw_free(bpw_t *w) {
    if (!w) {
        return;
    }
    for (size_t i = 0; i < w->n; i++) {
        wobj_t *o = &w->objs[i];
        if (o->type == T_DATA || o->type == T_ASCII || o->type == T_UTF16) {
            free(o->buf.p);
        } else if (o->type == T_ARRAY || o->type == T_DICT) {
            free(o->list.items);
        }
    }
    free(w->objs);
    free(w);
}

static wobj_t *new_obj(bpw_t *w, uint8_t type, bpw_obj *id) {
    if (w->n == w->cap) {
        size_t cap = w->cap ? w->cap * 2 : 16;
        wobj_t *objs = realloc(w->objs, cap * sizeof(*objs));
        if (!objs) {
            w->failed = true;
            *id = -1;
            return NULL;
        }
        w->objs = objs;
        w->cap = cap;
    }
    *id = (bpw_obj)w->n;
    wobj_t *o = &w->objs[w->n++];
    memset(o, 0, sizeof(*o));
    o->type = type;
    return o;
}

bpw_obj bpw_dict(bpw_t *w) {
    bpw_obj id;
    new_obj(w, T_DICT, &id);
    return id;
}

bpw_obj bpw_array(bpw_t *w) {
    bpw_obj id;
    new_obj(w, T_ARRAY, &id);
    return id;
}

static bool is_ascii(const char *s) {
    for (; *s; s++) {
        if ((uint8_t)*s >= 0x80) {
            return false;
        }
    }
    return true;
}

// UTF-8 to big-endian UTF-16; malformed bytes become U+FFFD.
static uint8_t *utf8_to_utf16be(const char *s, size_t *out_len) {
    size_t slen = strlen(s);
    uint8_t *out = malloc(slen * 4 + 1);
    if (!out) {
        return NULL;
    }
    size_t pos = 0;
    const uint8_t *p = (const uint8_t *)s, *end = p + slen;
    while (p < end) {
        uint32_t cp;
        size_t n;
        if (*p < 0x80) {
            cp = *p;
            n = 1;
        } else if ((*p & 0xE0) == 0xC0) {
            cp = *p & 0x1F;
            n = 2;
        } else if ((*p & 0xF0) == 0xE0) {
            cp = *p & 0x0F;
            n = 3;
        } else if ((*p & 0xF8) == 0xF0) {
            cp = *p & 0x07;
            n = 4;
        } else {
            cp = 0xFFFD;
            n = 1;
        }
        if (n > 1) {
            if ((size_t)(end - p) < n) {
                cp = 0xFFFD;
                n = (size_t)(end - p);
            } else {
                for (size_t i = 1; i < n; i++) {
                    if ((p[i] & 0xC0) != 0x80) {
                        cp = 0xFFFD;
                        n = i;
                        break;
                    }
                    cp = (cp << 6) | (p[i] & 0x3F);
                }
            }
        }
        p += n;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            uint16_t hi = 0xD800 + (cp >> 10), lo = 0xDC00 + (cp & 0x3FF);
            out[pos++] = hi >> 8;
            out[pos++] = hi & 0xFF;
            out[pos++] = lo >> 8;
            out[pos++] = lo & 0xFF;
        } else {
            out[pos++] = cp >> 8;
            out[pos++] = cp & 0xFF;
        }
    }
    *out_len = pos;
    return out;
}

bpw_obj bpw_string(bpw_t *w, const char *utf8) {
    bpw_obj id;
    bool ascii = is_ascii(utf8);
    wobj_t *o = new_obj(w, ascii ? T_ASCII : T_UTF16, &id);
    if (!o) {
        return id;
    }
    if (ascii) {
        o->buf.len = strlen(utf8);
        o->buf.p = malloc(o->buf.len + 1);
        if (o->buf.p) {
            memcpy(o->buf.p, utf8, o->buf.len);
        }
    } else {
        o->buf.p = utf8_to_utf16be(utf8, &o->buf.len);
    }
    if (!o->buf.p) {
        w->failed = true;
    }
    return id;
}

bpw_obj bpw_uint(bpw_t *w, uint64_t value) {
    bpw_obj id;
    wobj_t *o = new_obj(w, T_INT, &id);
    if (o) {
        o->u = value;
    }
    return id;
}

bpw_obj bpw_bool(bpw_t *w, bool value) {
    bpw_obj id;
    wobj_t *o = new_obj(w, T_SIMPLE, &id);
    if (o) {
        o->b = value;
    }
    return id;
}

bpw_obj bpw_real(bpw_t *w, double value) {
    bpw_obj id;
    wobj_t *o = new_obj(w, T_REAL, &id);
    if (o) {
        o->r = value;
    }
    return id;
}

bpw_obj bpw_data(bpw_t *w, const void *data, size_t len) {
    bpw_obj id;
    wobj_t *o = new_obj(w, T_DATA, &id);
    if (!o) {
        return id;
    }
    o->buf.p = malloc(len ? len : 1);
    o->buf.len = len;
    if (o->buf.p) {
        memcpy(o->buf.p, data, len);
    } else {
        w->failed = true;
    }
    return id;
}

static void list_push(bpw_t *w, bpw_obj container, bpw_obj item) {
    if (container < 0 || item < 0) {
        w->failed = true;
        return;
    }
    wobj_t *o = &w->objs[container];
    if (o->list.n == o->list.cap) {
        size_t cap = o->list.cap ? o->list.cap * 2 : 8;
        bpw_obj *items = realloc(o->list.items, cap * sizeof(*items));
        if (!items) {
            w->failed = true;
            return;
        }
        o->list.items = items;
        o->list.cap = cap;
    }
    o->list.items[o->list.n++] = item;
}

void bpw_dict_set(bpw_t *w, bpw_obj dict, const char *key, bpw_obj value) {
    list_push(w, dict, bpw_string(w, key));
    list_push(w, dict, value);
}

void bpw_array_add(bpw_t *w, bpw_obj array, bpw_obj value) {
    list_push(w, array, value);
}

typedef struct {
    uint8_t *p;
    size_t len, cap;
    bool failed;
} out_t;

static void out_put(out_t *o, const void *data, size_t len) {
    if (o->failed) {
        return;
    }
    if (o->len + len > o->cap) {
        size_t cap = o->cap ? o->cap : 256;
        while (cap < o->len + len) {
            cap *= 2;
        }
        uint8_t *p = realloc(o->p, cap);
        if (!p) {
            o->failed = true;
            return;
        }
        o->p = p;
        o->cap = cap;
    }
    memcpy(o->p + o->len, data, len);
    o->len += len;
}

static void out_be(out_t *o, uint64_t v, size_t n) {
    uint8_t buf[8];
    for (size_t i = 0; i < n; i++) {
        buf[n - 1 - i] = (uint8_t)(v >> (8 * i));
    }
    out_put(o, buf, n);
}

static size_t int_width_log2(uint64_t v) {
    if (v <= 0xFF) {
        return 0;
    }
    if (v <= 0xFFFF) {
        return 1;
    }
    if (v <= 0xFFFFFFFF) {
        return 2;
    }
    return 3;
}

static void out_marker(out_t *o, uint8_t type, size_t count) {
    if (count < 0x0F) {
        uint8_t m = (uint8_t)(type << 4 | count);
        out_put(o, &m, 1);
        return;
    }
    uint8_t m = (uint8_t)(type << 4 | 0x0F);
    out_put(o, &m, 1);
    size_t lg = int_width_log2(count);
    uint8_t im = (uint8_t)(T_INT << 4 | lg);
    out_put(o, &im, 1);
    out_be(o, count, (size_t)1 << lg);
}

static size_t bytes_for(uint64_t v) {
    size_t n = 1;
    while (n < 8 && (v >> (8 * n)) != 0) {
        n++;
    }
    return n;
}

uint8_t *bpw_serialize(bpw_t *w, bpw_obj root, size_t *len) {
    if (w->failed || root < 0 || (size_t)root >= w->n) {
        return NULL;
    }
    size_t ref_size = bytes_for(w->n - 1);
    size_t *offsets = malloc(w->n * sizeof(size_t));
    if (!offsets) {
        return NULL;
    }
    out_t o = {0};
    out_put(&o, MAGIC, MAGIC_LEN);

    for (size_t i = 0; i < w->n; i++) {
        offsets[i] = o.len;
        const wobj_t *obj = &w->objs[i];
        switch (obj->type) {
        case T_SIMPLE: {
            uint8_t m = obj->b ? SIMPLE_TRUE : SIMPLE_FALSE;
            out_put(&o, &m, 1);
            break;
        }
        case T_INT: {
            // Values with the top bit set would read back negative as
            // 8-byte ints; Apple stores those as 16 bytes.
            if (obj->u >> 63) {
                uint8_t m = T_INT << 4 | 4;
                out_put(&o, &m, 1);
                out_be(&o, 0, 8);
                out_be(&o, obj->u, 8);
            } else {
                size_t lg = int_width_log2(obj->u);
                uint8_t m = (uint8_t)(T_INT << 4 | lg);
                out_put(&o, &m, 1);
                out_be(&o, obj->u, (size_t)1 << lg);
            }
            break;
        }
        case T_REAL: {
            uint8_t m = T_REAL << 4 | 3;
            uint64_t bits;
            memcpy(&bits, &obj->r, sizeof(bits));
            out_put(&o, &m, 1);
            out_be(&o, bits, 8);
            break;
        }
        case T_DATA:
        case T_ASCII:
            out_marker(&o, obj->type, obj->buf.len);
            out_put(&o, obj->buf.p, obj->buf.len);
            break;
        case T_UTF16:
            out_marker(&o, obj->type, obj->buf.len / 2);
            out_put(&o, obj->buf.p, obj->buf.len);
            break;
        case T_ARRAY:
            out_marker(&o, T_ARRAY, obj->list.n);
            for (size_t k = 0; k < obj->list.n; k++) {
                out_be(&o, (uint64_t)obj->list.items[k], ref_size);
            }
            break;
        case T_DICT:
            out_marker(&o, T_DICT, obj->list.n / 2);
            for (size_t k = 0; k < obj->list.n; k += 2) {
                out_be(&o, (uint64_t)obj->list.items[k], ref_size);
            }
            for (size_t k = 1; k < obj->list.n; k += 2) {
                out_be(&o, (uint64_t)obj->list.items[k], ref_size);
            }
            break;
        }
    }

    size_t table = o.len;
    size_t offset_size = bytes_for(table);
    for (size_t i = 0; i < w->n; i++) {
        out_be(&o, offsets[i], offset_size);
    }
    free(offsets);

    uint8_t trailer[TRAILER_LEN] = {0};
    trailer[6] = (uint8_t)offset_size;
    trailer[7] = (uint8_t)ref_size;
    out_put(&o, trailer, 8);
    out_be(&o, w->n, 8);
    out_be(&o, (uint64_t)root, 8);
    out_be(&o, table, 8);

    if (o.failed) {
        free(o.p);
        return NULL;
    }
    *len = o.len;
    return o.p;
}

#endif  // !AK_NATIVE
