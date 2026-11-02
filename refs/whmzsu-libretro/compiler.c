/*
 * compiler.c —— 在核心里跑官方 pxt 编译器（png 卡带 → binary.js）
 *
 * 思路：
 *   retro_load_game 收到一张 png 卡带 → cart_load_png 取出工程文件表（JSON）
 *   → 这里开一个**临时** JSRuntime，注入内嵌的 pxt_hack.js（自带 TypeScript 编译器）
 *     和 target.json（pxt-arcade 的全部库源码），跑官方
 *     Host → MainPackage.installAllAsync → getCompileOptionsAsync → pxtc.compile
 *   → 拿到 binary.js 后立刻销毁这个 runtime（峰值 ~210MB 不残留），交给主 runtime 跑。
 *
 * 文件系统在 JS 侧（runtime/compiler.js）用内存 VFS 实现，C 这里只提供：
 *   __asset(name)  取内嵌资产（LZMA 解压）
 *   __emit(src)    接收编译产物
 *   __fail(msg)    编译失败
 *   __log(msg)     进度/诊断
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

#include "compiler.h"

#if defined(ARCADE_NO_INCART_COMPILER)

int compiler_is_embedded(void) { return 0; }

const char *compiler_build_id(void) { return "none"; }

int compiler_build(const char *proj_json, size_t proj_len,
                   char **out_js, size_t *out_len,
                   void (*log)(const char *msg, void *ud), void *log_ud,
                   char *err, size_t errcap)
{
    (void)proj_json; (void)proj_len; (void)out_js; (void)out_len;
    (void)log; (void)log_ud;
    if (err && errcap) snprintf(err, errcap, "这个构建没有内嵌编译器");
    return -1;
}

#else  /* ---------------- 正式实现 ---------------- */

#include "quickjs.h"
#include "decompress.h"
#include "pxt_compiler_bundle.h"

#ifndef COMPILER_MEM_MB
#define COMPILER_MEM_MB 0     /* 0 = 不设上限；实测编译峰值约 210MB */
#endif

/*
 * 编译器递归能吃到多深的栈。
 *
 * 为什么不是 4MB：QuickJS 只在**超过**这个上限时才抛 stack overflow，宿主线程的
 * 真实栈它管不了。RetroArch 在 Windows 上给主线程的栈只有 1MB（我们自己编的
 * core_test.exe 是 16MB，所以早期 4MB 一直没出事），上限一旦大于宿主栈，
 * 递归深了就是段错误而不是报错。
 *
 * 实测（把上限逐级压低复测）：ping / sample_arcade / cube-land / jumpy-platformer
 * 四张卡带在 512KB 下全部编译通过，说明真实深度不到 512KB。这里取 768KB：
 * 比实测值多 50% 余量，加上 C 侧几十 KB 帧也稳稳落在 1MB 宿主栈之内。
 */
#ifndef COMPILER_STACK_KB
#define COMPILER_STACK_KB 768
#endif

/* 编译总预算。桌面 5~6 秒，掌机按 10 倍算也够。 */
#ifndef COMPILER_TIMEOUT_S
#define COMPILER_TIMEOUT_S 300
#endif

typedef struct {
    char *out;
    size_t out_len;
    int done;
    char err[512];
    void (*log)(const char *msg, void *ud);
    void *log_ud;
} compile_ctx;

static compile_ctx *g_cc;

static void cc_log(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (g_cc && g_cc->log) g_cc->log(buf, g_cc->log_ud);
}

/*
 * 注意：这里只允许**一次** vsnprintf。同一个函数里放第二次变参调用，
 * x86_64-windows-gnu 下生成的代码会把栈写坏（实测在第二次调用里 SEGV，
 * 且与内容无关），所以干脆直接格式化到 cc->err 里。
 */
static void cc_fail(const char *fmt, ...)
{
    if (!g_cc) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_cc->err, sizeof(g_cc->err), fmt, ap);
    va_end(ap);
}

static double now_ms(void)
{
    return (double)clock() * 1000.0 / (double)CLOCKS_PER_SEC;
}

