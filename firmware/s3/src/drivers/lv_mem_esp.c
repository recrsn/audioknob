// LVGL's allocator on the S3 (LV_STDLIB_CUSTOM in lv_conf.h): everything in
// PSRAM. Internal RAM is kept for DMA, task stacks and AirPlay; LVGL 9.6's
// built-in pool would be a static array there, with no hook to place it.
//
// Putting render-time scratch (draw tasks, mask buffers) in internal RAM was
// measured and gained nothing: the cache already absorbs those accesses.

#include "esp_heap_caps.h"
#include "lvgl.h"

#define CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

void lv_mem_init(void) {
}

void lv_mem_deinit(void) {
}

lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes) {
    LV_UNUSED(mem);
    LV_UNUSED(bytes);
    return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool) {
    LV_UNUSED(pool);
}

void *lv_malloc_core(size_t size) {
    return heap_caps_malloc(size, CAPS);
}

void *lv_realloc_core(void *p, size_t new_size) {
    return heap_caps_realloc(p, new_size, CAPS);
}

void lv_free_core(void *p) {
    heap_caps_free(p);
}

void lv_mem_monitor_core(lv_mem_monitor_t *mon_p) {
    LV_UNUSED(mon_p);
}

lv_result_t lv_mem_test_core(void) {
    return LV_RESULT_OK;
}
