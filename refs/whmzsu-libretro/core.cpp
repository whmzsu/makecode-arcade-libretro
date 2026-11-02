/*
 * core.cpp —— libretro 核心实现（makecode_arcade_libretro，Hermes 引擎版）
 *
 * 与旧核心（makecode-arcade-core / QuickJS）的分工：
 *   .png 卡带解析、内嵌 pxt 编译器（png → binary.js）、编译落盘缓存 —— 原样复用
 *   （C 模块 cart.c / compiler.c / cache.c，编译器仍跑在临时 QuickJS 里，
 *    5 秒一次性开销，与每帧性能无关）。
 *   游戏运行时 —— 换成 Hermes：
 *     - binary.js 先经 hermes::compileJS 预编译成 HBC 字节码（AOT，一次性），
 *       再 evaluate。**字节码按内容哈希落盘**，第二次加载零编译直接跑。
 *     - pxt sim bundle（runtime JS + pxtsim/common-sim/sim）同样走 Hermes。
 *     - microtask 抽干换 rt.drainMicrotasks()；宿主函数走 jsi HostFunction。
 *
 * 每帧路径与旧核心完全一致（行为对齐，方便逐字节对比输出）：
 *   arcade_step(16.7) -> 抽干 microtask -> arcade_fb32() 取 uint32 屏缓冲
 *   -> C 转 XRGB8888 -> video_cb；arcade_audio() 拉 44.1kHz int16 -> audio_batch_cb
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <math.h>
#include <time.h>
#include <string>
#include <memory>
#include <optional>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include "libretro.h"

#include <jsi/jsi.h>
#include <hermes/hermes.h>
#include <hermes/CompileJS.h>

#include "pxt_bundle.h"
#include "cart.h"
#include "compiler.h"
#include "cache.h"

/* cache.c 的缓存目录由本文件提供（core.cpp 定义，hbc_for_source / retro_load_game 用） */
static const char *cache_dir_public(void);

#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT
#endif

#define SCREEN_W 160
#define SCREEN_H 120
#define AUDIO_HZ 44100
#define FRAME_MS (1000.0 / 60.0)

using namespace std::string_literals;
namespace jsi = facebook::jsi;

/* ---------------- libretro 回调 ---------------- */

static retro_log_printf_t log_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_t audio_sample_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;

static bool g_bitmask_supported = false;
static bool g_ab_swap = false;
static bool g_compile_cache = true;
static unsigned g_port_device = RETRO_DEVICE_JOYPAD;
static int g_select_state = 0;
static int g_pending_reset = 0;
static retro_environment_t environ_cb;

static void arc_log(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (log_cb) log_cb(RETRO_LOG_INFO, "[arcade] %s\n", buf);
    else fprintf(stderr, "[arcade] %s\n", buf);
}

/* ---------------- 性能统计（与旧核心同一格式，日志可直接对比） ---------------- */

static int g_perf_on;
static double g_perf_step, g_perf_present, g_perf_audio;
static int g_perf_frames;
static long g_blit_ok, g_blit_fb;
static double g_blit_ms;

static char g_cart_name[80];

static double perf_now_ms(void)
{
#ifdef _WIN32
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
#endif
}

static double now_ms(void) { return perf_now_ms(); }

static void perf_flush(void)
{
    if (g_perf_frames <= 0) return;
    double n = (double)g_perf_frames;
    if (g_blit_ok || g_blit_fb)
        arc_log("perf: step %.1fms present %.1fms audio %.1fms total %.1fms (budget 16.7) "
                "blit %ld/t fb %ld %.2fms",
                g_perf_step / n, g_perf_present / n, g_perf_audio / n,
                (g_perf_step + g_perf_present + g_perf_audio) / n,
                g_blit_ok / (long)n, g_blit_fb / (long)n, g_blit_ms / n);
    else
        arc_log("perf: step %.1fms present %.1fms audio %.1fms total %.1fms (budget 16.7)",
                g_perf_step / n, g_perf_present / n, g_perf_audio / n,
                (g_perf_step + g_perf_present + g_perf_audio) / n);
    g_perf_step = g_perf_present = g_perf_audio = 0;
    g_blit_ok = g_blit_fb = 0;
    g_blit_ms = 0;
    g_perf_frames = 0;
}

/* ---------------- 状态 ---------------- */

static std::unique_ptr<jsi::Runtime> g_rt;   /* Hermes 运行时（游戏） */
/* jsi::Function 没有默认构造，用 optional 持有句柄 */
static std::optional<jsi::Function> fn_boot, fn_step, fn_rgba, fn_button, fn_audio, fn_fb;
static bool g_fns_valid = false;
static char *g_source = nullptr;             /* binary.js 全文 */
static uint32_t *g_fb;
static int g_loaded;

