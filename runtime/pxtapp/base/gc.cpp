#include "pxtbase.h"

#ifndef GC_BLOCK_SIZE
#define GC_BLOCK_SIZE (1024 * 16)
#endif

#ifndef GC_MAX_ALLOC_SIZE
#define GC_MAX_ALLOC_SIZE (GC_BLOCK_SIZE - 16)
#endif

#ifndef GC_ALLOC_BLOCK
#define GC_ALLOC_BLOCK xmalloc
#endif

#ifdef PXT64
#define HIGH_SHIFT 48
#define BYTES_TO_WORDS(x) ((x) >> 3)
#define WORDS_TO_BYTES(x) ((x) << 3)
#define ALIGN_TO_WORD(x) (((x) + 7) & (~7ULL))
#define VAR_BLOCK_WORDS(vt) ((uint32_t)(uint64_t)(vt) >> 2)
#else
#define HIGH_SHIFT 28
#define BYTES_TO_WORDS(x) ((x) >> 2)
#define WORDS_TO_BYTES(x) ((x) << 2)
#define ALIGN_TO_WORD(x) (((x) + 3) & (~3U))
#define VAR_BLOCK_WORDS(vt) (((uint32_t)(vt) << 4) >> (4 + 2))
#endif

#define FREE_MASK (1ULL << (HIGH_SHIFT + 3))
#define ARRAY_MASK (1ULL << (HIGH_SHIFT + 2))
#define PERMA_MASK (1ULL << (HIGH_SHIFT + 1))
#define MARKED_MASK 0x1
#define ANY_MARKED_MASK 0x3

// the bit operations should be faster than loading large constants
#define IS_FREE(vt) ((uintptr_t)(vt) >> (HIGH_SHIFT + 3))
#define IS_ARRAY(vt) (((uintptr_t)(vt) >> (HIGH_SHIFT + 2)) & 1)
#define IS_PERMA(vt) (((uintptr_t)(vt) >> (HIGH_SHIFT + 1)) & 1)
#define IS_VAR_BLOCK(vt) ((uintptr_t)(vt) >> (HIGH_SHIFT + 2))
#define IS_MARKED(vt) ((uintptr_t)(vt)&MARKED_MASK)
#define IS_LIVE(vt) (IS_MARKED(vt) || (((uintptr_t)(vt) >> (HIGH_SHIFT)) == 0x6))

//#define PXT_GC_DEBUG 1
#ifndef PXT_GC_CHECKS
#define PXT_GC_CHECKS 1
#endif
//#define PXT_GC_STRESS 1

//#define PXT_GC_CHECKS 1

#define MARK(v)                                                                                    \
    do {                                                                                           \
        GC_CHECK(inGCArea(v), 42);                                                                 \
        *(uintptr_t *)(v) |= MARKED_MASK;                                                          \
    } while (0)

#ifdef PXT_GC_DEBUG
#define LOG DMESG
#define VLOG DMESG
#define VVLOG DMESG
#else
#define LOG NOLOG
#define VLOG NOLOG
#define VVLOG NOLOG
#endif

#ifdef PXT_GC_CHECKS
#define GC_CHECK(cond, code)                                                                       \
    if (!(cond))                                                                                   \
    oops(code)
#else
#define GC_CHECK(cond, code) ((void)0)
#endif

// ============================================================================
// MCA A-scheme (probe/pxt32-gc-a): 32 位 GC 块标记旁路表
//
// 上游把 FREE/ARRAY/PERMA 三个块类型标记编码进对象 vtable word 的 bit31/30/29，
// 并要求真 vtable 地址 < 0x10000000（Cortex-M flash/RAM 布局）。32 位 Linux/
// Android 的 .so 是 ET_DYN，vtable 落在 0xf7xxxxxx —— bit31=1 使每个活对象都
// 被 sweep 判成 free block，VAR_BLOCK_WORDS 解出巨大 size 后 memset 越界。
//
// A 方案把这 3 个标记从 vtable word 搬到按 GC word 地址索引的旁路字节表：
//   * MARK(bit0) 保留 —— vtable 4 字节对齐，getVTable() 的 &~1 与
//     VAR_BLOCK_WORDS 的 (vt<<4)>>6 都对 bit0 免疫，无需改动。
//   * size 的 (<<2) 编码保留 —— VAR_BLOCK_WORDS 在去掉高位 MASK 后仍然正确。
//   * 对象内存布局与 .pxt64 镜像格式零改动。
//
// 下面这组宏把"读写标记"抽象成对象指针操作：PXT32 走旁路表，PXT64 展开成
// 与原来逐字相同的位运算 —— 因此 64 位产物的机器码与行为完全不变。
// ============================================================================
#ifdef PXT32
#define MCA_GCT_FREE  0x01
#define MCA_GCT_ARRAY 0x02
#define MCA_GCT_PERMA 0x04

static uint8_t *mcaGcTags;
static uintptr_t mcaGcTagsWords;

