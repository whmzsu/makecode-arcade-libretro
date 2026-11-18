// 极简 32 位 libretro 宿主：专测「关闭内容 → 重新打开」这条路。
//
// 为什么需要它：桌面 x86_64 构建走 PXT64 代码路径，32 位平台（含 Vita）
// 走 PXT32 A 方案（GC 旁路表 / 注入堆）。python 宿主是 64 位进程，装不了
// 32 位 .so，所以用 C 写一个最小前端，zig 编成 32 位后直接跑。
//
// 流程：init -> load -> run N 帧 -> （可选）unload/deinit/再 init/再 load -> run N 帧
// 两轮末帧哈希必须一致；不一致就是「二次加载」bug 的桌面复现。
//
// 用法：./reopen32 <core.so> <cart.pxt64> [frames] [--reinit]

#include <dlfcn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RETRO_API_VERSION 1

enum {
    RETRO_DEVICE_JOYPAD = 1,
};
enum {
    ENV_SET_ROTATION = 1,
    ENV_GET_OVERSCAN = 2,
    ENV_GET_CAN_DUPE = 3,
    ENV_SET_MESSAGE = 6,
    ENV_SHUTDOWN = 7,
    ENV_SET_PERFORMANCE_LEVEL = 8,
    ENV_GET_SYSTEM_DIRECTORY = 9,
    ENV_SET_PIXEL_FORMAT = 10,
    ENV_SET_INPUT_DESCRIPTORS = 11,
    ENV_GET_VARIABLE = 15,
    ENV_GET_VARIABLE_UPDATE = 17,
    ENV_SET_SUPPORT_NO_GAME = 18,
    ENV_GET_LOG_INTERFACE = 27,
    ENV_GET_SAVE_DIRECTORY = 31,
    ENV_GET_INPUT_BITMASKS = 51,
};
#define PIXEL_FORMAT_0RGB1555 0
#define PIXEL_FORMAT_RGB565 2

struct retro_game_info {
    const char *path;
    const void *data;
    size_t size;
    const char *meta;
};
struct retro_message {
    const char *msg;
    unsigned frames;
};
struct retro_log_callback {
    void (*log)(int level, const char *fmt, ...);
};

static int g_pixfmt = -1;
static char g_msg[256];
static unsigned char *g_frame;
static int g_w, g_h;

static void log_cb(int level, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "  [core] ");
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static bool env_cb(unsigned cmd, void *data) {
    switch (cmd) {
    case ENV_GET_LOG_INTERFACE: {
        struct retro_log_callback *cb = (struct retro_log_callback *)data;
        cb->log = log_cb;
        return true;
    }
    case ENV_SET_PIXEL_FORMAT:
        g_pixfmt = *(int *)data;
        fprintf(stderr, "  [host] pixfmt=%d\n", g_pixfmt);
        return g_pixfmt == PIXEL_FORMAT_RGB565 || g_pixfmt == PIXEL_FORMAT_0RGB1555;
    case ENV_SET_MESSAGE: {
        const struct retro_message *m = (const struct retro_message *)data;
        snprintf(g_msg, sizeof(g_msg), "%s", m->msg);
        fprintf(stderr, "  [osd] %s\n", m->msg);
        return true;
    }
    case ENV_GET_SYSTEM_DIRECTORY:
    case ENV_GET_SAVE_DIRECTORY:
        *(const char **)data = "/tmp";
        return true;
    case ENV_GET_CAN_DUPE:
        *(bool *)data = true;
        return true;
    case ENV_SET_SUPPORT_NO_GAME:
    case ENV_SET_INPUT_DESCRIPTORS:
        return true;
    case ENV_GET_INPUT_BITMASKS:
        return false;
    case ENV_GET_VARIABLE:
        return false;
    case ENV_GET_VARIABLE_UPDATE:
        *(bool *)data = false;
        return true;
    case ENV_SET_PERFORMANCE_LEVEL:
        return true;
    default:
        return false;
    }
}

static void video_cb(const void *data, unsigned w, unsigned h, size_t pitch) {
    if (!data)
        return;
    g_w = (int)w;
    g_h = (int)h;
    if (!g_frame)
        g_frame = (unsigned char *)malloc((size_t)w * h * 2);
    const unsigned char *src = (const unsigned char *)data;
    for (unsigned y = 0; y < h; y++)
        memcpy(g_frame + (size_t)y * w * 2, src + y * pitch, (size_t)w * 2);
}