typedef struct { const char *name; unsigned id; int state; } button_t;
static button_t g_buttons[] = {
    { "left",  RETRO_DEVICE_ID_JOYPAD_LEFT,  0 },
    { "right", RETRO_DEVICE_ID_JOYPAD_RIGHT, 0 },
    { "up",    RETRO_DEVICE_ID_JOYPAD_UP,    0 },
    { "down",  RETRO_DEVICE_ID_JOYPAD_DOWN,  0 },
    { "A",     RETRO_DEVICE_ID_JOYPAD_A,     0 },
    { "B",     RETRO_DEVICE_ID_JOYPAD_B,     0 },
    { "menu",  RETRO_DEVICE_ID_JOYPAD_START, 0 },
};
#define NBUTTONS ((int)(sizeof(g_buttons) / sizeof(g_buttons[0])))

/* ---------------- 音频 ---------------- */

typedef struct { double freq; long samples; int amp; } tone_t;
#define MAX_TONES 8
static tone_t g_tones[MAX_TONES];
static int g_ntones;
static double g_phase;
static int16_t g_audio[AUDIO_HZ / 30 * 2 + 8];

static int volume_to_amp(int vol)
{
    if (vol <= 0) return 0;
    if (vol > 255) vol = 255;
    return (int)(12000.0 * sqrt((double)vol / 255.0));
}

static void tone_push(double freq, int ms, int vol)
{
    if (g_ntones >= MAX_TONES) {
        memmove(g_tones, g_tones + 1, sizeof(tone_t) * (MAX_TONES - 1));
        g_ntones = MAX_TONES - 1;
    }
    g_tones[g_ntones].freq = freq;
    g_tones[g_ntones].samples = (long)((double)ms * AUDIO_HZ / 1000.0);
    g_tones[g_ntones].amp = volume_to_amp(vol);
    g_ntones++;
}

static void js_exception(const char *where, const jsi::JSError &e)
{
    arc_log("%s exception: %s", where, e.getMessage().c_str());
}

/* ---------------- jsi 宿主函数（对应旧核心的 __native_*） ---------------- */

static jsi::Value hf_native_log(jsi::Runtime &rt, const jsi::Value &, const jsi::Value *args, size_t count)
{
    (void)rt;
    std::string lvl = count > 0 && args[0].isString() ? args[0].getString(rt).utf8(rt) : "js";
    std::string msg = count > 1 && args[1].isString() ? args[1].getString(rt).utf8(rt) : "";
    if (!msg.empty()) arc_log("[%s] %s", lvl.c_str(), msg.c_str());
    return jsi::Value::undefined();
}

static jsi::Value hf_native_now(jsi::Runtime &rt, const jsi::Value &, const jsi::Value *, size_t)
{
    /* 微秒级时钟：量单次绘制/单帧小项 */
    return jsi::Value(now_ms() * 1000.0);
}

static jsi::Value hf_native_tone(jsi::Runtime &rt, const jsi::Value &, const jsi::Value *args, size_t count)
{
    (void)rt;
    int32_t freq = 440, ms = 100, vol = 255;
    if (count > 0 && args[0].isNumber()) freq = (int32_t)args[0].asNumber();
    if (count > 1 && args[1].isNumber()) ms = (int32_t)args[1].asNumber();
    if (count > 2 && args[2].isNumber()) vol = (int32_t)args[2].asNumber();
    if (freq > 20 && freq < 20000 && ms > 0 && ms < 5000)
        tone_push((double)freq, ms, vol);
    return jsi::Value::undefined();
}

static jsi::Value hf_native_stop_audio(jsi::Runtime &, const jsi::Value &, const jsi::Value *, size_t)
{
    g_ntones = 0;
    return jsi::Value::undefined();
}

/* ---- 精灵批量绘制（与旧核心逐字节同语义，见 core.c v0.15 注释） ---- */

static int img_data_of(jsi::Runtime &rt, const jsi::Value &img, uint8_t **data, size_t *len)
{
    if (!img.isObject()) return -1;
    jsi::Object o = img.getObject(rt);
    jsi::Value d = o.getProperty(rt, "data");
    if (!d.isObject()) return -1;
    jsi::Object dobj = d.getObject(rt);
    /* jsi 的 getArrayBuffer 是同指针强转，只能对真正的 ArrayBuffer 用；
       TypedArray 必须先取它的 .buffer 属性（与 typed_bytes_of 同一套路） */
    jsi::Value bufv = dobj.getProperty(rt, "buffer");
    if (!bufv.isObject()) return -1;
    jsi::Object bufobj = bufv.getObject(rt);
    if (!bufobj.isArrayBuffer(rt)) return -1;
    jsi::ArrayBuffer ab = bufobj.getArrayBuffer(rt);
    jsi::Value offv = dobj.getProperty(rt, "byteOffset");
    size_t off = offv.isNumber() ? (size_t)offv.asNumber() : 0;
    *data = ab.data(rt) + off;
    *len = ab.length(rt);
    return 0;
}

static int img_int_of(jsi::Runtime &rt, const jsi::Value &img, const char *key)
{
    if (!img.isObject()) return -1;
    jsi::Value v = img.getObject(rt).getProperty(rt, key);
    return v.isNumber() ? (int)v.asNumber() : -1;
}

