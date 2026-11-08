// mca_host_android.c —— Android 上的无头 libretro 宿主（arm64-v8a，bionic）
//
// 为什么需要它：掌机的 /tmp 是 noexec，也没有 python/ctypes，跑不了
// tools/libretro_host.py。这个 C 版只做回归最需要的那件事——加载核心、
// 跑 N 帧、算末帧 MD5——好与工具机基线逐张比对。
//
// 哈希口径必须与 libretro_host.py 完全一致：md5(最后一帧 width*height*2 字节)，
// 忽略 pitch（核心实际就是 width*2）。改这里就等于改了基线，别乱动。
//
// 编译（NDK r27c）：
//   aarch64-linux-android30-clang -O2 -I../../core mca_host_android.c -o mca_host -ldl
// 用法：
//   ./mca_host <core.so> <cart.pxt64> <frames> [--data-dir=DIR] [--quiet]

#include <dlfcn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "libretro.h"

// MD5 实现在 tools/mca_md5.h（与测试 APK 共用同一份）
#include "mca_md5.h"

// ---------------------------------------------------------------- 核心符号表

typedef struct {
    void (*init)(void);
    void (*deinit)(void);
    unsigned (*api_version)(void);
    void (*get_system_info)(struct retro_system_info*);
    void (*get_system_av_info)(struct retro_system_av_info*);
    void (*set_environment)(retro_environment_t);
    void (*set_video_refresh)(retro_video_refresh_t);
    void (*set_audio_sample)(retro_audio_sample_t);
    void (*set_audio_sample_batch)(retro_audio_sample_batch_t);
    void (*set_input_poll)(retro_input_poll_t);
    void (*set_input_state)(retro_input_state_t);
    void (*set_controller_port_device)(unsigned, unsigned);
    void (*reset)(void);
    bool (*load_game)(const struct retro_game_info*);
    void (*unload_game)(void);
    void (*run)(void);
} core_t;

// ---------------------------------------------------------------- 全局状态

static core_t g_core;
static char g_data_dir[512] = ".";
static int g_quiet = 0;

static void* g_frame;         // 上一帧副本（RGB565）
static size_t g_frame_cap;
static unsigned g_frame_w, g_frame_h;
static unsigned long g_video_frames;
static unsigned long g_distinct;
static unsigned long g_audio_frames;
static int g_audio_peak;

// Android 上卡带镜像是 mmap 到低地址的，收尾必须 munmap 而不是 free
static unsigned char* g_cart_map;
static size_t g_cart_map_len;

static void say(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char* fmt, ...) {
    if (g_quiet) return;
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

// ---------------------------------------------------------------- 回调

static void log_cb(enum retro_log_level level, const char* fmt, ...) {
    if (g_quiet || level < RETRO_LOG_WARN) return;
    va_list ap;
    va_start(ap, fmt);
    printf("[%s] ", level == RETRO_LOG_ERROR ? "E" : (level == RETRO_LOG_WARN ? "W" : "I"));
    vprintf(fmt, ap);
    va_end(ap);
}

static bool env_cb(unsigned cmd, void* data) {
    switch (cmd) {
        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
            const enum retro_pixel_format* fmt = (const enum retro_pixel_format*)data;
            // 只认 RGB565：与 py 宿主一致，核心本来就优先这个
            if (*fmt == RETRO_PIXEL_FORMAT_RGB565) {
                say("[host] SET_PIXEL_FORMAT = RGB565");
                return true;
            }
            say("[host] SET_PIXEL_FORMAT = %d（拒绝，只用 RGB565）", (int)*fmt);
            return false;
        }
        case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: {
            struct retro_log_callback* cb = (struct retro_log_callback*)data;
            cb->log = log_cb;
            return true;
        }
        case RETRO_ENVIRONMENT_SET_VARIABLES:
            say("[host] SET_VARIABLES");
            return true;
        case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
            say("[host] SET_CONTROLLER_INFO");
            return true;
        case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
            return true;
        case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS:
            return false;
        case RETRO_ENVIRONMENT_GET_CAN_DUPE: {
            if (data) *(bool*)data = true;
            return true;
        }
        case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: {
            if (data) *(const char**)data = g_data_dir;
            return true;
        }
        case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
            return false;   // 让核心走 input_state_cb，逻辑简单可预期
        case RETRO_ENVIRONMENT_GET_VARIABLE:
            return false;   // 本核心没有核心选项
        case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: {
            if (data) *(bool*)data = false;
            return true;
        }
        default:
            return false;
    }
}

