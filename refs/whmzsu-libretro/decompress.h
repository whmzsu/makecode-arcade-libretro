/*
 * decompress.h —— LZMA-alone 解压（卡带隐写 blob 与内嵌编译器载荷共用）
 */
#ifndef ARCADE_DECOMPRESS_H
#define ARCADE_DECOMPRESS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif
#include <stdint.h>

/*
 * 解压 .lzma（5 字节属性 + 8 字节长度 + 数据）。
 * 成功返回 0，*out 由 malloc 分配、末尾补 '\0'，长度写进 *out_len。
 * 失败返回非 0，原因写进 err（可为 NULL）。
 *
 * 也接受“长度字段未知（0xFF..）”的流：解到缓冲不够时会翻倍重试。
 */
int arc_lzma_alone(const uint8_t *src, size_t src_len,
                   uint8_t **out, size_t *out_len,
                   char *err, size_t errcap);

#ifdef __cplusplus
}
#endif

#endif
