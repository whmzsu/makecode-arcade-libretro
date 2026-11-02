// 固定地址映射与"跨会话指针重定位" —— 见 mca_fixed.h 的设计说明。
//
// 这里只做两件最基础的事：
//   1) 在指定地址拿一块匿名可读写内存，拿不到就干净地失败；
//   2) 报出本模块（core 自己的 .so/.dll）的加载区间，并按区间做指针重定位。
// 所有"跨进程存档要不要生效"的判断都发生在上层（scheduler.cpp 的快照校验），
// 这里不掺业务逻辑。

#include "pxt.h"
#include "mca_fixed.h"

#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

#if !defined(_WIN32) && !defined(__APPLE__)
#include <stdio.h>
#include <stdlib.h>
#endif

// 已建立的固定区间（用于 mcaIsFixedPtr / 诊断）。GC 堆、镜像、预留位，够用。
#define MCA_FIXED_MAX 8
namespace {
struct FixedRange {
    unsigned long long addr;
    size_t size;
};
FixedRange g_ranges[MCA_FIXED_MAX];
int g_rangeCount = 0;
} // namespace

extern "C" void *mcaFixedMap(unsigned long long addr, size_t size) {
    if (!addr || !size)
        return NULL;

    void *got = NULL;

#if defined(_WIN32)
    // 先按"恰好这个地址"申请；VirtualAlloc 在目标被占时会给别的地址，
    // 这种结果对我们没用（快照里的绝对地址要求分毫不差），直接释放。
    got = VirtualAlloc((void *)(uintptr_t)addr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (got && (unsigned long long)(uintptr_t)got != addr) {
        VirtualFree(got, 0, MEM_RELEASE);
        got = NULL;
    }
#else
#ifndef MAP_FIXED_NOREPLACE
    // glibc 2.17 的头文件里没有（内核 4.17 才引入），值由内核 ABI 固定
    #define MAP_FIXED_NOREPLACE 0x100000
#endif
#ifndef MAP_ANONYMOUS
    #define MAP_ANONYMOUS MAP_ANON
#endif
    void *p = mmap((void *)(uintptr_t)addr, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED) {
        got = NULL;
    } else if ((unsigned long long)(uintptr_t)p != addr) {
        // 老内核不认识 MAP_FIXED_NOREPLACE：当成普通 mmap 给了别的地址。
        // 不能留着（否则内存泄漏且地址没钉住），释放后按失败处理。
        munmap(p, size);
        got = NULL;
    } else {
        got = p;
    }
#endif

    if (!got)
        return NULL;

    if (g_rangeCount < MCA_FIXED_MAX) {
        g_ranges[g_rangeCount].addr = addr;
        g_ranges[g_rangeCount].size = size;
        g_rangeCount++;
    }
    return got;
}

extern "C" int mcaIsFixedPtr(const void *p) {
    unsigned long long v = (unsigned long long)(uintptr_t)p;
    for (int i = 0; i < g_rangeCount; ++i) {
        if (v >= g_ranges[i].addr && v < g_ranges[i].addr + g_ranges[i].size)
            return 1;
    }
    return 0;
}

extern "C" int mcaFixedCount() { return g_rangeCount; }

// ---- 单例专用 bump 区 ----
namespace {
uint8_t *g_bumpBase = NULL;
size_t g_bumpUsed = 0;
} // namespace

extern "C" void *mcaFixedBump(size_t size) {
    if (!g_bumpBase) {
        g_bumpBase = (uint8_t *)mcaFixedMap(
            (unsigned long long)(uintptr_t)GC_BASE + MCA_SINGLETON_OFFSET,
            MCA_SINGLETON_MAX_BYTES);
        if (!g_bumpBase)
            return NULL;
    }
    size = (size + 15) & ~(size_t)15;
    if (g_bumpUsed + size > MCA_SINGLETON_MAX_BYTES)
        return NULL;
    void *p = g_bumpBase + g_bumpUsed;
    g_bumpUsed += size;
    memset(p, 0, size);
    return p;
}

extern "C" void mcaFixedBumpReset() { g_bumpUsed = 0; }

// ---- 本模块（core 的 .so / .dll）的加载区间 -------------------------------
//
// 为什么需要：GC 堆里除了"堆内互指"，还存着**指向本模块代码**的指针
// （RefAction 里编译进内核的回调、fiber 的 wakeFn 等）。动态加载器给模块
// 选的基址每个进程都可能不同 —— 换一次进程，这些指针对应的就是别的代码，
// 恢复后一调用直接段错误（真机上是"加载存档后 RetroArch 崩掉"）。
//
// 所以快照要记下保存时的模块区间，恢复时把落在区间内的值整体平移。
// 区间取"模块全部映射"的最小起点到最大终点：Windows 用 VirtualQuery 顺着
// MEM_IMAGE 段走；Linux/Android 读 /proc/self/maps 按路径归组。
namespace {
unsigned long long g_modBase = 0;
unsigned long long g_modSize = 0;
int g_modTried = 0;

#if defined(_WIN32)

void computeModuleRange() {
    HMODULE h = NULL;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)(const void *)&computeModuleRange, &h) ||
        !h)
        return;
    unsigned long long base = (unsigned long long)(uintptr_t)h;
    unsigned long long end = base;
    uint8_t *p = (uint8_t *)h;
    // PE 映像可能分成多个 MEM_IMAGE 区，一路走到不属于本映像为止
    for (int i = 0; i < 1024; ++i) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi))
            break;
        if (mbi.Type != MEM_IMAGE || (uintptr_t)mbi.AllocationBase != base)
            break;
        unsigned long long next = (unsigned long long)(uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        if (next > end)
            end = next;
        p = (uint8_t *)(uintptr_t)next;
    }
    g_modBase = base;
    g_modSize = (size_t)(end - base);
}

