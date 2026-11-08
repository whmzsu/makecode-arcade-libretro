// contract_test.cpp — 假前端「契约自检」：不需要卡带，只查 libretro 契约。
//
// 这个程序存在的理由：0.3.4 修的 SET_CONTROLLER_INFO 缺终止哨兵，是"只在
// 干净 gcc 构型才炸"的未定义行为——真前端读越界，栈上残留恰好是字符串
// 常量时 SIGSEGV，而 zig/clang 产物因为那块内存恰好为 0 一直没暴露。
// 与其再靠真机撞，不如让假前端按 libretro 规范**严格**遍历一遍：
//
//   1. get_system_info：valid_extensions 只能是 pxt64（不含 png）
//   2. 像素格式必须在 load_game 之前协商（0.3.4 之前拖到 load_game 里，
//      失败也只打 WARN，前端按默认 0RGB1555 解读 → 颜色错）
//   3. SET_INPUT_DESCRIPTORS / SET_CONTROLLER_INFO / SET_VARIABLES 三个数组
//      都必须有终止哨兵，且元素本身字段合法（缺哨兵 = 越界读）
//   4. load_game 只接受 .pxt64：喂 PNG 魔数或垃圾字节必须返回 false
//      （v0.3.5 起核心不再解码 PNG，也不为 PNG 做任何提示）
//
// 用法：contract_test [--reject-rgb565]
//   --reject-rgb565  模拟拒绝 RGB565 的前端，检查核心是否退回 0RGB1555
//                    （退回后写帧仍必须是 16bpp、尺寸不变）
//
// 退出码：0 全过；1 有断言失败；2 用法错误
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../libretro.h"

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

// ---------------------------------------------------------------- 断言
static int g_fail = 0;

static void fail(const char* fmt, ...) {
    ++g_fail;
    va_list ap;
    va_start(ap, fmt);
    std::fputs("FAIL: ", stderr);
    std::vfprintf(stderr, fmt, ap);
    std::fputc('\n', stderr);
    va_end(ap);
}

static void ok(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::fputs("ok   ", stdout);
    std::vfprintf(stdout, fmt, ap);
    std::fputc('\n', stdout);
    va_end(ap);
}

#define CHECK(cond, ...)      \
    do {                      \
        if (cond)             \
            ok(__VA_ARGS__);  \
        else                  \
            fail(__VA_ARGS__);\
    } while (0)