// 返回 p 对应 GC word 的 tag 字节槽，按需扩容（按 2 的幂）。
static inline uint8_t *mcaGctSlot(void *p) {
    uintptr_t idx = ((uintptr_t)p - (uintptr_t)mca_gc_base) >> 2;
    GC_CHECK((uintptr_t)p >= (uintptr_t)mca_gc_base && idx < (1U << 30), 42);
    if (idx >= mcaGcTagsWords) {
        uintptr_t ncap = mcaGcTagsWords ? mcaGcTagsWords : (1U << 16);
        while (ncap <= idx)
            ncap <<= 1;
        auto n = (uint8_t *)xmalloc(ncap);
        if (mcaGcTags) {
            memcpy(n, mcaGcTags, mcaGcTagsWords);
            xfree(mcaGcTags);
        }
        mcaGcTags = n;
        mcaGcTagsWords = ncap;
    }
    return &mcaGcTags[idx];
}

// 读 tag 不扩容：不在表内的地址（尚未分配的对象）tag 视为 0。
static inline uint8_t mcaGct(void *p) {
    uintptr_t idx = ((uintptr_t)p - (uintptr_t)mca_gc_base) >> 2;
    return (mcaGcTags && (uintptr_t)p >= (uintptr_t)mca_gc_base && idx < mcaGcTagsWords)
               ? mcaGcTags[idx]
               : 0;
}

// —— 读标记（参数是对象/块头指针）——
#define GC_IS_FREE(p)       (mcaGct(p) & MCA_GCT_FREE)
#define GC_IS_ARRAY(p)      (mcaGct(p) & MCA_GCT_ARRAY)
#define GC_IS_PERMA(p)      (mcaGct(p) & MCA_GCT_PERMA)
#define GC_IS_VAR(p)        (mcaGct(p) & (MCA_GCT_FREE | MCA_GCT_ARRAY))
#define GC_IS_LIVE(p)                                                                              \
    (IS_MARKED(VT(p)) || (mcaGct(p) == (MCA_GCT_ARRAY | MCA_GCT_PERMA)))
#define GC_IS_PERMAARRAY(p) (mcaGct(p) == (MCA_GCT_ARRAY | MCA_GCT_PERMA))

// —— 写标记（p 是 RefObject* / RefBlock*，words 是块大小，单位为 word）——
#define GC_SET_FREE(p, words)                                                                      \
    do {                                                                                           \
        (p)->setVT((uintptr_t)(words) << 2);                                                       \
        *mcaGctSlot(p) = MCA_GCT_FREE;                                                             \
    } while (0)
#define GC_SET_ARRAY(p, words)                                                                     \
    do {                                                                                           \
        (p)->setVT((uintptr_t)(words) << 2);                                                       \
        *mcaGctSlot(p) = MCA_GCT_ARRAY;                                                            \
    } while (0)
#define GC_SET_ARRAY_PERMA(p, words)                                                               \
    do {                                                                                           \
        (p)->setVT((uintptr_t)(words) << 2);                                                       \
        *mcaGctSlot(p) = MCA_GCT_ARRAY | MCA_GCT_PERMA;                                            \
    } while (0)
#define GC_ADD_PERMA(p)  (*mcaGctSlot(p) |= MCA_GCT_PERMA)
#define GC_ADD_FREE(p)   (*mcaGctSlot(p) |= MCA_GCT_FREE)
// 分配出去的对象：清掉原 free 块残留的标记，变成普通对象。
#define GC_CLEAR_TAG(p)  (*mcaGctSlot(p) = 0)

// —— 裸 word 指针版本（gcAllocateArray / app_alloc_at 用 uintptr_t*）——
#define GC_SET_ARRAY_WORD(r, words)                                                                \
    do {                                                                                           \
        *(r) = (uintptr_t)(words) << 2;                                                            \
        *mcaGctSlot(r) = MCA_GCT_ARRAY;                                                            \
    } while (0)
#define GC_SET_ARRAY_PERMA_WORD(r, words)                                                          \
    do {                                                                                           \
        *(r) = (uintptr_t)(words) << 2;                                                            \
        *mcaGctSlot(r) = MCA_GCT_ARRAY | MCA_GCT_PERMA;                                            \
    } while (0)
#define GC_ADD_PERMA_WORD(r) (*mcaGctSlot(r) |= MCA_GCT_PERMA)
#define GC_ADD_FREE_WORD(r)  (*mcaGctSlot(r) |= MCA_GCT_FREE)

#else // PXT64 —— 全部展开成与原来逐字相同的表达式

#define GC_IS_FREE(p)       IS_FREE(VT(p))
#define GC_IS_ARRAY(p)      IS_ARRAY(VT(p))
#define GC_IS_PERMA(p)      IS_PERMA(VT(p))
#define GC_IS_VAR(p)        IS_VAR_BLOCK(VT(p))
#define GC_IS_LIVE(p)       IS_LIVE(VT(p))
#define GC_IS_PERMAARRAY(p) (((uintptr_t)VT(p) >> (HIGH_SHIFT + 1)) == 3)

