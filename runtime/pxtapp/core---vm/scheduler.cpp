#include "pxt.h"
#include "mca_state.h"
#include "mca_tick.h"
#include "mca_fixed.h"

// MCA：随机状态存取（base/core.cpp），快照要带上它
namespace pxt {
unsigned mcaGetRandomState();
void mcaSetRandomState(unsigned v);
// MCA v0.2.11：快照恢复后修正绑定链上的 pending 事件指针（base/core.cpp）
void mcaFixupHandlerPending(void *(*mapFn)(void *));
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
// 最坏等待：本来以为只有"exec_loop 一个 tick（毫秒级）/ update 的帧等待 /
// target_yield 的 1ms"，所以给了宿主 2 秒预算。**这个假设是错的**：fiber 里
// 只要有一段不调 pause 的循环（游戏初始化、紧凑 CPU 循环），exec_loop 就会
// 一直跑到它自然让出为止 —— 桌面上几十毫秒，真机 ARM 慢一个数量级，实测
// 超过 2 秒，于是加载存档失败 `pause failed -2`。现在 exec_loop 自己每 2048
// 条指令查一次 mca_pause_pending() 并让出，见 vm.cpp。
//
// 协议是 want/done 两个序号（v0.2.13 重写；v0.2.12 的"停稳次数 +1"有竞态）：
//   宿主 pause ：my = ++mcaPauseSeq; mcaPauseWant = my; 等 mcaPauseDone == my
//   宿主 resume：mcaPauseWant = 0
//   VM         ：want != 0 且 want != done → done = want（= 已停稳）→ 睡到 want 变
//
// 为什么不能用"停稳次数 +1"：RetroArch 读档前会先 retro_serialize 存一个撤销点，
// 两次 pause 紧挨着。上一次的 resume（request=0）VM 还没观察到就又被置成 1，
// 于是 VM 待在暂停循环里不出来、也不再产生第二个 ack，宿主等的序号永远不到。
// 真机 trace 就是这个样子：`pause timeout 5s: spot=3 loops=742 req=742`
// —— VM 明明停在安全点上（spot=3 = 暂停中），宿主却在干等。
// 序号比较没有"必须被观察到"的要求：换个序号，VM 必然先退出内层循环再停一次。
static volatile unsigned mcaPauseWant = 0; // 宿主写的请求序号（0 = 无请求）
static volatile unsigned mcaPauseDone = 0; // VM 写的"已为哪个序号停稳"
static volatile unsigned mcaPauseSeq = 0;  // 宿主自增（只有宿主写）
static volatile int mcaPaused = 0;
// VM 线程当前在干什么（只在暂停超时时打到 trace 里，用来区分"卡在解释器"
// 和"卡在等帧/清理"）：
//   0 = 主循环顶   1 = 等前端取帧   2 = 跑 fiber   3 = 暂停中
static volatile int mcaVmSpot = 0;

// v0.2.14：读档后确认"VM 真的重新跑起来了"。pxt_state_load 成功时置 1，
// VM 线程从暂停里醒来时打一行 trace 并清掉 —— 这行缺失就说明恢复后 VM
// 再没回到主循环（真机上表现为"读档后画面停在原处"）。
static volatile int mcaSnapLoaded = 0;

// 单调毫秒（墙钟差，用于量暂停时长）。只在诊断路径上用，不进热路径。
static uint64_t mcaNowMs() {
    struct timeval t;
    gettimeofday(&t, NULL);
    return (uint64_t)t.tv_sec * 1000ull + (uint64_t)(t.tv_usec / 1000);
}

int mca_pause_pending() {
    const unsigned w = mcaPauseWant;
    return w != 0 && w != mcaPauseDone;
}

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
    va_list ap;
    // 开文件必须在锁内：mcaTraceTried 不是原子的，VM 线程和前端线程同时第一次
    // 写 trace 时两边都会 fopen(...,"w")，后开的把先写的内容截掉（实测桌面
    // 上 "[vm] ..." 那几行经常整个消失）。开文件只发生一次，开销无所谓。
    pthread_mutex_lock(&mcaTraceMutex);
    mcaTraceOpen();
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

