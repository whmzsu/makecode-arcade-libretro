// mca_profile.h — 编译期可选的分段性能计时（默认关闭，零开销）
//
// 目的：低端设备（RK3326 / R36S 等）跑重卡带只有 ~1.08× 余量，需要知道每帧
// 时间花在哪一段，而不是只看"总吞吐"盲猜。总吞吐口径见 docs/DEVICE-TEST.md，
// 注意"实际节拍"被 Python 宿主 16ms sleep 封顶，不是性能指标。
//
// 用法：
//   make -f core/Makefile platform=unix CXXFLAGS=-DMCA_PROFILE      （前端 + VM 两线程都插桩）
//   tools/libretro_host.py ... 2>&1 | grep MCA-PROF                （宿主转发 stderr）
// 打开后每 MCA_PROF_EVERY 帧往 stderr 打一次各阶段累计的 us/call；画面与游戏
// 逻辑完全不受影响（计时只在段前后读单调钟，不参与任何计算）。默认构建所有
// 宏展开成 ((void)0)，不 include time.h、不定义符号，golden 逐位一致。
//
// 段位含义：
//   FRONT_TOTAL  retro_run 全程（前端线程一帧）
//   PULL_SCREEN  vm_pull_screen —— 前端等 VM 产帧 + 跨线程拷贝
//   PIXEL_CONV   BGRA→RGB565 循环（76800 像素/帧）
//   AUDIO        取音频 + 单声道展开立体声
//   VIDEO        g_video 回调（前端把帧缩放送显，通常不在核心控制内）
//   VM_EXEC      exec_loop —— 游戏逻辑 + 绘制（含 image.cpp 的 blit）
//   VM_PUBLISH   pxt_screen_publish —— 拷缓冲 + 广播
#ifndef MCA_PROFILE_H
#define MCA_PROFILE_H

#ifdef MCA_PROFILE

#include <stdint.h>
#include <stdio.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

#ifndef MCA_PROF_EVERY
#define MCA_PROF_EVERY 300
#endif

enum McaProfStage {
    MCA_PF_FRONT_TOTAL = 0,
    MCA_PF_PULL_SCREEN,
    MCA_PF_PIXEL_CONV,
    MCA_PF_AUDIO,
    MCA_PF_VIDEO,
    MCA_PF_VM_EXEC,
    MCA_PF_DRAW_IMAGE,   // drawImageCore 子段（VM_EXEC 的子集，量化 blit 占比）
    MCA_PF_VM_PUBLISH,
    MCA_PF_N
};

struct McaProfSlot { uint64_t ns; uint64_t calls; };
// 定义在 libretro_core.cpp（唯一定义点）；scheduler.cpp 等其它 TU 只引用。
extern struct McaProfSlot g_mca_prof[MCA_PF_N];

static inline uint64_t mca_prof_now(void) {
#if defined(_WIN32)
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint64_t)(c.QuadPart * 1000000000LL / f.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
}

// 段计时宏：BEGIN 声明一个 tag 唯一的起点，END 累加到对应 stage。
// tag 在同一 C++ 作用域内必须唯一（编译器会检查重名）。
#define MCA_PF_BEGIN(tag) const uint64_t _mca_pf_##tag = mca_prof_now()
#define MCA_PF_END(tag, stage) do {                                   \
    g_mca_prof[stage].ns += mca_prof_now() - _mca_pf_##tag;          \
    g_mca_prof[stage].calls++;                                        \
} while (0)

// 由 libretro_core.cpp 实现：每 MCA_PROF_EVERY 次调用打印一帧均值。
void mca_prof_report(void);

#else  // !MCA_PROFILE —— 全空，零开销

#define MCA_PF_BEGIN(tag) ((void)0)
#define MCA_PF_END(tag, stage) ((void)0)
#define mca_prof_report() ((void)0)

#endif  // MCA_PROFILE
#endif  // MCA_PROFILE_H
