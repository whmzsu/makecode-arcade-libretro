// vm_host.cpp — pxt VM 宿主层的 core 侧封装
//
// 职责：镜像装载（section 链截断 + 8 字节对齐可写缓冲）、存档目录、
// 按键事件沿、每帧拉屏/拉音频/倒日志/查 panic。
// 全部走官方宿主 ABI（platform.h，与 hw---vm/sdlmain.cpp 一致），
// 实现来自 runtime/pxtapp（vendored，游戏无关，见 docs/M3-VM-HOST.md）。
#include "platform.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

namespace mca {

size_t pxt64_image_length(const uint8_t* data, size_t size) {
    size_t off = 0;
    while (off + 8 <= size) {
        uint32_t sect = 0;
        std::memcpy(&sect, data + off + 4, 4); // 每个 section 头：2 类型 +2 aux +4 size
        if (sect == 0 || (sect & 7) || off + sect > size) break;
        off += sect;
    }
    return off;
}

namespace {

// 镜像所有权：VM 的 unloadVMImage() 会 free(img->dataStart)，所以启动缓冲
// 必须是 malloc 分配的（不能是 std::vector 的内部指针），且每次重启都要
// 换新的一块 —— 旧块已被 VM 释放。
std::vector<uint8_t> g_vm_orig; // 原始镜像（宿主持有，永不交给 VM）
size_t g_vm_len = 0;
std::string g_save_dir;
bool g_vm_up = false;
bool g_reset_pending = false;
unsigned g_reset_frames = 0; // 等待 VM 线程退出的帧数（超时兜底）

// v0.2.14：VM 线程"猝死"的自愈。
//   g_vm_was_running：上一帧 VM 是否还活着。只在 true→false 的**跳变**上触发
//     自动重启 —— 用电平判定的话，启动就失败的坏卡带会让 retro_run 每帧都
//     重启一次，变成更糟的死循环。
//   g_auto_restarts：一次游戏装载内的自动重启次数上限。连续死 3 次说明是
//     卡带/环境问题，重启没有意义，留黑屏 + panic 横幅让人看得见。
bool g_vm_was_running = false;
int g_auto_restarts = 0;

bool mkdir_p(const std::string& path) {
    if (path.empty()) return false;
    std::string cur;
    for (size_t i = 0; i <= path.size(); ++i) {
        const char c = (i == path.size()) ? '/' : path[i];
        cur.push_back(c);
        if (c != '/' && c != '\\') continue;
        if (cur.size() <= 1) continue; // 跳过根
        std::string dir = cur.substr(0, cur.size() - 1);
        if (dir.empty()) continue;
#if defined(_WIN32)
        _mkdir(dir.c_str());
#else
        mkdir(dir.c_str(), 0777);
#endif
    }
    return true;
}

// 用原始镜像的一份干净拷贝启动 VM（VM 会就地打补丁，所以每次都要新拷贝）
bool start_vm_from_orig() {
    if (g_vm_len == 0) return false;
    // 8 字节对齐由 malloc 保证（max_align_t ≥ 8）；多留一点尾巴给 VM 的就地改写
    uint8_t* buf = static_cast<uint8_t*>(std::malloc(g_vm_len + 16));
    if (!buf) return false;
    std::memcpy(buf, g_vm_orig.data(), g_vm_len);
    if (!g_save_dir.empty()) pxt_vm_set_data_directory(g_save_dir.c_str());
    pxt_vm_start_buffer(buf, static_cast<unsigned>(g_vm_len));
    return true;
}

} // namespace

bool vm_start_from_image(const uint8_t* data, size_t size, const char* save_dir) {
    // 换游戏：老 VM 必须先完全停掉，否则两套线程共用全局状态，且新镜像
    // 装载期间老线程还在跑（前端随后会 dlclose/重新装载核心）。
    if (g_vm_up) vm_stop();

    const size_t len = pxt64_image_length(data, size);
    if (len < 64 || (len & 7)) return false;

    g_vm_orig.assign(data, data + len);
    g_vm_len = len;
    g_save_dir = save_dir ? save_dir : "";
    if (!g_save_dir.empty()) mkdir_p(g_save_dir);

    if (!start_vm_from_orig()) return false;
    g_vm_up = true;
    g_reset_pending = false;
    // 换游戏/重新装载 = 新一轮，自动重启配额重新计
    g_vm_was_running = false;
    g_auto_restarts = 0;
    g_reset_frames = 0;
    return true;
}

bool runtime_available() { return g_vm_up; }

void vm_request_reset() {
    if (!g_vm_up) return;
    g_reset_pending = true;
    g_reset_frames = 0;
    pxt_vm_trace("request reset");
    pxt_vm_request_reset();
}

bool vm_poll_restart() {
    // MCA v0.2.14：两种"要重启"都接住。
    //
    // 以前只认玩家显式按重置（g_reset_pending）。可 VM 线程自己 panic 掉时是
    // 走 systemReset() → pthread_exit 的，g_reset_pending 仍是 false —— 前端
    // 于是继续照常拉帧（拿黑屏），永不重启：真机上就是"画面冻住、进程不崩也
    // 不退出"。这里补一句"线程没了就自动重启"，把崩溃降级成"游戏重新开始"。
    const bool running = pxt_vm_running();
    if (!g_reset_pending && g_vm_up && g_vm_was_running && !running) {
        if (g_auto_restarts < 3) {
            g_auto_restarts++;
            g_reset_pending = true;
            g_reset_frames = 0;
            char msg[96];
            std::snprintf(msg, sizeof(msg), "vm thread gone unexpectedly -> auto restart (%d/3)",
                          g_auto_restarts);
            pxt_vm_trace(msg);
        } else {
            pxt_vm_trace("vm keeps dying, auto restart limit reached -> giving up");
        }
    }
    g_vm_was_running = running;

    if (!g_reset_pending) return false;
    if (!pxt_vm_poll_exit()) {
        // VM 线程还没退出；正常几毫秒内就完事，给 2 秒兜底
        if (++g_reset_frames < 120) return false;
        g_reset_pending = false;
        pxt_vm_trace("TIMEOUT waiting for vm thread exit (2s)");
        return false; // 放弃本次重置（卡在死循环的极端情况）
    }
    g_reset_pending = false;
    g_reset_frames = 0;
    pxt_vm_trace("vm thread gone, starting fresh vm");
    const bool ok = start_vm_from_orig();
    if (ok) g_vm_was_running = pxt_vm_running();
    return ok;
}

bool reset_pending() { return g_reset_pending; }

// MCA PATCH 8：真正停掉 VM 线程。
//
// RetroArch 换游戏 / 关闭核心的流程是：
//     retro_unload_game() → retro_deinit() → dlclose(核心库)
// 我们的 VM 跑在独立线程里，以前这两个回调什么都不做，于是 dlclose 之后
// VM 线程还在执行已经被卸载的代码段 —— 立刻 SIGSEGV，前端表现为"直接崩溃
// 退出"（换游戏、"重启游戏"都会走这条路）。
// 现在：先让 VM 线程自己清理并退出（pxt_vm_stop 内部 join），再清宿主状态。
void vm_stop() {
    // 不按 g_vm_up 提前返回：装载半途失败时线程可能已经在跑，而 g_vm_up 还没置位。
    // pxt_vm_stop() 自己会在"没有线程"时直接返回，重复调用也安全。
    g_reset_pending = false;
    g_reset_frames = 0;
    pxt_vm_trace("unload: stopping vm");
    pxt_vm_stop();
    g_vm_up = false;
    g_vm_orig.clear();
    g_vm_orig.shrink_to_fit();
    g_vm_len = 0;
}

void vm_raise_key(int key, bool down) {
    const int ev = down ? EVENT_DOWN : EVENT_UP;
    pxt_raise_event(ev, key);
    pxt_raise_event(ev, 0); // 官方宿主每次补发一个 "any key" 事件
}

void vm_pull_screen(uint32_t* bgra) {
    pxt_screen_get_pixels(kScreenWidth, kScreenHeight, bgra);
}

void vm_pull_audio(int16_t* mono, unsigned samples) {
    pxt_get_audio_samples(mono, samples);
}

// 把 VM 内部日志倒给宿主回调；返回本次倒出的字节数
size_t vm_flush_logs(void (*emit)(const char* line, size_t len)) {
    static char buf[16 * 1024];
    size_t total = 0;
    for (;;) {
        const int n = pxt_get_logs(0, buf, static_cast<int>(sizeof(buf)) - 1);
        if (n <= 0) break;
        buf[n] = 0;
        emit(buf, static_cast<size_t>(n));
        total += static_cast<size_t>(n);
    }
    return total;
}

int vm_panic_code() { return pxt_get_panic_code(); }

// 诊断用：VM 线程/在线状态。NX 上排查"跑几秒后崩"时需要区分
// "VM 还在推进" 与 "VM 线程已经没了但前端还在拉帧"。
bool vm_thread_alive() { return pxt_vm_running() != 0; }
bool vm_online() { return pxt_vm_online() != 0; }

} // namespace mca