/* ---------------- 注入给 JS 的宿主函数 ---------------- */

/* 载荷以十六进制字符串存放（见 scripts/gen_compiler_bundle.py 里为什么不用 \xNN） */
static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void hex_decode(const char *hex, size_t n, uint8_t *out)
{
    for (size_t i = 0; i < n; i++) {
        int hi = hexval((unsigned char)hex[i * 2]);
        int lo = hexval((unsigned char)hex[i * 2 + 1]);
        out[i] = (uint8_t)(((hi < 0 ? 0 : hi) << 4) | (lo < 0 ? 0 : lo));
    }
}

/* __asset(name) -> string | null */
static JSValue js_asset(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) return JS_NULL;
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_NULL;

    const pxt_asset *hit = NULL;
    for (unsigned i = 0; i < pxt_compiler_asset_count; i++) {
        if (!strcmp(pxt_compiler_assets[i].name, name)) { hit = &pxt_compiler_assets[i]; break; }
    }
    JS_FreeCString(ctx, name);
    if (!hit) return JS_NULL;

    size_t blen = hit->hex_len / 2;
    uint8_t *bin = (uint8_t *)malloc(blen);
    if (!bin)
        return JS_ThrowInternalError(ctx, "asset %s: 内存不足（%u 字节）",
                                     hit->name, (unsigned)blen);
    hex_decode(hit->hex, blen, bin);

    uint8_t *raw = NULL;
    size_t raw_len = 0;
    char err[128] = { 0 };
    int rc = arc_lzma_alone(bin, blen, &raw, &raw_len, err, sizeof(err));
    free(bin);   /* 压缩数据只是中间态，解压完立刻还回去 */
    if (rc != 0)
        return JS_ThrowInternalError(ctx, "asset %s: %s", hit->name, err);

    JSValue s = JS_NewStringLen(ctx, (const char *)raw, raw_len);
    free(raw);   /* QuickJS 已经复制过一份，C 缓冲立刻还给系统 */
    return s;
}

/* __has(name) -> bool —— 只查表，不解压（pxt 会大量 stat 探路） */
static JSValue js_has(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) return JS_FALSE;
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return JS_FALSE;
    int hit = 0;
    for (unsigned i = 0; i < pxt_compiler_asset_count; i++) {
        if (!strcmp(pxt_compiler_assets[i].name, name)) { hit = 1; break; }
    }
    JS_FreeCString(ctx, name);
    return JS_NewBool(ctx, hit);
}

/* __list(prefix) -> [name...] —— 资产名清单（JS 侧据此拼虚拟目录树） */
static JSValue js_list(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    JSValue arr = JS_NewArray(ctx);
    const char *prefix = NULL;
    size_t plen = 0;
    if (argc > 0) prefix = JS_ToCStringLen(ctx, &plen, argv[0]);
    uint32_t n = 0;
    for (unsigned i = 0; i < pxt_compiler_asset_count; i++) {
        const char *name = pxt_compiler_assets[i].name;
        if (!prefix || plen == 0 || !strncmp(name, prefix, plen))
            JS_SetPropertyUint32(ctx, arr, n++, JS_NewString(ctx, name));
    }
    if (prefix) JS_FreeCString(ctx, prefix);
    return arr;
}

/* __emit(src) —— 编译成功 */
static JSValue js_emit(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    if (!g_cc || argc < 1) return JS_UNDEFINED;
    size_t n = 0;
    const char *s = JS_ToCStringLen(ctx, &n, argv[0]);
    if (!s) return JS_UNDEFINED;
    free(g_cc->out);
    g_cc->out = (char *)malloc(n + 1);
    if (g_cc->out) {
        memcpy(g_cc->out, s, n);
        g_cc->out[n] = '\0';
        g_cc->out_len = n;
        g_cc->done = 1;
    } else {
        cc_fail("输出 %u 字节时内存不足", (unsigned)n);
    }
    JS_FreeCString(ctx, s);
    return JS_UNDEFINED;
}

