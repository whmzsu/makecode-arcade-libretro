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

// MCA 诊断：注入次数与基址（屏幕 OSD 展示，见 libretro_core.cpp）。
extern "C" int mcaGcHeapInitCount = 0;
extern "C" void *mcaGcHeapBase = nullptr;

void mcaVitaInitGcHeap() {
    const unsigned heap_size = MCA_GC_HEAP_SIZE;
    // MCA PATCH 18：必须幂等。
    //
    // 前端"关闭内容 → 重新打开"不是进程重启：Vita 上核心库链接了
    // -z nodelete，dlclose 不会真正卸载，retro_init 会被**再次调用**。
    // 这里若每次都 malloc 一块新的 2MB，mca_gc_base 就换到另一个地址，而
    // 上一块永远不释放。桌面/Android 走 PXT32 的 mmap(MAP_FIXED) 分支，
    // 地址恒为 GC_BASE=0x20000000，所以这个问题在桌面上完全看不见；
    // 裸机上表现为"同一次前端会话里第二次加载后画面颜色错乱、
    // 重启前端又正常"。
    static void *cached = nullptr;
    static unsigned cached_size = 0;
    mcaGcHeapInitCount++;
    if (cached) {
        mcaGcHeapBase = cached;
        mca_vm_set_gc_heap(cached, cached_size);
        fprintf(stderr, "[MCA][vita] GC heap reused @ %p (%u bytes, re-init)\n", cached,
                cached_size);
        return;
    }
    void *heap = malloc(heap_size);
    if (!heap) {
        fprintf(stderr, "[MCA][vita] GC heap alloc failed (%u bytes)\n", heap_size);
        return;
    }
    cached = heap;
    cached_size = heap_size;
    mcaGcHeapBase = heap;
    mca_vm_set_gc_heap(heap, heap_size);
    fprintf(stderr, "[MCA][vita] GC heap injected @ %p (%u bytes)\n", heap, heap_size);
}

#endif // _PXT_VITA