static void audio_cb(int16_t l, int16_t r) { (void)l; (void)r; }
static size_t audio_batch_cb(const int16_t *d, size_t n) { (void)d; return n; }
static void input_poll_cb(void) {}
static int16_t input_state_cb(unsigned port, unsigned dev, unsigned idx, unsigned id) {
    (void)port; (void)dev; (void)idx; (void)id;
    return 0;
}

static uint32_t fnv32(const void *p, size_t n, uint32_t h) {
    const unsigned char *b = (const unsigned char *)p;
    for (size_t i = 0; i < n; i++) {
        h ^= b[i];
        h *= 16777619u;
    }
    return h;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <core.so> <cart.pxt64> [frames] [--reinit]\n", argv[0]);
        return 2;
    }
    const char *core = argv[1];
    const char *cart = argv[2];
    int frames = argc > 3 && argv[3][0] != '-' ? atoi(argv[3]) : 120;
    int reinit = 0;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--reinit"))
            reinit = 1;

    void *lib = dlopen(core, RTLD_LAZY);
    if (!lib) {
        fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return 3;
    }
#define SYM(name) name##_t name = (name##_t)dlsym(lib, #name)
    typedef void (*retro_init_t)(void);
    typedef void (*retro_deinit_t)(void);
    typedef void (*retro_set_environment_t)(bool (*)(unsigned, void *));
    typedef void (*retro_set_video_refresh_t)(void (*)(const void *, unsigned, unsigned, size_t));
    typedef void (*retro_set_audio_sample_t)(void (*)(int16_t, int16_t));
    typedef void (*retro_set_audio_sample_batch_t)(size_t (*)(const int16_t *, size_t));
    typedef void (*retro_set_input_poll_t)(void (*)(void));
    typedef void (*retro_set_input_state_t)(int16_t (*)(unsigned, unsigned, unsigned, unsigned));
    typedef bool (*retro_load_game_t)(const struct retro_game_info *);
    typedef void (*retro_unload_game_t)(void);
    typedef void (*retro_set_controller_port_device_t)(unsigned, unsigned);
    typedef void (*retro_run_t)(void);

    SYM(retro_init);
    SYM(retro_deinit);
    SYM(retro_set_environment);
    SYM(retro_set_video_refresh);
    SYM(retro_set_audio_sample);
    SYM(retro_set_audio_sample_batch);
    SYM(retro_set_input_poll);
    SYM(retro_set_input_state);
    SYM(retro_load_game);
    SYM(retro_unload_game);
    SYM(retro_set_controller_port_device);
    SYM(retro_run);
    if (!retro_init || !retro_load_game || !retro_run) {
        fprintf(stderr, "missing symbols\n");
        return 4;
    }


    FILE *f = fopen(cart, "rb");
    if (!f) {
        fprintf(stderr, "open %s failed\n", cart);
        return 5;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *raw = (unsigned char *)malloc((size_t)sz);
    if (fread(raw, 1, (size_t)sz, f) != (size_t)sz) {
        fprintf(stderr, "read failed\n");
        return 5;
    }
    fclose(f);

    struct retro_game_info gi;
    gi.path = cart;
    gi.data = raw;
    gi.size = (size_t)sz;
    gi.meta = NULL;

    for (int round = 0; round <= reinit; round++) {
        if (round > 0) {
            fprintf(stderr, "== 关闭内容：unload -> deinit -> init -> load（核心不卸载）\n");
            retro_unload_game();
            retro_deinit();
        }
        retro_set_environment(env_cb);
        retro_set_video_refresh(video_cb);
        retro_set_audio_sample(audio_cb);
        retro_set_audio_sample_batch(audio_batch_cb);
        retro_set_input_poll(input_poll_cb);
        retro_set_input_state(input_state_cb);
        retro_init();
        retro_set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
        if (!retro_load_game(&gi)) {
            fprintf(stderr, "load_game FAILED (round %d)\n", round + 1);
            return 6;
        }
        for (int i = 0; i < frames; i++)
            retro_run();

        uint32_t h = fnv32(g_frame, (size_t)g_w * g_h * 2, 2166136261u);
        fprintf(stderr, "ROUND%d-FRAME-HASH %08x (%dx%d)\n", round + 1, h, g_w, g_h);
    }
    fprintf(stderr, "done\n");
    return 0;
}
