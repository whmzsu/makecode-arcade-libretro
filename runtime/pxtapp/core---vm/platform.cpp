#include "pxt.h"

#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>

#ifdef _WIN32
#include <windows.h>
#endif

namespace pxt {

static void initRandomSeed() {
    int seed = 0xC0DA1;
#ifdef _WIN32
    // MCA PATCH 4（本项目）：Windows 没有 /dev/urandom。原实现在 open() 返回
    // -1 之后仍然对 fd=-1 调 read()，UCRT 会判定"无效参数"并走
    // _invoke_watson → __fastfail(0xC0000409)：进程**立刻**消失，不产生
    // 任何日志、栈回溯或 WER 记录（libretro 前端里就表现为"RetroArch 直接
    // 崩了，啥信息都没有"）。这里改用系统时间 + 栈地址做种子。
    seed ^= (int)GetTickCount64();
    seed ^= (int)(uintptr_t)&seed;
#else
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        read(fd, &seed, sizeof(seed));
        close(fd);
    }
#endif
    seedRandom(seed);
}

void sendSerial(const char *data, int len) {
    /*
    if (!serial) {
        serial = new codal::_mbed::Serial(USBTX, NC);
        serial->baud(9600);
    }
    serial->send((uint8_t*)data, len);
    */
}

extern "C" void drawPanic(int code)
{
    // TODO
}


extern "C" void target_init()
{
    initRandomSeed();
}

void updateScreen(Image_ img);

void screen_init() {
    updateScreen(NULL);
}

}