static jsi::Value hf_native_blit(jsi::Runtime &rt, const jsi::Value &, const jsi::Value *args, size_t count)
{
    /* argv: dst, src, x, y, clear —— 语义与 sim.js drawImageCore 一致 */
    if (count < 5 || !args[0].isObject() || !args[1].isObject())
        return jsi::Value(-1);
    int x = 0, y = 0, clear = 0;
    if (args[2].isNumber()) x = (int)args[2].asNumber();
    if (args[3].isNumber()) y = (int)args[3].asNumber();
    if (args[4].isNumber()) clear = (int)args[4].asNumber();

    uint8_t *dd, *sd;
    size_t dl, sl;
    if (img_data_of(rt, args[0], &dd, &dl) || img_data_of(rt, args[1], &sd, &sl))
        return jsi::Value(-1);
    int dw = img_int_of(rt, args[0], "_width"), dh = img_int_of(rt, args[0], "_height");
    int sw = img_int_of(rt, args[1], "_width"), sh = img_int_of(rt, args[1], "_height");
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0)
        return jsi::Value(-1);
    if ((size_t)dw * dh > dl || (size_t)sw * sh > sl)
        return jsi::Value(-1);

    int sx = 0, sy = 0;
    if (x < 0) { sx = -x; x = 0; }
    if (y < 0) { sy = -y; y = 0; }
    int cols = sw - sx; if (x + cols > dw) cols = dw - x;
    int rows = sh - sy; if (y + rows > dh) rows = dh - y;
    if (cols <= 0 || rows <= 0)
        return jsi::Value(1);

    for (int r = 0; r < rows; r++) {
        uint8_t *d = dd + (size_t)(y + r) * dw + x;
        const uint8_t *s = sd + (size_t)(sy + r) * sw + sx;
        if (clear)
            memcpy(d, s, (size_t)cols);
        else
            for (int c = 0; c < cols; c++)
                if (s[c]) d[c] = s[c];
    }
    return jsi::Value(1);
}

static jsi::Value hf_native_blit_wrap(jsi::Runtime &rt, const jsi::Value &tv, const jsi::Value *args, size_t count)
{
    if (!g_perf_on)
        return hf_native_blit(rt, tv, args, count);
    double t0 = perf_now_ms();
    jsi::Value r = hf_native_blit(rt, tv, args, count);
    g_blit_ms += perf_now_ms() - t0;
    if (r.asNumber() < 0) g_blit_fb++; else g_blit_ok++;
    return r;
}

/* ---------------- 字节码缓存（Hermes 专属层） ----------------
 *
 * binary.js 是文本，Hermes 每次加载还要做一次 AOT 编译（大游戏上百 ms 级）。
 * 编译产物 HBC 是确定的，按「源码内容 + 字节码格式指纹」落盘：
 *   <存档目录>/compile-cache/hbc<16hex>.bin
 * 与 binary.js 的编译缓存同目录（cache.c 的 MCC1 容器直接复用，
 * 用不同的指纹空间避免误命中）。命中后 evaluate 的就是纯字节码。
 */

static jsi::Buffer *buffer_from_string(std::string s);

struct VecBuffer : public jsi::Buffer {
    explicit VecBuffer(std::string s) : data_(std::move(s)) {}
    size_t size() const override { return data_.size(); }
    const uint8_t *data() const override { return (const uint8_t *)data_.data(); }
    std::string data_;
};

static jsi::Buffer *buffer_from_string(std::string s)
{
    return new VecBuffer(std::move(s));
}

/* 字节码格式指纹：拿「固定探针源码」现编一份，指纹 = 产物哈希。
 * Hermes 字节码版本/编码一变指纹就变，旧缓存自动失效。 */
static void hbc_fingerprint(char out[17])
{
    std::string probe;
    if (!hermes::compileJS("1", "probe.js", probe, true))
        probe = "probe-failed";    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < probe.size(); i++) {
        h ^= probe[i];
        h *= 0x100000001b3ull;
    }
    h ^= (uint64_t)probe.size() << 40;
    snprintf(out, 17, "%016llx", (unsigned long long)h);
}

/* source 全文 -> HBC 字节码（带落盘缓存）。失败返回空串（调用方回退直接 eval 源码）。 */
static std::string hbc_for_source(const char *source, size_t len)
{
    const char *cdir = cache_dir_public();
    if (!cdir || !g_compile_cache)
        return std::string();

    char fp[17];
    hbc_fingerprint(fp);

    char key[17];
    cache_key(source, len, fp, key);

    char *data = nullptr;
    size_t dlen = 0;
    int cr = cache_load(cdir, key, fp, &data, &dlen);
    if (cr == 0) {
        std::string bc(data, dlen);
        free(data);
        arc_log("bytecode cache hit: %u bytes (%s)", (unsigned)dlen, key);
        return bc;
    }
    free(data);

    double t0 = now_ms();
    std::string bc;
    if (!hermes::compileJS(std::string(source, len), "binary.js", bc, true) || bc.empty()) {
        arc_log("hermes compileJS failed (falling back to source eval)");
        return std::string();
    }    arc_log("hermes AOT: %u bytes, %.0f ms", (unsigned)bc.size(), now_ms() - t0);

    if (cache_save(cdir, key, fp, bc.data(), bc.size()) == 0)
        arc_log("bytecode cache saved (%s/%s.bin)", cdir, key);
    return bc;
}

