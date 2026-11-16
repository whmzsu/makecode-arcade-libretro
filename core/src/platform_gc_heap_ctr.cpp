// platform_gc_heap_ctr.cpp — Nintendo 3DS GC 堆注入（仅 platform=ctr 编入）
//
// 3DS 是静态链接（retro_init 直接被前端调用，无 dlopen），PXT32 的 GC 堆
// 不能靠 mmap(MAP_FIXED)。这里在 retro_init 早期分配一块连续堆，注入
// scheduler.cpp 的 mca_vm_set_gc_heap()，之后 GC 在该区间内 bump 分配。
//
// 堆大小：64MB。New 3DS 有 256MB RAM，RetroArch 自身 + 帧缓冲占一小部分，
// 64MB 给 VM 堆绰绰有余（实测最大卡 Mouse_Adventure 382KB，21 卡全量
// 在 64MB 注入堆下逐位一致）。分配失败不 panic —— 让后续 VM 启动照常走
// 未注入的 mmap 路径（在 3DS 上大概率失败并可见报错），而不是在这里静默
// 用一个更小的堆掩盖问题。
#include "platform_gc_heap.h"

#if defined(_PXT_CTR)

#include <cstdlib>
#include <cstdio>

// scheduler.cpp 定义，extern "C"（DLLEXPORT）
extern "C" void mca_vm_set_gc_heap(void *base, unsigned size);

void mcaCtrInitGcHeap() {
    const unsigned heap_size = 64u * 1024u * 1024u;
    void *heap = malloc(heap_size);
    if (!heap) {
        fprintf(stderr, "[MCA][ctr] GC heap alloc failed (%u bytes)\n", heap_size);
        return;
    }
    mca_vm_set_gc_heap(heap, heap_size);
    fprintf(stderr, "[MCA][ctr] GC heap injected @ %p (%u bytes)\n", heap, heap_size);
}

#endif // _PXT_CTR
