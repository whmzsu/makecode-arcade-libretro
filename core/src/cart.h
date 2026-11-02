/*
 * cart.h —— MakeCode Arcade 卡带（.png）解析
 */
#ifndef ARCADE_CART_H
#define ARCADE_CART_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /*
     * 整个工程的源码表：JSON 对象 {"main.ts": "...", "images.g.ts": "...", ...}。
     * 不能只取 main.ts——生成资源（.g.ts / .jres）和它是一起编译的，
     * 只给 main.ts 会缺 assets/helpers/myTiles 这些符号。
     * 少数老卡带的 source 是裸源码，此时 is_source_json = 0。
     */
    char *source;
    size_t source_len;
    int source_is_json;
    char error[256];    /* 失败原因 */
} cart_t;

/* 从 PNG 卡带里取出工程源码。返回 0 成功，非 0 失败（error 里有原因）。 */
int cart_load_png(const unsigned char *buf, size_t len, cart_t *out);

void cart_free(cart_t *c);

#ifdef __cplusplus
}
#endif

#endif
