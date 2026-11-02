// vm_smoke.cpp — 独立宿主冒烟测试：直接跑官方 pxt VM（core---vm + screen---ext + mixer---ext）
//
// 用途：在不接 RetroArch 的前提下验证 "pxt64 字节码镜像 -> VM 执行 -> 帧输出" 链路。
// 与官方 sdlmain.cpp 的宿主契约完全一致（7 个 C 函数），只是把 SDL 换成写 BMP。
//
// 构建（见 docs/M3-VM-HOST.md）：
//   zig c++ ... vm_smoke.cpp objs/*.o -o vm_smoke.exe
// 运行：
//   vm_smoke.exe <game.pxt64> <out_prefix> [frames] [--press=KEY]

#include <chrono>
#include <sys/stat.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

// ---- 官方 VM 宿主 ABI（与 hw---vm/sdlmain.cpp 一一对应） -------------------
extern "C" {
void pxt_screen_get_pixels(int width, int height, uint32_t* screen);
void pxt_raise_event(int src, int val);
void pxt_vm_start(const char* fn);
void pxt_vm_start_buffer(uint8_t* data, unsigned len);
int pxt_get_logs(int logtype, char* dst, int maxSize);
int pxt_get_panic_code();
void pxt_get_audio_samples(int16_t* buf, unsigned numSamples);
// 宿主必须先指定数据目录，否则 settings 默认写 /tmp（Windows 上不存在）
void pxt_vm_set_data_directory(const char* path);
}

namespace {

constexpr int kWidth = 160;
constexpr int kHeight = 120;

// sdlmain.cpp 里的事件常量：2050 = KEY UP, 2051 = KEY DOWN
constexpr int kKeyUp = 2050;
constexpr int kKeyDown = 2051;

// Key 枚举（sdlmain.cpp）
enum Key {
    KEY_LEFT = 1,
    KEY_UP,
    KEY_RIGHT,
    KEY_DOWN,
    KEY_A,
    KEY_B,
    KEY_MENU,
};

int key_from_name(const std::string& s) {
    if (s == "left" || s == "LEFT") return KEY_LEFT;
    if (s == "up" || s == "UP") return KEY_UP;
    if (s == "right" || s == "RIGHT") return KEY_RIGHT;
    if (s == "down" || s == "DOWN") return KEY_DOWN;
    if (s == "a" || s == "A") return KEY_A;
    if (s == "b" || s == "B") return KEY_B;
    if (s == "menu" || s == "MENU") return KEY_MENU;
    return 0;
}

void raise_key(int key, int ev) {
    pxt_raise_event(ev, key);
    pxt_raise_event(ev, 0); // "any key"
}

// 把 BGRA32 缓冲写成 32 位 BMP（BMP 原生就是 BGRA，低位在前）
bool write_bmp(const char* path, const uint32_t* px, int w, int h) {
    const int row_bytes = w * 4;
    const int img_bytes = row_bytes * h;
    const int file_bytes = 54 + img_bytes;

    std::vector<uint8_t> file(static_cast<size_t>(file_bytes), 0);
    uint8_t* p = file.data();
    p[0] = 'B'; p[1] = 'M';
    auto put32 = [](uint8_t* d, uint32_t v) { std::memcpy(d, &v, 4); };
    auto put16 = [](uint8_t* d, uint16_t v) { std::memcpy(d, &v, 2); };
    put32(p + 2, static_cast<uint32_t>(file_bytes));
    put32(p + 10, 54);
    put32(p + 14, 40);
    put32(p + 18, static_cast<uint32_t>(w));
    put32(p + 22, static_cast<uint32_t>(h)); // 正值 = 自下而上
    put16(p + 26, 1);
    put16(p + 28, 32);
    put32(p + 34, static_cast<uint32_t>(img_bytes));

    // BMP 行序自下而上
    for (int y = 0; y < h; ++y) {
        const uint32_t* src = px + static_cast<size_t>(h - 1 - y) * w;
        std::memcpy(p + 54 + static_cast<size_t>(y) * row_bytes, src, row_bytes);
    }

    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    const bool ok = std::fwrite(file.data(), 1, file.size(), f) == file.size();
    std::fclose(f);
    return ok;
}

void flush_logs() {
    static char buf[16 * 1024];
    for (;;) {
        int n = pxt_get_logs(0, buf, static_cast<int>(sizeof(buf)) - 1);
        if (n <= 0) break;
        buf[n] = 0;
        std::fputs(buf, stderr);
        if (buf[n - 1] != '\n') std::fputc('\n', stderr);
    }
}

// 统计非零像素，判断 VM 是否真的在渲染
size_t count_nonzero(const uint32_t* px, size_t n) {
    size_t c = 0;
    for (size_t i = 0; i < n; ++i)
        if (px[i] & 0x00ffffffu) ++c;
    return c;
}

// .pxt64 文件 = 镜像 section 链 + 尾部附加的源码映射（SRCM…）。
// VM 的 loadVMImage 要求「长度 8 字节对齐」且「section 恰好吃满长度」，
// 所以这里按 section 链走一遍，得到真正的镜像长度。
size_t image_section_end(const std::vector<uint8_t>& d) {
    size_t off = 0;
    while (off + 8 <= d.size()) {
        uint32_t size = 0;
        std::memcpy(&size, d.data() + off + 4, 4);
        if (size == 0 || (size & 7) || off + size > d.size()) break;
        off += size;
    }
    return off;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <game.pxt64> <out_prefix> [frames] [--press=KEY] [--press-at=N]\n",
                     argv[0]);
        return 2;
    }

    const char* cart = argv[1];
    const char* out_prefix = argv[2];
    int frames = argc > 3 ? std::atoi(argv[3]) : 60;
    int press_key = 0;
    int press_at = 30;
    for (int i = 3; i < argc; ++i) {
        if (std::strncmp(argv[i], "--press=", 8) == 0) press_key = key_from_name(argv[i] + 8);
        else if (std::strncmp(argv[i], "--press-at=", 11) == 0) press_at = std::atoi(argv[i] + 11);
    }
    if (frames <= 0) frames = 60;

    // 1) 载入镜像（pxt_vm_start_buffer 会就地打补丁，所以要可写、且多留 16 字节余量）
    FILE* f = std::fopen(cart, "rb");
    if (!f) {
        std::fprintf(stderr, "cannot open %s\n", cart);
        return 1;
    }
    std::fseek(f, 0, SEEK_END);
    long len = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> raw(static_cast<size_t>(len), 0);
    if (std::fread(raw.data(), 1, static_cast<size_t>(len), f) != static_cast<size_t>(len)) {
        std::fprintf(stderr, "short read\n");
        std::fclose(f);
        return 1;
    }
    std::fclose(f);

    const size_t img_len = image_section_end(raw);
    std::fprintf(stderr, "[vm] file %s: %ld bytes, image sections end at %zu (trailing %ld)\n",
                 cart, len, img_len, static_cast<long>(raw.size() - img_len));
    if (img_len < 64) {
        std::fprintf(stderr, "bad image (no sections)\n");
        return 1;
    }

    // VM 会就地给镜像打补丁（写返回值/虚表指针），并且要求 8 字节对齐的内存
    std::vector<uint64_t> image(img_len / 8 + 2, 0);
    std::memcpy(image.data(), raw.data(), img_len);

    // 2) 存档/设置目录（游戏写 settings、记录分数都靠它）
    const std::string data_dir = std::string(out_prefix) + "_data";
