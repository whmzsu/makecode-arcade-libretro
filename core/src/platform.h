// platform.h — pxt VM 宿主 ABI（对齐官方 hw---vm/sdlmain.cpp）+ libretro 接线约定
//
// 该 ABI 由 runtime/pxtapp（vendored 自 pxt 官方 core---vm/screen---ext/
// mixer---ext）实现；core/src/vm_host.cpp 是 core 侧封装，
// libretro_core.cpp 通过它驱动 VM。详见 docs/M3-VM-HOST.md。
#pragma once

#include <cstdint>
#include <cstddef>

// —— 官方 VM 宿主 ABI（extern "C"，与 sdlmain.cpp 中 dlsym 的符号一致）——
extern "C" {

// 从内存镜像启动 VM（字节码镜像：官方为 .pxt64 文件内容）。
// 对应 sdlmain 的 pxt_vm_start(fn) / pxt_vm_start_buffer(data, len)。
void pxt_vm_start_buffer(uint8_t* data, unsigned len);

// 输入事件：src = 2050(抬起)/2051(按下)，val = 按键编号（1..7，0=any）。
// 官方每次还会补发一个 val=0 的 "any" 事件。
void pxt_raise_event(int src, int val);

// 拉取一帧画面：BGRA32，宽高应为 160x120。
void pxt_screen_get_pixels(int width, int height, uint32_t* screen);

// 拉取音频：44100Hz 单声道 S16。
void pxt_get_audio_samples(int16_t* buf, unsigned numSamples);

// 日志与崩溃码（pxt_get_panic_code：0 正常，-1 用户代码重启中，
// >=1000 软崩溃自动重启，其他为致命错误）。
int pxt_get_logs(int logtype, char* dst, int maxSize);
int pxt_get_panic_code();

// 存档/设置目录（游戏写 settings、高分记录都写到这里；必须在启动 VM 前调用）。
void pxt_vm_set_data_directory(const char* path);

// —— 本项目扩展（MCA PATCH 5）：宿主侧重启 ——
//
// 官方 ABI 是"单游戏单进程"，没有 stop/reset。这里利用 VM 自带的
// systemReset()（清 fiber / handler / GC 后 pthread_exit）实现真重启：
//   pxt_vm_request_reset() 置标志 → VM 线程在下一轮主循环退出
//   pxt_vm_poll_exit()     轮询到 1 表示线程已退出、可以安全 join
//   宿主用原始镜像的**新拷贝**重新 pxt_vm_start_buffer()
void pxt_vm_request_reset();
// MCA PATCH 6：重启路径 trace（落到存档目录的 mca_trace.log）
void pxt_vm_trace(const char* msg);
const char* pxt_vm_trace_path();   // trace 文件实际路径（找不到文件时为说明串）
int pxt_vm_poll_exit();   // 返回 1 表示 VM 线程已退出（读后自动清零）
// 停掉 VM 线程并 join + 释放镜像。返回 1 表示已完全停止（可安全 dlclose）。
// 卸载内容 / 换游戏 / 关闭核心时必须调用。
int pxt_vm_stop();
int pxt_vm_running();     // VM 线程是否仍在运行

} // extern "C"

namespace mca {

// 显示参数（与真机一致）
constexpr int kScreenWidth = 160;
constexpr int kScreenHeight = 120;
constexpr int kFrameRate = 60;
constexpr double kFrameTimeMs = 1000.0 / 60.0;
constexpr int kAudioSampleRate = 44100;
constexpr int kAudioSamplesPerFrame = 735; // 44100/60

// 按键编号：与官方 Key 枚举一致（sdlmain.cpp）
constexpr int KEY_LEFT = 1, KEY_UP = 2, KEY_RIGHT = 3, KEY_DOWN = 4;
constexpr int KEY_A = 5, KEY_B = 6, KEY_MENU = 7;
constexpr int EVENT_UP = 2050, EVENT_DOWN = 2051;

// RetroPad 掩码（libretro_core 内部用）
enum ArcadeButton {
    BTN_A = 1 << 0, BTN_B = 1 << 1,
    BTN_LEFT = 1 << 2, BTN_RIGHT = 1 << 3,
    BTN_UP = 1 << 4, BTN_DOWN = 1 << 5,
    BTN_MENU = 1 << 6,
};

// .pxt64 文件布局：8 字节对齐的 section 链 + 尾部 SRCM 源码映射。
// VM 的 loadVMImage 要求「长度 8 对齐且 section 恰好吃满」，所以宿主
// 加载时必须按 section 链截断。此函数返回真实镜像长度（截断点）。
size_t pxt64_image_length(const uint8_t* data, size_t size);

// VM 生命周期：首次启动用 vm_start_from_image()。之后要"重启游戏"用
// vm_request_reset() + vm_poll_restart()（见下）。
bool vm_start_from_image(const uint8_t* data, size_t size, const char* save_dir);

// VM 是否已启动（由 vm_host.cpp 实现）
bool runtime_available();

// 请求重启（任意帧调用；下一帧起 VM 线程会退出并重新启动）
void vm_request_reset();
// 每帧调用：VM 线程已退出则用原始镜像重启并返回 true（本帧画面不可用）
bool vm_poll_restart();
bool reset_pending(); // 已请求但尚未完成
// 彻底停掉 VM 线程（卸载内容 / 切换游戏 / 关闭核心时必须调，否则前端
// dlclose 了核心库，VM 线程还在跑里面的代码 → 段错误）。可重复调用。
void vm_stop();

// —— 每帧宿主服务（vm_host.cpp）——
void vm_raise_key(int key, bool down);              // 按键事件沿（含 any-key 补发）
void vm_pull_screen(uint32_t* bgra);                // 拉一帧 BGRA32 (160x120)
void vm_pull_audio(int16_t* mono, unsigned samples); // 拉一帧 44.1kHz 单声道 S16
size_t vm_flush_logs(void (*emit)(const char*, size_t)); // 倒 VM 日志
int vm_panic_code();                                // 0 正常；见 M3-VM-HOST.md

} // namespace mca
