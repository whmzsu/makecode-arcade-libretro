/*
 * decompress.c —— LZMA-alone 解压
 *
 * 两处在用：cart.c（PNG 卡带里的工程 blob）、compiler.c（内嵌的 pxt 编译器载荷）。
 * 上限 64MB：卡带 blob 与 pxt_hack.js/target.json 都在 9MB 以内，留足余量。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "decompress.h"
#include "LzmaDec.h"

#define MAX_OUT (64u << 20)

static void set_err(char *err, size_t cap, const char *fmt, ...)
{
    if (!err || cap == 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
}

static void *lzma_alloc(ISzAllocPtr p, size_t size) { (void)p; return malloc(size); }
static void lzma_free(ISzAllocPtr p, void *addr) { (void)p; free(addr); }

int arc_lzma_alone(const uint8_t *src, size_t src_len,
                   uint8_t **out, size_t *out_len,
                   char *err, size_t errcap)
{
    if (!src || src_len < 14) {
        set_err(err, errcap, "lzma: too short");
        return -1;
    }

    uint8_t props[5];
    memcpy(props, src, 5);

    /* .lzma 头：5 字节属性 + 8 字节解压后长度（可能 0xFFFFFFFFFFFFFFFF 表示未知） */
    uint64_t declared = 0;
    for (int i = 0; i < 8; i++)
        declared |= (uint64_t)src[5 + i] << (8 * i);
    int has_size = (declared != 0 && declared != (uint64_t)-1 && declared <= MAX_OUT);

    const uint8_t *data = src + 13;
    size_t data_len = src_len - 13;

    ISzAlloc alloc;
    alloc.Alloc = lzma_alloc;
    alloc.Free = lzma_free;

    size_t cap = has_size ? (size_t)declared : (size_t)(data_len * 6 + 65536);
    if (cap < 65536) cap = 65536;
    if (cap > MAX_OUT) cap = MAX_OUT;

    for (;;) {
        uint8_t *dst = (uint8_t *)malloc(cap + 1);
        if (!dst) {
            set_err(err, errcap, "lzma: oom");
            return -1;
        }
        SizeT dst_len = (SizeT)cap;
        SizeT src_used = (SizeT)data_len;
        ELzmaStatus status;
        SRes res = LzmaDecode(dst, &dst_len, data, &src_used, props, 5,
                              LZMA_FINISH_END, &status, &alloc);
        if (res == SZ_OK) {
            dst[dst_len] = 0;
            *out = dst;
            *out_len = (size_t)dst_len;
            return 0;
        }
        free(dst);
        /*
         * 长度字段未知（0xFF..FF）的流没法预分配：解到一半缓冲不够时
         * liblzma 既可能报 OUTPUT_EOF，也可能直接报 DATA，所以只要还没到上限
         * 就一律翻倍重试。pxt 自己导出的卡带会写真实长度，但别的工具
         * （比如 python lzma.compress(FORMAT_ALONE)）常写未知长度。
         */
        if (!has_size && cap < MAX_OUT) { cap *= 2; continue; }
        if (res == SZ_ERROR_OUTPUT_EOF && cap < MAX_OUT) { cap *= 2; continue; }
        set_err(err, errcap, "lzma: decode failed (%d)", (int)res);
        return -1;
    }
}
