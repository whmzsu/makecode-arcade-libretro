#include "pxt.h"
#include "mca_fixed.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

// MCA PATCH 3：VM 线程栈大小（见 spinThread 里的说明）
#ifndef MCA_VM_STACK_SIZE
#define MCA_VM_STACK_SIZE (8 * 1024 * 1024)
#endif

namespace pxt {

// MCA PATCH 5：定义在 scheduler.cpp（VM 主循环与 systemReset 都在那边）
extern volatile int mcaResetRequest;
extern volatile int mcaVmExited;
// MCA v0.2.10：主循环是否已停在"等前端要帧"的闸门上（定义在 scheduler.cpp）
extern volatile int mcaMainLoopLive;
// MCA PATCH 10：随机序列 = f(镜像哈希)（定义在 platform.cpp）
void seedRandomFromImage();
// MCA PATCH 6：重启路径埋点（定义在 scheduler.cpp）
void mcaTrace(const char *fmt, ...);

VMImage *vmImg;

// MCA v0.2.11：卡带镜像也钉在固定地址。
//
// img->dataStart 直接指向这块缓冲，而 GC 堆里的字符串字面量、函数引用、
// VTable 指针全是指向它的绝对地址 —— 镜像换地址就等于所有对象内容失效。
// 和 GC 堆同理：拿不到固定地址就退回普通分配（游戏照跑，跨会话存档退化）。
static uint8_t *g_imgBuf = NULL;

// MCA v0.2.13：卡带指纹必须算在**未打补丁**的原始镜像字节上。
//
// vm_patch_image() 会把 vtable 指针和内核函数指针（都指向 core 自己的模块）
// 就地写回镜像。Linux/Android 的 .so 走 ASLR，换个进程这些地址就变 —— 之前
// 指纹是拿 patch 之后的 dataStart..dataEnd 现算的，于是"同一个卡带、换个
// 进程"得到不同指纹，跨会话读档被当成"换了卡带"直接拒绝（-10）。Windows 的
// DLL 基址固定，所以桌面回归一直没暴露这条。真机日志：
//   [snap] 拒绝恢复：卡带不同（快照 181608 字节/1064f705，当前 181608 字节/e2483410）
static uint32_t g_imgHash;
static uint32_t g_imgBytes;

static uint32_t mcaFnv1a(const uint8_t *b, uint32_t n) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; ++i)
        h = (h ^ b[i]) * 16777619u;
    return h;
}

uint32_t mca_image_hash() { return g_imgHash; }
uint32_t mca_image_bytes() { return g_imgBytes; }

static uint8_t *mcaImageBuffer(unsigned len) {
    if (!g_imgBuf) {
        g_imgBuf = (uint8_t *)mcaFixedMap((unsigned long long)(uintptr_t)GC_BASE + MCA_IMG_OFFSET,
                                         MCA_IMG_MAX_BYTES);
        if (g_imgBuf)
            mcaTrace("[img] 镜像缓冲固定在 %p（%u 字节上限）", (void *)g_imgBuf,
                     (unsigned)MCA_IMG_MAX_BYTES);
        else
            mcaTrace("[img] 取不到固定镜像地址（GC_BASE+1MB），跨会话快照将不可用");
    }
    if (g_imgBuf && len <= MCA_IMG_MAX_BYTES)
        return g_imgBuf;
    return NULL;
}

static void vmStartCore(uint8_t *data, unsigned len) {
    // 先拷进固定缓冲再解析：loadVMImage 会把 data 指针直接存进 img->dataStart，
    // 之后所有代码都按这个地址引用镜像内容。
    if (data != g_imgBuf) {
        uint8_t *fixed = mcaImageBuffer(len);
        if (fixed) {
            memcpy(fixed, data, len);
            data = fixed;
        }
    }

    // 指纹取 patch 之前的原始字节（理由见 g_imgHash 的说明）。放在这里而不是
    // 装载完之后：vm_patch_image 下面就会改写 data。
    g_imgBytes = len;
    g_imgHash = mcaFnv1a(data, len);

    mcaTrace("[vm] start: unload image");
    unloadVMImage(vmImg);
    vmImg = NULL;

    gcPreStartup();

    auto state = vm_alloc_patch_state();
    vm_patch_image(state, data, len);
    vm_finish_patch(state);

    auto img = loadVMImage(data, len);
    if (img->errorCode) {
        dmesg("validation error %d at 0x%x", img->errorCode, img->errorOffset);
        return;
    } else {
        dmesg("Validation OK");
    }
    vmImg = img;

    gcStartup();
    // MCA PATCH 7：镜像装载完成，前端可以重新拉画面/音频了。
    // （此刻 instWDisplay 还是 NULL，拉取会得到黑屏，安全。）
    pxt_vm_set_online(1);
    mcaTrace("[vm] image loaded, gcStartup done (online), img %u bytes/%08x",
             g_imgBytes, g_imgHash);

    // MCA PATCH 10：随机序列 = f(镜像哈希)。必须在此处（游戏代码运行前）
    // 播种：settings 模块和游戏启动代码都会用 Math.random。
    seedRandomFromImage();

    globals = (TValue *)app_alloc(sizeof(TValue) * getNumGlobals());
    memset(globals, 0, sizeof(TValue) * getNumGlobals());
    mcaTrace("[vm] globals allocated, entering initRuntime");

    initRuntime();
    mcaTrace("[vm] initRuntime returned");
}

