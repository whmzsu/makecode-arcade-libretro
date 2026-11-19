/*
 * api17_shim.c —— 让 API 21 编译的核心在 SDK 17（Android 4.2.2）上 dlopen 成功
 *
 * 背景：78P01 这类老机（MT6592 / armeabi-v7a / SDK 17）的 bionic 缺 9 个符号，
 *       发版 android-armv7 核心（APP_PLATFORM=android-21）在 soinfo_relocate 阶段
 *       报 "cannot locate symbol ..." 而 EXIT=3。逐个补齐后核心即可正常执行，
 *       且 21 卡末帧哈希与 android21 基线逐位一致（2026-11-19 实测）。
 *
 * 用法（测试脚手架，不进发版包）：
 *   NDK=../common\ tools/android-ndk-r27c
 *   "$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe" \
 *       --target=armv7a-linux-androideabi21 -shared -fPIC \
 *       tools/android_test/api17_shim.c -o /data/local/tmp/mca32/shim.so -ldl
 *   adb shell 'cd /data/local/tmp/mca32 && LD_PRELOAD=./shim.so ./mca_host ...'
 *
 * 注意：这只是测试脚手架。玩家侧用 RA32 装核心不受影响（RetroArch 自带这些符号）。
 *       垫片只影响这 9 个函数；核心数值路径未改，故哈希可比。
 */
#include <dlfcn.h>
#include <stdlib.h>
#include <wchar.h>

typedef double (*fn1)(double);
static fn1 f_log;
static double lg(double x) {
    if (!f_log) f_log = (fn1)dlsym(RTLD_NEXT, "log");
    return f_log(x);
}

/* math.h：API 21 才加入 log2 */
double log2(double x) { return lg(x) * 1.4426950408889634074; }

/* stdlib/wchar：API 21 才加入的 strtod/wcstod 变体 */
float strtof(const char *s, char **e) { return (float)strtod(s, e); }
long double strtold(const char *s, char **e) { return (long double)strtod(s, e); }
float wcstof(const wchar_t *s, wchar_t **e) { return (float)wcstod(s, e); }
long double wcstold(const wchar_t *s, wchar_t **e) { return (long double)wcstod(s, e); }
long long wcstoll(const wchar_t *s, wchar_t **e, int b) { return strtoll((const char *)s, (char **)e, b); }
unsigned long long wcstoull(const wchar_t *s, wchar_t **e, int b) { return strtoull((const char *)s, (char **)e, b); }

/* libc private：android_set_abort_message 无实际用途；unwind 返回空表避免崩栈 */
void android_set_abort_message(const char *msg) { (void)msg; }
void *dl_unwind_find_exidx(void *pc, int *pcount) { (void)pc; if (pcount) *pcount = 0; return (void *)0; }
