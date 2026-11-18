// platform_gc_heap_ctr.cpp — Nintendo 3DS GC 堆注入（仅 platform=ctr 编入）
//
// 3DS 是静态链接（retro_init 直接被前端调用，无 dlopen），PXT32 的 GC 堆
// 不能靠 mmap(MAP_FIXED)。这里在 retro_init 早期分配一块连续堆，注入
// scheduler.cpp 的 mca_vm_set_gc_heap()，之后 GC 在该区间内 bump 分配。
//
// 堆大小：2MB（MCA_GC_HEAP_SIZE 可覆盖）。
// 2026-11-17 实测定容：x86-32（PXT32，与 3DS 同指针宽度）64 卡全量 600 帧
// GC 高水位最大 128KB（stereoscopic_3d / low-poly_picachu），中位数 64KB，
// Mouse_Adventure 补测 64KB。2MB = 16 倍余量；越界走
// "gc heap exhausted" 明确 panic（scheduler.cpp），不会静默写坏堆。
// OLD3DS 应用内存 ~64MB：2MB 堆 + 256KB VM 栈 + ~1MB 代码段，
// 给 RetroArch 前端留足空间。分配失败不 panic —— 让后续 VM 启动照常走
// 未注入的 mmap 路径（在 3DS 上大概率失败并可见报错），而不是在这里静默
// 用一个更小的堆掩盖问题。
#include "platform_gc_heap.h"

#if defined(_PXT_CTR)

#include <cstdlib>
#include <cstdio>

// scheduler.cpp 定义，extern "C"（DLLEXPORT）
extern "C" void mca_vm_set_gc_heap(void *base, unsigned size);

#ifndef MCA_GC_HEAP_SIZE
#define MCA_GC_HEAP_SIZE (2u * 1024u * 1024u)
#endif

// MCA 诊断：注入次数与基址（屏幕 OSD 展示，见 libretro_core.cpp）。
// 与 vita 版的同名符号各自在自己的平台宏里定义，同一个构建里只会有一个非空。
extern "C" int mcaGcHeapInitCount = 0;
extern "C" void *mcaGcHeapBase = nullptr;

void mcaCtrInitGcHeap() {
    const unsigned heap_size = MCA_GC_HEAP_SIZE;
    // MCA PATCH 18：必须幂等（理由同 platform_gc_heap_vita.cpp）。
    // 3DS 上核心是 CIA 里静态链接的，前端"关闭内容 → 重新打开"同样会再次
    // 调 retro_init；重复 malloc 会把 mca_gc_base 换到新地址并泄漏旧块。
    static void *cached = nullptr;
    static unsigned cached_size = 0;
    mcaGcHeapInitCount++;
    if (cached) {
        mcaGcHeapBase = cached;
        mca_vm_set_gc_heap(cached, cached_size);
        fprintf(stderr, "[MCA][ctr] GC heap reused @ %p (%u bytes, re-init)\n", cached,
                cached_size);
        return;
    }
    void *heap = malloc(heap_size);
    if (!heap) {
        fprintf(stderr, "[MCA][ctr] GC heap alloc failed (%u bytes)\n", heap_size);
        return;
    }
    cached = heap;
    cached_size = heap_size;
    mcaGcHeapBase = heap;
    mca_vm_set_gc_heap(heap, heap_size);
    fprintf(stderr, "[MCA][ctr] GC heap injected @ %p (%u bytes)\n", heap, heap_size);
}

#endif // _PXT_CTR
