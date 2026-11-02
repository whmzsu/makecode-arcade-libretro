#pragma once
// mca_state.h — retro_serialize 快照（MCA 项目）共享状态结构。
//
// v1 设计：**同会话内恢复**。pxt VM 的堆内指针、fiber 上下文、栈副本全部
// 是本进程的绝对地址，快照把"安全点时刻"的全部可变 VM 状态原样搬走，
// 恢复时要求 gcBase/vmImg 与快照时一致（同一次加载游戏内必然成立；
// 换游戏/重启后加载会因地址不同被拒绝——RetroArch 的 rewind/runahead 与
// 会话内存档都满足该前提，跨会话的磁盘存档留待 v2 固定地址堆）。
//
// 安全点 = mainRunLoop 顶部（所有 fiber 的解释器状态要么在自己的
// stackCopy 里，要么在 vmImg->stackBase 共享操作数栈里，VM 线程不在
// exec_loop 中间，不需要抓 C 栈）。

#include <stdint.h>

namespace pxt {

// gc.cpp 的静态状态（gc.cpp 填/写；scheduler.cpp 负责搬进搬出快照）
struct McaGcState {
    void *gcBase;
    void *gcAllocPtr;    // 堆切块游标（heapUsed = gcAllocPtr - gcBase）
    void *firstBlock;    // mark-sweep 空闲块链
    void *firstFree;
    void *midPtr;
    uint8_t inGC;        // 安全点上必须是 0（无 GC 进行中）
    void *globals;       // 全局变量数组（在 GC 堆内）
    void *pendingArrays; // 安全点上必须是 NULL（只在 gc() 内非空）
    // LLSegment（system malloc 缓冲；内容由调用方按 len*sizeof(TValue) 搬运）
    void *gcRootsData;
    uint32_t gcRootsLen, gcRootsSize;
    void *workQueueData;
    uint32_t workQueueLen, workQueueSize;
    uint32_t stats[6];   // GCStats（统计用，回带保持一致）
};

// gc.cpp 实现：把 GC 静态状态搬进/搬出结构（LLSegment 缓冲内容另搬）
void mcaGcGetState(McaGcState *s);
void mcaGcSetState(const McaGcState *s);
// LLSegment 是 gc.cpp 的文件内静态：恢复时调度器要按这两个指针把内容读回去
// （跨会话下缓冲已由 mcaGcSetState 重新分配，快照里的旧地址不可用）
void *mcaGcRootsData();
void *mcaGcWorkQueueData();

// SINGLETON 宏生成的快照访问器（instX 静态指针指向 GC 堆，堆回绕后必须
// 一起恢复，否则是野指针）。宏定义本体在 pxtbase.h（SINGLETON 之前），
// 这里只留注释说明。

} // namespace pxt
