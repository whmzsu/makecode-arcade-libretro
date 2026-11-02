#include "pxt.h"
#include "mca_state.h"
#include "mca_tick.h"

// MCA：随机状态存取（base/core.cpp），快照要带上它
namespace pxt {
unsigned mcaGetRandomState();
void mcaSetRandomState(unsigned v);
}

#include <stdlib.h>
#include <stdio.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <errno.h>
#include <stdarg.h>

#ifndef PXT_ESP32
// __MINGW32__ is defined on both mingw32 and mingw64
#ifdef __MINGW32__
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#endif

#define HANDLER_RUNNING 0x0001

// should this be something like CXX11 or whatever?
#define THROW throw()
#define THREAD_DBG(...)

static uint8_t in_xmalloc_panic;

void *xmalloc(size_t sz) {
    // MCA v0.2.10：calloc 语义（清零）。
    // malloc 的旧内容会让"读到未初始化字段"的结果随进程堆历史变化 ——
    // 同一输入的两次运行就不一样，存档 / rewind 无从确定。
    // 分配量都在 fiber / 事件 / 栈副本这个量级，清零开销可忽略。
    auto r = calloc(1, sz);
    if (r == NULL) {
        DMESG("failed to allocate %d bytes", sz);
        if (in_xmalloc_panic) {
            target_panic(PANIC_GC_OOM);
        } else {
            in_xmalloc_panic = 1;
            soft_panic(PANIC_GC_OOM); // this can happen on esp32 etc; shouldn't on linux/ios etc
        }
    }
    return r;
}

void *operator new(size_t size) {
    return xmalloc(size);
}
void *operator new[](size_t size) {
    return xmalloc(size);
}

void operator delete(void *p)THROW {
    xfree(p);
}
void operator delete[](void *p) THROW {
    xfree(p);
}

uint8_t *gcBase;