/* ---------------- Hermes 初始化 ---------------- */

static int js_init(void)
{
    /* JIT 默认是关的（RuntimeConfig::EnableJIT 缺省 false），必须显式打开；
       ARCADE_NO_JIT=1 可关掉做对照。堆上限与旧核心一致（192MB）。 */
    ::hermes::vm::GCConfig gc = ::hermes::vm::GCConfig::Builder()
                                    .withMaxHeapSize(192u << 20)
                                    .build();
    ::hermes::vm::RuntimeConfig::Builder cfg;
    cfg.withGCConfig(gc);
    const char *nojit = getenv("ARCADE_NO_JIT");
    if (!nojit || !nojit[0])
        cfg.withEnableJIT(true);
    g_rt = facebook::hermes::makeHermesRuntime(cfg.build());
    if (!g_rt)
        return 0;
    jsi::Runtime &rt = *g_rt;

    auto reg = [&](const char *name, int narg,
                   jsi::HostFunctionType fn) {
        rt.global().setProperty(rt, name,
            jsi::Function::createFromHostFunction(rt,
                jsi::PropNameID::forAscii(rt, name), narg, fn));
    };
    reg("__native_log", 2, hf_native_log);
    reg("__native_now", 0, hf_native_now);
    reg("__native_blit", 5, hf_native_blit_wrap);
    reg("__native_tone", 3, hf_native_tone);
    reg("__native_stop_audio", 0, hf_native_stop_audio);

    try {
        rt.evaluateJavaScript(
            std::unique_ptr<jsi::Buffer>(buffer_from_string(
                std::string((const char *)pxt_bundle, pxt_bundle_len))),
            "pxt_bundle.js");
    } catch (const jsi::JSError &e) {
        js_exception("bundle eval", e);
        return 0;
    }

    /* 缓存入口函数句柄 */
    try {
        auto getfn = [&](const char *name) -> jsi::Function {
            jsi::Value v = rt.global().getProperty(rt, name);
            if (!v.isObject() || !v.getObject(rt).isFunction(rt))
                throw std::string(name);
            return v.getObject(rt).asFunction(rt);
        };
        fn_boot = getfn("arcade_boot");
        fn_step = getfn("arcade_step");
        fn_rgba = getfn("arcade_rgba");
        fn_button = getfn("arcade_set_button");
        fn_audio = getfn("arcade_audio");
        fn_fb = getfn("arcade_fb32");
    } catch (const std::string &name) {
        arc_log("bundle did not export %s", name.c_str());
        return 0;
    }
    g_fns_valid = true;
    return 1;
}

static void js_shutdown(void)
{
    g_fns_valid = false;
    /* 先丢弃 JS 对象句柄，再销毁 runtime */
    fn_boot.reset(); fn_step.reset(); fn_rgba.reset();
    fn_button.reset(); fn_audio.reset(); fn_fb.reset();
    if (g_rt) {
        g_rt->drainMicrotasks();
        g_rt.reset();
    }
}

static int js_boot_impl(const char *source)
{
    jsi::Runtime &rt = *g_rt;
    try {
        jsi::Value arg = jsi::String::createFromUtf8(rt, source);
        jsi::Value ret = (*fn_boot).call(rt, arg);
        if (ret.isString()) {
            std::string err = ret.getString(rt).utf8(rt);
            if (!err.empty()) {
                arc_log("boot failed: %s", err.c_str());
                return 0;
            }
        }
    } catch (const jsi::JSError &e) {
        js_exception("boot", e);
        return 0;
    }

    /* 自检/基准：把 pxt 虚拟时钟钉成固定步长（与旧核心同约定） */
    const char *fd = getenv("ARCADE_FIXED_DT");
    if (fd && *fd) {
        try {
            jsi::Value f = rt.global().getProperty(rt, "__setFixedStep");
            if (f.isObject() && f.getObject(rt).isFunction(rt))
                f.getObject(rt).asFunction(rt).call(rt, jsi::Value(atoi(fd)));
        } catch (const jsi::JSError &e) {
            js_exception("setFixedStep", e);
        }
    }
    return 1;
}

/* 把 binary.js 交进 Hermes：优先走字节码缓存，没缓存则现编 */
static int js_load_source(void)
{
    std::string bc = hbc_for_source(g_source, strlen(g_source));
    jsi::Runtime &rt = *g_rt;
    try {
        if (!bc.empty()) {
            rt.evaluateJavaScript(
                std::unique_ptr<jsi::Buffer>(buffer_from_string(std::move(bc))),
                "binary.js.hbc");
        } else {
            rt.evaluateJavaScript(
                std::unique_ptr<jsi::Buffer>(buffer_from_string(std::string(g_source))),
                "binary.js");
        }
    } catch (const jsi::JSError &e) {
        js_exception("load binary.js", e);
        return 0;
    }
    return 1;
}