static void vmStartFile(const char *fn) {
    auto f = fopen(fn, "rb");
    if (!f) {
        dmesg("cannot open %s", fn);
        return;
    }

    fseek(f, 0, SEEK_END);
    auto len = (unsigned)ftell(f);
    fseek(f, 0, SEEK_SET);
    // 优先直接读进固定缓冲（省一次拷贝，且地址从一开始就钉住）
    auto data = mcaImageBuffer(len);
    if (!data)
        data = (uint8_t *)malloc(len + 16);
    fread(data, len, 1, f);
    fclose(f);

    vmStartCore(data, len);
}

static uint8_t *vm_data;
static unsigned vm_len;
static const char *vm_filename;

static void *multiStart(void *) {
    if (vm_filename)
        vmStartFile(vm_filename);
    else
        vmStartCore(vm_data, vm_len);
    // MCA PATCH 7：线程要走了，先把输出通道关掉，免得前端在已释放的
    // 堆/单例上取帧（清理路径已经在 resetRuntimeState 里关过，这里是
    // panic 直接退出的情况兜底）。
    mcaMainLoopLive = 0;
    pxt_vm_set_online(0);
    // MCA PATCH 6：真正的线程退出点才置位。之前是在清理中途置位，宿主可能
    // 提前 pthread_join 一个还在跑清理的线程 —— 真机上就是"卡住不动"。
    mcaVmExited = 1;
    mcaTrace("[vm] thread exit");
    return NULL;
}

void vmStart() {
    auto fn = pxt::initialArgv[1];
    vmStartFile(fn);
}

pthread_t vm_thread;
int vm_has_thread;

static void spinThread() {
    if (vm_has_thread) {
        void *dummy;
        if (!panicCode)
            panicCode = -1;
        mcaTrace("[host] join old vm thread");
        pthread_join(vm_thread, &dummy);
        mcaTrace("[host] join done");
        vm_has_thread = 0;
    }
    panicCode = 0;
    // MCA PATCH 3（本项目）：显式指定 VM 线程栈大小。
    // pthread_create 的 attr 为 NULL 时，线程栈取的是**宿主可执行文件**的
    // PE 栈预留：官方 sdlmain.exe 大而无碍，但 libretro 前端（RetroArch.exe
    // 只有 2MB）会让 VM 在 initRuntime 阶段栈溢出 —— Windows 上表现为进程
    // 无声消失、无任何日志。固定 8MB 与前端无关。
    pthread_attr_t vm_attr;
    pthread_attr_init(&vm_attr);
    pthread_attr_setstacksize(&vm_attr, MCA_VM_STACK_SIZE);
    pthread_create(&vm_thread, &vm_attr, multiStart, NULL);
    pthread_attr_destroy(&vm_attr);
    vm_has_thread = 1;
    mcaTrace("[host] new vm thread created");

    // MCA v0.2.10：等 VM 真正就绪（镜像装载完 + 主循环停在取帧闸门上）再返回，
    // 也就是让 retro_load_game 变成同步的。
    //
    // 不等的话，起步阶段前端会白拉若干帧黑屏：那些帧不产生 VM 圈数，于是
    // 「前端第 N 帧」与「VM 第 N 圈」之间留下一个**由墙钟决定**的偏移
    // （实测 12 帧只跑了 6~8 圈，两次运行还不一样）—— 同一输入、同一卡带，
    // 末帧画面却不同，存档/rewind/录像比对全部失效。
    //
    // 判据是 mcaMainLoopLive（initRuntime 里游戏启动代码已跑完、显示已建好、
    // 主循环正准备接帧），不是单纯的 online（online 在 gcStartup 之后就置位，
    // 那时游戏还没初始化，第一帧会退回黑屏）。
    int waited = 0;
    for (; waited < 20000; ++waited) {
        if ((pxt_vm_online() && mcaMainLoopLive) || mcaVmExited || panicCode)
            break;
#if defined(_WIN32)
        Sleep(1);
#else
        usleep(1000);
#endif
    }
    mcaTrace("[host] vm ready after %dms (online=%d live=%d)", waited, (int)pxt_vm_online(),
             (int)mcaMainLoopLive);
}

