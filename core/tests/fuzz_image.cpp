// fuzz_image.cpp — libFuzzer 目标：pxt64 镜像的解析/校验层
//
// 为什么挑这一层：`loadVMImage()` 是唯一"外部字节进来就立刻被解释"的地方——
// 段表、vtable、字面量表、iface 名字、函数校验全在这几百行里，且**没有 VM 线程**，
// 单进程可反复调用，最适合 fuzz。今天这条 PATCH 11（ALLOC_ARRAY 少分配 7 字节）
// 就是在这层被 ASAN 抓到的：一张正常卡带走到 loadIfaceNames 就踩了。
//
// 不做的事：不跑 vm_patch_image、不起线程、不执行 game 代码。故意如此——
// 执行层一崩就分不清"坏输入被优雅拒绝"还是"我们越界"，噪声太大。
//
// 用法见 tools/fuzz_pxt.sh；崩溃样本落在 out/fuzz/crash-*.pxt64，直接当回归用例。

#include "pxt.h"   // 必须先于 vm.h：vm.h 用 uint64_t/TValue 但不自己 include 头
#include "vm.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    // 真实卡带 100KB~2MB；限制上限免得 fuzzer 把时间花在超大输入上
    if (size < 64 || size > (4u << 20))
        return 0;

    // loadVMImage 要求 data 与 length 都 8 字节对齐（见 CHECK_AT 1000/1001）
    size_t n = size & ~(size_t)7;
    void *buf = calloc(1, n);
    if (!buf)
        return 0;
    memcpy(buf, data, n);

    pxt::VMImage *img = pxt::loadVMImage(buf, (unsigned)n);
    if (img) {
        // 正常路径 dataStart 指向 buf，unloadVMImage 会 free 它；
        // 校验在设置 dataStart 之前就失败的，buf 得自己收（否则每轮漏 4MB）
        bool owned = (img->dataStart == (uint64_t *)buf);
        pxt::unloadVMImage(img);
        if (!owned)
            free(buf);
    } else {
        free(buf);
    }
    return 0;
}