/* ---------------- libretro 接口 ---------------- */

EXPORT void retro_init(void)
{
    g_fb = (uint32_t *)calloc(SCREEN_W * SCREEN_H, sizeof(uint32_t));
}

EXPORT void retro_deinit(void)
{
    free(g_fb);
    g_fb = NULL;
}

EXPORT unsigned retro_api_version(void) { return RETRO_API_VERSION; }

EXPORT void retro_set_environment(retro_environment_t cb)
{
    environ_cb = cb;
    if (!cb)
        return;
    static struct retro_variable vars[] = {
        { "makecode_arcade_ab_swap",
          "Swap A/B Buttons; disabled|enabled" },
        { "makecode_arcade_compile_cache",
          "Compile Cache (png carts, ~5s saved); enabled|disabled" },
        { "makecode_arcade_bytecode_cache",
          "Bytecode Cache (Hermes AOT, faster load); enabled|disabled" },
        { "makecode_arcade_perf",
          "Performance Stats (log every 2s); disabled|enabled" },
        { NULL, NULL }
    };
    cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void *)vars);
}

EXPORT void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
EXPORT void retro_set_audio_sample(retro_audio_sample_t cb) { audio_sample_cb = cb; }
EXPORT void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
EXPORT void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
EXPORT void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }

static bool g_bytecode_cache = true;

static void read_options(void)
{
    struct retro_variable v;
    g_ab_swap = false;
    g_compile_cache = true;
    g_bytecode_cache = true;
    if (!environ_cb)
        return;
    v.key = "makecode_arcade_ab_swap";
    v.value = NULL;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &v) && v.value)
        g_ab_swap = (strcmp(v.value, "enabled") == 0);
    v.key = "makecode_arcade_compile_cache";
    v.value = NULL;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &v) && v.value)
        g_compile_cache = (strcmp(v.value, "enabled") == 0);
    v.key = "makecode_arcade_bytecode_cache";
    v.value = NULL;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &v) && v.value)
        g_bytecode_cache = (strcmp(v.value, "enabled") == 0);
    v.key = "makecode_arcade_perf";
    v.value = NULL;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &v) && v.value)
        g_perf_on = (strcmp(v.value, "enabled") == 0);
}

/*
 * 编译缓存目录（cache.c 需要；binary.js 缓存与 HBC 字节码缓存共用）。
 * RetroArch 下是 <存档目录>/compile-cache/。ARCADE_CACHE_DIR 可覆盖。
 */
static char g_cache_dir_buf[1024];
static const char *cache_dir_public(void)
{
    const char *env = getenv("ARCADE_CACHE_DIR");
    const char *base = NULL;

    if (env && env[0]) {
        snprintf(g_cache_dir_buf, sizeof(g_cache_dir_buf), "%s", env);
        return g_cache_dir_buf;
    }
    if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &base) && base && base[0]) {
        snprintf(g_cache_dir_buf, sizeof(g_cache_dir_buf), "%s/compile-cache", base);
        return g_cache_dir_buf;
    }
    return NULL;
}

static void set_input_descriptors(void)
{
    static struct retro_input_descriptor d[9];
    if (!environ_cb)
        return;
    memset(d, 0, sizeof(d));

    int n = 0;
    unsigned a_id = g_ab_swap ? RETRO_DEVICE_ID_JOYPAD_B : RETRO_DEVICE_ID_JOYPAD_A;
    unsigned b_id = g_ab_swap ? RETRO_DEVICE_ID_JOYPAD_A : RETRO_DEVICE_ID_JOYPAD_B;

    d[n].id = a_id; d[n].description = g_ab_swap ? "B -> MakeCode A (Jump/OK)" : "A -> MakeCode A (Jump/OK)"; n++;
    d[n].id = b_id; d[n].description = g_ab_swap ? "A -> MakeCode B" : "B -> MakeCode B"; n++;
    d[n].id = RETRO_DEVICE_ID_JOYPAD_UP;    d[n].description = "D-Pad Up";    n++;
    d[n].id = RETRO_DEVICE_ID_JOYPAD_DOWN;  d[n].description = "D-Pad Down";  n++;
    d[n].id = RETRO_DEVICE_ID_JOYPAD_LEFT;  d[n].description = "D-Pad Left";  n++;
    d[n].id = RETRO_DEVICE_ID_JOYPAD_RIGHT; d[n].description = "D-Pad Right"; n++;
    d[n].id = RETRO_DEVICE_ID_JOYPAD_START;  d[n].description = "Start -> Menu"; n++;
    d[n].id = RETRO_DEVICE_ID_JOYPAD_SELECT; d[n].description = "Select -> Reset Game"; n++;
    d[n].id = 0;
    d[n].description = NULL;

    for (int i = 0; i <= n; i++) {
        d[i].port = 0;
        d[i].device = RETRO_DEVICE_JOYPAD;
        d[i].index = 0;
    }
    environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, d);
}

