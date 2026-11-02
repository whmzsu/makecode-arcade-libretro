#include "pxt.h"

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
    auto r = malloc(sz);
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
static volatile int mcaRestartDone = 0;

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

// trace 同时写文件与 stderr：文件好找，stderr 会进 RetroArch 自己的日志。
void mcaTrace(const char *fmt, ...) {
    mcaTraceOpen();
    va_list ap;
    va_start(ap, fmt);
    if (mcaTraceFile) {
        vfprintf(mcaTraceFile, fmt, ap);
        fputc('\n', mcaTraceFile);
        fflush(mcaTraceFile);
    }
    va_end(ap);
    if (mcaTraceFile != stderr) {
        va_start(ap, fmt);
        vfprintf(stderr, fmt, ap);
        fputc('\n', stderr);
        fflush(stderr);
        va_end(ap);
    }
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

#ifndef PXT_ESP32
static uint64_t startTime;
#endif

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

void target_yield() {
#ifdef PXT_ESP32
    vTaskDelay(1);
#else
    sleep_core_us(1000);
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
static uint64_t currTime() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000000LL + tv.tv_usec;
}

uint64_t current_time_us() {
    if (!startTime)
        startTime = currTime();
    return currTime() - startTime;
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
    for (;;) {
        // MCA PATCH 5：宿主请求重启 → 退出主循环，交给 initRuntime 末尾的
        // systemReset()（释放 fiber/GC 后 pthread_exit），宿主随后重新 start。
        if (mcaResetRequest) {
            mcaResetRequest = 0;
            mcaTrace("[vm] reset request seen");
            hostRestartCleanup();
            mcaTrace("[vm] cleanup returned, leaving main loop");
            return;   // initRuntime 看到 mcaRestartDone 就不再 systemReset()
        }
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

int allocateNotifyEvent() {
    static volatile int notifyId;
    return ++notifyId;
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

} // namespace pxt