namespace pxt {

// MCA PATCH 5（本项目）：宿主请求重启（libretro 的 reset 键）。
//
// 官方宿主是"单游戏单进程"，VM 没有 stop 接口；但 systemReset() 本身就是
// 一套完整的重启收尾（清 fiber / handler 绑定 / GC，然后 pthread_exit）。
// 它只能在 VM 线程里跑，所以宿主线程只置标志，由 mainRunLoop 自行退出去
// 交给 initRuntime 末尾的 systemReset()。
//   mcaResetRequest: 宿主线程写、VM 线程读（单字节标志，volatile 足够）
//   mcaVmExited    : VM 线程**真正退出前**（multiStart 末尾）写，宿主读后清零。
//                    不能在清理中途就置位 —— 那时线程还在跑，宿主若立刻
//                    pthread_join 只能等，真机上表现为"重置后整个前端卡死"。
//   mcaRestartDone : 清理已做完，initRuntime 跳过 systemReset()
volatile int mcaResetRequest = 0;
volatile int mcaVmExited = 0;
// MCA v0.2.10：主循环是否已经在"等前端要帧"的闸门上。
// 宿主用它判断"VM 是否已完成 initRuntime 并且随时能出一帧" ——
// 装载同步化（vmload.cpp 的 spinThread）等到它才返回，这样前端第 1 帧
// 就能立刻拿到第 1 圈的画面，帧号与圈号从第一帧起严格对齐。
volatile int mcaMainLoopLive = 0;
static volatile int mcaRestartDone = 0;
// MCA：快照暂停协议（pxt_state_pause/resume）。宿主线程在 retro_serialize /
// unserialize 期间要求 VM 线程停到本安全点（mainRunLoop 顶部）：
//   - 所有 fiber 的解释器状态要么在自己的 stackCopy，要么在 vmImg->stackBase
//     共享操作数栈里；VM 线程不在 exec_loop 中间 → 不需要抓 C 栈
//   - 暂停期间虚拟时钟不走（帧驱动：不取帧就不前进），恢复后 wakeTime
//     语义不变，也不存在"暂停期间游戏时间溜走"
// 最坏等待：exec_loop 一个 tick（毫秒级）/ update 的 50ms 帧等待 /
// target_yield 的 1ms —— 宿主侧 2 秒超时足够。
static volatile int mcaPauseRequest = 0;
static volatile int mcaPaused = 0;

// MCA PATCH 6（本项目）：重启路径埋点。
//
// 这套重启是"VM 线程自己清理干净再退出、宿主再起新线程"，涉及两次线程切换，
// 真机上卡住时前端只会看到一张静止的画面，日志也没有 —— 所以每一步都写一行
// trace（默认落到存档目录的 mca_trace.log，失败退回 stderr），卡在哪一步一眼可见。
// 只写十几行、每次 flush，对性能没有影响。
static FILE *mcaTraceFile;
static int mcaTraceTried;
static char mcaTracePathBuf[512] = "";

// 依次尝试几个位置：存档目录 → 当前目录 → /tmp（掌机上一定可写）。
// 同时**每个进程启动都截断重建**，避免把上次的旧内容当成这次的结果。
static void mcaTraceOpen() {
    if (mcaTraceTried) return;
    mcaTraceTried = 1;

    const char *dir = pxt_vm_get_data_directory();
    if (dir && *dir) {
        snprintf(mcaTracePathBuf, sizeof(mcaTracePathBuf), "%s/mca_trace.log", dir);
        mcaTraceFile = fopen(mcaTracePathBuf, "w");
    }
    if (!mcaTraceFile) {
        snprintf(mcaTracePathBuf, sizeof(mcaTracePathBuf), "mca_trace.log");
        mcaTraceFile = fopen(mcaTracePathBuf, "w");
    }
#if !defined(_WIN32)
    if (!mcaTraceFile) {
        snprintf(mcaTracePathBuf, sizeof(mcaTracePathBuf), "/tmp/mca_trace.log");
        mcaTraceFile = fopen(mcaTracePathBuf, "w");
    }
#endif
    if (!mcaTraceFile) {
        snprintf(mcaTracePathBuf, sizeof(mcaTracePathBuf), "(no file, stderr only)");
        mcaTraceFile = stderr;
        return;
    }
    fprintf(mcaTraceFile, "# mca trace -> %s\n", mcaTracePathBuf);
    fflush(mcaTraceFile);
    dmesg("mca trace: %s", mcaTracePathBuf);
}

// trace 只写文件（互斥），**不写 stderr**。
//
// 早先版本同时写 stderr 想让日志进 RetroArch 自己的输出。问题是高频 trace
// （例如排查时每圈主循环一行）会把 stderr 管道/终端写满：管道没人读、或
// Windows 控制台逐字符输出，都会让**调用线程阻塞在 write 上** —— VM 线程
// 卡在 trace 里，表现出来像是"暂停请求永远读不到"。日志路径已经在启动时
// 用 dmesg() 报过一次，用户照着找文件即可。
static pthread_mutex_t mcaTraceMutex = PTHREAD_MUTEX_INITIALIZER;

void mcaTrace(const char *fmt, ...) {
    mcaTraceOpen();
    va_list ap;
    pthread_mutex_lock(&mcaTraceMutex);
    if (mcaTraceFile) {
        va_start(ap, fmt);
        vfprintf(mcaTraceFile, fmt, ap);
        fputc('\n', mcaTraceFile);
        fflush(mcaTraceFile);
        va_end(ap);
    } else {
        va_start(ap, fmt);
        vfprintf(stderr, fmt, ap);
        fputc('\n', stderr);
        fflush(stderr);
        va_end(ap);
    }
    pthread_mutex_unlock(&mcaTraceMutex);
}

DLLEXPORT void pxt_vm_trace(const char *msg) { mcaTrace("[host] %s", msg); }

DLLEXPORT const char *pxt_vm_trace_path() {
    mcaTraceOpen();
    return mcaTracePathBuf;
}

// MCA PATCH 7：「VM 在线」标志。
//
// 前端线程每帧都会来拉画面/音频。重启间隙里 VM 的堆、单例、混音器都被释放了，
// 此时拉取必须变成"黑屏 + 静音"的纯本地操作，绝不能碰 VM 状态。
// 这个标志由 VM 线程自己维护：清理第一步置 0，新镜像装载成功后置 1。
static volatile int pxtVmLive = 1;
DLLEXPORT int pxt_vm_online() { return pxtVmLive; }
DLLEXPORT void pxt_vm_set_online(int online) { pxtVmLive = online ? 1 : 0; }

FiberContext *allFibers;
FiberContext *currentFiber;
static pthread_mutex_t eventMutex;
static pthread_cond_t newEventBroadcast;

static struct Event *eventHead, *eventTail;

struct Event {
    struct Event *next;
    int source;
    int value;
};

Event lastEvent;

Event *mkEvent(int source, int value) {
    auto res = new Event();
    memset(res, 0, sizeof(Event));
    res->source = source;
    res->value = value;
    return res;
}

volatile int panicCode;
extern "C" void drawPanic(int code);

void schedule() {
    auto f = currentFiber;
    if (!f->wakeTime && !f->waitSource)
        oops(55);
    f->resumePC = f->pc;
    f->pc = NULL; // this will break the exec_loop()
}

void dmesg_flush();

static void panic_core(int error_code) {
    int prevErr = errno;

    panicCode = error_code;

    drawPanic(error_code);

    DMESG("PANIC %d", error_code % 1000);
    DMESG("errno=%d %s", prevErr, strerror(prevErr));

    dmesg_flush();
}

extern "C" void target_panic(int error_code) {
    panic_core(error_code);

#if defined(PXT_ESP32)
    // sleep_core_us(5 * 1000 * 1000);
    abort();
#elif defined(PXT_VM)
    systemReset();
#else
    while (1)
        sleep_core_us(10000);
#endif
}

DLLEXPORT int pxt_get_panic_code() {
    return panicCode;
}

void ets_log_dmesg();
void soft_panic(int errorCode) {
    if (errorCode >= 999)
        errorCode = 999;
    if (errorCode <= 0)
        errorCode = 1;
    vm_stack_trace();
    panic_core(1000 + errorCode);
#if defined(PXT_ESP32)
    ets_log_dmesg();
    sleep_core_us(4000000);
    abort();
#else
    systemReset();
#endif
}

void sleep_core_us(uint64_t us) {
#ifdef PXT_ESP32
    uint64_t endp = esp_timer_get_time() + us;
    while (esp_timer_get_time() < endp)
        ;
#else
    struct timespec ts;
    ts.tv_sec = us / 1000000;
    ts.tv_nsec = (us % 1000000) * 1000;
    while (nanosleep(&ts, &ts))
        ;
#endif
}

// MCA v0.2.10：target_yield 等「下一帧被前端取走」的信号，而不是睡固定
// 时长（Windows 上 nanosleep 粒度实际 6-15ms，墙钟外推时钟下会把游戏压到
// 6 倍慢）。帧驱动时钟下虚拟时间只在前端取帧时前进，这里只要在两件事之
// 一时醒来：帧到了 / 兜底超时到了。具体等待由 mca_tick.cpp 按平台实现
// （Windows 用 WinAPI 事件 —— winpthreads 的 cond 在竞争下会丢唤醒且超时
// 不生效，实测把 VM 线程永久挂在 target_yield）。
void target_yield() {
#ifdef PXT_ESP32
    vTaskDelay(1);
#else
    mca_tick_wait(2);
#endif
}

void sleep_ms(uint32_t ms) {
    currentFiber->wakeTime = current_time_ms() + ms;
    schedule();
}

void sleep_us(uint64_t us) {
    if (us > 20000) {
        sleep_ms((uint32_t)(us / 1000));
    } else {
        sleep_core_us(us);
    }
}

#ifndef PXT_ESP32
// MCA PATCH 10：虚拟时钟（快进 / 慢放 / 存档确定性的基础）。
//
// 游戏的节拍全部挂在 current_time_ms() 上：forever 每 20ms 一个 tick、
// pause(ms) 等 wakeTime —— 都是"现在几点"。墙钟不能用作游戏时间：
// 前端以高于 60fps 的频率调 retro_run 时，墙钟不变，游戏不会加速；
// 而且同一帧号对应的墙钟时刻每次都不同，存档 / rewind 无从确定。
//
// v0.2.10 的模型是**前端请求驱动、一圈一帧**：
//   · 前端每请求一帧 → g_frameRequests++（并发 mca_tick_signal 唤醒 VM）
//   · VM 主循环每圈先等一张许可，然后 g_virtTimeNum += 一帧、发布一帧
// 于是 VM 的圈数严格等于前端帧数，同一帧号恒定对应同一虚拟时刻，
// 同一输入序列 + 同一快照必然产出逐字节相同的画面。
//
// 快进 / 慢放不再需要倍率：前端多取帧游戏就快（虚拟时间按帧推进），
// 前端少取帧游戏就慢，这是 libretro 的固有语义（RetroArch 的 fast-forward
// 就是提高 retro_run 频率）。pxt_set_time_scale 保留为兼容接口，不再缩放
// 时间步长 —— 缩放会让一层循环里跳过多个 20ms 周期，forever 丢 tick，
// 正是确定性崩掉的来源。
static volatile uint64_t g_frameRequests = 0; // 前端线程写：请求了几帧
static uint64_t g_loops = 0;                  // VM 线程写：已推进了几圈
static uint64_t g_virtTimeNum = 0;            // VM 线程本地推进，单位 1/3 µs
// 已发布的帧序号（= 发出去的第几帧）。前端按序号配对，不靠 bool 标志。
// screen.cpp 的发布函数在同一个锁内写它，保证"序号可见 ⇒ 缓冲已就绪"。
volatile uint64_t mcaPubSeq = 0;

DLLEXPORT void pxt_set_time_scale(int percent) {
    (void)percent; // 见上面的说明：快进由前端取帧频率决定
}

// 前端每请求一帧调一次（screen.cpp 的 get_pixels 开头），返回本次请求的序号。
// 前端必须等到 pxt_vm_pub_seq() >= 该序号才取画面 —— 这样"请求 N 拿到第 N 帧"，
// 不会因为 bool 标志被后续帧覆盖而错配（实测会积压 10+ 帧，末帧取决于
// unload 时 VM 恰好跑到哪里，同一输入两次运行结果不同）。
DLLEXPORT uint64_t pxt_vm_frame_request() {
    g_frameRequests++;
    mca_tick_signal();
    return g_frameRequests;
}

DLLEXPORT uint64_t pxt_vm_pub_seq() {
    return mcaPubSeq;
}

// VM 线程：还有没有未处理的帧请求
static int mca_frame_requested() {
    return g_frameRequests > g_loops;
}

// 一帧的虚拟时间：1/60 秒 = 16666.7 µs，单位是 1/3 µs → 50000
#define MCA_FRAME_STEP 50000u

uint64_t current_time_us() {
    return g_virtTimeNum / 3;
}
#endif

int current_time_ms() {
    return (int)(current_time_us() / 1000);
}

void disposeFiber(FiberContext *t) {
    if (allFibers == t) {
        allFibers = t->next;
    } else {
        for (auto tt = allFibers; tt; tt = tt->next) {
            if (tt->next == t) {
                tt->next = t->next;
                break;
            }
        }
    }

    // DMESG("free: %p %p", t, t->stackCopy);

    xfree(t->stackCopy);
    xfree(t);

    if (currentFiber == t)
        currentFiber = NULL;
}

#define INITIAL_STACK_COPY_SIZE 16

FiberContext *setupThread(Action a, TValue arg = 0, HandlerBinding *hb = NULL) {
#if 0
    int numThreads = 0;
    for (auto p = allFibers; p; p = p->next)
        numThreads++;
    DMESG("setup thread: %p #%d", a, numThreads);
    //if (numThreads > 10)
    //    abort();
#endif
    auto t = (FiberContext *)xmalloc(sizeof(FiberContext));
    memset(t, 0, sizeof(*t));
    if (!vmImg->stackBase) {
        vmImg->stackBase = (TValue *)xmalloc(VM_STACK_SIZE * sizeof(TValue));
        vmImg->stackTop = vmImg->stackBase + VM_STACK_SIZE;
        vmImg->stackLimit = vmImg->stackBase + VM_MAX_FUNCTION_STACK + 5;
    }
    t->stackCopy = (TValue *)xmalloc(sizeof(TValue) * INITIAL_STACK_COPY_SIZE);
    t->stackCopySize = INITIAL_STACK_COPY_SIZE;
    t->sp = vmImg->stackTop - 8;

    // DMESG("thr: %p %p", t, t->stackCopy);

    auto ptr = t->stackCopy;
    *ptr++ = TAG_STACK_BOTTOM;
    *ptr++ = 0;
    *ptr++ = arg;
    *ptr++ = 0;
    *ptr++ = 0;
    *ptr++ = 0;
    *ptr++ = 0;
    *ptr++ = (TValue)0xf00df00df00df00d;

    t->handlerBinding = hb;
    auto ra = (RefAction *)a;
    // we only pass 1 argument, but can in fact handle up to 4
    if (ra->numArgs > 2)
        target_panic(PANIC_INVALID_IMAGE);
    t->currAction = ra;
    t->resumePC = actionPC(ra);

    t->img = vmImg;
    t->imgbase = (uint16_t *)vmImg->dataStart;

    // add at the end
    if (allFibers)
        for (auto p = allFibers; p; p = p->next) {
            if (!p->next) {
                p->next = t;
                break;
            }
        }
    else
        allFibers = t;

    return t;
}

void runInParallel(Action a) {
    setupThread(a);
}

void runForever(Action a) {
    auto f = setupThread(a);
    f->foreverPC = f->resumePC;
}

void waitForEvent(int source, int value) {
    currentFiber->waitSource = source;
    currentFiber->waitValue = value;
    schedule();
}

FiberContext *suspendFiber() {
    currentFiber->waitSource = PXT_WAIT_SOURCE_PROMISE;
    schedule();
    return currentFiber;
}

void resumeFiberWithFn(FiberContext *ctx, fiber_resume_t fn, void *arg) {
    if (ctx->waitSource != PXT_WAIT_SOURCE_PROMISE)
        oops(52);
    ctx->waitSource = 0;
    ctx->wakeFn = fn;
    ctx->wakeFnArg = arg;
}

void resumeFiber(FiberContext *ctx, TValue v) {
    if (ctx->waitSource != PXT_WAIT_SOURCE_PROMISE)
        oops(52);
    ctx->waitSource = 0;
    ctx->r0 = v;
}

static void startHandler(HandlerBinding *hb, Event &e) {
    if (!hb)
        return;
    lastEvent = e; // this is quite racy
    if (hb->flags & HANDLER_RUNNING) {
        auto tmp = mkEvent(e.source, e.value);
        if (hb->pending == NULL) {
            hb->pending = tmp;
        } else {
            int numev = 0;
            auto p = hb->pending;
            for (; p->next; p = p->next)
                numev++;
            if (numev >= 10) {
                xfree(tmp);
                return;
            }
            p->next = tmp;
        }
    } else {
        hb->flags |= HANDLER_RUNNING;
        setupThread(hb->action, fromInt(e.value), hb);
    }
}

static void dispatchEvent(Event &e) {
    startHandler(findBinding(e.source, e.value), e);
    startHandler(findBinding(e.source, DEVICE_EVT_ANY), e);
}

static void wakeFibers() {
    for (;;) {
        pthread_mutex_lock(&eventMutex);
        if (eventHead == NULL) {
            pthread_mutex_unlock(&eventMutex);
            return;
        }
        Event *ev = eventHead;
        eventHead = ev->next;
        if (eventHead == NULL)
            eventTail = NULL;
        pthread_mutex_unlock(&eventMutex);

        for (auto thr = allFibers; thr; thr = thr->next) {
            if (thr->waitSource == 0)
                continue;
            if (thr->waitValue != ev->value && thr->waitValue != DEVICE_EVT_ANY)
                continue;
            if (thr->waitSource == ev->source) {
                thr->waitSource = 0;
            } else if (thr->waitSource == DEVICE_ID_NOTIFY && ev->source == DEVICE_ID_NOTIFY_ONE) {
                thr->waitSource = 0;
                break; // do not wake up any other threads
            }
        }

        dispatchEvent(*ev);
        delete ev;
    }
}

static void saveStack() {
    auto f = currentFiber;
    if (!f)
        return;
    int sizeNeeded = vmImg->stackTop - f->sp;
    // DMESG("save %d %p", sizeNeeded, f);
    if (!f->stackCopy || sizeNeeded > f->stackCopySize) {
        xfree(f->stackCopy);
        f->stackCopySize = sizeNeeded + 10;
        f->stackCopy = (TValue *)xmalloc(f->stackCopySize * sizeof(TValue));
        // DMESG(" -> %p", f->stackCopy);
    }
    memcpy(f->stackCopy, f->sp, sizeNeeded * sizeof(TValue));
}

static void restoreStack() {
    auto f = currentFiber;
    memcpy(f->sp, f->stackCopy, (uint8_t *)vmImg->stackTop - (uint8_t *)f->sp);
}

static void hostRestartCleanup();

static void mainRunLoop() {
    FiberContext *f = NULL;
    mcaMainLoopLive = 1;
    for (;;) {
        // MCA PATCH 5：宿主请求重启 → 退出主循环，交给 initRuntime 末尾的
        // systemReset()（释放 fiber/GC 后 pthread_exit），宿主随后重新 start。
        if (mcaResetRequest) {
            mcaResetRequest = 0;
            mcaTrace("[vm] reset request seen");
            // 时钟对账：loops 应等于前端已请求帧数，virtMs = loops/60 秒。
            // 对不上就是帧闸门/序号配对出了问题（确定性回归的第一线索）。
            mcaTrace("[vm] clock: loops=%llu requests=%llu virtMs=%llu",
                     (unsigned long long)g_loops, (unsigned long long)g_frameRequests,
                     (unsigned long long)(g_virtTimeNum / 3000));
            hostRestartCleanup();
            mcaTrace("[vm] cleanup returned, leaving main loop");
            mcaMainLoopLive = 0;
            return;   // initRuntime 看到 mcaRestartDone 就不再 systemReset()
        }
        // MCA：快照暂停。宿主拿到 mcaPaused 后搬状态（save/load 都可能发生），
        // 恢复时 fiber 链整个被替换 —— 本地 f 必须作废，否则下一次 while(f)
        // 会踩已释放的旧 fiber
        if (mcaPauseRequest) {
            mcaPaused = 1;
            // 暂停期间低占用地等宿主搬完状态（sleep_core_us 的亚毫秒在
            // Windows 上退化成 Sleep(0)，会把一个核空转满）
            while (mcaPauseRequest)
                mca_tick_wait(5);
            mcaPaused = 0;
            f = NULL;
            continue;
        }
        // 一圈一帧：等前端请求下一帧再推进。
        // 没有这个同步，VM 的圈数会随调度在「前端帧数」附近漂移，虚拟时间
        // 跟着漂，同一输入的两次运行画面就分岔（存档确定性的前提）。
        if (!mca_frame_requested()) {
            mca_tick_wait(5);
            continue;
        }
        g_loops++;
        g_virtTimeNum += MCA_FRAME_STEP;
        // 发布一帧给前端（内容 = 上一圈逻辑执行完后的画面缓冲）。
        // 放在圈顶而不是圈尾：圈内有多处 continue，放圈尾会漏帧，帧数就对不上了。
        pxt_screen_publish(g_loops); // 拷缓冲 + 置序号 + 广播（同一把锁）
        if (panicCode)
            return;
        wakeFibers();
        auto now = current_time_ms();
        auto fromBeg = false;
        if (!f) {
            f = allFibers;
            fromBeg = true;
        }
        while (f) {
            if (f->wakeTime && now >= (int)f->wakeTime)
                f->wakeTime = 0;
            if (!f->wakeTime && !f->waitSource)
                break;
            f = f->next;
        }
        if (f) {
            if (currentFiber != f) {
                saveStack();
                currentFiber = f;
                restoreStack();
            }
            f->pc = f->resumePC;
            f->resumePC = NULL;
            if (f->wakeFn) {
                auto fn = f->wakeFn;
                f->wakeFn = NULL;
                f->r0 = fn(f->wakeFnArg);
                if (f->wakeTime || f->waitSource)
                    continue; // we got suspended again
            }
            exec_loop(f);
            if (panicCode)
                return;
            auto n = f->next;
            if (f->resumePC == NULL) {
                if (f->foreverPC) {
                    f->resumePC = f->foreverPC;
                    f->wakeTime = current_time_ms() + 20;
                    // restore stack, as setupThread() does it
                    for (int i = 0; i < 5; ++i) {
                        if (*--f->sp == TAG_STACK_BOTTOM)
                            break;
                    }
                    if (*f->sp != TAG_STACK_BOTTOM)
                        target_panic(PANIC_INVALID_IMAGE);
                } else {
                    auto hb = f->handlerBinding;
                    if (hb) {
                        auto pev = hb->pending;
                        if (pev) {
                            hb->pending = pev->next;
                            setupThread(hb->action, fromInt(pev->value), hb);
                            xfree(pev);
                        } else {
                            f->handlerBinding->flags &= ~HANDLER_RUNNING;
                        }
                    }
                    disposeFiber(f);
                }
            }
            f = n;
        } else if (fromBeg) {
            target_yield();
        }
    }
}

// MCA：通知 ID 从函数内 static 提到文件级 —— 快照要把它一起搬走，
// 否则恢复后新分配的通知 ID 可能与恢复回来的 promise 等待值撞号
static volatile int mcaNotifyId;

int allocateNotifyEvent() {
    return ++mcaNotifyId;
}

void raiseEvent(int id, int event) {
    auto e = mkEvent(id, event);
    pthread_mutex_lock(&eventMutex);
    if (eventTail == NULL) {
        if (eventHead != NULL)
            oops(51);
        eventHead = eventTail = e;
    } else {
        eventTail->next = e;
        eventTail = e;
    }
    pthread_cond_broadcast(&newEventBroadcast);
    pthread_mutex_unlock(&eventMutex);
}

DLLEXPORT void pxt_raise_event(int id, int event) {
    raiseEvent(id, event);
}

void registerWithDal(int id, int event, Action a, int flags) {
    // TODO support flags
    setBinding(id, event, a);
}

uint32_t afterProgramPage() {
    return 0;
}

char **initialArgv;

void screen_init();
void initKeys();
void target_startup();

void initRuntime() {
    current_time_ms();
    mca_tick_init(); // 帧节拍原语（幂等；重启路径上重复调用无害）
    mca_pub_init();  // 画面发布事件（前端等帧用，同样幂等）
    target_startup();

    setupThread((TValue)vmImg->entryPoint);

    target_init();
    screen_init();
    initKeys();

    DMESG("start main loop");

    mcaTrace("[vm] main loop entered");
    mainRunLoop();
    if (mcaRestartDone) {
        // 宿主重启：清理已在 hostRestartCleanup() 里完成，
        // 这里正常 return，让 VM 线程干净退出（宿主的 pthread_join 才安全）
        mcaRestartDone = 0;
        mcaTrace("[vm] initRuntime return (restart path)");
        return;
    }
    mcaTrace("[vm] main loop ended without restart -> systemReset");
    systemReset();
}

// MCA PATCH 5：GC 分配游标从函数内 static 提到文件级，好让 systemReset()
// 在重启时把它绕回堆首。64 位下 VM 堆固定 1MB（PXT_VM_HEAP_ALLOC_BITS=20），
// 不回绕的话重开几次游戏就 soft_panic(20)。
static uint8_t *gcAllocPtr = (uint8_t *)GC_BASE;

void *gcAllocBlock(size_t sz) {
#ifdef PXT_ESP32
    void *r = xmalloc(sz);
#else
    uint8_t *&currPtr = gcAllocPtr;
    sz = (sz + GC_PAGE_SIZE - 1) & ~(GC_PAGE_SIZE - 1);
#if defined(PXT64) || defined(__MINGW32__)
    if (!gcBase) {
        gcBase = (uint8_t *)xmalloc(1 << PXT_VM_HEAP_ALLOC_BITS);
        // MCA v0.2.10：堆清零。malloc 的旧内容会让"读到未初始化字段"的
        // 行为随进程堆历史变化 —— 同一输入的两次运行结果就不同，存档/rewind
        // 的确定性无从谈起（实测清掉之前 20 帧就跑岔）。1MB 一次 memset 的开销
        // 只在开局付一次。
        memset(gcBase, 0, (size_t)1 << PXT_VM_HEAP_ALLOC_BITS);
        currPtr = gcBase;
    }
    void *r = currPtr;
    if ((uint8_t *)currPtr - gcBase > (1 << PXT_VM_HEAP_ALLOC_BITS) - (int)sz)
        soft_panic(20);
#else
    void *r = mmap(currPtr, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (r == MAP_FAILED) {
        DMESG("mmap %p failed; err=%d", currPtr, errno);
        target_panic(PANIC_INTERNAL_ERROR);
    }
#endif
    currPtr = (uint8_t *)r + sz;
#endif

    if (isReadOnly((TValue)r)) {
        DMESG("mmap returned read-only address: %p", r);
        target_panic(PANIC_INTERNAL_ERROR);
    }
    return r;
}

void gcProcessStacks(int flags) {
    int cnt = 0;
    for (auto f = allFibers; f; f = f->next) {
        TValue *end, *ptr;
        if (f == currentFiber) {
            end = vmImg->stackTop;
            ptr = f->sp;
        } else {
            end = f->stackCopy + (vmImg->stackTop - f->sp);
            ptr = f->stackCopy;
        }
        gcProcess((TValue)f->currAction);
        gcProcess((TValue)f->r0);
        if (flags & 2)
            DMESG("RS%d:%p/%d", cnt++, ptr, end - ptr);
        // VLOG("mark: %p - %p", ptr, end);
        while (ptr < end) {
            gcProcess(*ptr++);
        }
    }
}

#define MAX_RESET_FN 32
static reset_fn_t resetFunctions[MAX_RESET_FN];

void registerResetFunction(reset_fn_t fn) {
    for (int i = 0; i < MAX_RESET_FN; ++i) {
        if (!resetFunctions[i]) {
            resetFunctions[i] = fn;
            return;
        }
    }

    target_panic(PANIC_INTERNAL_ERROR);
}

// MCA PATCH 5（本项目）：VM 的"回到初始状态"收尾序列。
// 官方 systemReset() 把这段内联在里面，宿主侧重启要复用它但**不能** pthread_exit
//（Windows/mingw 下从 VM 线程 pthread_exit 会在 fiber 栈已错位时崩，
// 实测 0xC0000005；改成让线程正常 return 即可安全 join），所以抽出来共用。
static void resetRuntimeState() {
    // MCA PATCH 7：**第一步**就让前端侧的输出通道失效。
    // 从这里到新一局 gcStartup 之间，前端线程仍然每帧来拉画面/音频；它一旦
    // 碰到已经被释放的 instWDisplay / dac / WSynthesizer 就是 use-after-free，
    // 而 pxt_screen_get_pixels 里的 target_panic 还会在**前端线程**上
    // pthread_exit —— 表现就是"按重置后前端挂起、不崩也不退"。
    mcaTrace("[vm] cleanup 0/7 output offline");
    pxt_vm_set_online(0);   // 前端从这一行起拿黑屏+静音，不再碰 VM 状态
    pxt_screen_shutdown();  // 丢掉 instWDisplay

    mcaTrace("[vm] cleanup 1/6 gcFreeze");
    gcFreeze();

    mcaTrace("[vm] cleanup 2/6 reset callbacks");
    for (int i = 0; i < MAX_RESET_FN; ++i) {
        auto fn = resetFunctions[i];
        if (fn)
            fn();
    }

    // PATCH 9：单例的 static 指针活过 GC 重置，必须丢 —— 否则重启后
    // getWSynthesizer() 返回上一局的野指针（不重新构造 → dac 永远 NULL →
    // 没声音；地址被复用时更会类型错乱写坏内存）。
    //
    // 顺序很重要：**必须放在 reset 回调之后**。回调里 melody 的 stopPlaying()
    // 会调 getWSynthesizer()，若此时单例已被清空，它会当场 new 一个新的到
    // 即将被 gcReset 掉的旧堆里 —— 等于又留下一个野指针（实测就是这么复现的：
    // trace 里 ExtDAC 在 cleanup 0/7 之后又被创建了一次）。
    mcaTrace("[vm] cleanup 2.5/6 drop singletons (music/audio)");
    pxt_audio_shutdown();   // dac = NULL（指向 VM 堆）
    pxt_music_shutdown();   // instWSynthesizer = NULL

    mcaTrace("[vm] cleanup 3/6 coreReset");
    coreReset(); // clears handler bindings

    mcaTrace("[vm] cleanup 4/6 dispose fibers");
    currentFiber = NULL;
    while (allFibers) {
        disposeFiber(allFibers);
    }

    // this will consume all events, but won't dispatch anything, since all listener maps are empty
    mcaTrace("[vm] cleanup 5/6 wakeFibers");
    wakeFibers();

    // mark all GC memory as free
    mcaTrace("[vm] cleanup 6/6 gcReset");
    gcReset();

    // GC 分配游标绕回堆首 —— 与第一次启动时的状态一致（64 位下 VM 堆固定
    // 1MB，不回绕的话重开几次游戏就 soft_panic(20)）
#if defined(PXT64) || defined(__MINGW32__)
    if (gcBase)
        gcAllocPtr = gcBase;
#else
    gcAllocPtr = (uint8_t *)GC_BASE;
#endif
    // 清空 reset 回调槽：registerResetFunction() 在槽满时会 target_panic，
    // 而 systemReset 只调回调不清表，重启时重复注册就会 panic
    for (int i = 0; i < MAX_RESET_FN; ++i)
        resetFunctions[i] = NULL;

    // MCA PATCH 6：块清单也要一起忘掉。切块游标已经绕回堆首，若 firstBlock 里
    // 还留着上一局的块，新块会落在同一地址 → linkFreeBlock() 自环 → sweep() 死循环
    //（真机上就是"重置后画面冻住"）。清空后新一局从堆首重新切块 = 冷启动。
    gcForgetAllBlocks();
    // 兜底：万一还有静态指针在清理途中被重建（比如某个 reset 回调里 new 了
    // 单例），这里再丢一次。此时堆已作废，任何残留指针都是野的。
    pxt_screen_shutdown();
    pxt_audio_shutdown();
    pxt_music_shutdown();
    mcaTrace("[vm] cleanup done (blocks forgotten)");
}

// 宿主说"重开这一局"（libretro 的 reset 键 / Restart）
// 清理过程本身若再触发 panic（比如 GC_CHECK / oops），会经 target_panic →
// systemReset() 递归回到这里。真机上这条路是无限递归 = 画面冻住不动。
// 用一个标志挡住重入：已经在清理就只把线程退掉，不再跑第二遍。
static volatile int mcaResetting;

static void hostRestartCleanup() {
    dmesg("HOST RESET");
    mcaTrace("[vm] HOST RESET begin");
    if (!panicCode)
        panicCode = -1;
    mcaResetting = 1;
    resetRuntimeState();
    mcaResetting = 0;
    mcaTrace("[vm] HOST RESET end");
    // mcaVmExited 由 multiStart() 在线程真正返回前设置（PATCH 6）：
    // 在这里置位会让宿主的 pthread_join 提前去等一个还在清理的线程。
    mcaRestartDone = 1;
}

void systemReset() {
#ifdef PXT_ESP32
    esp_restart();
#else
    if (!panicCode)
        panicCode = -1;

    dmesg("TARGET RESET");

    if (mcaResetting) {
        // 清理中途又 panic：直接让线程退出，避免递归清理打死循环
        mcaTrace("[vm] systemReset re-entry during cleanup -> pthread_exit");
        pxt_vm_set_online(0);
        pthread_exit(NULL);
    }

    mcaResetting = 1;
    resetRuntimeState();
    mcaResetting = 0;
    mcaTrace("[vm] systemReset cleanup done -> pthread_exit");

    pthread_exit(NULL);
#endif
}


// ===== MCA：retro_serialize 快照（v1：同会话内恢复）=====
//
// 设计与约束见 mca_state.h。协调器在本文件（fiber/event/时钟/通知 ID 这些
// 调度器静态都在这里）；GC 静态经 mcaGcGetState/SetState（gc.cpp），
// 单例指针经各文件生成的 mca_snap_* 访问器（SINGLETON 宏 + sound.cpp 的 dac）。
//
// 缓冲布局（同进程原生字节序，不做可移植编码——v1 只承诺同会话恢复）：
//   [McaSnapHeader]
//   [GC 堆 heapUsed 字节]
//   [vmImg->stackBase 共享操作数栈]
//   fiber 记录 ×N：{McaSnapFiber}[FiberContext][stackCopy 内容]
//   event 记录 ×N：{McaSnapEvent}[Event]
//   [McaSchedState]（调度器/时钟/单例静态的值）
//   [gcRoots 内容][workQueue 内容]（LLSegment 缓冲，一般长度 0）

#define MCA_SNAP_MAGIC    0x3141434Du  // "MCA1"
#define MCA_SNAP_VERSION  1u
#define MCA_SNAP_MAX_FIBERS 64
#define MCA_SNAP_MAX_EVENTS 512

struct McaSnapHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t heapUsed;
    uint32_t numFibers;
    uint32_t numEvents;
    uint32_t stackBaseBytes;
    uint32_t totalSize;
    uint32_t reserved;
    uintptr_t gcBase;   // 恢复校验：非同一会话直接拒绝
    uintptr_t vmImg;
    McaGcState gc;
};

struct McaSnapFiber {
    FiberContext *ctxAddr;
    uint32_t ctxBytes;
    uint32_t copyBytes;
    TValue *copyAddr;
};

struct McaSnapEvent {
    Event *addr;
    uint32_t bytes;
    uint32_t reserved;
};

struct McaSchedState {
    FiberContext *allFibers, *currentFiber;
    Event *eventHead_, *eventTail_;
    Event lastEvent_;
    int panicCode_;
    int notifyId_;
    uint64_t virtTimeNum_;  // 按帧推进的虚拟时钟（1/3 µs 单位）
    uint64_t loops_;        // 已推进的圈数（前端请求计数要对齐它）
    unsigned randomState_;  // LFSR 随机状态（不存的话恢复后随机序列会漂）
    void *instWDisplay_, *instWSynthesizer_, *dac_;
};

extern "C" {
void *mca_snap_WDisplay();
void mca_unsnap_WDisplay(void *p);
void *mca_snap_WSynthesizer();
void mca_unsnap_WSynthesizer(void *p);
void *mcaSnapDac();
void mcaUnsnapDac(void *p);
}

namespace {

struct McaSnapWriter {
    uint8_t *p, *end;
    bool put(const void *src, size_t n) {
        if ((size_t)(end - p) < n)
            return false;
        if (src)
            memcpy(p, src, n);
        else
            memset(p, 0, n);
        p += n;
        return true;
    }
};

struct McaSnapReader {
    const uint8_t *p, *end;
    bool get(void *dst, size_t n) {
        if ((size_t)(end - p) < n)
            return false;
        memcpy(dst, p, n);
        p += n;
        return true;
    }
};

} // anonymous namespace

DLLEXPORT int pxt_state_pause() {
    if (!pxtVmLive || mcaResetRequest)
        return -1; // VM 未运行 / 正在重启，没有安全点可等
    mcaPauseRequest = 1;

    // 超时用**墙钟差**判定，不要用「循环次数 × 睡眠时长」：
    // sleep_core_us(500) 在 Windows 上把亚毫秒截断成 Sleep(0)，4000 次循环
    // 实际只跑几毫秒 —— 按次数算的话暂停窗口约等于 0，VM 还没来得及回到
    // 循环顶就被判超时（实测 tick enter=9/ret=8，VM 正睡在 target_yield 里）。
    // 这里改成 2 秒真实预算，VM 最坏 15ms 一醒（Sleep 粒度），足够。
    struct timeval t0;
    gettimeofday(&t0, NULL);
    for (;;) {
        if (mcaPaused)
            break;
        struct timeval t1;
        gettimeofday(&t1, NULL);
        long elapsedMs = (t1.tv_sec - t0.tv_sec) * 1000L + (t1.tv_usec - t0.tv_usec) / 1000L;
        if (elapsedMs >= 2000) {
            mcaPauseRequest = 0;
            return -2; // 超时：VM 卡在意外位置，放弃快照而不是挂死前端
        }
        sleep_core_us(1000);
    }
    return 0;
}

DLLEXPORT void pxt_state_resume() {
    mcaPauseRequest = 0;
}

// MCA：快照用的堆区间抽象。
//   64 位/MinGW：gcBase 起 1MB malloc 缓冲（PXT_VM_HEAP_ALLOC_BITS=20）
//   32 位      ：GC_BASE 起按页 mmap 的固定地址区，块线性分配，所以
//               [GC_BASE, gcAllocPtr) 全部已映射 —— 按同样的"基址+已用"
//               方式搬即可；没有固定的容量上界，serialize_size 取 16MB
//               （超限的活快照返回 -3 拒绝，不会写坏缓冲）。
static void *mcaSnapHeapBase() {
#if defined(PXT64) || defined(__MINGW32__)
    return gcBase;
#else
    return (uint8_t *)GC_BASE;
#endif
}
static size_t mcaSnapHeapCap() {
#if defined(PXT64) || defined(__MINGW32__)
    return (size_t)1 << PXT_VM_HEAP_ALLOC_BITS;
#else
    return (size_t)16 << 20;
#endif
}

// retro_serialize_size 必须恒定（RetroArch 按它一次性分配/校验），所以给
// 最坏情况上界：满堆 + 64 fiber × 满栈副本 + 512 event + 静态区。
// 实际快照远小于此（一般 ~几十 KB + 已用堆）。
DLLEXPORT unsigned pxt_state_max_size() {
    size_t n = 0;
    n += sizeof(McaSnapHeader);
    n += mcaSnapHeapCap();                                       // GC 堆
    n += VM_STACK_SIZE * sizeof(TValue);                         // 共享操作数栈
    n += MCA_SNAP_MAX_FIBERS *
         (sizeof(McaSnapFiber) + sizeof(FiberContext) +
          VM_STACK_SIZE * sizeof(TValue));                       // fiber+栈副本上界
    n += MCA_SNAP_MAX_EVENTS * (sizeof(McaSnapEvent) + sizeof(Event));
    n += sizeof(McaSchedState) + 4096;                           // 静态区+零头
    return (unsigned)n;
}

DLLEXPORT int pxt_state_save(void *buf, unsigned cap) {
    if (!mcaPaused)
        return -20; // 必须先 pxt_state_pause()

    McaGcState gc;
    mcaGcGetState(&gc);
    gc.gcAllocPtr = gcAllocPtr; // 切块游标本体在本文件
    if (!mcaSnapHeapBase() || !vmImg)
        return -1;  // VM 未启动
    if (gc.inGC || gc.pendingArrays)
        return -2;  // 不在安全点（到不了，防御）
    size_t heapUsed = (uint8_t *)gc.gcAllocPtr - (uint8_t *)mcaSnapHeapBase();
    if (heapUsed > mcaSnapHeapCap())
        return -3;

    int numFibers = 0;
    for (auto f = allFibers; f; f = f->next)
        if (++numFibers > MCA_SNAP_MAX_FIBERS)
            return -4;
    int numEvents = 0;
    for (auto e = eventHead; e; e = e->next)
        if (++numEvents > MCA_SNAP_MAX_EVENTS)
            return -5;

    McaSnapWriter w{(uint8_t *)buf, (uint8_t *)buf + cap};
    McaSnapHeader h;
    memset(&h, 0, sizeof(h));
    h.magic = MCA_SNAP_MAGIC;
    h.version = MCA_SNAP_VERSION;
    h.heapUsed = (uint32_t)heapUsed;
    h.numFibers = (uint32_t)numFibers;
    h.numEvents = (uint32_t)numEvents;
    h.stackBaseBytes = (uint32_t)(VM_STACK_SIZE * sizeof(TValue));
    h.gcBase = (uintptr_t)gc.gcBase;
    h.vmImg = (uintptr_t)vmImg;
    h.gc = gc;
    if (!w.put(&h, sizeof(h)))
        return -6;
    if (!w.put(gc.gcBase, heapUsed))
        return -6;
    if (!w.put(vmImg->stackBase, h.stackBaseBytes))
        return -6;
    for (auto f = allFibers; f; f = f->next) {
        McaSnapFiber r;
        r.ctxAddr = f;
        r.ctxBytes = (uint32_t)sizeof(FiberContext);
        r.copyAddr = f->stackCopy;
        r.copyBytes = f->stackCopy ? (uint32_t)(f->stackCopySize * (int)sizeof(TValue)) : 0;
        if (!w.put(&r, sizeof(r)) || !w.put(f, sizeof(FiberContext)))
            return -6;
        if (r.copyBytes && !w.put(f->stackCopy, r.copyBytes))
            return -6;
    }
    for (auto e = eventHead; e; e = e->next) {
        McaSnapEvent r;
        r.addr = e;
        r.bytes = (uint32_t)sizeof(Event);
        if (!w.put(&r, sizeof(r)) || !w.put(e, sizeof(Event)))
            return -6;
    }
    McaSchedState st;
    memset(&st, 0, sizeof(st));
    st.allFibers = allFibers;
    st.currentFiber = currentFiber;
    st.eventHead_ = eventHead;
    st.eventTail_ = eventTail;
    st.lastEvent_ = lastEvent;
    st.panicCode_ = panicCode;
    st.notifyId_ = mcaNotifyId;
    st.virtTimeNum_ = g_virtTimeNum;
    st.loops_ = g_loops;
    st.randomState_ = mcaGetRandomState();
    st.instWDisplay_ = mca_snap_WDisplay();
    st.instWSynthesizer_ = mca_snap_WSynthesizer();
    st.dac_ = mcaSnapDac();
    if (!w.put(&st, sizeof(st)))
        return -6;
    if (gc.gcRootsLen &&
        !w.put(gc.gcRootsData, gc.gcRootsLen * sizeof(TValue)))
        return -6;
    if (gc.workQueueLen &&
        !w.put(gc.workQueueData, gc.workQueueLen * sizeof(TValue)))
        return -6;

    size_t used = (size_t)(w.p - (uint8_t *)buf);
    ((McaSnapHeader *)buf)->totalSize = (uint32_t)used;
    mcaTrace("[snap] saved %zu bytes (heap %zu, fibers %d, events %d)",
             used, heapUsed, numFibers, numEvents);
    return (int)used;
}

DLLEXPORT int pxt_state_load(const void *buf, unsigned size) {
    if (!mcaPaused)
        return -20;

    McaSnapReader r{(const uint8_t *)buf, (const uint8_t *)buf + size};
    McaSnapHeader h;
    if (!r.get(&h, sizeof(h)) || h.magic != MCA_SNAP_MAGIC || h.version != MCA_SNAP_VERSION)
        return -1;
    McaGcState cur;
    mcaGcGetState(&cur);
    // v1 只支持同会话恢复：堆与镜像必须还在快照时的地址
    if (!cur.gcBase || !vmImg)
        return -1;
    if (h.gcBase != (uintptr_t)cur.gcBase || h.vmImg != (uintptr_t)vmImg)
        return -10;
    if (h.heapUsed > mcaSnapHeapCap())
        return -3;
    if (h.gc.gcAllocPtr != (uint8_t *)h.gc.gcBase + h.heapUsed)
        return -3;
    if (h.stackBaseBytes != VM_STACK_SIZE * sizeof(TValue))
        return -11;

    // 先释放当前 fiber/event：恢复会整体替换链。快照块地址可能已被快照后
    // 新分配的对象复用，覆写它是安全的 —— 所有引用根（GC 堆 / 调度器静态 /
    // fiber / event）都会被恢复成快照时的值，没有任何东西再指向新对象
    for (auto f = allFibers; f;) {
        auto n = f->next;
        xfree(f->stackCopy);
        xfree(f);
        f = n;
    }
    allFibers = NULL;
    currentFiber = NULL;
    for (auto e = eventHead; e;) {
        auto n = e->next;
        xfree(e);
        e = n;
    }
    eventHead = eventTail = NULL;

    if (!r.get(cur.gcBase, h.heapUsed))          // GC 堆内容
        return -12;
    if (!r.get(vmImg->stackBase, h.stackBaseBytes))
        return -12;
    for (uint32_t i = 0; i < h.numFibers; ++i) {
        McaSnapFiber rec;
        if (!r.get(&rec, sizeof(rec)) || rec.ctxBytes != sizeof(FiberContext))
            return -12;
        if (!r.get(rec.ctxAddr, rec.ctxBytes))
            return -12;
        if (rec.copyBytes && !r.get(rec.copyAddr, rec.copyBytes))
            return -12;
    }
    for (uint32_t i = 0; i < h.numEvents; ++i) {
        McaSnapEvent rec;
        if (!r.get(&rec, sizeof(rec)) || rec.bytes != sizeof(Event))
            return -12;
        if (!r.get(rec.addr, rec.bytes))
            return -12;
    }
    McaSchedState st;
    if (!r.get(&st, sizeof(st)))
        return -12;

    // GC 静态结构先行（gcBase/gcAllocPtr/firstBlock/globals...），堆内容上面已写
    mcaGcSetState(&h.gc);
    gcAllocPtr = (uint8_t *)h.gc.gcAllocPtr;

    allFibers = st.allFibers;
    currentFiber = st.currentFiber;
    eventHead = st.eventHead_;
    eventTail = st.eventTail_;
    lastEvent = st.lastEvent_;
    panicCode = st.panicCode_;
    mcaNotifyId = st.notifyId_;
    g_virtTimeNum = st.virtTimeNum_;
    // 恢复后把圈数对齐到前端已请求的帧数：快照这一帧的许可已经被消费掉了，
    // 若沿用旧计数，主循环会以为还有一帧待跑（或反过来多跑一圈）。
    g_loops = g_frameRequests;
    mcaSetRandomState(st.randomState_);
    mca_unsnap_WDisplay(st.instWDisplay_);
    mca_unsnap_WSynthesizer(st.instWSynthesizer_);
    mcaUnsnapDac(st.dac_);

    // LLSegment 缓冲内容（结构里的 data 指针已恢复）
    if (h.gc.gcRootsLen &&
        !r.get(h.gc.gcRootsData, h.gc.gcRootsLen * sizeof(TValue)))
        return -12;
    if (h.gc.workQueueLen &&
        !r.get(h.gc.workQueueData, h.gc.workQueueLen * sizeof(TValue)))
        return -12;

    mcaTrace("[snap] loaded (heap %u, fibers %u, events %u)",
             h.heapUsed, h.numFibers, h.numEvents);
    return 0;
}

} // namespace pxt