static void set_controller_info(void)
{
    static struct retro_controller_description types[2];
    static struct retro_controller_info infos[1];
    if (!environ_cb)
        return;
    types[0].desc = "MakeCode Arcade Gamepad";
    types[0].id = RETRO_DEVICE_JOYPAD;
    types[1].desc = NULL;
    types[1].id = 0;
    infos[0].types = types;
    infos[0].num_types = 1;
    environ_cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, infos);
}

EXPORT void retro_get_system_info(struct retro_system_info *info)
{
    memset(info, 0, sizeof(*info));
    info->library_name = "MakeCode Arcade JIT (Hermes)";
    info->library_version = "1.0.0";
    info->valid_extensions = "js|png";
    info->need_fullpath = false;
    info->block_extract = true;
}

EXPORT void retro_get_system_av_info(struct retro_system_av_info *info)
{
    memset(info, 0, sizeof(*info));
    info->geometry.base_width = SCREEN_W;
    info->geometry.base_height = SCREEN_H;
    info->geometry.max_width = SCREEN_W;
    info->geometry.max_height = SCREEN_H;
    info->geometry.aspect_ratio = 4.0f / 3.0f;
    info->timing.fps = 60.0;
    info->timing.sample_rate = AUDIO_HZ;
}

EXPORT void retro_set_controller_port_device(unsigned port, unsigned device)
{
    if (port != 0)
        return;
    g_port_device = device;
}

static void set_pixel_format(void)
{
    if (environ_cb) {
        enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
        if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
            arc_log("frontend rejected XRGB8888");
    }
}

/* ---------------- 内容识别与加载 ---------------- */

static int looks_like_png(const unsigned char *d, size_t n)
{
    static const unsigned char sig[8] = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
    return n >= 8 && memcmp(d, sig, 8) == 0;
}

static void compile_log(const char *msg, void *ud)
{
    (void)ud;
    arc_log("%s", msg);
}

EXPORT bool retro_load_game(const struct retro_game_info *game)
{
    struct retro_log_callback log;
    if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
        log_cb = log.log;

    if (!game || !game->data || game->size == 0) {
        arc_log("no content data");
        return false;
    }
    set_pixel_format();
    set_controller_info();
    read_options();
    set_input_descriptors();
    g_bitmask_supported = environ_cb &&
        environ_cb(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, NULL);

    free(g_source);
    g_source = NULL;

    g_cart_name[0] = 0;
    if (game->path && game->path[0]) {
        const char *base = strrchr(game->path, '/');
        const char *p2 = strrchr(game->path, '\\');
        if (p2 > base) base = p2;
        snprintf(g_cart_name, sizeof(g_cart_name), "%s", base ? base + 1 : game->path);
    }

    if (looks_like_png((const unsigned char *)game->data, game->size)) {
        /* ---- png 卡带：当场编译（QuickJS 内嵌编译器，与旧核心同一条路） ---- */
        cart_t cart;
        memset(&cart, 0, sizeof(cart));
        arc_log("PNG 卡带 %u 字节，正在取出工程", (unsigned)game->size);
        if (cart_load_png((const unsigned char *)game->data, game->size, &cart) != 0) {
            arc_log("卡带解析失败: %s", cart.error[0] ? cart.error : "未知原因");
            cart_free(&cart);
            return false;
        }
        arc_log("工程源码 %u 字节（%s）", (unsigned)cart.source_len,
                cart.source_is_json ? "文件表" : "裸 TS");

        if (!cart.source_is_json) {
            arc_log("这张卡带只有裸 TS 源码（缺少生成的资源文件），无法编译");
            cart_free(&cart);
            return false;
        }
        if (!compiler_is_embedded()) {
            arc_log("本构建没有内嵌编译器。请先在 PC 上转换："
                    "node tools/cart_build.js 游戏.png -o out/ ，再加载 out/游戏/binary.js");
            cart_free(&cart);
            return false;
        }

        char err[256] = { 0 };
        size_t js_len = 0;
        double t0 = now_ms();

        char ckey[17] = { 0 };
        const char *cdir = g_compile_cache ? cache_dir_public() : NULL;
        int hit = 0;

        if (cdir) {
            cache_key(cart.source, cart.source_len, compiler_build_id(), ckey);
            int cr = cache_load(cdir, ckey, compiler_build_id(), &g_source, &js_len);
            if (cr == 0) {
                hit = 1;
                arc_log("编译缓存命中：binary.js %u 字节（键 %s）",
                        (unsigned)js_len, ckey);
            } else if (cr < 0) {
                arc_log("编译缓存 %s 损坏或来自别的编译器，重新编译", ckey);
            }
        }

        if (!hit) {
            arc_log("开始编译（首次约 5~6 秒）…");
            int rc = compiler_build(cart.source, cart.source_len, &g_source, &js_len,
                                    compile_log, NULL, err, sizeof(err));
            if (rc != 0) {
                arc_log("编译失败: %s", err[0] ? err : "未知错误");
                free(g_source);
                g_source = NULL;
                cart_free(&cart);
                return false;
            }
            arc_log("编译完成：binary.js %u 字节，用时 %.1f 秒",
                    (unsigned)js_len, (now_ms() - t0) / 1000.0);

            if (cdir) {
                if (cache_save(cdir, ckey, compiler_build_id(), g_source, js_len) == 0)
                    arc_log("已写入编译缓存（%s/%s.bin）", cdir, ckey);
                else
                    arc_log("编译缓存写入失败（不影响游戏）");
            }
        }
        cart_free(&cart);
    } else {
        /* ---- 已经是编译产物 binary.js ---- */
        g_source = (char *)malloc(game->size + 1);
        if (!g_source) {
            arc_log("out of memory loading binary.js (%u bytes)", (unsigned)game->size);
            return false;
        }
        memcpy(g_source, game->data, game->size);
        g_source[game->size] = '\0';
        arc_log("binary.js = %u bytes", (unsigned)game->size);
    }

    if (!js_init()) {
        js_shutdown();
        free(g_source);
        g_source = NULL;
        return false;
    }
    if (!js_load_source()) {
        js_shutdown();
        free(g_source);
        g_source = NULL;
        return false;
    }
    if (!js_boot_impl(g_source)) {
        js_shutdown();
        free(g_source);
        g_source = NULL;
        return false;
    }
    g_loaded = 1;
    arc_log("game booted: %s", g_cart_name[0] ? g_cart_name : "(stdin)");
    return true;
}

