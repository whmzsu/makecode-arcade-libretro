// nx_diag.h — NX（Switch）诊断入口：开机里程碑 + 运行期 trace
//
// 为什么单独一个头：调用点（libretro_core.cpp 里 11 处）不该带平台条件。
// 以前平台判断散在 60 行实现和 11 个调用点之间，读接口实现时得反复跨过
// `#if defined(__SWITCH__)`。现在平台条件只出现在这个文件里一次：
//
//   * 非 Switch：全部展开成 (void)0，编译期消失，连符号都不进产物；
//   * Switch    ：写 sdmc:/switch/ 下的日志文件（实现见 nx_diag.cpp）。
//
// 运行期 trace（MCA_NX_RUN / MCA_NX_MODE）只在诊断构建里编译，
// 见 tools/nx-probe/build-diag.sh。
#pragma once

#if defined(__SWITCH__)

void nxBoot(const char* fmt, ...);   // 直写 sdmc:/switch/mca_boot.log，一行一 flush

#if defined(MCA_NX_DIAG)
void nxRunLog(const char* fmt, ...);
int  nxMode(void);
#define MCA_NX_BOOT(...) nxBoot(__VA_ARGS__)
#define MCA_NX_RUN(...)  nxRunLog(__VA_ARGS__)
#define MCA_NX_MODE()    nxMode()
#else
#define MCA_NX_BOOT(...) nxBoot(__VA_ARGS__)
#define MCA_NX_RUN(...)  ((void)0)
#define MCA_NX_MODE()    (0)
#endif

#else  // 非 Switch

#define MCA_NX_BOOT(...) ((void)0)
#define MCA_NX_RUN(...)  ((void)0)
#define MCA_NX_MODE()    (0)

#endif
