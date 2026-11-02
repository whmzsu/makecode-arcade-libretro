#include "pxt.h"

#if !defined(_MSC_VER)
#include <sys/time.h> // gettimeofday：取帧等待的绝对超时（MCA PATCH 7）
#endif

namespace pxt {
class WDisplay {
  public:
    uint32_t currPalette[16];
    bool newPalette, dataWaiting;
    uint8_t *screenBuf;

    int width, height;

    WDisplay();
    void updateLoop();
    void update(Image_ img);
};

SINGLETON(WDisplay);

WDisplay::WDisplay() {
    width = getConfig(CFG_DISPLAY_WIDTH, 160);
    height = getConfig(CFG_DISPLAY_HEIGHT, 128);
    DMESG("init display: %dx%d", width, height);
    screenBuf = new uint8_t[width * height / 2 + 20];
    newPalette = false;
}

//% expose
int setScreenBrightnessSupported() {
    return 0;
}

//% expose
void setScreenBrightness(int level) {
    // TODO
}

//% expose
void setPalette(Buffer buf) {
    auto display = getWDisplay();
    if (48 != buf->length)
        target_panic(PANIC_SCREEN_ERROR);
    for (int i = 0; i < 16; ++i) {
        uint8_t r = buf->data[i * 3];
        uint8_t g = buf->data[i * 3 + 1];
        uint8_t b = buf->data[i * 3 + 2];
        display->currPalette[i] = (0xff << 24) | (r << 16) | (g << 8) | (b << 0);
    }
    display->newPalette = true;
}

static pthread_mutex_t screenMutex;
static pthread_cond_t dataBroadcast;
static int numGetPixels;

static void fill_screen(uint32_t *screen, int width, int height, uint32_t color) {
    const int n = width * height;
    for (int i = 0; i < n; ++i)
        screen[i] = color;
}

// MCA PATCH 7：这个函数在**前端线程**上执行，三条硬规则：
//   1) 绝不 target_panic。VM 版的 target_panic → systemReset() → pthread_exit，
//      在前端线程上等于把整个前端注销掉（真机上就是"重置后挂起"）。
//   2) 先拿锁再读 instWDisplay。shutdown 在锁内把它置空，否则会 use-after-free。
//   3) 等待必须有上限。上游写的是 `pthread_cond_timedwait(&c,&m,&{0,100ms})`，
//      但 timespec 是**绝对**时间，1970 年那一刻早已过去 → 永远返回 EINVAL，
//      实际等于不等待；这里改成真正的 16ms 绝对超时（一帧的预算）。
DLLEXPORT void pxt_screen_get_pixels(int width, int height, uint32_t *screen) {
    if (!pxt_vm_online()) {
        fill_screen(screen, width, height, 0xff000000);
        return;
    }

    pthread_mutex_lock(&screenMutex);

    auto disp = instWDisplay;
    if (!disp || !pxt_vm_online()) {
        pthread_mutex_unlock(&screenMutex);
        fill_screen(screen, width, height, 0xff000000);
        return;
    }
    numGetPixels++;

    if (!disp->dataWaiting) {
        struct timeval tv;
        struct timespec deadline;
        gettimeofday(&tv, NULL);
        deadline.tv_sec = tv.tv_sec;
        deadline.tv_nsec = tv.tv_usec * 1000L + 16 * 1000 * 1000L;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&dataBroadcast, &screenMutex, &deadline);
    }

    // 等待期间可能已经被 shutdown（重置）：disp 已失效，不能再碰
    if (!pxt_vm_online() || disp != instWDisplay) {
        pthread_mutex_unlock(&screenMutex);
        fill_screen(screen, width, height, 0xff000000);
        return;
    }

    if (width != disp->width || height != disp->height) {
        // 尺寸不符是镜像配置问题，蓝屏让玩家/开发者看得见，但不要杀前端
        pthread_mutex_unlock(&screenMutex);
        fill_screen(screen, width, height, 0xff0000ff);
        return;
    }

    if (panicCode > 0) {
        int n = width * height;
        uint32_t *p = screen;
        // blue screen
        while (n--)
            *p++ = 0xff0000ff;
    } else {
        auto sp = disp->screenBuf;
        auto pal = disp->currPalette;
        for (int x = 0; x < width; ++x) {
            uint32_t *p = screen + x;
            for (int y = 0; y < (height >> 1); ++y) {
                uint8_t v = *sp++;
                *p = pal[v & 0xf];
                p += width;
                *p = pal[v >> 4];
                p += width;
            }
        }
    }
    pthread_cond_broadcast(&dataBroadcast);
    disp->dataWaiting = false;
    pthread_mutex_unlock(&screenMutex);
}

// MCA PATCH 7：重置时丢掉屏幕的静态状态。必须在 gcReset() 之前调用 ——
// instWDisplay / screenBuf 都活在 VM 堆上，清理完就是野指针。
DLLEXPORT void pxt_screen_shutdown() {
    pthread_mutex_lock(&screenMutex);
    instWDisplay = NULL;
    numGetPixels = 0;
    pthread_cond_broadcast(&dataBroadcast); // 叫醒可能正在等取帧的线程
    pthread_mutex_unlock(&screenMutex);
}

void WDisplay::update(Image_ img) {
    if (!img)
        return;

    if (img->bpp() != 4 || img->width() != width || img->height() != height)
        target_panic(PANIC_SCREEN_ERROR);

    pthread_mutex_lock(&screenMutex);
    // if the data have not been picked up, but it had been in the past, wait
    //
    // MCA PATCH 7：上游这里是**无超时**的 pthread_cond_wait。前端一旦不取帧
    //（重置期间、前端暂停、窗口最小化），VM 线程就永远停在这里，连
    // mcaResetRequest 都读不到 —— 真机上正是"按重置后画面冻住、不崩不退"。
    // 改成最多等 50ms（约 3 帧），超时就丢掉这一帧继续跑。
    if (dataWaiting && numGetPixels) {
        struct timeval tv;
        struct timespec deadline;
        gettimeofday(&tv, NULL);
        deadline.tv_sec = tv.tv_sec;
        deadline.tv_nsec = tv.tv_usec * 1000L + 50 * 1000 * 1000L;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&dataBroadcast, &screenMutex, &deadline);
    }
    if (!pxt_vm_online()) {
        pthread_mutex_unlock(&screenMutex);
        return;
    }
    memcpy(screenBuf, img->pix(), img->pixLength());
    dataWaiting = true;
    pthread_cond_broadcast(&dataBroadcast);
    pthread_mutex_unlock(&screenMutex);

    if (newPalette) {
        newPalette = false;
    }
}

//% expose
void updateScreen(Image_ img) {
    getWDisplay()->update(img);
}

//% expose
void updateStats(String msg) {
    DMESG("stats: %s", msg->getUTF8Data());
}
} // namespace pxt