// ---------------------------------------------------------------- 假前端状态
// 用命名空间隔离，避免和 core 源里的同名全局碰撞
namespace fe {

static bool g_reject_rgb565 = false;
static bool g_pixel_format_done = false;   // SET_PIXEL_FORMAT 是否已收到
static bool g_pixel_format_565 = false;    // 最终接受的是不是 RGB565
static int g_pixel_format_calls = 0;
static int g_descriptors_calls = 0;
static int g_controller_calls = 0;
static int g_variables_calls = 0;
static int g_no_game_calls = 0;
static bool g_no_game_value = false;
static bool g_load_called = false;
// 第一次 load_game 那一刻的状态快照：像素格式必须已经协商完
static bool g_pf_at_first_load = false;

static bool env_cb(unsigned cmd, void* data) {
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: {
        auto* cb = static_cast<struct retro_log_callback*>(data);
        cb->log = nullptr;  // 本测试不接日志（静音，输出只看断言）
        return true;
    }
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
        auto* fmt = static_cast<enum retro_pixel_format*>(data);
        ++g_pixel_format_calls;
        g_pixel_format_done = true;
        if (*fmt != RETRO_PIXEL_FORMAT_RGB565 && *fmt != RETRO_PIXEL_FORMAT_0RGB1555) {
            fail("SET_PIXEL_FORMAT 收到未知格式 %d", (int)*fmt);
            return false;
        }
        if (g_reject_rgb565 && *fmt == RETRO_PIXEL_FORMAT_RGB565) {
            g_pixel_format_565 = false;
            return false;  // 明确拒绝
        }
        g_pixel_format_565 = (*fmt == RETRO_PIXEL_FORMAT_RGB565);
        return true;
    }
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME: {
        ++g_no_game_calls;
        g_no_game_value = data ? *static_cast<bool*>(data) : false;
        return true;
    }
    case RETRO_ENVIRONMENT_SET_VARIABLES: {
        ++g_variables_calls;
        auto* v = static_cast<struct retro_variable*>(data);
        int n = 0;
        if (!v) {
            fail("SET_VARIABLES 传了 NULL 数组");
            return false;
        }
        // 规范：key == NULL 结束；核心声明"0 个选项"，所以第一条就该结束
        while (n < 32 && v[n].key) {
            fail("核心选项非空：%s = %s", v[n].key, v[n].value ? v[n].value : "(null)");
            ++n;
        }
        if (n >= 32) fail("SET_VARIABLES 没有终止哨兵（越界读风险）");
        else ok("SET_VARIABLES 终止哨兵正常（%d 个选项）", n);
        return true;
    }
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS: {
        ++g_descriptors_calls;
        auto* d = static_cast<struct retro_input_descriptor*>(data);
        int n = 0;
        if (!d) {
            fail("SET_INPUT_DESCRIPTORS 传了 NULL 数组");
            return false;
        }
        while (n < 64 && d[n].description) {
            if (d[n].port > 0) fail("描述符 port 超范围：%u", d[n].port);
            if (d[n].device != RETRO_DEVICE_JOYPAD)
                fail("描述符 device 不是 JOYPAD：%u", d[n].device);
            ++n;
        }
        if (n >= 64) fail("SET_INPUT_DESCRIPTORS 没有终止哨兵（越界读风险）");
        else if (n == 0) fail("SET_INPUT_DESCRIPTORS 一个描述符都没有");
        else ok("SET_INPUT_DESCRIPTORS 终止哨兵正常（%d 条）", n);
        return true;
    }
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO: {
        ++g_controller_calls;
        auto* info = static_cast<struct retro_controller_info*>(data);
        int n = 0;
        if (!info) {
            fail("SET_CONTROLLER_INFO 传了 NULL 数组");
            return false;
        }
        // 这一条正是 0.3.4 崩溃的地方：前端只按 types == NULL 停止，
        // 没有长度参数，缺哨兵就会读栈上残留。
        while (n < 8 && info[n].types) {
            if (info[n].num_types == 0) fail("控制器条目 %d 的 num_types 为 0", n);
            for (unsigned i = 0; i < info[n].num_types && i < 8; ++i) {
                if (!info[n].types[i].desc)
                    fail("控制器条目 %d 的类型 %u 缺 desc", n, i);
            }
            ++n;
        }
        if (n >= 8) fail("SET_CONTROLLER_INFO 没有终止哨兵（0.3.4 的崩溃点）");
        else if (n == 0) fail("SET_CONTROLLER_INFO 一个端口都没声明");
        else ok("SET_CONTROLLER_INFO 终止哨兵正常（%d 个端口）", n);
        return true;
    }
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
        return false;  // 走逐键查询路径
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY: {
        static const char* dir = "contract_test_data";
        *static_cast<const char**>(data) = dir;
        return true;
    }
    case RETRO_ENVIRONMENT_GET_CAN_DUPE: {
        if (data) *static_cast<bool*>(data) = true;
        return true;
    }
    default:
        return false;
    }
}

// 视频回调：只校验形状，不看内容（没有卡带时是测试画面）
static unsigned g_vw = 0, g_vh = 0;
static size_t g_vpitch = 0;
static int g_video_frames = 0;

static void video_cb(const void* data, unsigned w, unsigned h, size_t pitch) {
    if (!data) return;  // dup 帧
    ++g_video_frames;
    g_vw = w;
    g_vh = h;
    g_vpitch = pitch;
}

static size_t audio_batch_cb(const int16_t*, size_t frames) { return frames; }
static void input_poll_cb(void) {}
static int16_t input_state_cb(unsigned, unsigned, unsigned, unsigned) { return 0; }

}  // namespace fe

