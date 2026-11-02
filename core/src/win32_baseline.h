// win32_baseline.h — 构建垫片：把 Windows API 基线抬到 Win7（_WIN32_WINNT 0x0601）
//
// 为什么需要：mingw-gcc 默认按 XP（0x0502）裁 windows.h，而
// runtime/pxtapp/core---vm/platform.cpp 用了 GetTickCount64（Vista+），
// 用 gcc 编 Windows 目标会直接报 "'GetTickCount64' was not declared in this
// scope"。官方 buildbot（libretro 的 windows-x64-mingw 模板）正是 mingw-gcc，
// 所以这不是 CI 的临时补丁，而是构建配置的一部分。
//
// 为什么不用命令行 -D_WIN32_WINNT：zig 的 mingw 已经把它定在 0x0a00，再加一个
// 不同值的 -D 会让每个编译单元都报 -Wmacro-redefined，而且把 zig 的基线从
// Win10 降下来。这里用 #ifndef 只在编译器没给的时候兜底：
//   gcc（未定义）→ 0x0601；zig（0x0a00）→ 保持原值，零告警。
//
// 由 core/Makefile 与 tools/build_target.sh 的 Windows 分支以 -include 引入，
// 不是源码依赖（任何平台都不该主动 include 它）。
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601 // Windows 7
#endif