#define GC_SET_FREE(p, words)       (p)->setVT(FREE_MASK | ((uintptr_t)(words) << 2))
#define GC_SET_ARRAY(p, words)      (p)->setVT(ARRAY_MASK | ((uintptr_t)(words) << 2))
#define GC_SET_ARRAY_PERMA(p, words)                                                               \
    (p)->setVT(ARRAY_MASK | PERMA_MASK | ((uintptr_t)(words) << 2))
#define GC_ADD_PERMA(p)  (p)->setVT(VT(p) | PERMA_MASK)
#define GC_ADD_FREE(p)   (p)->setVT(VT(p) | FREE_MASK)
#define GC_CLEAR_TAG(p)  ((void)0)

#define GC_SET_ARRAY_WORD(r, words)       *(r) = ARRAY_MASK | ((uintptr_t)(words) << 2)
#define GC_SET_ARRAY_PERMA_WORD(r, words) *(r) = ARRAY_MASK | PERMA_MASK | ((uintptr_t)(words) << 2)
#define GC_ADD_PERMA_WORD(r)              *(r) |= PERMA_MASK
#define GC_ADD_FREE_WORD(r)               *(r) |= FREE_MASK

#endif // PXT32 vs PXT64

namespace pxt {

// keep in sync with base/control.ts, function gcStats()
struct GCStats {
    uint32_t numGC;
    uint32_t numBlocks;
    uint32_t totalBytes;
    uint32_t lastFreeBytes;
    uint32_t lastMaxBlockBytes;
    uint32_t minFreeBytes;
};

static GCStats gcStats;

//% expose
Buffer getGCStats() {
    return mkBuffer((uint8_t *)&gcStats, sizeof(gcStats));
}

//%
void popThreadContext(ThreadContext *ctx);
//%
ThreadContext *pushThreadContext(void *sp, void *endSP);

unsigned RefRecord_gcsize(RefRecord *r) {
    VTable *tbl = getVTable(r);
    return BYTES_TO_WORDS(tbl->numbytes);
}

#ifdef PXT_GC_THREAD_LIST
ThreadContext *threadContexts;
#endif

#define IN_GC_ALLOC 1
#define IN_GC_COLLECT 2
#define IN_GC_FREEZE 4
#define IN_GC_PREALLOC 8

#ifndef PXT_VM
static TValue *tempRoot;
static uint8_t tempRootLen;
#endif

uint8_t inGC;

void popThreadContext(ThreadContext *ctx) {
#ifndef PXT_VM
    VLOG("pop: %p", ctx);

    if (!ctx)
        return;

    auto n = ctx->stack.next;
    if (n) {
        VLOG("seg %p", n);
        ctx->stack.top = n->top;
        ctx->stack.bottom = n->bottom;
        ctx->stack.next = n->next;
        app_free(n);
    } else {
#ifdef PXT_GC_THREAD_LIST
        if (ctx->next)
            ctx->next->prev = ctx->prev;
        if (ctx->prev)
            ctx->prev->next = ctx->next;
        else {
            if (threadContexts != ctx)
                oops(41);
            threadContexts = ctx->next;
            if (threadContexts)
                threadContexts->prev = NULL;
        }
#endif
        app_free(ctx);
        setThreadContext(NULL);
    }
#endif
}

#define ALLOC(tp) (tp *)app_alloc(sizeof(tp))

ThreadContext *pushThreadContext(void *sp, void *endSP) {
#ifdef PXT_VM
    return NULL;
#else
    if (PXT_IN_ISR())
        target_panic(PANIC_CALLED_FROM_ISR);

    auto curr = getThreadContext();
    tempRoot = (TValue *)endSP;
    tempRootLen = (uintptr_t *)sp - (uintptr_t *)endSP;
    if (curr) {
#ifdef PXT_GC_THREAD_LIST
#ifdef PXT_GC_DEBUG
        auto ok = false;
        for (auto p = threadContexts; p; p = p->next)
            if (p == curr) {
                ok = true;
                break;
            }
        if (!ok)
            oops(49);
#endif
#endif
        auto seg = ALLOC(StackSegment);
        VLOG("stack %p / %p", seg, curr);
        seg->top = curr->stack.top;
        seg->bottom = curr->stack.bottom;
        seg->next = curr->stack.next;
        curr->stack.next = seg;
    } else {
        curr = ALLOC(ThreadContext);
        LOG("push: %p", curr);
        curr->globals = globals;
        curr->stack.next = NULL;
        curr->thrownValue = TAG_NON_VALUE;
        curr->tryFrame = NULL;

#ifdef PXT_GC_THREAD_LIST
        curr->next = threadContexts;
        curr->prev = NULL;
        if (curr->next)
            curr->next->prev = curr;
        threadContexts = curr;
#endif
        setThreadContext(curr);
    }
    tempRootLen = 0;
    curr->stack.bottom = sp;
    curr->stack.top = NULL;
    return curr;
#endif
}

class RefBlock : public RefObject {
  public:
    RefBlock *nextFree;
};

struct GCBlock {
    GCBlock *next;
    uint32_t blockSize;
    RefObject data[0];
};

struct PendingArray {
    PendingArray *next;
    TValue *data;
    unsigned len;
};

#define PENDING_ARRAY_THR 100

static PendingArray *pendingArrays;
static LLSegment gcRoots;
LLSegment workQueue; // (ab)used by consString making
static GCBlock *firstBlock;
static RefBlock *firstFree;
static uint8_t *midPtr;

static bool inGCArea(void *ptr) {
    for (auto block = firstBlock; block; block = block->next) {
        if ((void *)block->data <= ptr && ptr < (void *)((uint8_t *)block->data + block->blockSize))
            return true;
    }
    return false;
}

#define NO_MAGIC(vt) ((VTable *)vt)->magic != VTABLE_MAGIC
#define VT(p) (*(uintptr_t *)(p))
// MCA A-scheme：ARRAY 标记走 GC_IS_ARRAY(p)（PXT32 旁路表 / PXT64 位测试），
// 其余两项（只读、mark）保持原样。64 位下 GC_IS_ARRAY(p) == IS_ARRAY(VT(p))，
// 与原来的 (VT(p) & ARRAY_MASK) 逐位等价。
#define SKIP_PROCESSING(p)                                                                         \
    (isReadOnly(p) || (VT(p) & ANY_MARKED_MASK) || GC_IS_ARRAY(p) || NO_MAGIC(VT(p)))

void gcMarkArray(void *data) {
    auto segBl = (uintptr_t *)data - 1;
    GC_CHECK(!IS_MARKED(VT(segBl)), 47);
    MARK(segBl);
}

void gcScan(TValue v) {
    if (SKIP_PROCESSING(v))
        return;
    MARK(v);
    workQueue.push(v);
}

void gcScanMany(TValue *data, unsigned len) {
    // VLOG("scan: %p %d", data, len);
    for (unsigned i = 0; i < len; ++i) {
        auto v = data[i];
        // VLOG("psh: %p %d %d", v, isReadOnly(v), (*(uint32_t *)v & 1));
        if (SKIP_PROCESSING(v))
            continue;
        MARK(v);
        workQueue.push(v);
        if (workQueue.getLength() > PENDING_ARRAY_THR) {
            i++;
            // store rest of the work for later, when we have cleared the queue
            auto pa = (PendingArray *)xmalloc(sizeof(PendingArray));
            pa->next = pendingArrays;
            pa->data = data + i;
            pa->len = len - i;
            pendingArrays = pa;
            break;
        }
    }
}

void gcScanSegment(Segment &seg) {
    auto data = seg.getData();
    if (!data)
        return;
    VVLOG("seg %p %d", data, seg.getLength());
    gcMarkArray(data);
    gcScanMany(data, seg.getLength());
}

#define getScanMethod(vt) ((RefObjectMethod)(((VTable *)(vt))->methods[2]))
#define getSizeMethod(vt) ((RefObjectSizeMethod)(((VTable *)(vt))->methods[3]))

void gcProcess(TValue v) {
    if (SKIP_PROCESSING(v))
        return;
    VVLOG("gcProcess: %p", v);
    MARK(v);
    auto scan = getScanMethod(VT(v) & ~ANY_MARKED_MASK);
    if (scan)
        scan((RefObject *)v);
    for (;;) {
        while (workQueue.getLength()) {
            auto curr = (RefObject *)workQueue.pop();
            VVLOG(" - %p", curr);
            scan = getScanMethod(curr->vt() & ~ANY_MARKED_MASK);
            if (scan)
                scan(curr);
        }
        if (pendingArrays) {
            auto pa = pendingArrays;
            pendingArrays = pa->next;
            auto data = pa->data;
            auto len = pa->len;
            xfree(pa);
            gcScanMany(data, len);
        } else {
            break;
        }
    }
}

static void mark(int flags) {
#ifdef PXT_GC_DEBUG
    flags |= 2;
#endif
    auto data = gcRoots.getData();
    auto len = gcRoots.getLength();
    if (flags & 2) {
        DMESG("--MARK");
        DMESG("RP:%p/%d", data, len);
    }
    for (unsigned i = 0; i < len; ++i) {
        auto d = data[i];
        if ((uintptr_t)d & 1) {
            d = *(TValue *)((uintptr_t)d & ~1);
        }
        gcProcess(d);
    }

#ifdef PXT_GC_THREAD_LIST
    for (auto ctx = threadContexts; ctx; ctx = ctx->next) {
        gcProcess(ctx->thrownValue);
        for (auto seg = &ctx->stack; seg; seg = seg->next) {
            auto ptr = (TValue *)threadAddressFor(ctx, seg->top);
            auto end = (TValue *)threadAddressFor(ctx, seg->bottom);
            VLOG("mark: %p - %p", ptr, end);
            while (ptr < end) {
                gcProcess(*ptr++);
            }
        }
    }
#else
    gcProcessStacks(flags);
#endif

    if (globals) {
#ifdef PXT_VM
        auto nonPtrs = vmImg->infoHeader->nonPointerGlobals;
#else
        auto nonPtrs = bytecode[21];
#endif
        len = getNumGlobals() - nonPtrs;
        data = globals + nonPtrs;
        if (flags & 2)
            DMESG("RG:%p/%d", data, len);
        VLOG("globals: %p %d", data, len);
        for (unsigned i = 0; i < len; ++i) {
            gcProcess(*data++);
        }
    }

#ifndef PXT_VM
    data = tempRoot;
    len = tempRootLen;
    for (unsigned i = 0; i < len; ++i) {
        gcProcess(*data++);
    }
#endif
}

static uint32_t getObjectSize(RefObject *o) {
    auto vt = o->vt() & ~ANY_MARKED_MASK;
    uint32_t r;
    GC_CHECK(vt != 0, 49);
    // MCA A-scheme：变长块（free/array）标记在 PXT32 下已搬到旁路表，
    // 不能再从 vt 高位判断；GC_IS_VAR/GC_IS_FREE 在 PXT64 下仍是基于 vt 位测试。
    if (GC_IS_VAR(o)) {
        r = VAR_BLOCK_WORDS(vt);
    } else {
        auto sz = getSizeMethod(vt);
        // GC_CHECK(0x2000 <= (intptr_t)sz && (intptr_t)sz <= 0x100000, 47);
        r = sz(o);
    }
    GC_CHECK(1 <= r && (r <= BYTES_TO_WORDS(GC_MAX_ALLOC_SIZE) || GC_IS_FREE(o)), 41);
    return r;
}

static void setupFreeBlock(GCBlock *curr) {
    gcStats.numBlocks++;
    gcStats.totalBytes += curr->blockSize;
    GC_SET_FREE(&curr->data[0], TOWORDS(curr->blockSize));
    ((RefBlock *)curr->data)[0].nextFree = firstFree;
    firstFree = (RefBlock *)curr->data;
    midPtr = (uint8_t *)curr->data + curr->blockSize / 4;
}

static void linkFreeBlock(GCBlock *curr) {
    // blocks need to be sorted by address for midPtr to work
    if (!firstBlock || curr < firstBlock) {
        curr->next = firstBlock;
        firstBlock = curr;
    } else {
        for (auto p = firstBlock; p; p = p->next) {
            if (!p->next || curr < p->next) {
                curr->next = p->next;
                p->next = curr;
                break;
            }
        }
    }
}

void gcPreAllocateBlock(uint32_t sz) {
    auto curr = (GCBlock *)GC_ALLOC_BLOCK(sz);
    curr->blockSize = sz - sizeof(GCBlock);
    LOG("GC pre-alloc: %p", curr);
    GC_CHECK((curr->blockSize & 3) == 0, 40);
    setupFreeBlock(curr);
    linkFreeBlock(curr);
}

static GCBlock *allocateBlockCore() {
    int sz = GC_BLOCK_SIZE;
    void *dummy = NULL;
#ifdef GC_GET_HEAP_SIZE
    if (firstBlock) {
#ifdef GC_STACK_BASE
        if (!firstBlock->next) {
            int memSize = getConfig(CFG_RAM_BYTES, 0);
            int codalEnd = GC_STACK_BASE;
            // round up to 1k - there is sometimes a few bytes below the stack
            codalEnd = (codalEnd + 1024) & ~1023;
            int codalSize = codalEnd & 0xffffff;
            sz = memSize - codalSize - 4;
            if (sz > 0) {
                auto curr = (GCBlock *)codalEnd;
                curr->blockSize = sz - sizeof(GCBlock);
                return curr;
            }
        }
#endif
        gc(2); // dump roots
        soft_panic(PANIC_GC_OOM);
    }
    auto lowMem = getConfig(CFG_LOW_MEM_SIMULATION_KB, 0);
    auto sysHeapSize = getConfig(CFG_SYSTEM_HEAP_BYTES, 4 * 1024);
    auto heapSize = GC_GET_HEAP_SIZE();
    DMESG("heap: %d (minus %d sys bytes)", heapSize, sysHeapSize);
    sz = heapSize - sysHeapSize;
    if (lowMem) {
        auto memIncrement = 32 * 1024;
        // get the memory size - assume it's increment of 32k,
        // and we don't statically allocate more than 32k
        auto memSize = ((heapSize + memIncrement - 1) / memIncrement) * memIncrement;
        int fillerSize = memSize - lowMem * 1024;
        if (fillerSize > 0) {
            dummy = GC_ALLOC_BLOCK(fillerSize);
            sz -= fillerSize;
        }
    }
#endif
    auto curr = (GCBlock *)GC_ALLOC_BLOCK(sz);
    curr->blockSize = sz - sizeof(GCBlock);
    // make sure reference to allocated block is stored somewhere, otherwise
    // GCC optimizes out the call to GC_ALLOC_BLOCK
    curr->data[4].setVT((uintptr_t)dummy);
    return curr;
}

__attribute__((noinline)) static void allocateBlock() {
    auto curr = allocateBlockCore();
    DMESG("GC block %db @ %p", curr->blockSize, curr);
    GC_CHECK((curr->blockSize & 3) == 0, 40);
    setupFreeBlock(curr);
    linkFreeBlock(curr);
}

static void sweep(int flags) {
    RefBlock *prevFreePtr = NULL;
    uint32_t freeSize = 0;
    uint32_t totalSize = 0;
    uint32_t maxFreeBlock = 0;
    firstFree = NULL;

    gcStats.numGC++;

    for (auto h = firstBlock; h; h = h->next) {
        auto d = h->data;
        auto words = BYTES_TO_WORDS(h->blockSize);
        auto end = d + words;
        totalSize += words;
        VLOG("sweep: %p - %p", d, end);
        while (d < end) {
            if (GC_IS_LIVE(d)) {
                VVLOG("Live %p", d);
                d->setVT(d->vt() & ~MARKED_MASK);
                d += getObjectSize(d);
            } else {
                auto start = (RefBlock *)d;
                while (d < end) {
                    if (GC_IS_FREE(d)) {
                        VVLOG("Free %p", d);
                    } else if (GC_IS_LIVE(d)) {
                        break;
                    } else if (GC_IS_ARRAY(d)) {
                        VVLOG("Dead Arr %p", d);
                    } else {
                        VVLOG("Dead Obj %p", d);
                        GC_CHECK(d->vtable->magic == VTABLE_MAGIC, 41);
                        d->destroyVT();
                        VVLOG("destroyed");
                    }
                    d += getObjectSize(d);
                }
                auto sz = d - (RefObject *)start;
                freeSize += sz;
                if (sz > (int)maxFreeBlock)
                    maxFreeBlock = sz;
#ifdef PXT_GC_CHECKS
                memset((void *)start, 0xff, WORDS_TO_BYTES(sz));
#endif
                GC_SET_FREE(start, sz);
                if (sz > 1) {
                    start->nextFree = NULL;
                    if (!prevFreePtr) {
                        firstFree = start;
                    } else {
                        prevFreePtr->nextFree = start;
                    }
                    prevFreePtr = start;
                }
            }
        }
    }

    if (midPtr) {
        uint32_t currFree = 0;
#ifdef PXT_ESP32
        auto limit = freeSize * 1 / 4;
#else
        auto limit = freeSize * 1 / 2;
#endif
        for (auto p = firstFree; p; p = p->nextFree) {
            auto len = VAR_BLOCK_WORDS(p->vtable);
            currFree += len;
            if (currFree > limit) {
                midPtr = (uint8_t *)p + ((limit - currFree + len) << 2);
                break;
            }
        }
    }

    freeSize = WORDS_TO_BYTES(freeSize);
    totalSize = WORDS_TO_BYTES(totalSize);
    maxFreeBlock = WORDS_TO_BYTES(maxFreeBlock);

    gcStats.lastFreeBytes = freeSize;
    gcStats.lastMaxBlockBytes = maxFreeBlock;

    if (gcStats.minFreeBytes == 0 || gcStats.minFreeBytes > freeSize)
        gcStats.minFreeBytes = freeSize;

    if (flags & 1)
        DMESG("GC %d/%d free; %d maxBlock", freeSize, totalSize, maxFreeBlock);
    else
        LOG("GC %d/%d free; %d maxBlock", freeSize, totalSize, maxFreeBlock);

#ifndef GC_GET_HEAP_SIZE
    // if the heap is 90% full, allocate a new block
    if (freeSize * 10 <= totalSize) {
        allocateBlock();
    }
#endif
}

void gc(int flags) {
    startPerfCounter(PerfCounters::GC);
    GC_CHECK(!(inGC & IN_GC_COLLECT), 40);
    inGC |= IN_GC_COLLECT;
    VLOG("GC mark");
    mark(flags);
    VLOG("GC sweep");
    sweep(flags);
    VLOG("GC done");
    stopPerfCounter(PerfCounters::GC);
    inGC &= ~IN_GC_COLLECT;
}

#ifdef GC_GET_HEAP_SIZE
extern "C" void free(void *ptr) {
    if (!ptr)
        return;
    if (inGCArea(ptr))
        app_free(ptr);
    else
        xfree(ptr);
}

extern "C" void *malloc(size_t sz) {
    if (PXT_IN_ISR() || inGC)
        return xmalloc(sz);
    else
        return app_alloc(sz);
}

extern "C" void *realloc(void *ptr, size_t size) {
    if (inGCArea(ptr)) {
        void *mem = malloc(size);

        if (ptr != NULL && mem != NULL) {
            auto r = (uintptr_t *)ptr;
            GC_CHECK(GC_IS_PERMAARRAY(r - 1), 41);
            size_t blockSize = VAR_BLOCK_WORDS(r[-1]);
            memcpy(mem, ptr, min(blockSize * sizeof(void *), size));
            free(ptr);
        }

        return mem;
    } else {
        return device_realloc(ptr, size);
    }
}
#endif

void *gcAllocateArray(int numbytes) {
    numbytes = ALIGN_TO_WORD(numbytes);
    numbytes += sizeof(void *);
    auto r = (uintptr_t *)gcAllocate(numbytes);
    GC_SET_ARRAY_WORD(r, TOWORDS(numbytes));
    return r + 1;
}

static void *gcAllocAt(void *hint, int numbytes) {
    gc(0);
    size_t numwords = BYTES_TO_WORDS(ALIGN_TO_WORD(numbytes));

    for (auto p = firstFree; p; p = p->nextFree) {
        GC_CHECK(!isReadOnly((TValue)p), 49);
        auto vt = p->vtable;
        GC_CHECK(GC_IS_FREE(p), 43);
        int offset = BYTES_TO_WORDS((uint8_t *)hint - (uint8_t *)p);
        int left = (int)(VAR_BLOCK_WORDS(vt) - numwords - offset);
        // we give ourselves some space here, so we don't get some strange overlaps
        if (offset >= 8 && left >= 8) {
            auto nf = (RefBlock *)((void **)p + numwords + offset);
            GC_SET_FREE(nf, left);
            nf->nextFree = p->nextFree;
            p->nextFree = nf;
            GC_SET_FREE(p, offset);
            p = (RefBlock *)((void **)p + offset);
            p->setVT(0);
            GC_CLEAR_TAG(p);
            return p;
        }
    }

    return NULL;
}

void *app_alloc_at(void *at, int numbytes) {
    if (numbytes < 8)
        return NULL;
    if (!at)
        return NULL;

    numbytes = ALIGN_TO_WORD(numbytes) + sizeof(void *);
    auto r = (uintptr_t *)gcAllocAt((uintptr_t *)at - 1, numbytes);
    if (!r)
        return NULL;
    GC_SET_ARRAY_PERMA_WORD(r, TOWORDS(numbytes));
    gc(0);
    return r + 1;
}

void *app_alloc(int numbytes) {
    if (!numbytes)
        return NULL;

    // gc(0);
    auto r = (uintptr_t *)gcAllocateArray(numbytes);
    GC_ADD_PERMA_WORD(r - 1);
    return r;
}

void *app_free(void *ptr) {
    auto r = (uintptr_t *)ptr;
    GC_CHECK(GC_IS_PERMAARRAY(r - 1), 41);
    GC_ADD_FREE_WORD(r - 1);
    return r;
}

void gcFreeze() {
    inGC |= IN_GC_FREEZE;
}

void gcReset() {
    inGC &= ~IN_GC_FREEZE;

    gcRoots.setLength(0);

    if (inGC)
        oops(41);

    if (workQueue.getLength())
        oops(41);

    memset(&gcStats, 0, sizeof(gcStats));
    firstFree = NULL;
    for (auto h = firstBlock; h; h = h->next) {
        setupFreeBlock(h);
    }
}

#ifdef PXT_VM
// MCA PATCH 6（本项目）：重启时把 GC 的"块清单"整个忘掉。
//
// gcAllocBlock() 从固定的 VM 堆（1MB）里按顺序切 64KB 块，切块游标在重启时被
// 绕回堆首（见 scheduler.cpp 的 resetRuntimeState）。如果 firstBlock 链还留着
// 上一局的块，新块会落在**一模一样的地址**上，linkFreeBlock() 的排序插入就会把
// 这个块插到自己后面（self-cycle）—— 之后任何一次 sweep()/gcReset() 遍历
// firstBlock 都是死循环，表现为"重置后画面冻住、进程不崩也不退出"。
// 清空清单后，新一局从堆首重新切块，与首次冷启动完全一致。
void gcForgetAllBlocks() {
    firstBlock = NULL;
    firstFree = NULL;
    midPtr = NULL;
#ifdef PXT32
    // MCA A-scheme：重启后堆地址回绕到 GC_BASE，旁路表里的旧标记全部作废；
    // 新一局 setupFreeBlock / gcAllocate 会按需重新写入。
    if (mcaGcTags)
        memset(mcaGcTags, 0, mcaGcTagsWords);
#endif
    memset(&gcStats, 0, sizeof(gcStats));
    // 旧堆里的一切都不再有效：globals 指针必须一起丢掉。
    // 否则新一局第一次 gc() 会拿上一局的 globals 去 mark 已被复用的内存
    // （GC_CHECK 失败 → oops(41)）；而 globals 会在 vmStartCore 里重新 app_alloc。
    globals = NULL;
}

void gcPreStartup() {
    inGC |= IN_GC_PREALLOC;
}

void gcStartup() {
    inGC &= ~IN_GC_PREALLOC;
}
#endif

void *gcAllocate(int numbytes) {
    size_t numwords = BYTES_TO_WORDS(ALIGN_TO_WORD(numbytes));
    // VVLOG("alloc %d bytes %d words", numbytes, numwords);

    if (numbytes > GC_MAX_ALLOC_SIZE)
        soft_panic(PANIC_GC_TOO_BIG_ALLOCATION);

    if (PXT_IN_ISR() || (inGC & (IN_GC_PREALLOC | IN_GC_ALLOC | IN_GC_COLLECT | IN_GC_FREEZE)))
        target_panic(PANIC_CALLED_FROM_ISR);

    inGC |= IN_GC_ALLOC;

#if defined(PXT_GC_CHECKS) && !defined(PXT_VM)
    {
        auto curr = getThreadContext();
        if (curr && !curr->stack.top)
            oops(46);
    }
#endif

#ifdef PXT_GC_STRESS
    gc(0);
#endif

    for (int i = 0;; ++i) {
        RefBlock *prev = NULL;
        for (auto p = firstFree; p; p = p->nextFree) {
            VVLOG("p=%p", p);
            if (i == 0 && (uint8_t *)p > midPtr) {
                VLOG("past midptr %p; gc", midPtr);
                break;
            }
            GC_CHECK(!isReadOnly((TValue)p), 49);
            auto vt = p->vtable;
            if (!GC_IS_FREE(p))
                oops(43);
            int left = (int)(VAR_BLOCK_WORDS(vt) - numwords);
            VVLOG("%p %d - %d = %d", (void *)vt, (int)VAR_BLOCK_WORDS(vt), (int)numwords, left);
            if (left >= 0) {
                auto nf = (RefBlock *)((void **)p + numwords);
                auto nextFree = p->nextFree; // p and nf can overlap when allocating 4 bytes
                // VVLOG("nf=%p nef=%p", nf, nextFree);
                if (left)
                    GC_SET_FREE(nf, left);
                if (left >= 2) {
                    nf->nextFree = nextFree;
                } else {
                    nf = nextFree;
                }
                if (prev)
                    prev->nextFree = nf;
                else
                    firstFree = nf;
                p->setVT(0);
                GC_CLEAR_TAG(p);
                VVLOG("GC=>%p %d %p -> %p,%p", p, numwords, nf, nf ? nf->nextFree : 0,
                      nf ? (void *)nf->vtable : 0);
                GC_CHECK(!nf || !nf->nextFree || !isReadOnly((TValue)nf->nextFree), 48);
                inGC &= ~IN_GC_ALLOC;
                return p;
            }
            prev = p;
        }

        // we didn't find anything, try GC
        if (i == 0)
            gc(0);
        // GC didn't help, try new block
        else if (i == 1) {
            DMESG("gcAlloc(%d) (%d/%d free; %d max block) -> new block", numbytes,
                  gcStats.lastFreeBytes, gcStats.totalBytes, gcStats.lastMaxBlockBytes);
            allocateBlock();
        } else
            // the block allocated was apparently too small
            soft_panic(PANIC_GC_OOM);
    }
}

static void removePtr(TValue v) {
    int len = gcRoots.getLength();
    auto data = gcRoots.getData();
    // scan from the back, as this is often used as a stack
    for (int i = len - 1; i >= 0; --i) {
        if (data[i] == v) {
            if (i == len - 1) {
                gcRoots.pop();
            } else {
                data[i] = gcRoots.pop();
            }
            return;
        }
    }
    oops(40);
}

void registerGC(TValue *root, int numwords) {
    if (!numwords)
        return;

    if (numwords > 1) {
        while (numwords-- > 0) {
            registerGC(root++, 1);
        }
        return;
    }

    gcRoots.push((TValue)((uintptr_t)root | 1));
}

void unregisterGC(TValue *root, int numwords) {
    if (!numwords)
        return;
    if (numwords > 1) {
        while (numwords-- > 0) {
            unregisterGC(root++, 1);
        }
        return;
    }

    removePtr((TValue)((uintptr_t)root | 1));
}

void registerGCPtr(TValue ptr) {
    if (isReadOnly(ptr))
        return;
    gcRoots.push(ptr);
}

void unregisterGCPtr(TValue ptr) {
    if (isReadOnly(ptr))
        return;
    removePtr(ptr);
}

void RefImage::scan(RefImage *t) {
    gcScan((TValue)t->buffer);
}

void RefCollection::scan(RefCollection *t) {
    gcScanSegment(t->head);
}

void RefAction::scan(RefAction *t) {
    gcScanMany(t->fields, t->len);
}

void RefRefLocal::scan(RefRefLocal *t) {
    gcScan(t->v);
}

void RefMap::scan(RefMap *t) {
    gcScanSegment(t->keys);
    gcScanSegment(t->values);
}

void RefRecord_scan(RefRecord *r) {
    VTable *tbl = getVTable(r);
    gcScanMany(r->fields, BYTES_TO_WORDS(tbl->numbytes - sizeof(RefRecord)));
}

#define SIZE(off) TOWORDS(sizeof(*t) + (off))

unsigned RefImage::gcsize(RefImage *t) {
    return SIZE(0);
}

unsigned RefCollection::gcsize(RefCollection *t) {
    return SIZE(0);
}

unsigned RefAction::gcsize(RefAction *t) {
    return SIZE(WORDS_TO_BYTES(t->len));
}

unsigned RefRefLocal::gcsize(RefRefLocal *t) {
    return SIZE(0);
}

unsigned RefMap::gcsize(RefMap *t) {
    return SIZE(0);
}

} // namespace pxt