#ifdef _WIN32
    mkdir(data_dir.c_str());
#else
    mkdir(data_dir.c_str(), 0777);
#endif
    pxt_vm_set_data_directory(data_dir.c_str());

    // 3) 启动 VM（内部起独立线程，和官方宿主一致）
    pxt_vm_start_buffer(reinterpret_cast<uint8_t*>(image.data()), static_cast<unsigned>(img_len));

    // 4) 帧循环：拉屏 + 拉音频 + 推按键
    std::vector<uint32_t> fb(static_cast<size_t>(kWidth) * kHeight, 0);
    std::vector<int16_t> audio(735 * 2, 0); // 约 1/60 秒 @44.1kHz
    bool pressed = false;
    size_t best = 0;
    int panic_code = 0;

    for (int i = 0; i < frames; ++i) {
        if (press_key && i == press_at) {
            raise_key(press_key, kKeyDown);
            pressed = true;
            std::fprintf(stderr, "[vm] frame %d: press key %d\n", i, press_key);
        } else if (pressed && i == press_at + 8) {
            raise_key(press_key, kKeyUp);
            pressed = false;
            std::fprintf(stderr, "[vm] frame %d: release key %d\n", i, press_key);
        }

        pxt_screen_get_pixels(kWidth, kHeight, fb.data());
        pxt_get_audio_samples(audio.data(), static_cast<unsigned>(audio.size() / 2));
        flush_logs();

        size_t nz = count_nonzero(fb.data(), fb.size());
        if (nz > best) best = nz;

        if (i == 0 || i == frames / 2 || i == frames - 1) {
            char path[512];
            std::snprintf(path, sizeof(path), "%s_%03d.bmp", out_prefix, i);
            if (write_bmp(path, fb.data(), kWidth, kHeight))
                std::fprintf(stderr, "[vm] frame %d: %zu/%zu px non-blank -> %s\n", i, nz, fb.size(), path);
        }

        panic_code = pxt_get_panic_code();
        if (panic_code != 0) {
            std::fprintf(stderr, "[vm] panic code %d at frame %d\n", panic_code, i);
            break;
        }

        // 按 ~60fps 节拍推进，模拟真实宿主
        // （VM 自身有调度器，宿主只需以接近刷新率的频率取帧）
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    flush_logs();
    std::fprintf(stderr, "[vm] done: frames=%d max_non_blank=%zu panic=%d\n", frames, best, panic_code);

    // 非空判定：至少 1% 像素有颜色，视为真的渲染出了内容
    return (best > static_cast<size_t>(kWidth) * kHeight / 100) ? 0 : 3;
}