// 与 read_file 配对：Android 走 munmap，其他平台走 free
static void release_cart(unsigned char* data) {
#if defined(__ANDROID__)
    (void)data;
    if (g_cart_map) {
        munmap(g_cart_map, g_cart_map_len);
        g_cart_map = NULL;
        g_cart_map_len = 0;
    }
#else
    free(data);
#endif
}

static void video_cb(const void* data, unsigned width, unsigned height, size_t pitch) {
    (void)pitch;
    if (!data || !width || !height) return;

    // 与 py 宿主同一口径：连读 width*height*2，忽略 pitch
    const size_t bytes = (size_t)width * height * 2;
    if (bytes > g_frame_cap) {
        void* p = realloc(g_frame, bytes);
        if (!p) return;
        g_frame = p;
        g_frame_cap = bytes;
    }
    memcpy(g_frame, data, bytes);
    if (width != g_frame_w || height != g_frame_h) {
        g_frame_w = width;
        g_frame_h = height;
    }
    ++g_video_frames;
}

// 独立帧：与上一帧不同才算游戏真的走了一帧（turbo 下重复帧很多）
static void note_distinct(void) {
    static unsigned char* prev;
    static size_t prev_cap;
    const size_t bytes = (size_t)g_frame_w * g_frame_h * 2;
    if (!g_frame || !bytes) return;
    if (bytes > prev_cap) {
        void* p = realloc(prev, bytes);
        if (!p) return;
        prev = p;
        prev_cap = bytes;
    }
    if (memcmp(prev, g_frame, bytes) != 0) {
        memcpy(prev, g_frame, bytes);
        ++g_distinct;
    }
}

static size_t audio_batch_cb(const int16_t* data, size_t frames) {
    g_audio_frames += frames;
    const size_t n = frames * 2;
    for (size_t i = 0; i < n; i += 4) {
        int v = data[i] < 0 ? -data[i] : data[i];
        if (v > g_audio_peak) g_audio_peak = v;
    }
    return frames;
}

static void audio_cb(int16_t left, int16_t right) {
    (void)left;
    (void)right;
}

static void input_poll_cb(void) {}

static int16_t input_state_cb(unsigned port, unsigned device, unsigned index, unsigned id) {
    (void)port;
    (void)device;
    (void)index;
    (void)id;
    return 0;   // 回归不对按键做断言，全 0 即可复现基线
}

// ---------------------------------------------------------------- 工具

static unsigned char* read_file(const char* path, size_t* out_size) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    const long size = ftell(f);
    if (size <= 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
#if defined(__ANDROID__)
    // 卡带镜像也必须落在 < 2^48：pxt 的 NaN-boxing 会把高 16 位非 0 的指针
    // 当成 double（Android 的 bionic malloc 返回 0xb400_...）。真机 RetroArch
    // 里内容 buffer 由前端分配，同样在高地址 —— 核心侧必须自己搬到低地址，
    // 这里先用低地址 mmap 模拟"正确的前端"。
    const size_t map_len = ((size_t)size + 4095u) & ~(size_t)4095u;
    unsigned char* buf = (unsigned char*)mmap((void*)(uintptr_t)0x600000000ULL, map_len,
                                              PROT_READ | PROT_WRITE,
                                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (buf == MAP_FAILED) {
        fclose(f);
        return NULL;
    }
    // 这块是 mmap 来的，不能用 free() 还（jemalloc 会在 huge_salloc 里炸）。
    // 记下地址与长度，统一由 release_cart() 走 munmap。
    g_cart_map = buf;
    g_cart_map_len = map_len;
#else
    unsigned char* buf = (unsigned char*)malloc((size_t)size);
    if (!buf) {
        fclose(f);
        return NULL;
    }
#endif
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
        release_cart(buf);   // Android: munmap；其他平台: free
        fclose(f);
        return NULL;
    }
    fclose(f);
    *out_size = (size_t)size;
    return buf;
}

static int load_core(const char* path, void** handle_out) {
    void* h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        fprintf(stderr, "[host] dlopen FAIL: %s\n", dlerror());
        return 0;
    }
#define BIND(field, name)                                                        \
    do {                                                                         \
        *(void**)(&g_core.field) = dlsym(h, name);                                \
        if (!g_core.field) {                                                      \
            fprintf(stderr, "[host] missing symbol: %s\n", name);                  \
            return 0;                                                             \
        }                                                                         \
    } while (0)
    BIND(init, "retro_init");
    BIND(deinit, "retro_deinit");
    BIND(api_version, "retro_api_version");
    BIND(get_system_info, "retro_get_system_info");
    BIND(get_system_av_info, "retro_get_system_av_info");
    BIND(set_environment, "retro_set_environment");
    BIND(set_video_refresh, "retro_set_video_refresh");
    BIND(set_audio_sample, "retro_set_audio_sample");
    BIND(set_audio_sample_batch, "retro_set_audio_sample_batch");
    BIND(set_input_poll, "retro_set_input_poll");
    BIND(set_input_state, "retro_set_input_state");
    BIND(set_controller_port_device, "retro_set_controller_port_device");
    BIND(reset, "retro_reset");
    BIND(load_game, "retro_load_game");
    BIND(unload_game, "retro_unload_game");
    BIND(run, "retro_run");