    // MCA v0.2.14：panic 必须进 trace 并带上线程身份。
    // 此前 panic 只走 dmesg（进前端日志），mca_trace.log 里是空白 —— 于是
    // "加载存档后卡死、日志停在 cleanup 2/6" 这类问题无法判断到底是哪条
    // 线程、哪个 panic 码。DMESG 那两行保持原样（前端日志里还要看）。
    mcaTrace("[vm] PANIC %d on %s thread (errno=%d %s)", error_code,
             pxt_vm_is_vm_thread() ? "vm" : "frontend", prevErr, strerror(prevErr));

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
    // MCA v0.2.14：panic 之后走哪条路，取决于"当前在谁身上、跑到哪一步"。
    //
    // (1) 前端线程（retro_run / 拉音频）：**绝不能跑清理**。
    //     systemReset() 末端是 pthread_exit()，在前端线程上等于把 RetroArch 的
    //     render 线程注销掉（UI 永久卡住）；而且此刻它可能正握着 irqMutex /
    //     screenMutex，清理再进去抢就是自死锁 —— 真机日志精确停在
    //     "[vm] cleanup 2/6 reset callbacks"，那里调的 stopPlaying() 第一件事
    //     就是 target_disable_irq()。
    //     做法：只登记 panic 码，重启交给 VM 线程。
    //
    // (2) VM 线程、主循环已在跑：**也不当场 systemReset**。
    //     panic 基本都发生在 fiber 里，从那儿 pthread_exit 有两个坑：
    //       * Mingw 上直接崩（0xC0000005，PATCH 5 踩过这个坑）；
    //       * 它绕过 multiStart() 的收尾，mcaVmExited 永远不置位 → 前端以为
    //         VM 还活着、永不重启 → 真机表现"画面冻在最后一帧，进程不崩不退"。
    //     做法：panicCode 已经置位，exec_loop 里的 `if (panicCode) break;`
    //     会立刻结束当前 fiber，控制权回到主循环顶；那里看到 panicCode 就走
    //     hostRestartCleanup() + return，线程从 multiStart 正常收尾退出。
    //
    // (3) VM 线程、主循环还没起来（initRuntime 阶段就失败）：没有更干净的
    //     返回点，只能当场清理退出。
    if (!pxt_vm_is_vm_thread()) {
        mcaTrace("[vm] panic on frontend thread -> defer reset to vm thread");
        if (pxt_vm_running())
            pxt_vm_request_reset();
        // 不 pthread_exit、不 while(1)：调用方拿到控制权后自行返回。
    } else if (mcaMainLoopLive) {
        mcaTrace("[vm] panic in vm thread -> unwind to main loop (no pthread_exit)");
    } else {
        systemReset();
    }
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
    // 与 target_panic 同一条规矩（见那里 (1)(2)(3) 的说明）。
    if (!pxt_vm_is_vm_thread()) {
        mcaTrace("[vm] soft_panic on frontend thread -> defer reset to vm thread");
        if (pxt_vm_running())
            pxt_vm_request_reset();
    } else if (mcaMainLoopLive) {
        mcaTrace("[vm] soft_panic in vm thread -> unwind to main loop (no pthread_exit)");
    } else {
        systemReset();
    }
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
        // MCA v0.2.14：panic 后从这里收尾退出。
        //
        // panic 发生在 fiber 里时，target_panic / soft_panic 只置标志就返回
        //（见那里的 (2)），exec_loop 的 `if (panicCode) break;` 会把控制权送回
        // 这里。走 hostRestartCleanup() 之后**正常 return**，线程从 multiStart
        // 收尾退出（mcaVmExited=1），前端据此自动重启 —— 而不是 pthread_exit
        //（Mingw 上崩、Linux 上会让前端以为 VM 还活着）。
        if (panicCode) {
            mcaTrace("[vm] panic %d seen at main loop -> cleanup and exit", panicCode);
            hostRestartCleanup();
            mcaMainLoopLive = 0;
            return;
        }
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
        // MCA：快照暂停（协议见文件顶部 mcaPauseWant 的说明）。宿主拿到停稳
        // 回执后搬状态（save/load 都可能发生），恢复时 fiber 链整个被替换 ——
        // 本地 f 必须作废，否则下一次 while(f) 会踩已释放的旧 fiber。
        {
            const unsigned want = mcaPauseWant;
            if (want && want != mcaPauseDone) {
                mcaPauseDone = want; // 已经在本安全点：先回执，再睡
                mcaPaused = 1;
                mcaVmSpot = 3;
                // 宿主 resume（want=0）或者换了新请求（want != want）都要醒。
                // 用"等于本次序号"而不是布尔量：两次 pause 紧挨着时，中间那个
                // resume 可能压根观察不到，但序号一定会变。
                //
                // v0.2.14：把"睡了多久"记下来。真机上出现过"读档后画面不动"，
                // 到底是没 resume（这里一直睡下去）还是 resume 了但 VM 跑飞，
                // 靠这一行区分：停了 >2s 说明宿主漏了 resume 或自己卡住了。
                const uint64_t pauseT0 = mcaNowMs();
                while (mcaPauseWant == want)
                    mca_tick_wait(5);
                const uint64_t pausedMs = mcaNowMs() - pauseT0;
                if (pausedMs > 2000)
                    mcaTrace("[snap] vm was paused %llu ms (host side slow?)",
                             (unsigned long long)pausedMs);
                mcaPaused = 0;
                mcaPauseDone = 0;
                mcaVmSpot = 0;
                f = NULL;
                // v0.2.14：读档后 VM 到底有没有重新跑起来，就看这一行有没有
                // 出现（以及紧随其后的 "first frame after load"）。
                if (mcaSnapLoaded) {
                    mcaSnapLoaded = 0;
                    mcaTrace("[snap] vm resumed after load (loops=%llu req=%llu panic=%d)",
                             (unsigned long long)g_loops, (unsigned long long)g_frameRequests,
                             panicCode);
                }
                continue;
            }
        }        // 一圈一帧：等前端请求下一帧再推进。
        // 没有这个同步，VM 的圈数会随调度在「前端帧数」附近漂移，虚拟时间
        // 跟着漂，同一输入的两次运行画面就分岔（存档确定性的前提）。
        if (!mca_frame_requested()) {
            mcaVmSpot = 1;
            mca_tick_wait(5);
            mcaVmSpot = 0;
            continue;
        }
        mcaVmSpot = 2;
        g_loops++;
        g_virtTimeNum += MCA_FRAME_STEP;
        // 发布一帧给前端（内容 = 上一圈逻辑执行完后的画面缓冲）。
        // 放在圈顶而不是圈尾：圈内有多处 continue，放圈尾会漏帧，帧数就对不上了。
        pxt_screen_publish(g_loops); // 拷缓冲 + 置序号 + 广播（同一把锁）
        // MCA v0.2.14：panic 之后不能在这里 return。
        // 直接 return 会回到 initRuntime 末尾的 systemReset()（pthread_exit），
        // 那条路在 Mingw 上崩、在 Linux 上绕过 multiStart 的收尾。改成 continue，
        // 让循环顶那个 panic 分支统一做 hostRestartCleanup() + 正常返回。
        if (panicCode)
            continue;
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
            // panic 后不能在这里 `return`（会落到 initRuntime 末尾的
            // systemReset() → pthread_exit），也不能单纯 `break`（那会跳出
            // 外层 for，同样从 mainRunLoop 返回）。就在这里把收尾做掉，
            // 然后 return 到 initRuntime —— 它看到 mcaRestartDone 就不再
            // systemReset()，线程从 multiStart 正常退出（mcaVmExited=1）。
            if (panicCode) {
                mcaTrace("[vm] panic %d after exec_loop -> cleanup and exit", panicCode);
                f = NULL;
                hostRestartCleanup();
                mcaMainLoopLive = 0;
                return;
            }
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
        // MCA v0.2.11：堆钉在 GC_BASE（32 位一直是这个地址，64 位原来是
        // xmalloc 拿到哪算哪）。堆里全是绝对指针，地址不固定的话，
        // "保存 → 退出 RetroArch → 重开游戏 → 加载存档"必然 -10。
        // 钉不住（地址被占）就退回普通分配：游戏照跑，跨会话存档退化，
        // 由快照校验去拒绝，而不是在这里崩。
        gcBase = (uint8_t *)mcaFixedMap((unsigned long long)GC_BASE,
                                       (size_t)1 << PXT_VM_HEAP_ALLOC_BITS);
        if (!gcBase) {
            gcBase = (uint8_t *)xmalloc(1 << PXT_VM_HEAP_ALLOC_BITS);
            mcaTrace("[heap] %p 取不到固定地址 %p，跨会话快照将不可用",
                     (void *)gcBase, (void *)(uintptr_t)GC_BASE);
        }
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
    // MCA v0.2.11：32 位也走固定映射。原来这里是不带 MAP_FIXED 的 mmap，
    // 内核只当"地址建议"，实际可能给别的地址 —— 那样 GC_BASE 的判断
    // （isPointer/isReadOnly）和快照的跨会话语义都是错的。
    void *r = mcaFixedMap((unsigned long long)(uintptr_t)currPtr, sz);
    if (!r) {
        // 目标地址被占：退回普通 mmap 保证能跑，跨会话存档退化
        r = mmap(currPtr, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (r == MAP_FAILED) {
            DMESG("mmap %p failed; err=%d", currPtr, errno);
            target_panic(PANIC_INTERNAL_ERROR);
        }
        mcaTrace("[heap] %p 取不到固定地址 %p，跨会话快照将不可用", r, currPtr);
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
    // MCA v0.2.14：先报"谁在清理"。清理只该发生在 VM 线程上；如果这行写成
    // frontend，说明有人从前端线程走了 systemReset（那会 pthread_exit 掉前端
    // 线程，并可能自死锁在 irqMutex 上）。
    mcaTrace("[vm] cleanup begin on %s thread (panic=%d)",
             pxt_vm_is_vm_thread() ? "vm" : "frontend", panicCode);

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

    // MCA v0.2.14：每个回调单独留痕。
    //
    // 真机上出现过"日志停在这一步不动"（cleanup 2/6 之后什么都没有）。
    // 这一步跑的是各模块注册的 reset 回调（v0.2.13 只有 melody 的
    // stopPlaying），出问题的话必须能看出是哪个回调、卡在它前面还是后面。
    mcaTrace("[vm] cleanup 2/6 reset callbacks");
    for (int i = 0; i < MAX_RESET_FN; ++i) {
        auto fn = resetFunctions[i];
        if (!fn)
            continue;
        mcaTrace("[vm]   reset fn #%d %p enter", i, (void *)fn);
        fn();
        mcaTrace("[vm]   reset fn #%d %p done", i, (void *)fn);
    }
    mcaTrace("[vm] cleanup 2/6 done");

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

// MCA v0.2.14：VM 线程自己退出的统一出口。
//
// 为什么不能只写 pthread_exit()：pthread_exit **不走** multiStart() 的 return
// 路径，那些收尾语句（mcaMainLoopLive=0 / offline / mcaVmExited=1）一行都不会
// 执行。于是宿主永远认为 VM 还活着（pxt_vm_running() == true），前端也就永远
// 不会走重启分支 —— 真机上的表现正是"清理日志停在中途，画面冻在最后一帧，
// 进程不崩也不退出"。凡是线程要自己死掉，都必须先把"我死了"写清楚。
static void vmThreadExitNow(const char *why) {
    mcaTrace("[vm] %s -> thread exit (marked)", why);
    mcaMainLoopLive = 0;
    pxt_vm_set_online(0);
    mcaVmExited = 1; // 宿主（vm_poll_restart）据此 join 并用原始镜像重新启动
    pthread_exit(NULL);
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
        vmThreadExitNow("systemReset re-entry during cleanup");
    }

    mcaResetting = 1;
    resetRuntimeState();
    mcaResetting = 0;
    mcaTrace("[vm] systemReset cleanup done");

    vmThreadExitNow("after cleanup");
#endif
}


// ===== MCA：retro_serialize 快照（v2：跨会话恢复）=====
//
// v1 只认"同一次加载游戏内"恢复：堆和镜像换了地址就 -10。可真机上的用法是
// "存一档 → 退出 RetroArch → 重开游戏 → 加载"，v1 因此形同摆设。
//
// v2 的前提是**地址可复现**：GC 堆钉在 GC_BASE、镜像钉在 GC_BASE+1MB
// （见 mca_fixed.h），于是堆里那些互相指向的绝对地址、镜像里的代码地址
// 在新进程里依然成立。剩下三类对象地址不可预测，逐个重定位：
//   * FiberContext（xmalloc）：重建并修正 next / stackCopy / sp / img
//   * Event（new）：重建并重串链；堆内 HandlerBinding::pending 跟着改指向
//   * stackCopy（xmalloc）：重建，sp 按保存时的字节偏移恢复
// 换卡带、或固定地址没抢到时，仍按 -10 拒绝（宁可拒绝也不让前端崩）。
//
// 缓冲布局（同进程原生字节序，不做可移植编码）：
//   [McaSnapHeader]
//   [GC 堆 heapUsed 字节]
//   [vmImg->stackBase 共享操作数栈]
//   fiber 记录 ×N：{McaSnapFiber}[FiberContext][stackCopy 内容]
//   event 记录 ×N：{McaSnapEvent}[Event]
//   [McaSchedState]（调度器/时钟/单例静态的值）
//   [gcRoots 内容][workQueue 内容]（LLSegment 缓冲，一般长度 0）

#define MCA_SNAP_MAGIC    0x3141434Du  // "MCA1"
#define MCA_SNAP_VERSION  3u
#define MCA_SNAP_MAX_FIBERS 64
#define MCA_SNAP_MAX_EVENTS 512
// sp 的三种归宿：不处理 / 指向 fiber 自己的栈副本 / 指向共享操作数栈。
// 两处地址都是每次运行才确定的，只能存偏移。
#define MCA_SNAP_SP_NONE   0u
#define MCA_SNAP_SP_COPY   1u
#define MCA_SNAP_SP_SHARED 2u

// 镜像指纹的算法在 vmload.cpp（mcaFnv1a），因为必须在打补丁**之前**算：
// patch 会把 vtable/内核函数指针写进镜像，跨进程（ASLR）就变。

struct McaSnapHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t heapUsed;
    uint32_t numFibers;
    uint32_t numEvents;
    uint32_t stackBaseBytes;
    uint32_t totalSize;
    uint32_t screenBytes; // 每块画面缓冲的字节数（0 = 该快照没带画面）
    uintptr_t gcBase;    // 恢复校验：堆必须还在同一地址（否则堆内指针全废）
    uintptr_t vmImg;     // 诊断用（VMImage 结构地址，堆里没有指向它的指针）
    uintptr_t dataStart; // 镜像基址：堆里的字面量/函数引用都指向它
    // 本模块（core 的 .so/.dll）在保存时的加载区间。堆里存着指向模块代码的
    // 指针（RefAction 的内核回调、fiber 的 wakeFn……），模块基址换进程就变，
    // 恢复时必须按差值平移（见 mcaRelocateModulePtrs）。
    unsigned long long modBase, modSize;
    // 卡带身份（镜像字节数的 FNV-1a 指纹）。v3 起堆/镜像都钉在固定地址，
    // "地址不符"不再能区分换了卡带 —— 拿别的游戏的快照恢复会把堆当成
    // 另一套布局去解释，必须在这里挡住。
    uint32_t imgBytes, imgHash;
    McaGcState gc;
};

struct McaSnapFiber {
    FiberContext *ctxAddr; // 仅用于诊断/顺序对应，恢复时按记录顺序重串
    uint32_t ctxBytes;
    uint32_t copyBytes;
    uint32_t spKind;       // MCA_SNAP_SP_*
    uint32_t spOffset;     // sp 相对栈副本 / 共享操作数栈的字节偏移
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
    // 只推进不取（干跑校验用）
    bool skip(size_t n) {
        if ((size_t)(end - p) < n)
            return false;
        p += n;
        return true;
    }
};

// 旧地址 → 新地址映射。加载时 VM 处于暂停状态（pxt_state_pause 保证），
// 这两个表只被恢复流程使用，所以用文件级静态即可，不必加锁。
struct McaAddrPair {
    uintptr_t oldAddr;
    void *newAddr;
};
McaAddrPair g_eventMap[MCA_SNAP_MAX_EVENTS];
uint32_t g_eventMapN = 0;

// 在一段内存里把等于"旧地址"的指针槽换成新地址（按指针宽度对齐扫描）。
// 候选集合很小（fiber 数 ≤ 64），逐个比较即可；不做区间推断，避免误改写。
static int mcaFixupPtrSlots(void *base, size_t bytes, const McaAddrPair *map, uint32_t n) {
    if (!base || !bytes || !n)
        return 0;
    uintptr_t start = ((uintptr_t)base + sizeof(uintptr_t) - 1) & ~(uintptr_t)(sizeof(uintptr_t) - 1);
    uintptr_t stop = (uintptr_t)base + bytes;
    if (start >= stop)
        return 0;
    uintptr_t *q = (uintptr_t *)start;
    const size_t cnt = (size_t)((stop - start) / sizeof(uintptr_t));
    int hits = 0;
    for (size_t i = 0; i < cnt; ++i) {
        if (!q[i])
            continue;
        for (uint32_t k = 0; k < n; ++k) {
            if (q[i] == map[k].oldAddr) {
                q[i] = (uintptr_t)map[k].newAddr;
                hits++;
                break;
            }
        }
    }
    return hits;
}

// 给 base/core.cpp 的绑定链用：把 pending 事件指针改到新地址。
// 找不到映射的（快照里没记录）一律置空 —— 宁可丢一个待处理事件，
// 也不能留着悬垂指针等下一帧解引用。
void *mcaMapEventAddr(void *p) {
    if (!p)
        return NULL;
    for (uint32_t i = 0; i < g_eventMapN; ++i)
        if (g_eventMap[i].oldAddr == (uintptr_t)p)
            return g_eventMap[i].newAddr;
    mcaTrace("[snap] 未映射的 pending 事件 %p，置空", p);
    return NULL;
}

} // anonymous namespace