EXPORT bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t num)
{
    (void)type; (void)info; (void)num;
    return false;
}

EXPORT void retro_unload_game(void)
{
    js_shutdown();
    free(g_source);
    g_source = NULL;
    g_loaded = 0;
}

EXPORT void retro_reset(void)
{
    if (g_source) {
        js_shutdown();
        if (js_init() && js_load_source() && js_boot_impl(g_source)) {
            arc_log("game reset ok");
        } else {
            js_shutdown();
            g_loaded = 0;
            arc_log("game reset FAILED");
        }
    }
}

static void poll_input(void)
{
    if (!input_poll_cb || !input_state_cb || !g_fns_valid) return;
    input_poll_cb();

    int mask = -1;
    if (g_bitmask_supported)
        mask = (int)input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK);

    jsi::Runtime &rt = *g_rt;
    for (int i = 0; i < NBUTTONS; i++) {
        unsigned id = g_buttons[i].id;

        if (g_ab_swap) {
            if (id == RETRO_DEVICE_ID_JOYPAD_A) id = RETRO_DEVICE_ID_JOYPAD_B;
            else if (id == RETRO_DEVICE_ID_JOYPAD_B) id = RETRO_DEVICE_ID_JOYPAD_A;
        }

        int down;
        if (mask >= 0)
            down = (mask >> id) & 1;
        else
            down = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, id) ? 1 : 0;

        if (down == g_buttons[i].state) continue;
        g_buttons[i].state = down;
        try {
            jsi::Value a = jsi::String::createFromAscii(rt, g_buttons[i].name);
            (*fn_button).call(rt, a, jsi::Value(down != 0));
        } catch (const jsi::JSError &e) {
            js_exception("button", e);
        }
    }

    {
        int sel;
        if (mask >= 0)
            sel = (mask >> RETRO_DEVICE_ID_JOYPAD_SELECT) & 1;
        else
            sel = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT) ? 1 : 0;
        if (sel && !g_select_state)
            g_pending_reset = 1;
        g_select_state = sel;
    }
}

/* 取 typed array 的底层字节（加 byteOffset），拿到与长度返回 0 */
static int typed_bytes_of(jsi::Runtime &rt, const jsi::Value &v,
                          const uint8_t **out, size_t *out_bytes, size_t *out_elems)
{
    if (!v.isObject()) return -1;
    jsi::Object o = v.getObject(rt);
    jsi::Value bufv = o.getProperty(rt, "buffer");
    if (!bufv.isObject()) return -1;
    jsi::Object bufobj = bufv.getObject(rt);
    if (!bufobj.isArrayBuffer(rt)) return -1;
    jsi::ArrayBuffer ab = bufobj.getArrayBuffer(rt);
    jsi::Value offv = o.getProperty(rt, "byteOffset");
    jsi::Value lenv = o.getProperty(rt, "length");
    size_t off = offv.isNumber() ? (size_t)offv.asNumber() : 0;
    size_t elems = lenv.isNumber() ? (size_t)lenv.asNumber() : 0;
    if (!ab.data(rt)) return -1;
    *out = ab.data(rt) + off;
    *out_elems = elems;
    *out_bytes = ab.length(rt) - off;
    return 0;
}

