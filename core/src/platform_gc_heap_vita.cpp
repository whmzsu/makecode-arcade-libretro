// platform_gc_heap_vita.cpp — PS Vita GC 堆注入（仅 platform=vita 编入）
//
// 与 3DS 同理：静态链接、无 mmap(MAP_FIXED)，用 newlib malloc 分配一块
// 连续堆注入 scheduler.cpp 的 mca_vm_set_gc_heap()。Vita 有 512MB RAM，
// 64MB 注入堆宽裕。分配失败只报日志、不 panic（理由见 ctr 版注释）。
#include "platform_gc_heap.h"

#if defined(_PXT_VITA)

#include <cstdlib>
#include <cstdio>

extern "C" void mca_vm_set_gc_heap(void *base, unsigned size);

void mcaVitaInitGcHeap() {
    const unsigned heap_size = 64u * 1024u * 1024u;
    void *heap = malloc(heap_size);
    if (!heap) {
        fprintf(stderr, "[MCA][vita] GC heap alloc failed (%u bytes)\n", heap_size);
        return;
    }
    mca_vm_set_gc_heap(heap, heap_size);
    fprintf(stderr, "[MCA][vita] GC heap injected @ %p (%u bytes)\n", heap, heap_size);
}

#endif // _PXT_VITA
