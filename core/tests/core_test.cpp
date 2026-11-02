// core_test.cpp — 假前端：直接调 libretro 接口跑 N 帧，画面存成 BMP。
// 参考 whmzsu/mcarcade-core tests/core_test.c 的思路（PPM），这里输出
// BMP 便于直接查看；后续接引擎后用同一套跑真实游戏帧。
//
// 用法: core_test <cart.png> <out.bmp> [frames] [--press=UP|DOWN|LEFT|RIGHT|A|B|START]
//   --press=KEY   从第 5 帧起按住该键（验证游戏内手感）
//   --every=N     每 N 帧另存一张 <out>_fNNN.bmp（看动画推进）
//   --data-dir=DIR 存档 / mca_trace.log 的落盘目录（默认 out/core_test_data）
#include <cstdio>
#include <cstdarg>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#endif

#include "../libretro.h"
#include "../src/platform.h"

// core 导出的 libretro 接口（libretro_core.cpp）
extern "C" {
void retro_init(void);
void retro_deinit(void);
unsigned retro_api_version(void);
void retro_set_environment(retro_environment_t);
void retro_set_video_refresh(retro_video_refresh_t);
void retro_set_audio_sample(retro_audio_sample_t);
void retro_set_audio_sample_batch(retro_audio_sample_batch_t);
void retro_set_input_poll(retro_input_poll_t);
void retro_set_input_state(retro_input_state_t);
void retro_get_system_info(struct retro_system_info*);
void retro_get_system_av_info(struct retro_system_av_info*);
bool retro_load_game(const struct retro_game_info*);
void retro_unload_game(void);
void retro_run(void);
}

static std::vector<uint16_t> g_frame;
static unsigned g_w = 0, g_h = 0;
static unsigned g_frame_no = 0;
static uint32_t g_hold_mask = 0;
static int g_hold_from = 5;
static int g_every = 0;
static int g_delay_ms = 16;   // 每帧等待：模拟前端节拍；--delay=0 复现零延迟连续调用
static const char* g_out_base = "out";
// 存档 / mca_trace.log 的落盘目录。默认放 out/ 下——这个程序常在项目根目录
// 被调用，用 "." 会在根目录生成 makecode_arcade/settings-v0 等垃圾。--data-dir= 可覆盖。
static char g_data_dir[512] = "out/core_test_data";

static void log_stderr(enum retro_log_level, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static bool env_cb(unsigned cmd, void* data) {
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: {
        auto cb = (struct retro_log_callback*)data;
        cb->log = log_stderr;
        return true;
    }
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        return true; // 接受 core 的选择（RGB565）
    case RETRO_ENVIRONMENT_GET_CAN_DUPE: {
        *(bool*)data = true;
        return true;
    }
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        *(const char**)data = g_data_dir;
        return true;
    default:
        return false;
    }
}

static void video_cb(const void* data, unsigned w, unsigned h, size_t pitch) {
    if (!data) return;
    g_w = w; g_h = h;
    g_frame.resize((size_t)w * h);
    for (unsigned y = 0; y < h; y++)
        memcpy(&g_frame[(size_t)y * w], (const uint8_t*)data + (size_t)y * pitch, w * 2);
}

static size_t audio_batch_cb(const int16_t*, size_t frames) { return frames; }
static void input_poll_cb(void) {}

static int key_from_name(const char* n) {
    if (!strcmp(n, "UP")) return RETRO_DEVICE_ID_JOYPAD_UP;
    if (!strcmp(n, "DOWN")) return RETRO_DEVICE_ID_JOYPAD_DOWN;
    if (!strcmp(n, "LEFT")) return RETRO_DEVICE_ID_JOYPAD_LEFT;
    if (!strcmp(n, "RIGHT")) return RETRO_DEVICE_ID_JOYPAD_RIGHT;
    if (!strcmp(n, "A")) return RETRO_DEVICE_ID_JOYPAD_A;
    if (!strcmp(n, "B")) return RETRO_DEVICE_ID_JOYPAD_B;
    if (!strcmp(n, "START")) return RETRO_DEVICE_ID_JOYPAD_START;
    return -1;
}

static int16_t input_state_cb(unsigned, unsigned, unsigned idx, unsigned id) {
    (void)idx;
    if ((int)g_frame_no < g_hold_from) return false;
    return (g_hold_mask >> id) & 1;
}

