// SAS-Watch: LV_USE_STDLIB_MALLOC=LV_STDLIB_CUSTOM の実装。
// LVGL のヒープ (オブジェクト・スタイル・イベント等) を PSRAM に置く。
// 既定の CLIB malloc だと SPIRAM_MALLOC_ALWAYSINTERNAL=1024 のため
// 1KB 以下の widget 確保が全部内蔵 SRAM に入り、画面を重ねると
// 空きが数百バイトまで枯渇していた。描画面のフルバッファや DMA バウンス
// は従来どおり個別に確保するのでこのヒープには来ない。
#include <stdlib.h>
#include "stdlib/lv_mem.h"
#include "esp_heap_caps.h"

void lv_mem_init(void) {}
void lv_mem_deinit(void) {}

lv_mem_pool_t lv_mem_add_pool(void* mem, size_t bytes) {
    (void)mem;
    (void)bytes;
    return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool) { (void)pool; }

void* lv_malloc_core(size_t size) {
    void* p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return p;
}

void lv_free_core(void* p) { heap_caps_free(p); }

void* lv_realloc_core(void* p, size_t new_size) {
    void* np =
        heap_caps_realloc(p, new_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!np) {
        np = heap_caps_realloc(p, new_size,
                               MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    return np;
}

void lv_mem_monitor_core(lv_mem_monitor_t* mon_p) {
    const size_t total =
        heap_caps_get_total_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const size_t free_b =
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    mon_p->total_size = total;
    mon_p->free_size = free_b;
    mon_p->free_biggest_size =
        heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    mon_p->used_pct = total ? (uint8_t)((total - free_b) * 100 / total) : 0;
    mon_p->frag_pct = (free_b && mon_p->free_biggest_size < free_b)
                          ? (uint8_t)(100 - mon_p->free_biggest_size * 100 /
                                                free_b)
                          : 0;
}

lv_result_t lv_mem_test_core(void) { return LV_RESULT_OK; }
