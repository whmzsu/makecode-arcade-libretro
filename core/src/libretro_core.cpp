// libretro_core.cpp — MakeCode Arcade libretro core（路线 B）
//
// M3 状态：pxt VM（vendored pxtapp）+ PNG/.pxt64 载入 + RetroPad 按键沿 +
//          BGRA→RGB565 输出 + 单声道音频复制成立体声。
// 载入格式：
//   * .pxt64     字节码镜像（tools/png2pxt64.py 离线转换产物）→ 直接启动 VM
//   * .png 卡带  若同目录存在 <同名>.pxt64 则自动旁载；否则提示转换并显示测试画面
//                （v0.2.8 起 valid_extensions 只报 pxt64，前端文件浏览器不再
//                 列出 .png；旁载仅供测试程序/高级用法直接调 API 时使用）
#include "../libretro.h"

#include "platform.h"
#include "png_loader.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

using namespace mca;

// ---- 崩溃现场 -----------------------------------------------------------
//
// 前端里 core 崩溃通常是"无声退出"：GUI 不显示任何东西，日志缓冲也没 flush。
// 这里挂一个未处理异常过滤器，至少把异常码与故障地址打到 stderr；
// 地址减去模块基址即为 .dll 内偏移，可直接对符号表定位函数。
// （Linux 侧的真机调试走 gdb / coredumpctl，见 docs/M3-VM-HOST.md）
#ifdef _WIN32
static LONG WINAPI mca_unhandled_filter(EXCEPTION_POINTERS* ep) {
    const EXCEPTION_RECORD* er = ep ? ep->ExceptionRecord : nullptr;
    if (er) {
        fprintf(stderr, "[MCA-CRASH] code=0x%08lx addr=%p\n",
                (unsigned long)er->ExceptionCode, er->ExceptionAddress);
        if (er->NumberParameters >= 2) {
            fprintf(stderr, "[MCA-CRASH] info0=%p info1=%p\n",
                    (void*)er->ExceptionInformation[0], (void*)er->ExceptionInformation[1]);
        }
        fflush(stderr);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

// ---- 全局（libretro 无 opaque context，用全局状态） ---------------------

static retro_environment_t g_env = nullptr;
static retro_video_refresh_t g_video = nullptr;
static retro_audio_sample_batch_t g_audio_batch = nullptr;
static retro_input_poll_t g_input_poll = nullptr;
static retro_input_state_t g_input_state = nullptr;

static retro_log_printf_t g_log = nullptr;
static bool g_pixel_format_set = false;
// 版本号只在这里出现一次：.info 打包脚本和运行时横幅都读它
static const char* const kCoreVersion = "0.2.8";

static unsigned g_frame = 0;
static bool g_game_loaded = false;

// ---- 输入配置 -----------------------------------------------------------
//
// 按键分三层，各管一段，互不重叠：
//   1. Settings -> Input -> Port 1 Controls：物理键 -> RetroPad 键（前端侧）
//   2. Quick Menu -> Controls：RetroPad 键 -> 游戏功能（我们发的描述符）
//   3. 核心选项：本核心**一个都没有**
//
// 早期版本把"交换 A/B""菜单键是谁""重置键是谁"做成核心选项，结果是同一件事
// 有两个改法、互相打架：改完选项描述符标签跟着变，Controls 页和 Options 页
// 显示的是两套说法（还出现过"两条 Menu、没有 Reset"）。
// 现在一律只走第 2 层：描述符把每个 RetroPad 键的用途写死写清楚，玩家想换键
// 就在 Controls 里把该功能绑到别的物理键 —— 不需要核心选项。
//
// 默认对齐真机掌机：RetroPad A/B = MakeCode A/B（右/下两个动作键），
// 右侧两个小键 START = 游戏内菜单、SELECT = 重置（重新开始）。
// 注意 MakeCode 的 menu 是**游戏内**菜单，与 RetroArch 自己的 "Menu Toggle"
// （呼出前端快速菜单）不是一回事，描述符标签特意用 "Game Menu" 区分。
static constexpr unsigned kMenuMask = 1u << RETRO_DEVICE_ID_JOYPAD_START;
static constexpr unsigned kResetMask = 1u << RETRO_DEVICE_ID_JOYPAD_SELECT;
static bool g_bitmask = false;      // 前端支持 RETRO_DEVICE_ID_JOYPAD_MASK
static unsigned g_port_device = RETRO_DEVICE_JOYPAD;
static bool g_prev_reset_down = false;

// 索引色 → RGB565 调色板（真机 16 色调色板的占位，M2 换成引擎实际调色板）
static uint16_t g_palette[16] = {
    0x0000, 0x002A, 0x5400, 0x542A, 0xA800, 0xA82A, 0xFC00, 0xFC2A,
    0x0015, 0x003F, 0x5415, 0x543F, 0xA815, 0xA83F, 0xFC15, 0xFC3F,
};

static uint16_t g_fb[kScreenWidth * kScreenHeight];
static int16_t g_audio[2 * 735];      // 立体声交错（音频回调格式）
static int16_t g_audio_mono[735];     // VM 输出为 44.1kHz 单声道 S16
static uint32_t g_bgra[kScreenWidth * kScreenHeight];
static uint32_t g_prev_buttons = 0;
static int g_last_panic = 0;

// 每帧从 VM 日志环倒出的回调（转发到 libretro 日志）
static void vm_log_emit(const char* text, size_t len) {
    (void)len;
    // VM 日志自带换行与时间戳前缀，且同一条会重复两遍（官方宿主亦如此），
    // 这里原样转给前端日志。
    if (g_log) g_log(RETRO_LOG_INFO, "[MCA-VM] %s", text);
}

// ---- 日志辅助 -----------------------------------------------------------

static void log_printf(enum retro_log_level level, const char* fmt, ...) {
    if (!g_log) return;
    // retro_log_printf_t 是变参而非 va_list 接口，不能直接转发 va_list
    //（ap 指针会被当成第一个变参），先格式化再转发。
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_log(level, "%s", buf);
}

// ---- libretro 必需接口 --------------------------------------------------

static bool set_pixel_format_if_needed() {
    if (g_pixel_format_set || !g_env) return g_pixel_format_set;
    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;
    if (g_env(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) {
        g_pixel_format_set = true;
        log_printf(RETRO_LOG_INFO, "[MCA] pixel format: RGB565\n");
    }
    return g_pixel_format_set;
}

// ---- 输入描述符 / 控制器类型 -------------------------------------------
//
// 每个 RetroPad 键只对应一个功能，一条一个标签，不做任何"一个键两种用途"。
// 玩家要换键就在 Controls 页把这个功能绑到别的物理键上，核心侧不需要开关。

// 把「RetroPad 键 → 游戏内功能」写进前端的 Controls 页。
// 不发这个，RetroArch 只显示 libretro 通用键名（B/Y/Select/Start…），
// 玩家不知道哪个是 MakeCode 的 A、B、菜单 —— 这就是"按键没法配"的直接原因。
// 注意数组要给 terminator 留位（N 条 + 1 空），越界会踩掉相邻静态变量
//（参考项目踩过：video 回调被踩成 NULL，画面永远出不来）。
static void set_input_descriptors() {
    if (!g_env) return;

    struct { unsigned id; const char* desc; } kDesc[] = {
        {RETRO_DEVICE_ID_JOYPAD_A,      "A -> MakeCode A (jump / OK)"},
        {RETRO_DEVICE_ID_JOYPAD_B,      "B -> MakeCode B (back / cancel)"},
        {RETRO_DEVICE_ID_JOYPAD_UP,     "D-Pad Up -> MakeCode Up"},
        {RETRO_DEVICE_ID_JOYPAD_DOWN,   "D-Pad Down -> MakeCode Down"},
        {RETRO_DEVICE_ID_JOYPAD_LEFT,   "D-Pad Left -> MakeCode Left"},
        {RETRO_DEVICE_ID_JOYPAD_RIGHT,  "D-Pad Right -> MakeCode Right"},
        // "Game Menu" 是游戏自己的菜单（controller.menu），
        // 与 RetroArch 的 "Menu Toggle"（呼出前端快速菜单）不是一回事，标签要能分开
        {RETRO_DEVICE_ID_JOYPAD_START,  "Start -> Game Menu (in-game menu key)"},
        {RETRO_DEVICE_ID_JOYPAD_SELECT, "Select -> Reset (restart game)"},
    };
    constexpr int kCount = (int)(sizeof(kDesc) / sizeof(kDesc[0]));

    static struct retro_input_descriptor d[kCount + 1];   // +1 = terminator
    std::memset(d, 0, sizeof(d));
    for (int i = 0; i < kCount; ++i) {
        d[i].port = 0;
        d[i].device = RETRO_DEVICE_JOYPAD;
        d[i].index = 0;
        d[i].id = kDesc[i].id;
        d[i].description = kDesc[i].desc;
    }
    d[kCount].id = 0;
    d[kCount].description = nullptr; // terminator

    g_env(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, d);
    log_printf(RETRO_LOG_INFO, "[MCA] input: %d descriptors sent\n", kCount);
}

// 声明端口 0 只支持 RetroPad：Controls 页的 "Device Type" 才有正确选项
static void set_controller_info() {
    if (!g_env) return;
    static struct retro_controller_description types[2] = {};
    static struct retro_controller_info infos[1] = {};
    types[0].desc = "MakeCode Arcade Gamepad";
    types[0].id = RETRO_DEVICE_JOYPAD;
    types[1].desc = nullptr;
    types[1].id = 0;
    infos[0].types = types;
    infos[0].num_types = 1;
    g_env(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, infos);
}

void retro_set_environment(retro_environment_t cb) {
    g_env = cb;
    if (!g_env) return;

    retro_log_callback log_cb = {};
    if (g_env(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log_cb))
        g_log = log_cb.log;

    bool supports_no_game = false;
    g_env(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &supports_no_game);

    // 核心选项：本核心**没有**核心选项，输入相关的一切都在 Controls 页改。
    // 发空表而不是不发，前端日志/选项页能明确看到"0 个选项"，
    // 不会让人以为漏加载了什么。
    static struct retro_variable no_vars[] = {{nullptr, nullptr}};
    g_env(RETRO_ENVIRONMENT_SET_VARIABLES, no_vars);
}

void retro_set_video_refresh(retro_video_refresh_t cb) { g_video = cb; }
void retro_set_audio_sample(retro_audio_sample_t) { /* 用 batch 接口 */ }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { g_audio_batch = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { g_input_poll = cb; }
void retro_set_input_state(retro_input_state_t cb) { g_input_state = cb; }

void retro_init(void) {
    g_frame = 0;
    g_game_loaded = false;
#ifdef _WIN32
    SetUnhandledExceptionFilter(mca_unhandled_filter);
    fprintf(stderr, "[MCA] module base %p\n", (void*)(uintptr_t)&retro_init);
    fflush(stderr);
#endif
    log_printf(RETRO_LOG_INFO, "[MCA] core init (route B skeleton)\n");
}

void retro_deinit(void) {
    // 保险：有些前端不调 retro_unload_game 就 deinit，这里再兜一次
    vm_stop();
    g_game_loaded = false;
}

unsigned retro_api_version(void) { return RETRO_API_VERSION; }

void retro_get_system_info(struct retro_system_info* info) {
    info->library_name = "MakeCode Arcade (route B)";
    info->library_version = kCoreVersion;
    info->need_fullpath = false;
    info->block_extract = false;
    info->valid_extensions = "pxt64";
}

void retro_get_system_av_info(struct retro_system_av_info* info) {
    std::memset(info, 0, sizeof(*info));
    info->geometry.base_width = kScreenWidth;
    info->geometry.base_height = kScreenHeight;
    info->geometry.max_width = kScreenWidth;
    info->geometry.max_height = kScreenHeight;
    info->geometry.aspect_ratio = 4.0f / 3.0f;
    info->timing.fps = kFrameRate;
    info->timing.sample_rate = kAudioSampleRate;
}

void retro_set_controller_port_device(unsigned port, unsigned device) {
    if (port != 0) return; // 单人：只有端口 0
    g_port_device = device;
    log_printf(RETRO_LOG_INFO, "[MCA] port 0 device -> %u (%s)\n", device,
               device == RETRO_DEVICE_JOYPAD ? "RetroPad"
               : device == RETRO_DEVICE_NONE ? "none"
                                             : "unsupported");
}

size_t retro_serialize_size(void) { return 0; }        // TODO(M4): 存档/回带
bool retro_serialize(void*, size_t) { return false; }
bool retro_unserialize(const void*, size_t) { return false; }

void retro_cheat_reset(void) {}
void retro_cheat_set(unsigned, bool, const char*) {}

// ---- 加载游戏 -----------------------------------------------------------

namespace {

// 从前端拿存档目录（GET_SAVE_DIRECTORY），失败则用系统目录，再失败用相对目录
std::string resolve_save_dir() {
    const char* dir = nullptr;
    if (g_env) {
        g_env(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &dir);
        if (dir && *dir) return std::string(dir) + "/makecode_arcade";
        g_env(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &dir);
        if (dir && *dir) return std::string(dir) + "/makecode_arcade";
    }
    return "makecode_arcade_data";
}

// .png 卡带 → 同目录 <同名>.pxt64 旁载（game->path 在内存载入时也可能提供）
bool try_sidecar_pxt64(const struct retro_game_info* game) {
    if (!game->path) return false;
    const std::string path = game->path;
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return false;
    const std::string sidecar = path.substr(0, dot) + ".pxt64";

    FILE* f = std::fopen(sidecar.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> data(static_cast<size_t>(size));
    const bool ok = std::fread(data.data(), 1, data.size(), f) == data.size();
    std::fclose(f);
    if (!ok) return false;

    log_printf(RETRO_LOG_INFO, "[MCA] sidecar bytecode found: %s\n", sidecar.c_str());
    return vm_start_from_image(data.data(), data.size(), resolve_save_dir().c_str());
}

bool load_pxt64(const uint8_t* data, size_t size) {
    const size_t img_len = pxt64_image_length(data, size);
    if (img_len < 64) {
        log_printf(RETRO_LOG_ERROR, "[MCA] not a valid .pxt64 image\n");
        return false;
    }
    return vm_start_from_image(data, size, resolve_save_dir().c_str());
}

} // namespace

bool retro_load_game(const struct retro_game_info* game) {
    if (!set_pixel_format_if_needed()) {
        log_printf(RETRO_LOG_WARN, "[MCA] frontend rejected RGB565\n");
    }

    // 输入相关的声明要在前端环境就绪后发出去，Controls 页才拿得到。
    set_controller_info();
    set_input_descriptors();
    g_bitmask = g_env && g_env(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, nullptr);
    log_printf(RETRO_LOG_INFO, "[MCA] input: bitmask=%d\n", (int)g_bitmask);

    if (!game || !game->data || game->size == 0) {
        log_printf(RETRO_LOG_ERROR, "[MCA] no game data\n");
        return false;
    }

    if (runtime_available()) {
        // VM 线程无停止接口（官方宿主为单游戏进程），换卡带需重载 core
        log_printf(RETRO_LOG_WARN,
                   "[MCA] VM already running; reload the core to switch games\n");
        return true;
    }

    g_frame = 0;
    g_prev_buttons = 0;
    g_last_panic = 0;

    const uint8_t* data = static_cast<const uint8_t*>(game->data);
    const size_t size = game->size;

    if (size > 8 && std::memcmp(data, "\x89PNG\r\n\x1a\n", 8) == 0) {
        // PNG 卡带：解出工程信息用于日志/校验；字节码走旁载文件
        CartProject project;
        if (arcade_png_load_project(data, size, project)) {
            log_printf(RETRO_LOG_INFO,
                       "[MCA] cartridge decoded: %llu bytes of source (%s)\n",
                       (unsigned long long)project.source.size(),
                       project.is_source_json ? "file table" : "bare source");
        } else {
            log_printf(RETRO_LOG_WARN, "[MCA] PNG cartridge decode failed\n");
        }
        if (try_sidecar_pxt64(game)) {
            g_game_loaded = true;
            return true;
        }
        log_printf(RETRO_LOG_WARN,
                   "[MCA] no bytecode. Convert first: "
                   "python tools/png2pxt64.py <cart.png> <cart.pxt64>\n");
        g_game_loaded = true; // 显示测试画面，不让前端报错
        return true;
    }

    if (load_pxt64(data, size)) {
        log_printf(RETRO_LOG_INFO, "[MCA] VM started (%llu-byte image)\n",
                   (unsigned long long)size);
        // 版本横幅 + trace 路径：真机上报障时先看这两行，就知道装的是哪一版、
        // 日志到底写在哪（mca_trace.log 找不到时这里会显示实际退回的位置）。
        log_printf(RETRO_LOG_INFO, "[MCA] core version %s, trace -> %s\n",
                   kCoreVersion, pxt_vm_trace_path());
        pxt_vm_trace("=== core started ===");
        g_game_loaded = true;
        return true;
    }

    log_printf(RETRO_LOG_ERROR, "[MCA] failed to start VM\n");
    // 启动半途失败时 VM 线程可能已经起来了（pxt_vm_start_buffer 立刻 spawn），
    // 这里必须收掉；否则前端随后 dlclose 就是"线程跑已卸载代码"的崩溃。
    vm_stop();
    return false;
}

bool retro_load_game_special(unsigned, const struct retro_game_info*, size_t) {
    return false;
}

void retro_unload_game(void) {
    // MCA PATCH 8：必须真正停掉 VM 线程。前端卸载内容后会 retro_deinit(),
    // 再 dlclose() 核心库；VM 线程若还活着，就是在跑已卸载的代码 → 崩溃。
    // 换游戏（同一个核心加载另一张卡带）也走这条路。
    log_printf(RETRO_LOG_INFO, "[MCA] unload -> stopping VM\n");
    vm_stop();
    g_game_loaded = false;
    g_frame = 0;
    g_prev_buttons = 0;
    g_prev_reset_down = false;
    g_last_panic = 0;
}

// ---- 复位 ---------------------------------------------------------------
//
// 两条路都通向"真重启"：
//   * 前端 Restart / Restart content → 这里
//   * 玩家按重置键（RetroPad SELECT，默认；可在 Controls 里改绑）→ retro_run 里
// 实现走 VM 自带的 systemReset()：清 fiber / handler 绑定 / GC 后
// pthread_exit，宿主再用原始镜像重新 pxt_vm_start_buffer()。
// 这是官方 vmStartFromUser() 的同一套机制，只是触发点不同。
void retro_reset(void) {
    g_frame = 0;
    g_prev_buttons = 0;
    g_prev_reset_down = false;
    g_last_panic = 0;
    std::memset(g_audio, 0, sizeof(g_audio));
    if (runtime_available()) {
        log_printf(RETRO_LOG_INFO, "[MCA] reset -> restarting game\n");
        vm_request_reset();
    }
}

unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }

void* retro_get_memory_data(unsigned) { return nullptr; }
size_t retro_get_memory_size(unsigned) { return 0; }

// ---- 每帧执行 -----------------------------------------------------------

namespace {

// M0 测试画面：滚动彩条 + 弹跳方块（M3 后替换为引擎渲染）
void render_test_frame() {
    const int stride = (g_frame / 4) % 16;
    for (int y = 0; y < kScreenHeight; y++) {
        for (int x = 0; x < kScreenWidth; x++) {
            g_fb[y * kScreenWidth + x] = g_palette[(x / 10 + y / 10 + stride) & 15];
        }
    }
    const int bx = (g_frame * 2) % (kScreenWidth - 20);
    const int by = (g_frame * 3) % (kScreenHeight - 20);
    for (int y = 0; y < 20; y++)
        for (int x = 0; x < 20; x++)
            g_fb[(by + y) * kScreenWidth + (bx + x)] = g_palette[0];
    for (int y = 2; y < 18; y++)
        for (int x = 2; x < 18; x++)
            g_fb[(by + y) * kScreenWidth + (bx + x)] = g_palette[15];
}

// 读一次前端的按键状态，返回 RetroPad id 位掩码。
// 前端支持 RETRO_DEVICE_ID_JOYPAD_MASK 时一次回调拿全部按键，
// 否则逐键查询（每帧 12 次跨模块调用，弱机上有差别）。
static unsigned read_joypad_mask() {
    if (!g_input_state) return 0;
    if (g_port_device == RETRO_DEVICE_NONE) return 0; // 前端把端口关掉了

    if (g_bitmask) {
        const int16_t m = g_input_state(0, RETRO_DEVICE_JOYPAD, 0,
                                        RETRO_DEVICE_ID_JOYPAD_MASK);
        return static_cast<unsigned>(static_cast<uint16_t>(m));
    }

    static const unsigned kIds[] = {
        RETRO_DEVICE_ID_JOYPAD_A,      RETRO_DEVICE_ID_JOYPAD_B,
        RETRO_DEVICE_ID_JOYPAD_X,      RETRO_DEVICE_ID_JOYPAD_Y,
        RETRO_DEVICE_ID_JOYPAD_LEFT,   RETRO_DEVICE_ID_JOYPAD_RIGHT,
        RETRO_DEVICE_ID_JOYPAD_UP,     RETRO_DEVICE_ID_JOYPAD_DOWN,
        RETRO_DEVICE_ID_JOYPAD_START,  RETRO_DEVICE_ID_JOYPAD_SELECT,
        RETRO_DEVICE_ID_JOYPAD_L,      RETRO_DEVICE_ID_JOYPAD_R,
        RETRO_DEVICE_ID_JOYPAD_L2,     RETRO_DEVICE_ID_JOYPAD_R2,
    };
    unsigned mask = 0;
    for (const unsigned id : kIds)
        if (g_input_state(0, RETRO_DEVICE_JOYPAD, 0, id)) mask |= 1u << id;
    return mask;
}

// RetroPad 掩码 → 街机按键位（BTN_*）。映射写死，换键在前端 Controls 里做。
static uint32_t map_buttons(unsigned jp) {
    uint32_t b = 0;
    if (jp & (1u << RETRO_DEVICE_ID_JOYPAD_A)) b |= BTN_A;
    if (jp & (1u << RETRO_DEVICE_ID_JOYPAD_B)) b |= BTN_B;
    if (jp & (1u << RETRO_DEVICE_ID_JOYPAD_LEFT)) b |= BTN_LEFT;
    if (jp & (1u << RETRO_DEVICE_ID_JOYPAD_RIGHT)) b |= BTN_RIGHT;
    if (jp & (1u << RETRO_DEVICE_ID_JOYPAD_UP)) b |= BTN_UP;
    if (jp & (1u << RETRO_DEVICE_ID_JOYPAD_DOWN)) b |= BTN_DOWN;
    // 菜单键：游戏内菜单（MakeCode 的 controller.menu）
    if (jp & kMenuMask) b |= BTN_MENU;
    return b;
}

// RetroPad 掩码 → 街机按键编号（platform.h KEY_*，与官方 Key 枚举一致）
void push_button_edges(uint32_t cur, uint32_t prev) {
    static const struct { uint32_t mask; int key; } kMap[] = {
        {BTN_A, KEY_A},     {BTN_B, KEY_B},
        {BTN_LEFT, KEY_LEFT}, {BTN_RIGHT, KEY_RIGHT},
        {BTN_UP, KEY_UP},   {BTN_DOWN, KEY_DOWN},
        {BTN_MENU, KEY_MENU},
    };
    const uint32_t changed = cur ^ prev;
    for (const auto& m : kMap) {
        if (changed & m.mask) vm_raise_key(m.key, (cur & m.mask) != 0);
    }
}

void render_vm_frame() {
    vm_flush_logs(vm_log_emit);

    vm_pull_screen(g_bgra);
    // BGRA32（字节序 B,G,R,A）→ RGB565
    for (int i = 0; i < kScreenWidth * kScreenHeight; ++i) {
        const uint32_t px = g_bgra[i];
        const uint32_t r = (px >> 16) & 0xf8;
        const uint32_t g = (px >> 8) & 0xfc;
        const uint32_t b = px & 0xf8;
        g_fb[i] = static_cast<uint16_t>((r << 8) | (g << 3) | (b >> 3));
    }

    vm_pull_audio(g_audio_mono, kAudioSamplesPerFrame);
    for (int i = 0; i < kAudioSamplesPerFrame; ++i)
        g_audio[2 * i] = g_audio[2 * i + 1] = g_audio_mono[i];

    const int panic = vm_panic_code();
    // -1 = 用户代码/宿主请求的重启中（systemReset 会把 panicCode 置 -1），
    // 不是真崩溃，不刷日志
    if (panic != 0 && panic != -1 && panic != g_last_panic) {
        log_printf(panic >= 1000 ? RETRO_LOG_WARN : RETRO_LOG_ERROR,
                   "[MCA-VM] panic code %d\n", panic);
        g_last_panic = panic;
    }
}

} // namespace

void retro_run(void) {
    if (g_input_poll) g_input_poll();

    if (g_frame == 0) log_printf(RETRO_LOG_INFO, "[MCA] first retro_run\n");

    if (runtime_available()) {
        const unsigned jp = read_joypad_mask();

        // 重置键：上升沿触发 VM 重启（真机右侧的 reset 小键）。
        // VM 没有 stop 接口，这里走 runtime 里 systemReset() 的重启路径，
        // 线程退出后由 vm_poll_restart() 用原始镜像重新启动。
        const bool reset_down = (jp & kResetMask) != 0;
        if (reset_down && !g_prev_reset_down) {
            log_printf(RETRO_LOG_INFO, "[MCA] reset button -> restarting game\n");
            vm_request_reset();
        }
        g_prev_reset_down = reset_down;

        if (vm_poll_restart()) {
            // 重启那一刻：VM 线程刚创建，画面/音频还没产出
            g_prev_buttons = 0;
            g_last_panic = 0;
            std::memset(g_audio, 0, sizeof(g_audio));
            std::memset(g_fb, 0, sizeof(g_fb));
        }

        if (!reset_pending()) {
            const uint32_t buttons = map_buttons(jp);
            push_button_edges(buttons, g_prev_buttons);
            g_prev_buttons = buttons;
        }

        // 重启等待期间**也必须继续拉帧**。上游 pxt 的屏幕是"生产者等消费者"
        // 模型：WDisplay::update() 里会等前端把上一帧取走（pxt_screen_get_pixels）。
        // 我们以前在这里一行 else if 跳过了拉取 —— VM 线程于是永远停在取帧等待里，
        // 连 mcaResetRequest 都读不到，重置就这么死住了（真机上"按重置后画面
        // 冻住、不崩不退"）。离线时拉取会立刻返回黑屏+静音，代价可忽略。
        render_vm_frame();
    } else {
        render_test_frame();
        std::memset(g_audio, 0, sizeof(g_audio));
    }

    if (g_audio_batch) g_audio_batch(g_audio, kAudioSamplesPerFrame);
    if (g_video) g_video(g_fb, kScreenWidth, kScreenHeight, kScreenWidth * sizeof(uint16_t));

    g_frame++;
}

// ---- 导出符号完整性守卫 -------------------------------------------------
//
// RetroArch 加载 core 时逐个 dlsym 下面这 25 个符号，缺任何一个都会
// "Failed to load symbol: ..." 直接放弃加载 —— 图形界面下没有任何提示，
// 表现为"RetroArch 无声退出"（曾在缺 retro_reset 时踩过：M3 只核对了 14 个）。
// 这里把全部符号强制引用一次：函数名拼错、被误删或漏实现时在编译期就报错，
// 而不是等真机上前端静默崩掉。
// 改 ABI 时同步更新 tools/inspect_binary.py 的 REQUIRED。
namespace {
typedef void (*abi_fn_t)(void);
const abi_fn_t kCoreAbi[] = {
    (abi_fn_t)retro_init,
    (abi_fn_t)retro_deinit,
    (abi_fn_t)retro_api_version,
    (abi_fn_t)retro_get_system_info,
    (abi_fn_t)retro_get_system_av_info,
    (abi_fn_t)retro_set_environment,
    (abi_fn_t)retro_set_video_refresh,
    (abi_fn_t)retro_set_audio_sample,
    (abi_fn_t)retro_set_audio_sample_batch,
    (abi_fn_t)retro_set_input_poll,
    (abi_fn_t)retro_set_input_state,
    (abi_fn_t)retro_set_controller_port_device,
    (abi_fn_t)retro_reset,
    (abi_fn_t)retro_run,
    (abi_fn_t)retro_load_game,
    (abi_fn_t)retro_load_game_special,
    (abi_fn_t)retro_unload_game,
    (abi_fn_t)retro_get_region,
    (abi_fn_t)retro_serialize_size,
    (abi_fn_t)retro_serialize,
    (abi_fn_t)retro_unserialize,
    (abi_fn_t)retro_cheat_reset,
    (abi_fn_t)retro_cheat_set,
    (abi_fn_t)retro_get_memory_data,
    (abi_fn_t)retro_get_memory_size,
};
static_assert(sizeof(kCoreAbi) / sizeof(kCoreAbi[0]) == 25,
              "libretro core must export exactly the 25 symbols RetroArch dlsym's");
} // namespace
