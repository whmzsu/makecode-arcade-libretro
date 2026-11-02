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

// ---- /proc/self/maps 文本解析（平台无关，可自测） -------------------------
//
// v0.2.14 修的真机 bug：这一段原来是"就地改写缓冲区"的（把每行的 '\n' 换成
// 0 来切行），于是同一份 maps 只能遍历一次 —— 而算区间天生要两遍：第一遍
// 找出"本模块的路径"，第二遍按路径把所有映射段聚合成 [lo, hi)。
//
// 真机日志正是用第二遍的报废缓冲算出来的：
//     [snap] 拒绝恢复：本平台取不到模块区间，跨会话恢复不安全
//     [snap] module relocate: old=0 size=0 cur=0 hits=0+0
//   → 第二遍 buf 里只剩第一行（vdso/vvar），一个路径都匹配不上，g_modBase 恒 0。
//   → 快照里的 modBase/modSize 全是 0，跨会话恢复被拒；同会话虽能读，
//     但一旦真跨进程就无从重定位。
//
// 现在改成**只读**解析：只接受 const char*，逐行用指针推进，路径拷到调用方
// 缓冲里。想看几遍看几遍，同一份文本重复解析结果必然一致（自测里就断言这点）。
struct McaMapLine {
    unsigned long long start, end;
    const char *path; // 指向 text 内部，长度为 pathLen（不含行尾）
    size_t pathLen;
};

// 解析 p 处的一行，返回下一行起点（NULL = 到末尾）。格式：
//   start-end perms offset dev inode  pathname
const char *mcaMapParseLine(const char *p, McaMapLine *out) {
    out->start = out->end = 0;
    out->path = p;
    out->pathLen = 0;
    if (!p || !*p)
        return NULL;

    const char *nl = strchr(p, '\n');
    const char *next = nl ? nl + 1 : NULL;

    const char *q = p;
    char *endp = NULL;
    out->start = strtoull(q, &endp, 16);
    if (!endp || *endp != '-')
        return next; // 不是映射行（空行/异常），跳过
    out->end = strtoull(endp + 1, &endp, 16);

    // 跳过 perms / offset / dev / inode 四个字段
    for (int i = 0; i < 4 && *endp && *endp != '\n'; ++i) {
        while (*endp == ' ')
            ++endp;
        while (*endp && *endp != ' ' && *endp != '\n')
            ++endp;
    }
    while (*endp == ' ')
        ++endp;
    out->path = endp;
    const char *e = endp;
    while (*e && *e != '\n' && *e != '\r')
        ++e;
    out->pathLen = (size_t)(e - endp);
    return next;
}

// probe 落在哪条映射里，就把"同路径的全部映射段"合成 [lo, hi)。
// 返回 1 = 成功，0 = 没找到（probe 不在任何映射中 / 路径为空）。
int mcaMapsProbe(const char *text, unsigned long long probe, unsigned long long *outLo,
                 unsigned long long *outHi) {
    if (!text || !outLo || !outHi)
        return 0;

    // 第一遍：找 probe 所在映射的路径（拷出来，第二遍要用）
    char want[512];
    want[0] = 0;
    for (const char *p = text; p;) {
        McaMapLine ml;
        const char *next = mcaMapParseLine(p, &ml);
        if (ml.pathLen && probe >= ml.start && probe < ml.end) {
            size_t n = ml.pathLen < sizeof(want) - 1 ? ml.pathLen : sizeof(want) - 1;
            memcpy(want, ml.path, n);
            want[n] = 0;
            break;
        }
        p = next;
    }
    if (!want[0])
        return 0;

    // 第二遍：按路径聚合所有段（.text / .rodata / .data 通常是三条独立映射）
    unsigned long long lo = 0, hi = 0;
    int any = 0;
    const size_t wantLen = strlen(want);
    for (const char *p = text; p;) {
        McaMapLine ml;
        const char *next = mcaMapParseLine(p, &ml);
        if (ml.pathLen == wantLen && memcmp(ml.path, want, wantLen) == 0) {
            if (!any || ml.start < lo)
                lo = ml.start;
            if (!any || ml.end > hi)
                hi = ml.end;
            any = 1;
        }
        p = next;
    }
    if (!any || hi <= lo)
        return 0;

    *outLo = lo;
    *outHi = hi;
    return 1;
}

#if !defined(_WIN32) && !defined(__APPLE__)
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
#endif

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

void computeModuleRange() {
    size_t len = 0;
    char *buf = readMaps(&len);
    if (!buf)
        return;
    // probe 用**数据符号**的地址（&g_modTried），不用函数地址：aarch64 -fPIC 下
    // 取函数地址可能拿到 PLT 跳板（虽然仍落在本模块内），数据符号没有这层
    // 间接性，落在哪儿都在 .bss 里，语义最直接。
    unsigned long long lo = 0, hi = 0;
    const int ok = mcaMapsProbe(buf, (unsigned long long)(uintptr_t)&g_modTried, &lo, &hi);
    free(buf);
    if (ok && hi > lo) {
        g_modBase = lo;
        g_modSize = hi - lo;
    }
}

#endif
} // namespace