/* __fail(msg) —— 编译失败 */
static JSValue js_fail(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) { cc_fail("编译失败"); return JS_UNDEFINED; }
    const char *s = JS_ToCString(ctx, argv[0]);
    if (s) { cc_fail("%s", s); cc_log("__fail: %s", s); JS_FreeCString(ctx, s); }
    return JS_UNDEFINED;
}

/* __log(msg) —— 进度与诊断 */
static JSValue js_log(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    (void)this_val;
    if (argc < 1) return JS_UNDEFINED;
    const char *s = JS_ToCString(ctx, argv[0]);
    if (s) { cc_log("%s", s); JS_FreeCString(ctx, s); }
    return JS_UNDEFINED;
}

/* ---------------- 主流程 ---------------- */

static void free_runtime(JSRuntime *rt, JSContext *ctx)
{
    if (ctx) JS_FreeContext(ctx);
    if (rt) JS_FreeRuntime(rt);
}

int compiler_is_embedded(void) { return 1; }

const char *compiler_build_id(void) { return pxt_compiler_id; }

int compiler_build(const char *proj_json, size_t proj_len,
                   char **out_js, size_t *out_len,
                   void (*log)(const char *msg, void *ud), void *log_ud,
                   char *err, size_t errcap)
{
    if (out_js) *out_js = NULL;
    if (out_len) *out_len = 0;

    compile_ctx cc;
    memset(&cc, 0, sizeof(cc));
    cc.log = log;
    cc.log_ud = log_ud;
    g_cc = &cc;

    double t0 = now_ms();
    JSRuntime *rt = JS_NewRuntime();
    if (!rt) { cc_fail("无法创建 JS 运行时"); goto fail; }

    /* 编译器峰值约 210MB（桌面实测），留出余量；超限时 QuickJS 抛 OOM 而不是崩溃 */
#if COMPILER_MEM_MB > 0
    JS_SetMemoryLimit(rt, (size_t)COMPILER_MEM_MB << 20);
#endif
    JS_SetMaxStackSize(rt, (size_t)COMPILER_STACK_KB << 10);

    JSContext *ctx = JS_NewContext(rt);
    if (!ctx) { free_runtime(rt, NULL); cc_fail("无法创建 JS 上下文"); goto fail; }

    {
        JSValue g = JS_GetGlobalObject(ctx);
        JS_SetPropertyStr(ctx, g, "__asset", JS_NewCFunction(ctx, js_asset, "__asset", 1));
        JS_SetPropertyStr(ctx, g, "__has",   JS_NewCFunction(ctx, js_has,   "__has", 1));
        JS_SetPropertyStr(ctx, g, "__list",  JS_NewCFunction(ctx, js_list,  "__list", 1));
        JS_SetPropertyStr(ctx, g, "__emit",  JS_NewCFunction(ctx, js_emit,  "__emit", 1));
        JS_SetPropertyStr(ctx, g, "__fail",  JS_NewCFunction(ctx, js_fail,  "__fail", 1));
        JS_SetPropertyStr(ctx, g, "__log",   JS_NewCFunction(ctx, js_log,   "__log", 1));
        JS_FreeValue(ctx, g);
    }

    /* 编译器胶水（VFS + CommonJS 装载器 + pxt 流程） */
    JSValue r = JS_Eval(ctx, (const char *)pxt_compiler_js, pxt_compiler_js_len,
                        "pxt_compiler.js", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        JSValue exc = JS_GetException(ctx);
        const char *m = JS_ToCString(ctx, exc);
        cc_fail("编译器胶水加载失败: %s", m ? m : "?");
        if (m) JS_FreeCString(ctx, m);
        JS_FreeValue(ctx, exc);
        JS_FreeValue(ctx, r);
        free_runtime(rt, ctx);
        goto fail;
    }
    JS_FreeValue(ctx, r);
    cc_log("编译器就绪");

    {
        JSValue g = JS_GetGlobalObject(ctx);
        JSValue fn = JS_GetPropertyStr(ctx, g, "__compile");
        JS_FreeValue(ctx, g);
        if (!JS_IsFunction(ctx, fn)) {
            JS_FreeValue(ctx, fn);
            cc_fail("编译器胶水没有导出 __compile");
            free_runtime(rt, ctx);
            goto fail;
        }
        JSValue arg = JS_NewStringLen(ctx, proj_json, proj_len);
        JSValue ret = JS_Call(ctx, fn, JS_UNDEFINED, 1, &arg);
        JS_FreeValue(ctx, arg);
        JS_FreeValue(ctx, fn);
        if (JS_IsException(ret)) {
            JSValue exc = JS_GetException(ctx);
            const char *m = JS_ToCString(ctx, exc);
            cc_fail("编译启动失败: %s", m ? m : "?");
            if (m) JS_FreeCString(ctx, m);
            JS_FreeValue(ctx, exc);
            JS_FreeValue(ctx, ret);
            free_runtime(rt, ctx);
            goto fail;
        }
        JS_FreeValue(ctx, ret);
    }

    /*
     * 编译全程是 promise 链，必须把 microtask 抽干。
     * 注意：只有 JS_ExecutePendingJob 返回 0（队列空）才能退出——
     * 带着未执行的 job 去 JS_FreeRuntime 会直接 abort。
     */
    {
        JSContext *c1 = NULL;
        JSValue g = JS_GetGlobalObject(ctx);
        JSValue fn_drain = JS_GetPropertyStr(ctx, g, "__drainTimers");
        JS_FreeValue(ctx, g);

        int jobs = 0;
        int failed = 0;
        for (;;) {
            int st = JS_ExecutePendingJob(rt, &c1);
            if (st < 0) {
                if (!cc.err[0]) {
                    JSValue exc = JS_GetException(c1 ? c1 : ctx);
                    const char *m = JS_ToCString(ctx, exc);
                    cc_fail("编译任务异常: %s", m ? m : "?");
                    if (m) JS_FreeCString(ctx, m);
                    JS_FreeValue(ctx, exc);
                }
                failed = 1;
            } else if (st > 0) {
                jobs++;
            } else {
                /* 队列空了：还有排队的 setTimeout 就跑一轮，否则收工 */
                if (cc.done || failed) break;
                if (JS_IsFunction(ctx, fn_drain)) {
                    JSValue r2 = JS_Call(ctx, fn_drain, JS_UNDEFINED, 0, NULL);
                    int more = JS_ToBool(ctx, r2);
                    JS_FreeValue(ctx, r2);
                    if (more) continue;
                }
                break;
            }
            if (cc.done || failed) continue;   /* 继续抽干，但不再管超时 */
            if (now_ms() - t0 > (double)COMPILER_TIMEOUT_S * 1000.0) {
                cc_fail("编译超时（%d 秒）", COMPILER_TIMEOUT_S);
                failed = 1;
            }
        }
        JS_FreeValue(ctx, fn_drain);
        (void)jobs;
    }

    {
        JSContext *c2 = NULL;
        while (JS_ExecutePendingJob(rt, &c2) > 0) { }   /* 再抽干一次，避免 free 时还有 job */
    }
    free_runtime(rt, ctx);

    if (!cc.out || !cc.done) {
        if (!cc.err[0]) cc_fail("编译器没有产出 binary.js");
        goto fail;
    }

    /* 调试用：ARCADE_DUMP_JS=<路径> 把产物落盘，方便和 PC 端 node tools/cart_build.js 对比 */
    {
        const char *dump = getenv("ARCADE_DUMP_JS");
        if (dump && dump[0]) {
            FILE *f = fopen(dump, "wb");
            if (f) {
                fwrite(cc.out, 1, cc.out_len, f);
                fclose(f);
                cc_log("产物已写到 %s", dump);
            }
        }
    }

    if (out_js) { *out_js = cc.out; cc.out = NULL; }
    if (out_len) *out_len = cc.out_len;
    free(cc.out);
    g_cc = NULL;
    return 0;

fail:
    free(cc.out);
    g_cc = NULL;
    if (err && errcap) snprintf(err, errcap, "%s", cc.err[0] ? cc.err : "未知错误");
    return -1;
}

#endif /* ARCADE_NO_INCART_COMPILER */
