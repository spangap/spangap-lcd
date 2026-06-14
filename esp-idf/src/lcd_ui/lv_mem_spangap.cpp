/* LVGL custom allocator backend → spangap's central memory policy (mem.h).
 *
 * Selected by CONFIG_LV_USE_CUSTOM_MALLOC (board kconfig). LVGL object/style/
 * draw-descriptor data is plain heap (not ISR-touched, not DMA — the panel DMA
 * transfer buffer is allocated separately), so it follows gp_alloc: PSRAM on
 * PSRAM targets, internal on no-PSRAM ones. Guarded on the Kconfig so a board
 * using LVGL's builtin/CLIB malloc doesn't get a duplicate definition.
 *
 * CUSTOM_MALLOC requires the FULL backend contract (see clib/lv_mem_core_clib.c):
 * init/deinit/add_pool/remove_pool/malloc/realloc/free/monitor/test. We mirror
 * the clib stubs and route the three real allocators through gp_*. */
#include "sdkconfig.h"
#if CONFIG_LV_USE_CUSTOM_MALLOC
#include "lvgl.h"
#include "mem.h"

extern "C" {

void lv_mem_init(void)   { /* nothing to init — backed by the global heap */ }
void lv_mem_deinit(void) { /* nothing to deinit */ }

lv_mem_pool_t lv_mem_add_pool(void * mem, size_t bytes) { LV_UNUSED(mem); LV_UNUSED(bytes); return NULL; }
void          lv_mem_remove_pool(lv_mem_pool_t pool)     { LV_UNUSED(pool); }

void * lv_malloc_core(size_t size)                 { return gp_alloc(size); }
void * lv_realloc_core(void * p, size_t new_size)  { return gp_realloc(p, new_size); }
void   lv_free_core(void * p)                      { gp_free(p); }

void lv_mem_monitor_core(lv_mem_monitor_t * mon_p) { LV_UNUSED(mon_p); }
lv_result_t lv_mem_test_core(void)                 { return LV_RESULT_OK; }

}
#endif