#undef BIND
    *handle_out = h;
    return 1;
}

// ---------------------------------------------------------------- main

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);   // 行缓冲：崩溃时别把结果留在缓冲区里
    if (argc < 4) {
        fprintf(stderr, "usage: %s <core.so> <cart.pxt64> <frames> [--data-dir=DIR] [--quiet]\n",
                argv[0]);
        return 2;
    }
    const char* core_path = argv[1];
    const char* cart_path = argv[2];
    int frames = atoi(argv[3]);
    for (int i = 4; i < argc; ++i) {
        if (strncmp(argv[i], "--data-dir=", 11) == 0)
            snprintf(g_data_dir, sizeof(g_data_dir), "%s", argv[i] + 11);
        else if (strcmp(argv[i], "--quiet") == 0)
            g_quiet = 1;
    }
    if (frames <= 0) {
        fprintf(stderr, "[host] frames must be > 0\n");
        return 2;
    }

    void* handle = NULL;
    if (!load_core(core_path, &handle)) return 3;

    // 顺序照 libretro 规范：环境 -> 回调 -> init
    g_core.set_environment(env_cb);
    g_core.set_video_refresh(video_cb);
    g_core.set_audio_sample(audio_cb);
    g_core.set_audio_sample_batch(audio_batch_cb);
    g_core.set_input_poll(input_poll_cb);
    g_core.set_input_state(input_state_cb);
    g_core.init();

    struct retro_system_info si;
    memset(&si, 0, sizeof(si));
    g_core.get_system_info(&si);
    say("[host] core=%s ver=%s ext=%s fullpath=%d api=%u", si.library_name ? si.library_name : "?",
        si.library_version ? si.library_version : "?", si.valid_extensions ? si.valid_extensions : "?",
        (int)si.need_fullpath, g_core.api_version());

    size_t size = 0;
    unsigned char* data = read_file(cart_path, &size);
    if (!data) {
        fprintf(stderr, "[host] cannot read cart: %s\n", cart_path);
        g_core.deinit();
        return 4;
    }
    say("[host] cart=%s (%zu bytes)", cart_path, size);

    struct retro_game_info gi;
    memset(&gi, 0, sizeof(gi));
    gi.path = cart_path;
    gi.data = data;
    gi.size = size;
    gi.meta = NULL;

    g_core.set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
    if (!g_core.load_game(&gi)) {
        fprintf(stderr, "[host] load_game FAIL (只支持 .pxt64 字节码镜像)\n");
        release_cart(data);
        g_core.deinit();
        return 5;
    }

    struct retro_system_av_info av;
    memset(&av, 0, sizeof(av));
    g_core.get_system_av_info(&av);
    say("[host] av %ux%u fps=%.3f sr=%.1f", av.geometry.base_width, av.geometry.base_height,
        av.timing.fps, av.timing.sample_rate);

    for (int i = 0; i < frames; ++i) {
        g_core.run();
        note_distinct();
    }

    if (g_frame && g_frame_w && g_frame_h) {
        char hex[33];
        mca_md5_hex(g_frame, (size_t)g_frame_w * g_frame_h * 2, hex);
        // 这两个字段是脚本 parse 的锚点，格式别改
        printf("FINAL-FRAME-HASH %s  (%ux%u, 独立帧 %lu)\n", hex, g_frame_w, g_frame_h, g_distinct);
    } else {
        printf("FINAL-FRAME-HASH NONE  (核心没有出帧)\n");
    }
    printf("STATS video=%lu distinct=%lu audio_frames=%lu audio_peak=%d\n", g_video_frames,
           g_distinct, g_audio_frames, g_audio_peak);

    g_core.unload_game();
    g_core.deinit();

    // 刻意不 dlclose：核心里有 static 析构，卸载后进程退出时 atexit 链仍会跳进
    // 已解除映射的代码段（实测 SIGSEGV 在 __run_exit_handlers）。真机 RetroArch
    // 退出时的 139 是同一类问题。测试用完直接退出，让内核回收。
    printf("HOST-OK\n");
    fflush(stdout);
    release_cart(data);
    free(g_frame);
    (void)handle;
    return 0;
}