static void *startFromUserWorker(void *fn) {
    void *dummy;
    pthread_join(vm_thread, &dummy);
    vm_thread = pthread_self();
    panicCode = 0;
    vmStartFile((char *)fn);
    return NULL;
}

static const char *lastFN;
void vmStartFromUser(const char *fn) {
    pthread_t pt;
    if (!fn && lastFN) {
        dmesg("re-starting %s", lastFN);
        fn = lastFN;
    }
    lastFN = fn;
    if (fn)
        pthread_create(&pt, NULL, startFromUserWorker, (void *)fn);
    systemReset();
}

DLLEXPORT void pxt_vm_start(const char *fn) {
    vm_filename = fn;
    spinThread();
}

DLLEXPORT void pxt_vm_start_buffer(uint8_t *data, unsigned len) {
    vm_filename = NULL;
    vm_data = data;
    vm_len = len;
    spinThread();
}

// MCA PATCH 5（本项目）：宿主侧的"重启游戏"接口。
//
// 用法（宿主必须严格按顺序）：
//   1) pxt_vm_request_reset()          置标志，VM 线程在下一轮主循环退出
//   2) 轮询 pxt_vm_poll_exit()         返回 1 表示 VM 线程已退出（线程已 join 安全）
//   3) 用**未打补丁的原始镜像**重新 pxt_vm_start_buffer(data, len)
// 镜像会被 VM 就地打补丁（vm_patch_image），所以第 3 步必须换一份干净的副本，
// 不能复用第一次传进来的缓冲区。
DLLEXPORT void pxt_vm_request_reset() {
    mcaResetRequest = 1;
}

DLLEXPORT int pxt_vm_poll_exit() {
    if (!mcaVmExited)
        return 0;
    mcaVmExited = 0;
    return 1;
}

DLLEXPORT int pxt_vm_running() {
    return vm_has_thread && !mcaVmExited;
}

// MCA PATCH 8（本项目）：彻底停掉 VM 线程。
//
// 前端（RetroArch）换游戏 / 关闭核心的顺序是：
//     retro_unload_game() → retro_deinit() → dlclose(核心库)
// 我们的 VM 在独立线程里跑，如果宿主什么都不做，dlclose 之后 VM 线程还在
// 执行已被 unmap 的代码段 → 段错误，前端直接崩退。
// 所以这里同步地：置重启标志让主循环走到清理 → 等线程退出 → pthread_join
// → 释放镜像。清理路径已经把输出通道置离线，前端此刻不取帧也不会拖住 VM
//（WDisplay::update 的取帧等待有 50ms 上限）。
DLLEXPORT int pxt_vm_stop() {
    if (!vm_has_thread)
        return 1;

    mcaTrace("[host] stop requested");
    pxt_vm_set_online(0); // 前端立刻拿黑屏/静音，不再碰 VM 状态
    mcaResetRequest = 1;

    // 最多等 3 秒：正常几十毫秒（清理 + 一次取帧超时）
    for (int i = 0; i < 3000 && !mcaVmExited; ++i) {
#if defined(_WIN32)
        Sleep(1);
#else
        usleep(1000);
#endif
    }
    if (!mcaVmExited) {
        mcaTrace("[host] stop TIMEOUT (vm thread still alive)");
        return 0;
    }

    void *dummy;
    pthread_join(vm_thread, &dummy);
    vm_has_thread = 0;
    mcaVmExited = 0;
    mcaResetRequest = 0;

    // 线程已退出，镜像（含 malloc 的启动缓冲和 VM 运算栈）由这里负责释放；
    // 不释放的话下次启动会重新 malloc，而这块永远不会回收。
    if (vmImg) {
        unloadVMImage(vmImg);
        vmImg = NULL;
    }
    // 兜底：panic 直接退出线程时清理序列没跑完，这里把输出通道与单例清干净，
    // 免得下一次 load_game（换游戏）拿到指向已释放堆的指针。
    pxt_screen_shutdown();
    pxt_audio_shutdown();
    pxt_music_shutdown();
    mcaTrace("[host] vm stopped (thread joined, image freed)");
    return 1;
}

} // namespace pxt