static void present_frame(void)
{
    jsi::Runtime &rt = *g_rt;

    /* 快路径：arcade_fb32() 返回 pxt 的 uint32 屏缓冲（0xAABBGGRR） */
    try {
        if (g_fns_valid) {
            jsi::Value t = (*fn_fb).call(rt);
            const uint8_t *p = nullptr;
            size_t bytes = 0, elems = 0;
            if (typed_bytes_of(rt, t, &p, &bytes, &elems) == 0 &&
                elems >= (size_t)SCREEN_W * SCREEN_H &&
                bytes >= (size_t)SCREEN_W * SCREEN_H * 4) {
                const uint8_t *src = p;
                for (int i = 0; i < SCREEN_W * SCREEN_H; i++) {
                    uint32_t v;
                    memcpy(&v, src + (size_t)i * 4, 4);
                    g_fb[i] = 0xff000000u | ((v & 0xffu) << 16) |
                              (v & 0xff00u) | ((v >> 16) & 0xffu);
                }
                if (video_cb) video_cb(g_fb, SCREEN_W, SCREEN_H, SCREEN_W * sizeof(uint32_t));
                return;
            }
        }
    } catch (const jsi::JSError &e) {
        js_exception("fb32", e);
    }

    /* 兼容路径：RGBA 字节序（arcade_fb32 不可用时） */
    try {
        jsi::Value v = (*fn_rgba).call(rt);
        const uint8_t *p = nullptr;
        size_t bytes = 0, elems = 0;
        if (typed_bytes_of(rt, v, &p, &bytes, &elems) == 0 &&
            bytes >= (size_t)SCREEN_W * SCREEN_H * 4) {
            for (int i = 0; i < SCREEN_W * SCREEN_H; i++) {
                g_fb[i] = 0xff000000u | ((uint32_t)p[i * 4] << 16) |
                          ((uint32_t)p[i * 4 + 1] << 8) | (uint32_t)p[i * 4 + 2];
            }
        }
    } catch (const jsi::JSError &e) {
        js_exception("rgba", e);
    }

    if (video_cb) video_cb(g_fb, SCREEN_W, SCREEN_H, SCREEN_W * sizeof(uint32_t));
}

static int pull_audio(int16_t *stereo, int max_frames)
{
    jsi::Runtime &rt = *g_rt;
    try {
        jsi::Value v = (*fn_audio).call(rt);
        const uint8_t *p = nullptr;
        size_t bytes = 0, elems = 0;
        if (typed_bytes_of(rt, v, &p, &bytes, &elems) == 0 && bytes >= 2) {
            int n = (int)(bytes / 2);
            if (n > max_frames) n = max_frames;
            const int16_t *src = (const int16_t *)p;
            for (int i = 0; i < n; i++) {
                stereo[i * 2] = src[i];
                stereo[i * 2 + 1] = src[i];
            }
            return n;
        }
    } catch (const jsi::JSError &e) {
        js_exception("audio", e);
    }
    return 0;
}

EXPORT void retro_run(void)
{
    if (!g_loaded || !g_fns_valid) return;

    bool updated = false;
    if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated) {
        read_options();
        set_input_descriptors();
        arc_log("options updated: ab_swap=%d cache=%d", (int)g_ab_swap, (int)g_compile_cache);
    }

    poll_input();

    if (g_pending_reset) {
        g_pending_reset = 0;
        arc_log("reset via SELECT");
        retro_reset();
    }

    double tp0 = g_perf_on ? perf_now_ms() : 0.0;
    try {
        (*fn_step).call(*g_rt, jsi::Value(FRAME_MS));
    } catch (const jsi::JSError &e) {
        js_exception("step", e);
    }

    /* pxt 协程续体全是 promise，每帧抽干 microtask（与旧核心同语义） */
    {
        int guard = 0;
        while (!g_rt->drainMicrotasks(5000)) {
            if (++guard > 50) break;
        }
    }

    double tp1 = g_perf_on ? perf_now_ms() : 0.0;
    present_frame();
    double tp2 = g_perf_on ? perf_now_ms() : 0.0;

    int frames = pull_audio(g_audio, AUDIO_HZ / 30);
    if (frames <= 0) {
        frames = AUDIO_HZ / 60;
        memset(g_audio, 0, (size_t)frames * 2 * sizeof(int16_t));
    }
    if (audio_batch_cb) audio_batch_cb(g_audio, frames);
    else if (audio_sample_cb)
        for (int i = 0; i < frames; i++)
            audio_sample_cb((uint16_t)g_audio[i * 2], (uint16_t)g_audio[i * 2 + 1]);

    if (g_perf_on) {
        g_perf_step += tp1 - tp0;
        g_perf_present += tp2 - tp1;
        g_perf_audio += perf_now_ms() - tp2;
        if (++g_perf_frames >= 120) perf_flush();
    }
}

EXPORT size_t retro_serialize_size(void) { return 0; }

EXPORT bool retro_serialize(void *data, size_t size)
{
    (void)data; (void)size;
    return false;
}

EXPORT bool retro_unserialize(const void *data, size_t size)
{
    (void)data; (void)size;
    return false;
}

EXPORT void retro_cheat_reset(void) { }
EXPORT void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
    (void)index; (void)enabled; (void)code;
}

EXPORT unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }

void *retro_get_memory_data(unsigned id) { (void)id; return NULL; }
EXPORT size_t retro_get_memory_size(unsigned id) { (void)id; return 0; }