DLLEXPORT int pxt_state_pause() {
    if (!pxtVmLive || mcaResetRequest) {
        // v0.2.15 发现 armhf 上走到这里（64 位不会）：-1 之前完全静默，
        // 连"哪个标志导致的"都看不见，先留下现场
        mcaTrace("[snap] pause reject -1: live=%d reset=%d panic=%d spot=%d",
                 pxtVmLive, mcaResetRequest, panicCode, mcaVmSpot);
        return -1; // VM 未运行 / 正在重启，没有安全点可等
    }
    // 换一个**新的**序号（协议见文件顶部）。绝不能写成"等停稳次数 +1"：
    // 连续两次 pause 时，中间那次 resume 可能压根没被 VM 观察到。
    unsigned my = ++mcaPauseSeq;
    if (!my)
        my = ++mcaPauseSeq; // 0 保留给"无请求"
    mcaPauseWant = my;

    // 超时用**墙钟差**判定，不要用「循环次数 × 睡眠时长」：
    // sleep_core_us(500) 在 Windows 上把亚毫秒截断成 Sleep(0)，4000 次循环
    // 实际只跑几毫秒 —— 按次数算的话暂停窗口约等于 0，VM 还没来得及回到
    // 循环顶就被判超时（实测 tick enter=9/ret=8，VM 正睡在 target_yield 里）。
    // 这里用真实预算，VM 最坏 15ms 一醒（Sleep 粒度），足够。
    struct timeval t0;
    gettimeofday(&t0, NULL);
    for (;;) {
        if (mcaPauseDone == my)
            break; // VM 已经停在这个序号对应的安全点上
        struct timeval t1;
        gettimeofday(&t1, NULL);
        long elapsedMs = (t1.tv_sec - t0.tv_sec) * 1000L + (t1.tv_usec - t0.tv_usec) / 1000L;
        if (elapsedMs >= 5000) {
            mcaPauseWant = 0;
            // 卡住时把 VM 线程的位置打出来：spot 1=等帧 2=跑 fiber（正常几毫秒
            // 就该回来）3=卡在暂停里 0=主循环顶但没看到请求（内存可见性）
            int nf = 0;
            for (auto f = allFibers; f; f = f->next)
                nf++;
            mcaTrace("[snap] pause timeout 5s: spot=%d want=%u done=%u "
                     "loops=%llu req=%llu fibers=%d live=%d online=%d panic=%d reset=%d",
                     mcaVmSpot, my, (unsigned)mcaPauseDone,
                     (unsigned long long)g_loops, (unsigned long long)g_frameRequests,
                     nf, (int)mcaMainLoopLive, pxtVmLive, panicCode, mcaResetRequest);
            return -2; // 超时：VM 卡在意外位置，放弃快照而不是挂死前端
        }
        sleep_core_us(1000);
    }
    return 0;
}