// ---------------------------------------------------------------- main
int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--reject-rgb565")) {
            fe::g_reject_rgb565 = true;
        } else {
            std::fprintf(stderr, "usage: %s [--reject-rgb565]\n", argv[0]);
            return 2;
        }
    }

    // ---- 1. 系统信息 ----------------------------------------------------
    struct retro_system_info sys = {};
    retro_get_system_info(&sys);
    CHECK(sys.library_name && *sys.library_name, "library_name 非空: %s",
          sys.library_name ? sys.library_name : "(null)");
    CHECK(sys.library_version && *sys.library_version, "library_version 非空: %s",
          sys.library_version ? sys.library_version : "(null)");
    CHECK(sys.valid_extensions && !std::strcmp(sys.valid_extensions, "pxt64"),
          "valid_extensions 只声明 pxt64（实际: %s）",
          sys.valid_extensions ? sys.valid_extensions : "(null)");
    CHECK(!sys.need_fullpath, "need_fullpath=false（内容从内存加载）");
    CHECK(!sys.block_extract, "block_extract=false（不让前端解压 .pxt64）");
    CHECK(retro_api_version() == RETRO_API_VERSION, "api_version == %d",
          RETRO_API_VERSION);

    // ---- 2. 环境就绪 ----------------------------------------------------
    retro_set_environment(fe::env_cb);
    retro_set_video_refresh(fe::video_cb);
    retro_set_audio_sample(nullptr);
    retro_set_audio_sample_batch(fe::audio_batch_cb);
    retro_set_input_poll(fe::input_poll_cb);
    retro_set_input_state(fe::input_state_cb);
    retro_init();

    struct retro_system_av_info av = {};
    retro_get_system_av_info(&av);
    CHECK(av.geometry.base_width == 160 && av.geometry.base_height == 120,
          "geometry %ux%u", av.geometry.base_width, av.geometry.base_height);
    CHECK(av.timing.fps > 0.0 && av.timing.sample_rate > 0.0,
          "timing fps=%.1f sample_rate=%.0f", av.timing.fps, av.timing.sample_rate);

    // ---- 3. 无效内容必须被拒绝 -----------------------------------------
    // 用 PNG 魔数开头：v0.3.5 起核心不再解码 PNG，应走通用校验失败，
    // 绝不能像旧实现那样 return true + 显示测试画面。
    static const uint8_t kPngLike[64] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    struct retro_game_info gi = {};
    gi.path = "contract_test.png";
    gi.data = kPngLike;
    gi.size = sizeof(kPngLike);
    fe::g_pf_at_first_load = fe::g_pixel_format_done;
    fe::g_load_called = true;
    bool loaded = retro_load_game(&gi);
    CHECK(!loaded, "PNG 魔数内容 load_game 返回 false（不显示彩条、不谎报成功）");

    uint8_t junk[16];
    std::memset(junk, 0xAA, sizeof(junk));
    gi.path = "contract_test.bin";
    gi.data = junk;
    gi.size = sizeof(junk);
    loaded = retro_load_game(&gi);
    CHECK(!loaded, "垃圾字节 load_game 返回 false");

    loaded = retro_load_game(nullptr);
    CHECK(!loaded, "load_game(NULL) 返回 false");

    // ---- 4. 各项环境声明的时机与内容 -----------------------------------
    CHECK(fe::g_pixel_format_calls > 0, "SET_PIXEL_FORMAT 已发出（%d 次）",
          fe::g_pixel_format_calls);
    CHECK(fe::g_pf_at_first_load,
          "像素格式在首次 load_game 之前协商（顺序正确）");
    if (fe::g_reject_rgb565) {
        CHECK(!fe::g_pixel_format_565, "拒绝 RGB565 后核心退回 0RGB1555");
    } else {
        CHECK(fe::g_pixel_format_565, "核心使用 RGB565");
    }
    CHECK(fe::g_descriptors_calls > 0, "SET_INPUT_DESCRIPTORS 已发出（%d 次）",
          fe::g_descriptors_calls);
    CHECK(fe::g_controller_calls > 0, "SET_CONTROLLER_INFO 已发出（%d 次）",
          fe::g_controller_calls);
    if (fe::g_no_game_calls) {
        // 传 false 等于没声明，属无效调用；真声明就该是 true
        CHECK(fe::g_no_game_value, "SET_SUPPORT_NO_GAME 只在 true 时才发");
    } else {
        ok("SET_SUPPORT_NO_GAME 未发送（不需要无内容模式）");
    }

    // ---- 5. 失败后的运行期仍然安全 -------------------------------------
    // 前端在 load 失败后不会调 retro_run，但核心自己的测试程序会；这里顺带
    // 确认测试画面路径不越界、pitch 仍按 16bpp 计算。
    for (int i = 0; i < 3; ++i) retro_run();
    CHECK(fe::g_video_frames >= 1, "retro_run 在无内容时也出帧（%d 帧）",
          fe::g_video_frames);
    CHECK(fe::g_vw == 160 && fe::g_vh == 120, "输出尺寸 %ux%u", fe::g_vw, fe::g_vh);
    CHECK(fe::g_vpitch == 160 * 2, "pitch=%zu（16bpp 行宽）", fe::g_vpitch);

    retro_unload_game();
    retro_deinit();

    if (g_fail) {
        std::fprintf(stderr, "\n%d 项契约检查失败\n", g_fail);
        return 1;
    }
    std::fputs("\n契约自检全部通过\n", stdout);
    return 0;
}