static void write_bmp(const char* path, unsigned w, unsigned h,
                      const std::vector<uint16_t>& rgb565) {
    FILE* f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); return; }
    const unsigned row = w * 3, datasz = row * h, pad = (4 - (row % 4)) % 4;
    uint8_t hdr[54] = {0};
    hdr[0] = 'B'; hdr[1] = 'M';
    *(uint32_t*)(hdr + 2) = 54 + datasz;
    *(uint32_t*)(hdr + 10) = 54;
    *(uint32_t*)(hdr + 14) = 40;
    *(int32_t*)(hdr + 18) = (int32_t)w;
    *(int32_t*)(hdr + 22) = (int32_t)h;
    *(uint16_t*)(hdr + 26) = 1;
    *(uint16_t*)(hdr + 28) = 24;
    *(uint32_t*)(hdr + 34) = datasz;
    fwrite(hdr, 1, 54, f);
    std::vector<uint8_t> line(row + pad, 0);
    for (int y = (int)h - 1; y >= 0; y--) {
        for (unsigned x = 0; x < w; x++) {
            uint16_t p = rgb565[(size_t)y * w + x];
            line[x * 3 + 0] = (uint8_t)((p & 31) << 3);         // B
            line[x * 3 + 1] = (uint8_t)(((p >> 5) & 63) << 2);  // G
            line[x * 3 + 2] = (uint8_t)(((p >> 11) & 31) << 3); // R
        }
        fwrite(line.data(), 1, line.size(), f);
    }
    fclose(f);
    fprintf(stderr, "wrote %s (%ux%u)\n", path, w, h);
}

// 递归创建目录（逐级建，已存在就忽略错误）
static void ensure_dir(const char* path) {
    std::string p(path);
    for (size_t i = 1; i <= p.size(); i++) {
        if (i < p.size() && p[i] != '/' && p[i] != '\\') continue;
        std::string sub = p.substr(0, i);
#if defined(_WIN32)
        _mkdir(sub.c_str());
#else
        mkdir(sub.c_str(), 0777);
#endif
    }
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <cart.png> <out.bmp> [frames] [--press=K] [--every=N] [--delay=ms] [--data-dir=DIR]\n", argv[0]);
        return 2;
    }
    int frames = argc > 3 ? atoi(argv[3]) : 30;
    for (int i = 4; i < argc; i++) {
        if (!strncmp(argv[i], "--press=", 8)) {
            int k = key_from_name(argv[i] + 8);
            if (k < 0) { fprintf(stderr, "bad key %s\n", argv[i] + 8); return 2; }
            g_hold_mask |= 1u << k;
        } else if (!strncmp(argv[i], "--every=", 8)) {
            g_every = atoi(argv[i] + 8);
        } else if (!strncmp(argv[i], "--delay=", 8)) {
            g_delay_ms = atoi(argv[i] + 8);
        } else if (!strncmp(argv[i], "--data-dir=", 11)) {
            snprintf(g_data_dir, sizeof(g_data_dir), "%s", argv[i] + 11);
        }
    }
    g_out_base = argv[2];
    ensure_dir(g_data_dir);

    FILE* f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> buf((size_t)size);
    if (fread(buf.data(), 1, buf.size(), f) != buf.size()) return 2;
    fclose(f);

    retro_set_environment(env_cb);
    retro_set_video_refresh(video_cb);
    retro_set_audio_sample_batch(audio_batch_cb);
    retro_set_audio_sample(nullptr);
    retro_set_input_poll(input_poll_cb);
    retro_set_input_state(input_state_cb);
    retro_init();

    struct retro_game_info info = {};
    info.path = argv[1];
    info.data = buf.data();
    info.size = buf.size();
    if (!retro_load_game(&info)) { fprintf(stderr, "load_game failed\n"); return 1; }

    for (int i = 0; i < frames; i++) {
        retro_run();
        g_frame_no++;
        // 按 60fps 节拍推进：VM 在独立线程初始化（显示/GC 需要几毫秒），
        // 无节拍空转会在 VM 起跑前跑完全部帧。
        if (g_delay_ms > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(g_delay_ms));
        if (g_every && g_frame_no % g_every == 0) {
            char path[512];
            snprintf(path, sizeof(path), "%.*s_f%03d.bmp", (int)strlen(g_out_base) - 4,
                     g_out_base, g_frame_no);
            write_bmp(path, g_w, g_h, g_frame);
        }
    }
    write_bmp(g_out_base, g_w, g_h, g_frame);

    retro_unload_game();
    retro_deinit();
    return 0;
}