DLLEXPORT void pxt_state_resume() {
    mcaPauseWant = 0;
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
    h.dataStart = (uintptr_t)vmImg->dataStart;
    h.modBase = mcaModuleBase();
    h.modSize = mcaModuleSize();
    // 卡带身份：取装载时算好的**原始**镜像指纹（vmload.cpp 在 vm_patch_image
    // 之前算）。不能在这里对着 dataStart..dataEnd 现算 —— 那块已经被 patch
    // 写进了 vtable/内核函数指针，跨进程（ASLR）就变。
    h.imgBytes = mca_image_bytes();
    h.imgHash = mca_image_hash();
    h.screenBytes = pxt_screen_buf_size();
    h.gc = gc;
    if (!w.put(&h, sizeof(h)))
        return -6;
    if (!w.put(gc.gcBase, heapUsed))
        return -6;
    if (!w.put(vmImg->stackBase, h.stackBaseBytes))
        return -6;
    // 画面：work（游戏正在写）+ pub（已发布），恢复后原样写回，
    // 否则加载存档的头两三帧是空的
    if (h.screenBytes) {
        uint8_t *tmp = (uint8_t *)xmalloc((size_t)h.screenBytes * 2);
        if (!tmp || pxt_screen_dump(tmp, tmp + h.screenBytes) != 0 ||
            !w.put(tmp, (size_t)h.screenBytes * 2)) {
            if (tmp)
                xfree(tmp);
            return -6;
        }
        xfree(tmp);
    }
    for (auto f = allFibers; f; f = f->next) {
        McaSnapFiber r;
        memset(&r, 0, sizeof(r));
        r.ctxAddr = f;
        r.ctxBytes = (uint32_t)sizeof(FiberContext);
        r.copyBytes = f->stackCopy ? (uint32_t)(f->stackCopySize * (int)sizeof(TValue)) : 0;
        // sp 指向 fiber 栈副本或共享操作数栈，两者地址每次都不同 —— 存偏移。
        // 栈副本优先（fiber 被挂起时 sp 指向副本）；否则看共享栈。
        r.spKind = MCA_SNAP_SP_NONE;
        r.spOffset = 0;
        if (f->sp) {
            if (f->stackCopy && (uint8_t *)f->sp >= (uint8_t *)f->stackCopy &&
                (uint8_t *)f->sp < (uint8_t *)f->stackCopy + r.copyBytes) {
                r.spKind = MCA_SNAP_SP_COPY;
                r.spOffset = (uint32_t)((uint8_t *)f->sp - (uint8_t *)f->stackCopy);
            } else if (vmImg->stackBase && (uint8_t *)f->sp >= (uint8_t *)vmImg->stackBase &&
                       (uint8_t *)f->sp <
                           (uint8_t *)vmImg->stackBase + h.stackBaseBytes) {
                r.spKind = MCA_SNAP_SP_SHARED;
                r.spOffset = (uint32_t)((uint8_t *)f->sp - (uint8_t *)vmImg->stackBase);
            }
        }
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
    if (!cur.gcBase || !vmImg || !vmImg->dataStart)
        return -1;

    // v2 的硬前提：堆与镜像必须还在快照时的地址。
    // 这两块是"指针可达性"的根 —— 堆里的对象互相指向、镜像里的字面量被
    // 堆对象引用，换了地址整片指针全废，没有任何办法补救（rebuild 等于
    // 重新实现 VM 序列化）。正常路径下它们由 mcaFixedMap 钉在
    // GC_BASE / GC_BASE+1MB；这里对不上说明换了卡带，或固定地址没抢到。
    // fiber/event 的地址反而无所谓 —— 下面逐个重建并修正引用。
    if (h.gcBase != (uintptr_t)cur.gcBase || h.dataStart != (uintptr_t)vmImg->dataStart) {
        mcaTrace("[snap] 拒绝恢复：地址不符（快照堆 %p 镜像 %p / 当前堆 %p 镜像 %p）",
                 (void *)h.gcBase, (void *)h.dataStart, (void *)cur.gcBase,
                 (void *)vmImg->dataStart);
        return -10;
    }
    {
        // 卡带身份：堆/镜像都钉在固定地址之后，"地址不符"不再能说明换了游戏，
        // 必须比字节数和指纹（否则 ping 的堆会被当成 jumpy 的布局解释）。
        // 指纹来自装载时的原始镜像（见 mca_image_hash）。
        const uint32_t curBytes = mca_image_bytes();
        const uint32_t curHash = mca_image_hash();
        if (h.imgBytes != curBytes || h.imgHash != curHash) {
            mcaTrace("[snap] 拒绝恢复：卡带不同（快照 %u 字节/%08x，当前 %u 字节/%08x）",
                     h.imgBytes, h.imgHash, curBytes, curHash);
            return -10;
        }
    }
    // 换进程判定：vmImg 是宿主每次装载时分配的，同一次运行内不变，换进程必变。
    // 跨进程恢复要重定位"指向 core 自身代码"的指针，前提是能问出模块的加载
    // 区间；问不出来（目前 macOS）就别硬来 —— 带着野回调跑，第一次调用就是
    // 段错误。宁可退回 v0.2.10 的"同会话专用"。
    const bool crossProc = (h.vmImg != (uintptr_t)vmImg);
    if (crossProc && (!h.modBase || !mcaModuleBase())) {
        mcaTrace("[snap] 拒绝恢复：本平台取不到模块区间，跨会话恢复不安全");
        return -13;
    }
    if (h.heapUsed > mcaSnapHeapCap())
        return -3;
    if (h.gc.gcAllocPtr != (uint8_t *)h.gc.gcBase + h.heapUsed)
        return -3;
    if (h.stackBaseBytes != VM_STACK_SIZE * sizeof(TValue))
        return -11;
    if (h.numFibers > MCA_SNAP_MAX_FIBERS || h.numEvents > MCA_SNAP_MAX_EVENTS)
        return -12;
    // 前端按 retro_serialize_size（最坏上界）给的缓冲，size 通常远大于实际
    // 快照长度；真正的边界是 header 里的 totalSize。只要求它能放得下。
    if (h.totalSize < sizeof(McaSnapHeader) || h.totalSize > size)
        return -12;
    r.end = (const uint8_t *)buf + h.totalSize; // 后续一切按实际长度校验

    // ---- 干跑：把所有记录边界先验一遍 ----
    // 重建过程会 release 掉当前 fiber/event，中途失败就留下半截状态（下一次
    // retro_run 直接踩空）。所以越界/尺寸问题必须在动手之前发现。
    {
        McaSnapReader v{r.p, r.end};
        if (!v.skip(h.heapUsed) || !v.skip(h.stackBaseBytes) ||
            !v.skip((size_t)h.screenBytes * 2))
            return -12;
        for (uint32_t i = 0; i < h.numFibers; ++i) {
            McaSnapFiber rec;
            if (!v.get(&rec, sizeof(rec)) || rec.ctxBytes != sizeof(FiberContext))
                return -12;
            if (!v.skip(rec.ctxBytes) || !v.skip(rec.copyBytes))
                return -12;
        }
        for (uint32_t i = 0; i < h.numEvents; ++i) {
            McaSnapEvent rec;
            if (!v.get(&rec, sizeof(rec)) || rec.bytes != sizeof(Event))
                return -12;
            if (!v.skip(rec.bytes))
                return -12;
        }
        if (!v.skip(sizeof(McaSchedState)))
            return -12;
        if (!v.skip(h.gc.gcRootsLen * sizeof(TValue)))
            return -12;
        if (!v.skip(h.gc.workQueueLen * sizeof(TValue)))
            return -12;
    }

    mcaTrace("[snap] load: dry-run ok, heap=%u fibers=%u events=%u total=%u",
             h.heapUsed, h.numFibers, h.numEvents, h.totalSize);

    // fiber/event 先全部释放：恢复会整体替换链。
    // 注意**不能**在原地写回（旧地址已经被 free 掉，属于"野内存"）：
    // 分配器随时可能把这块内存再给出去 —— 游戏下一次 setupThread 新建 fiber
    // 就会盖住恢复回来的那一个，表现为"读档后几十帧画面才走岔"（要等到真的
    // 新建 fiber 那一步）。所以下面一律 xmalloc 重建，旧地址只作为映射键。
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

    // 堆内容直接写回固定地址（同一块内存，指针天然有效）
    if (!r.get(cur.gcBase, h.heapUsed))
        return -12;
    if (!r.get(vmImg->stackBase, h.stackBaseBytes))
        return -12;
    if (h.screenBytes) {
        // 先换缓冲（跨会话旧地址已失效），再把快照里的画面写进去
        pxt_screen_rebind();
        unsigned cur = pxt_screen_buf_size();
        uint8_t *tmp = (uint8_t *)xmalloc((size_t)h.screenBytes * 2);
        if (!tmp || !r.get(tmp, (size_t)h.screenBytes * 2)) {
            if (tmp)
                xfree(tmp);
            return -12;
        }
        // 画面尺寸对不上说明快照来自另一套显示配置（理论上已被卡带校验挡住）
        if (cur == h.screenBytes)
            pxt_screen_restore(tmp, tmp + h.screenBytes);
        xfree(tmp);
    }
    // 堆里存着指向本模块代码的指针（RefAction 的内核回调、fiber 的 wakeFn…），
    // 模块基址换进程就变。按保存时的区间把命中的值整体平移 —— 不做这件事，
    // 跨会话恢复后第一次调用这些回调就是段错误（真机上"加载存档后崩前端"）。
    {
        int nHeap = mcaRelocateModulePtrs(cur.gcBase, h.heapUsed, h.modBase, h.modSize);
        int nStk = mcaRelocateModulePtrs(vmImg->stackBase, h.stackBaseBytes, h.modBase, h.modSize);
        mcaTrace("[snap] module relocate: old=%llx size=%llu cur=%llx hits=%d+%d",
                 h.modBase, h.modSize, mcaModuleBase(), nHeap, nStk);
    }

    // ---- fiber：重建 + 修正内部引用 ----
    McaAddrPair fmap[MCA_SNAP_MAX_FIBERS];
    uint32_t nF = 0;
    FiberContext *head = NULL, *tail = NULL;
    for (uint32_t i = 0; i < h.numFibers; ++i) {
        McaSnapFiber rec;
        if (!r.get(&rec, sizeof(rec)))
            return -12;
        // fiber 一律重建（地址变化由下面的 fmap / mcaFixupPtrSlots 兜住）：
        // 快照里的 ctxAddr / stackCopy 都是**上一次运行的地址**，同进程时虽然
        // 还在同一块内存里，但那块内存已经 free 过，不能当活对象用。
        FiberContext *nf = (FiberContext *)xmalloc(sizeof(FiberContext));
        if (!r.get(nf, sizeof(FiberContext)))
            return -12;
        // 结构体里有模块指针（wakeFn = 内核里的静态函数），先平移再改字段
        mcaRelocateModulePtrs(nf, sizeof(FiberContext), h.modBase, h.modSize);
        TValue *copy = NULL;
        if (rec.copyBytes) {
            copy = (TValue *)xmalloc(rec.copyBytes);
            if (!r.get(copy, rec.copyBytes))
                return -12;
        }
        // 三处不可复现的东西：栈副本本体、sp、镜像。
        nf->stackCopy = copy;
        if (rec.spKind == MCA_SNAP_SP_COPY && copy)
            nf->sp = (TValue *)((uint8_t *)copy + rec.spOffset);
        else if (rec.spKind == MCA_SNAP_SP_SHARED && vmImg->stackBase)
            nf->sp = (TValue *)((uint8_t *)vmImg->stackBase + rec.spOffset);
        else if (!copy)
            nf->sp = NULL;
        nf->img = vmImg;
        nf->imgbase = (uint16_t *)vmImg->dataStart;
        // 链表按记录顺序重串，不信快照里的 next 指针
        nf->next = NULL;
        if (tail)
            tail->next = nf;
        else
            head = nf;
        tail = nf;
        fmap[nF].oldAddr = (uintptr_t)rec.ctxAddr;
        fmap[nF].newAddr = nf;
        nF++;
    }

    // ---- event：重建 + 重串链 ----
    g_eventMapN = 0;
    Event *ehead = NULL, *etail = NULL;
    for (uint32_t i = 0; i < h.numEvents; ++i) {
        McaSnapEvent rec;
        if (!r.get(&rec, sizeof(rec)))
            return -12;
        auto ne = new Event();
        if (!r.get(ne, sizeof(Event)))
            return -12;
        ne->next = NULL;
        if (etail)
            etail->next = ne;
        else
            ehead = ne;
        etail = ne;
        g_eventMap[g_eventMapN].oldAddr = (uintptr_t)rec.addr;
        g_eventMap[g_eventMapN].newAddr = ne;
        g_eventMapN++;
    }

    McaSchedState st;
    if (!r.get(&st, sizeof(st)))
        return -12;

    // GC 静态结构先行（gcBase/gcAllocPtr/firstBlock/globals...），堆内容上面已写
    mcaGcSetState(&h.gc);
    gcAllocPtr = (uint8_t *)h.gc.gcAllocPtr;

    allFibers = head;
    currentFiber = head; // 先用链首，再按快照记录换算
    for (uint32_t i = 0; i < nF; ++i)
        if (fmap[i].oldAddr == (uintptr_t)st.currentFiber)
            currentFiber = (FiberContext *)fmap[i].newAddr;
    eventHead = ehead;
    eventTail = etail;
    lastEvent = st.lastEvent_;
    panicCode = st.panicCode_;
    mcaNotifyId = st.notifyId_;
    g_virtTimeNum = st.virtTimeNum_;
    // 恢复后把圈数对齐到前端已请求的帧数：快照这一帧的许可已经被消费掉了，
    // 若沿用旧计数，主循环会以为还有一帧待跑（或反过来多跑一圈）。
    g_loops = g_frameRequests;
    mcaSetRandomState(st.randomState_);
    // fiber 一律重建，地址随之变化，而**堆里可能有指向它们的值**（RefPromise
    // 之类持有 FiberContext*）。这里按旧→新表扫一遍堆和操作数栈把引用接上，
    // 否则这些对象指向已释放的 fiber：表现为"读档几十帧后画面走岔"。
    {
        int nRef = mcaFixupPtrSlots(cur.gcBase, h.heapUsed, fmap, nF);
        nRef += mcaFixupPtrSlots(vmImg->stackBase, h.stackBaseBytes, fmap, nF);
        if (nRef)
            mcaTrace("[snap] fiber refs fixed in heap/stack: %d", nRef);
    }
    // 堆里的 HandlerBinding::pending 指向旧 Event 地址，跟着改指向
    mcaFixupHandlerPending(mcaMapEventAddr);
    mca_unsnap_WDisplay(st.instWDisplay_);
    mca_unsnap_WSynthesizer(st.instWSynthesizer_);
    mcaUnsnapDac(st.dac_);

    // LLSegment 缓冲内容（mcaGcSetState 已按当前会话重建了缓冲，
    // 这里往新地址填内容；不能再按快照里的旧指针写）
    if (h.gc.gcRootsLen &&
        !r.get(mcaGcRootsData(), h.gc.gcRootsLen * sizeof(TValue)))
        return -12;
    // 根集/工作队列里也可能有模块指针
    mcaRelocateModulePtrs(mcaGcRootsData(), h.gc.gcRootsLen * sizeof(TValue), h.modBase,
                          h.modSize);
    if (h.gc.workQueueLen &&
        !r.get(mcaGcWorkQueueData(), h.gc.workQueueLen * sizeof(TValue)))
        return -12;

    mcaTrace("[snap] loaded (heap %u, fibers %u, events %u)",
             h.heapUsed, h.numFibers, h.numEvents);
    // 单例已恢复，但它们的明细缓冲（画面双缓冲等）是 malloc 的、跨会话地址
    // 已失效 —— 第一帧发布就会往野指针 memcpy。这里换新的。
    pxt_screen_rebind(); // 没带画面的快照也要重绑，避免往野指针写

    // v0.2.14：等 VM 线程真正从暂停里醒过来时，trace 里会补一行
    // "vm resumed after load"。没有这一行 = 恢复流程把 VM 弄丢了
    //（不是"画面没重画"，是根本没回到主循环），这是真机排查的关键分界。
    mcaSnapLoaded = 1;
    return 0;
}

} // namespace pxt