#elif defined(__APPLE__)

// macOS 目前只做编译验证（见 README 平台说明）：拿不到区间就不重定位。
void computeModuleRange() {}

#else

struct MapLine {
    unsigned long long start, end;
    const char *path;
};

// 读 /proc/self/maps 全部内容。失败返回 NULL。
char *readMaps(size_t *outLen) {
    FILE *fp = fopen("/proc/self/maps", "r");
    if (!fp)
        return NULL;
    size_t cap = 64 * 1024, len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) {
        fclose(fp);
        return NULL;
    }
    for (;;) {
        if (len + 4096 > cap) {
            char *nb = (char *)realloc(buf, cap * 2);
            if (!nb) {
                free(buf);
                fclose(fp);
                return NULL;
            }
            buf = nb;
            cap *= 2;
        }
        size_t n = fread(buf + len, 1, cap - len - 1, fp);
        len += n;
        if (n == 0)
            break;
    }
    fclose(fp);
    buf[len] = 0;
    *outLen = len;
    return buf;
}

typedef void (*MapFn)(const MapLine *, void *);

// 遍历 maps 行（就地改写换行符为 0）
void forEachMapLine(char *buf, MapFn fn, void *ctx) {
    char *line = buf;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = 0;
        MapLine ml;
        char *p = line;
        ml.start = strtoull(p, &p, 16);
        if (*p == '-') {
            ml.end = strtoull(p + 1, &p, 16);
            // 跳过 perms / offset / dev / inode 四个字段
            for (int i = 0; i < 4 && *p; ++i) {
                while (*p == ' ')
                    ++p;
                while (*p && *p != ' ')
                    ++p;
            }
            while (*p == ' ')
                ++p;
            ml.path = p;
            fn(&ml, ctx);
        }
        line = nl ? nl + 1 : NULL;
    }
}

struct FindCtx {
    unsigned long long probe;
    const char *path; // 找到后填入"包含 probe 的映射"的路径
    int found;
};

void findContainer(const MapLine *ml, void *ctx) {
    FindCtx *c = (FindCtx *)ctx;
    if (c->found)
        return;
    if (c->probe >= ml->start && c->probe < ml->end) {
        c->path = ml->path;
        c->found = 1;
    }
}

struct ExtentCtx {
    const char *path;
    unsigned long long lo, hi;
    int any;
};

void collectExtent(const MapLine *ml, void *ctx) {
    ExtentCtx *c = (ExtentCtx *)ctx;
    if (strcmp(ml->path, c->path) != 0)
        return;
    if (!c->any || ml->start < c->lo)
        c->lo = ml->start;
    if (!c->any || ml->end > c->hi)
        c->hi = ml->end;
    c->any = 1;
}

void computeModuleRange() {
    size_t len = 0;
    char *buf = readMaps(&len);
    if (!buf)
        return;
    FindCtx fc;
    fc.probe = (unsigned long long)(uintptr_t)(void *)&computeModuleRange;
    fc.path = "";
    fc.found = 0;
    forEachMapLine(buf, findContainer, &fc);
    if (!fc.found) {
        free(buf);
        return;
    }
    ExtentCtx ec;
    ec.path = fc.path;
    ec.lo = ec.hi = 0;
    ec.any = 0;
    forEachMapLine(buf, collectExtent, &ec);
    if (ec.any && ec.hi > ec.lo) {
        g_modBase = ec.lo;
        g_modSize = ec.hi - ec.lo;
    }
    free(buf);
}

#endif
} // namespace

extern "C" unsigned long long mcaModuleBase() {
    if (!g_modTried) {
        g_modTried = 1;
        computeModuleRange();
    }
    return g_modBase;
}

extern "C" unsigned long long mcaModuleSize() {
    if (!g_modTried) {
        g_modTried = 1;
        computeModuleRange();
    }
    return g_modSize;
}

extern "C" int mcaRelocateModulePtrs(void *p, size_t bytes, unsigned long long oldBase,
                                     unsigned long long oldSize) {
    if (!p || !bytes || !oldBase || !oldSize)
        return 0;
    unsigned long long curBase = mcaModuleBase();
    // 基址一样（同一进程、或加载器恰好选了同一地址）就不用动
    if (!curBase || curBase == oldBase)
        return 0;
    const long long delta = (long long)(curBase - oldBase);
    const unsigned long long oldEnd = oldBase + oldSize;

    // 按指针宽度对齐扫描：模块里的代码/数据指针都是对齐过的
    uintptr_t start = ((uintptr_t)p + sizeof(uintptr_t) - 1) & ~(uintptr_t)(sizeof(uintptr_t) - 1);
    uintptr_t stop = (uintptr_t)p + bytes;
    if (start >= stop)
        return 0;
    const size_t n = (size_t)((stop - start) / sizeof(uintptr_t));
    uintptr_t *q = (uintptr_t *)start;
    int hits = 0;
    for (size_t i = 0; i < n; ++i) {
        unsigned long long v = (unsigned long long)q[i];
        if (v >= oldBase && v < oldEnd) {
            q[i] = (uintptr_t)(v + (unsigned long long)delta);
            hits++;
        }
    }
    return hits;
}
