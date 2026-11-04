#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(void) {
    const size_t N = 16u << 20;
    const int LOOPS = 40;
    char *a = malloc(N), *b = malloc(N);
    if (!a || !b) { puts("alloc fail"); return 1; }
    memset(a, 1, N); memset(b, 2, N);

    volatile unsigned long sink = 0;
    double t0 = now();
    for (int i = 0; i < LOOPS; i++) {
        memcpy(b, a, N);
        __asm__ __volatile__("" ::: "memory");   /* 阻止把循环提到外面 */
        sink += (unsigned char)b[i];
    }
    double t1 = now();
    double gbs = (double)N * LOOPS * 2 / (t1 - t0) / 1e9;

    const size_t M = 4u << 20;
    size_t *tab = (size_t *)a;
    size_t slots = M / sizeof(size_t);
    for (size_t i = 0; i < slots; i++) tab[i] = (i + 1023) % slots;
    size_t idx = 0;
    const long ITER = 4000000;
    double t2 = now();
    for (long i = 0; i < ITER; i++) idx = tab[idx];
    double t3 = now();
    double lat = (t3 - t2) / ITER * 1e9;

    printf("memcpy %.2f GB/s | 随机访存延迟 %.1f ns | %lu\n", gbs, lat, sink + idx);
    free(a); free(b);
    return 0;
}
