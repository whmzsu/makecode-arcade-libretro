// runner.c —— Android 真机上的无头回归宿主（NativeActivity，targetSdk 28）
//
// 为什么要有它：真机验证最大的痛点是"每次都要在 RetroArch 里手点"。这个 APK
// 把 tools/mca_host_android.c 那套逻辑搬进 app 域，`am start` 一次就自动跑完
// 一批卡带、把末帧哈希写进 logcat 和 results.txt，全程不用碰屏幕。
//
// 为什么 targetSdk 必须是 28（见 docs/ANDROID-2026-11-08.md）：
// Android 11 起 bionic 对 targetSdk >= 30 的进程开启堆指针打标（TBI），malloc
// 返回 0xb400_...，而 pxt 的 isDouble(v) = (v >> 48) != 0 会把它们全判成数字
// → PANIC 906。RetroArch 自己的 targetSdk 就是 28，所以它在真机上没事；我们要
// 在 app 域复现同样的环境，targetSdk 也只能是 28。这也让这个 APK 变成了那条
// 结论的持续验证：只要它还能跑出哈希，说明打标假设仍然成立。
//
// 输入（都在 externalDataPath = /sdcard/Android/data/<pkg>/files 下）：
//   carts/*.pxt64     要跑的卡带（全部按文件名排序跑一遍）
//   only.txt          可选，只跑这里面写的那个文件名
//   frames.txt        可选，跑多少帧，默认 600
// 输出：
//   logcat tag MCA-TEST，逐张 RESULT 行；以及 results.txt（同样内容）
//   core 自己的日志走 MCA-CORE tag
//
// 用法（见 run_on_device.sh）：
//   adb push carts/*.pxt64 /sdcard/Android/data/org.mca.coretest/files/carts/
//   adb shell am start -n org.mca.coretest/android.app.NativeActivity
//   adb logcat -d -s MCA-TEST

#include <android/log.h>
#include <android_native_app_glue.h>
#include <dirent.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "libretro.h"
#include "mca_md5.h"

#define LOG_TAG "MCA-TEST"
#define CORE_LOG_TAG "MCA-CORE"

// ---------------------------------------------------------------- 输出

static char g_ext[512];      // externalDataPath
static FILE* g_results;      // 结果文件（可能为 NULL）
static int g_quiet_core;     // 核心 INFO 级日志默认不往 logcat 灌

static void say(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "%s", buf);
    if (g_results) {
        fputs(buf, g_results);
        fputc('\n', g_results);
        fflush(g_results);   // 逐行刷：万一某个卡带把进程打崩，前面的结果还在
    }
}

static void core_log(enum retro_log_level level, const char* fmt, ...) {
    if (g_quiet_core && level < RETRO_LOG_WARN) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    const char* s = buf;
    size_t n = strlen(buf);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
    int prio = level == RETRO_LOG_ERROR ? ANDROID_LOG_ERROR
               : level == RETRO_LOG_WARN ? ANDROID_LOG_WARN
                                         : ANDROID_LOG_INFO;
    __android_log_print(prio, CORE_LOG_TAG, "%s", s);
}

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

static core_t g_core;
static char g_data_dir[512] = ".";

static void* g_frame;
static size_t g_frame_cap;
static unsigned g_frame_w, g_frame_h;
static unsigned long g_video_frames;
static unsigned long g_distinct;
static unsigned long g_audio_frames;
static int g_audio_peak;

// ---------------------------------------------------------------- 回调

static bool env_cb(unsigned cmd, void* data) {
    switch (cmd) {
        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
            const enum retro_pixel_format* fmt = (const enum retro_pixel_format*)data;
            if (*fmt == RETRO_PIXEL_FORMAT_RGB565) return true;
            say("WARN 前端拒绝像素格式 %d（只认 RGB565）", (int)*fmt);
            return false;
        }
        case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: {
            struct retro_log_callback* cb = (struct retro_log_callback*)data;
            cb->log = core_log;
            return true;
        }
        case RETRO_ENVIRONMENT_SET_VARIABLES:
        case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
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
            return false;   // 走 input_state_cb，逻辑简单可预期
        case RETRO_ENVIRONMENT_GET_VARIABLE:
            return false;
        case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: {
            if (data) *(bool*)data = false;
            return true;
        }
        default:
            return false;
    }
}

