#pragma once
// 固定地址映射（MCA v0.2.11）
//
// 为什么需要：retro_serialize 快照里存的是**绝对地址**（GC 堆里的对象互相
// 指向、fiber 的 pc 指向镜像代码、单例指针……）。只要 GC 堆和镜像每次运行
// 都落在同一地址，这些指针在"保存 → 退出 RetroArch → 重新打开 → 加载存档"
// 这条路径上就依然有效，v1 的"同会话恢复"限制随之消失。
//
// 关键前提：**失败必须安全**。指定地址被别人占了（ASLR、DLL、别的 mmap）
// 就返回 NULL，调用方退回普通分配 —— 游戏照常跑，只是快照的跨会话能力
// 退化（加载时按 -10 拒绝），绝不能覆盖已有映射把前端搞崩。
//
// 实现差异：
//   Linux/macOS/Android: mmap + MAP_FIXED_NOREPLACE（4.17+）。老内核不认识
//     这个 flag 会当普通 mmap 处理，返回的地址不是我们要求的 → 检测到地址
//     不符就 munmap 并返回 NULL（等价于"该地址不可用"）。
//   Windows: VirtualAlloc 在指定地址 reserve+commit，地址不符时它自己返回
//     别的地址（不会覆盖），同样按失败处理并释放。

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// 固定区布局（相对 GC_BASE）：
//   [GC_BASE, GC_BASE + 1MB)         VM 堆（PXT_VM_HEAP_ALLOC_BITS=20）
//   [GC_BASE + 1MB, +4MB)            卡带镜像数据（img->dataStart 指向这里）
// 两者相邻但不重叠；镜像 4MB 足够（最大卡带实测 278KB）。
#define MCA_HEAP_BYTES ((size_t)1 << 20)
#define MCA_IMG_MAX_BYTES ((size_t)4 << 20)
#define MCA_IMG_OFFSET MCA_HEAP_BYTES
// 单例区（WDisplay / WSynthesizer 等）：它们在 pxt 里是 new 出来的普通堆
// 对象，但指针被存进快照且被堆内对象引用 —— 地址一变，恢复后一读字段就崩。
// 用固定区的 bump 分配器，启动顺序固定则地址固定。
#define MCA_SINGLETON_OFFSET (MCA_HEAP_BYTES + MCA_IMG_MAX_BYTES)
#define MCA_SINGLETON_MAX_BYTES ((size_t)512 << 10)

// 在 addr 处映射 size 字节（匿名、可读写、零初始化）。
// 成功返回的指针恰好等于 addr；失败返回 NULL，不产生任何副作用。
void *mcaFixedMap(unsigned long long addr, size_t size);

// p 是否来自 mcaFixedMap 建立的区间（用于 unload 时决定能不能 free）。
int mcaIsFixedPtr(const void *p);

// 已登记的固定区间数（诊断用）。
int mcaFixedCount();

// 单例用的固定区 bump 分配（16 字节对齐、零初始化）。
// 拿不到固定区或空间耗尽时返回 NULL，调用方退回普通 new。
void *mcaFixedBump(size_t size);
// VM 重启时回绕游标（单例会被重新构造，地址与上一局相同，反而有利于快照）
void mcaFixedBumpReset();

// ---- 模块区间与指针重定位 ------------------------------------------------
//
// 堆里还有一类"不可复现"的指针：指向**本模块代码**的（RefAction 里编译进
// 内核的回调、fiber 的 wakeFn……）。动态加载器给 .so/.dll 选的基址每个进程
// 都可能不同，跨会话恢复后这些指针指向别的代码，一调用就崩。
//
// 快照记录保存时的模块区间（base + size），恢复时对这些固定区内容做一次
// "命中区间就整体平移"的扫描。区间之外的字节一律不动。
unsigned long long mcaModuleBase(); // 0 = 取不到（此时不重定位）
unsigned long long mcaModuleSize();
// 把 [p, p+bytes) 里落在 [oldBase, oldBase+oldSize) 的指针值平移 (当前基址-旧基址)。
// 返回改写的槽数（诊断用）。
int mcaRelocateModulePtrs(void *p, size_t bytes, unsigned long long oldBase,
                          unsigned long long oldSize);

#ifdef __cplusplus
}
#endif
