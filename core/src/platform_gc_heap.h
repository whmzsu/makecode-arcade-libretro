// platform_gc_heap.h — 3DS/Vita 静态链接平台的 GC 堆接入入口
//
// 背景（probe/pxt32-3ds-vita）：A 方案的 32 位 GC 堆默认靠
// mmap(MAP_FIXED) 钉在 0x20000000。3DS/Vita 是静态链接（无 dlopen/mmap），
// 该地址也可能被 OS 占用；scheduler.cpp 因此提供运行时基址接口
// mca_vm_set_gc_heap(base, size)，在注入堆区间内 bump 分配、不调 mmap。
//
// 本头文件让调用点（libretro_core.cpp 的 retro_init）不带平台条件：
//   * 非 3DS/Vita：MCA_PLATFORM_GC_HEAP_INIT() 展开成 (void)0，连符号都不进产物；
//   * 3DS / Vita ：调各自的实现文件，按平台 API 分配一大块连续堆并注入。
//
// 平台条件只在这里出现一次；实现见 platform_gc_heap_ctr.cpp / _vita.cpp。
#pragma once

#if defined(_PXT_CTR)
void mcaCtrInitGcHeap();
#define MCA_PLATFORM_GC_HEAP_INIT() mcaCtrInitGcHeap()
#elif defined(_PXT_VITA)
void mcaVitaInitGcHeap();
#define MCA_PLATFORM_GC_HEAP_INIT() mcaVitaInitGcHeap()
#else
#define MCA_PLATFORM_GC_HEAP_INIT() ((void)0)
#endif