static void video_cb(const void* data, unsigned width, unsigned height, size_t pitch) {
    (void)pitch;
    if (!data || !width || !height) return;
    const size_t bytes = (size_t)width * height * 2;   // 口径与 py 宿主一致
    if (bytes > g_frame_cap) {
        void* p = realloc(g_frame, bytes);
        if (!p) return;
        g_frame = p;
        g_frame_cap = bytes;
    }
    memcpy(g_frame, data, bytes);
    g_frame_w = width;
    g_frame_h = height;
    ++g_video_frames;
}

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
    // targetSdk 28 → bionic 不打 TBI tag → 普通 malloc 的指针高 16 位为 0，
    // pxt 的 NaN-boxing 可以直接用。这里**不要**再学 mca_host_android.c 去
    // 手工 mmap 低地址：那是给 shell 域进程擦屁股的，app 域不需要。
    unsigned char* buf = (unsigned char*)malloc((size_t)size);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)size, f) != (size_t)size) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *out_size = (size_t)size;
    return buf;
}

static int load_core(void** handle_out) {
    void* h = dlopen("libmca_core.so", RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        say("FATAL dlopen libmca_core.so 失败: %s", dlerror());
        return 0;
    }
#define BIND(field, name)                                                        \
    do {                                                                         \
        *(void**)(&g_core.field) = dlsym(h, name);                                \
        if (!g_core.field) {                                                      \
            say("FATAL 缺符号: %s", name);                                         \
            return 0;                                                            \
        }                                                                        \
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

// 跑一张卡带；返回 0 成功，非 0 失败
static int run_one(const char* cart_path, const char* cart_name, int frames) {
    size_t size = 0;
    unsigned char* data = read_file(cart_path, &size);
    if (!data) {
        say("FAIL  %s  读不到文件", cart_name);
        return 4;
    }
    struct retro_game_info gi;
    memset(&gi, 0, sizeof(gi));
    gi.path = cart_path;
    gi.data = data;
    gi.size = size;
    gi.meta = NULL;

    g_frame_w = g_frame_h = 0;
    g_video_frames = g_distinct = g_audio_frames = 0;
    g_audio_peak = 0;

    if (!g_core.load_game(&gi)) {
        say("FAIL  %s  load_game 返回 false（%zu 字节）", cart_name, size);
        free(data);
        return 5;
    }
    for (int i = 0; i < frames; ++i) {
        g_core.run();
        note_distinct();
    }

    char hex[33] = "none";
    if (g_frame && g_frame_w && g_frame_h)
        mca_md5_hex(g_frame, (size_t)g_frame_w * g_frame_h * 2, hex);

    // 这一行是脚本 parse 的锚点，格式别改
    say("RESULT %s %s frames=%d %ux%u distinct=%lu audio=%lu/%d", cart_name, hex, frames,
        g_frame_w, g_frame_h, g_distinct, g_audio_frames, g_audio_peak);

    g_core.unload_game();
    free(data);
    return 0;
}

static int cmp_str(const void* a, const void* b) {
    return strcmp(*(const char* const*)a, *(const char* const*)b);
}

static int read_int_file(const char* path, int dflt) {
    FILE* f = fopen(path, "rb");
    if (!f) return dflt;
    int v = dflt;
    if (fscanf(f, "%d", &v) != 1 || v <= 0) v = dflt;
    fclose(f);
    return v;
}

static void read_line_file(const char* path, char* out, size_t cap) {
    out[0] = 0;
    FILE* f = fopen(path, "rb");
    if (!f) return;
    if (fgets(out, (int)cap, f)) {
        size_t n = strlen(out);
        while (n && (out[n - 1] == '\n' || out[n - 1] == '\r')) out[--n] = 0;
    }
    fclose(f);
}

// ---------------------------------------------------------------- 主流程（跑在自己的线程上）

static void* worker(void* arg) {
    struct android_app* app = (struct android_app*)arg;

    snprintf(g_data_dir, sizeof(g_data_dir), "%s", g_ext);
    char path[1024];

    char results_path[1024];
    snprintf(results_path, sizeof(results_path), "%s/results.txt", g_ext);
    g_results = fopen(results_path, "wb");
    if (!g_results) say("WARN 写不了 %s，结果只进 logcat", results_path);

    snprintf(path, sizeof(path), "%s/frames.txt", g_ext);
    const int frames = read_int_file(path, 600);

    void* handle = NULL;
    if (!load_core(&handle)) {
        say("FATAL 核心加载失败，结束");
        if (g_results) fclose(g_results);
        ANativeActivity_finish(app->activity);
        return NULL;
    }

    struct retro_system_info si;
    memset(&si, 0, sizeof(si));
    g_core.get_system_info(&si);
    say("CORE %s %s ext=%s fullpath=%d api=%u targetSdk=28",
        si.library_name ? si.library_name : "?", si.library_version ? si.library_version : "?",
        si.valid_extensions ? si.valid_extensions : "?", (int)si.need_fullpath,
        g_core.api_version());

    g_core.set_environment(env_cb);
    g_core.set_video_refresh(video_cb);
    g_core.set_audio_sample(audio_cb);
    g_core.set_audio_sample_batch(audio_batch_cb);
    g_core.set_input_poll(input_poll_cb);
    g_core.set_input_state(input_state_cb);
    g_core.init();
    g_core.set_controller_port_device(0, RETRO_DEVICE_JOYPAD);

    // only.txt：只跑一张（脚本按卡带逐个启动进程，天然隔离，一张崩了不影响下一张）
    char only[256];
    snprintf(path, sizeof(path), "%s/only.txt", g_ext);
    read_line_file(path, only, sizeof(only));

    int failures = 0;
    if (only[0]) {
        snprintf(path, sizeof(path), "%s/carts/%s", g_ext, only);
        say("RUN   %s frames=%d", only, frames);
        failures += run_one(path, only, frames) ? 1 : 0;
    } else {
        char dir_path[1024];
        snprintf(dir_path, sizeof(dir_path), "%s/carts", g_ext);
        DIR* d = opendir(dir_path);
        if (!d) {
            say("FATAL 打不开 %s", dir_path);
            if (g_results) fclose(g_results);
            ANativeActivity_finish(app->activity);
            return NULL;
        }
        char* names[256];
        int n = 0;
        struct dirent* e;
        while ((e = readdir(d)) && n < 256) {
            const size_t len = strlen(e->d_name);
            if (len < 7 || strcmp(e->d_name + len - 6, ".pxt64") != 0) continue;
            names[n++] = strdup(e->d_name);
        }
        closedir(d);
        qsort(names, (size_t)n, sizeof(char*), cmp_str);
        say("BATCH %d 张卡带，每张 %d 帧", n, frames);
        for (int i = 0; i < n; ++i) {
            snprintf(path, sizeof(path), "%s/carts/%s", dir_path, names[i]);
            if (run_one(path, names[i], frames) != 0) ++failures;
            free(names[i]);
        }
    }

    say("DONE failures=%d", failures);

    g_core.deinit();
    // 刻意不 dlclose：核心里有 static 析构，卸载后进程退出时 atexit 链会跳进
    // 已解除映射的代码段。让进程直接结束更安全。
    if (g_results) fclose(g_results);
    ANativeActivity_finish(app->activity);
    (void)handle;
    return NULL;
}

// ---------------------------------------------------------------- NativeActivity 入口（native_app_glue）

void android_main(struct android_app* app) {
    g_quiet_core = 1;
    app->onAppCmd = NULL;
    snprintf(g_ext, sizeof(g_ext), "%s",
             app->activity->externalDataPath ? app->activity->externalDataPath : "/sdcard");

    // 我们不画东西，所以不等窗口就绪：直接在工作线程里把活干完，
    // 主线程只负责把事件丢掉（glue 要求有人消费事件队列）。
    worker(app);
    while (!app->destroyRequested) {
        struct android_poll_source* src = NULL;
        int events = 0;
        const int timeout = 100;   // 100ms 后返回，好检查 destroyRequested
        if (ALooper_pollOnce(timeout, NULL, &events, (void**)&src) >= 0 && src)
            if (src->process) src->process(app, src);
    }
}
