// platform_gc_heap_vita.cpp — PS Vita GC 堆注入（仅 platform=vita 编入）
//
// 与 3DS 同理：静态链接、无 mmap(MAP_FIXED)，用 newlib malloc 分配一块
// 连续堆注入 scheduler.cpp 的 mca_vm_set_gc_heap()。
//
// 堆大小：2MB（MCA_GC_HEAP_SIZE 可覆盖）。定容依据见 ctr 版注释
//（2026-11-17 x86-32 64 卡实测 GC 高水位最大 128KB，2MB = 16 倍余量）。
// Vita 512MB RAM 本可给更多，但与 3DS 保持同一口径，便于问题对照。
// 分配失败只报日志、不 panic（理由见 ctr 版注释）。
#include "platform_gc_heap.h"

#if defined(_PXT_VITA)

#include <cstdlib>
#include <cstdio>

extern "C" void mca_vm_set_gc_heap(void *base, unsigned size);

#ifndef MCA_GC_HEAP_SIZE
#define MCA_GC_HEAP_SIZE (2u * 1024u * 1024u)
#endif

void mcaVitaInitGcHeap() {
    const unsigned heap_size = MCA_GC_HEAP_SIZE;
    void *heap = malloc(heap_size);
    if (!heap) {
        fprintf(stderr, "[MCA][vita] GC heap alloc failed (%u bytes)\n", heap_size);
        return;
    }
    mca_vm_set_gc_heap(heap, heap_size);
    fprintf(stderr, "[MCA][vita] GC heap injected @ %p (%u bytes)\n", heap, heap_size);
}

#endif // _PXT_VITA