// 只算一次。算完打一行 trace：跨会话读档全靠这个区间，日志里看一眼就知道
// 本平台到底取没取到（0 就是取不到 —— Linux/Android 上曾经恒为 0）。
static void mcaModuleEnsure() {
    if (g_modTried)
        return;
    g_modTried = 1;
    computeModuleRange();
    mcaTrace("[mod] module range base=%llx size=%llu (%s)", g_modBase, g_modSize,
             g_modBase ? "ok" : "UNAVAILABLE");
}

extern "C" MCA_TEST_EXPORT unsigned long long mcaModuleBase() {
    mcaModuleEnsure();
    return g_modBase;
}

extern "C" MCA_TEST_EXPORT unsigned long long mcaModuleSize() {
    mcaModuleEnsure();
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

// ---- 自测：/proc/self/maps 文本解析（v0.2.14） ----------------------------
//
// 真机上出过"g_modBase 恒为 0"的事故（见文件里 mcaMapsProbe 的注释），根因是
// 解析器就地改写缓冲区、第二遍遍历报废。桌面走 Windows 的 VirtualQuery 分支，
// 这段逻辑压根没被执行过 —— 所以必须有一条**不依赖平台**的回归。
//
// 样本是内嵌的，不读 /proc，任何平台都能跑；tools/test_modrange.py 通过
// ctypes 调本函数。顺带它也会断言真实分支（Linux 读 maps / Windows 走
// VirtualQuery）取到了非零区间。
static const char *kModSelfTestMaps =
    "5500000000-5500012000 r-xp 00000000 fd:01 4711  /data/cores/makecode_arcade_libretro.so\n"
    "5500012000-5500034000 r--p 00012000 fd:01 4711  /data/cores/makecode_arcade_libretro.so\n"
    "5500034000-5500036000 rw-p 00034000 fd:01 4711  /data/cores/makecode_arcade_libretro.so\n"
    "7f10000000-7f10020000 r-xp 00000000 fd:01 99    /usr/lib/libc.so.6\n"
    "7ffd0000-7ffd2000 rw-p 00000000 00:00 0       [stack]\n"
    "7fff0000-7fff1000 r-xp 00000000 00:00 0       [vdso]\n";

// 路径带空格、带 " (deleted)" 后缀的情形（卸载后又重新加载的模块就是这样）
static const char *kModSelfTestSpace =
    "10000-20000 r-xp 0 fd:01 7 /tmp/my core.so (deleted)\n"
    "20000-2a000 rw-p 0 fd:01 7 /tmp/my core.so (deleted)\n"
    "30000-31000 r-xp 0 fd:01 8 /tmp/other.so\n";

extern "C" MCA_TEST_EXPORT int mcaModuleMapSelfTest() {
    int fails = 0;
    unsigned long long lo = 0, hi = 0, lo2 = 0, hi2 = 0;

#define MCA_ST(cond)                                                                                \
    do {                                                                                            \
        if (!(cond)) {                                                                              \
            fails++;                                                                                \
            mcaTrace("[mod] self-test FAIL: %s (line %d)", #cond, __LINE__);                        \
        }                                                                                           \
    } while (0)

    // 三段落合成一个区间，不能把中间的空洞算进去，也不能被 libc/[stack] 干扰
    MCA_ST(mcaMapsProbe(kModSelfTestMaps, 0x5500010000ull, &lo, &hi));
    MCA_ST(lo == 0x5500000000ull && hi == 0x5500036000ull);

    // 同一份文本再解析一遍：结果必须一致。
    // 这一条就是 v0.2.14 那个 bug 的照妖镜 —— 解析器如果还就地改 '\n'，
    // 第二次调用只会看到第一行，lo/hi 直接变 0。
    MCA_ST(mcaMapsProbe(kModSelfTestMaps, 0x5500010000ull, &lo2, &hi2));
    MCA_ST(lo2 == lo && hi2 == hi);

    // [vdso] 这类无路径/带方括号的行也要能正常归组
    MCA_ST(mcaMapsProbe(kModSelfTestMaps, 0x7fff0800ull, &lo, &hi));
    MCA_ST(lo == 0x7fff0000ull && hi == 0x7fff1000ull);

    // 路径含空格 + (deleted)：必须按行进整段匹配，不能截到第一个空格
    MCA_ST(mcaMapsProbe(kModSelfTestSpace, 0x15000ull, &lo, &hi));
    MCA_ST(lo == 0x10000ull && hi == 0x2a000ull);

    // probe 不在任何映射里 → 明确返回 0（调用方据此判"取不到区间"）
    MCA_ST(!mcaMapsProbe(kModSelfTestMaps, 0x9999990000ull, &lo, &hi));
    // 空文本 / 非映射行也不能崩
    MCA_ST(!mcaMapsProbe("", 0x1000ull, &lo, &hi));
    MCA_ST(!mcaMapsProbe("garbage line\n", 0x1000ull, &lo, &hi));

    (void)lo2;
    (void)hi2;
#undef MCA_ST

    mcaTrace("[mod] self-test %s (%d fail)", fails ? "FAILED" : "ok", fails);
    return fails == 0 ? 1 : 0;
}
