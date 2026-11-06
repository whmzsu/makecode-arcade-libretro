// stub_core_fat.c — 胖桩核心：ra_fatstub.nro 的源
//
// 目的：把「我们的代码内容搞坏了堆」和「仅仅是内存足迹/布局不同」切开。
//
// 背景（2026-11-07 真机证据）：所有 crash report 的崩点全在 mesa/nouveau 的
// malloc/free 路径里（_malloc_r ← nouveau_mm_allocate ← ozone 菜单画光标；
// _free_r ← nouveau_fence_trigger_work ← swap_buffers），我们的核心函数
// 从未出现在栈上。野指针高位都是 0x3f…（双精度浮点的位模式），说明堆的
// 空闲链表被一段 double 数据覆盖。而发版 NRO 能进菜单、diag 的 mca_boot
// 在菜单就崩 —— 同一套核心代码、同一版 RA（v1.22.2）、同一条链接链，
// 唯一系统性差异是内存足迹（text 8.28MB / ro 7.9MB / bss 1.0MB）。
//
// 判定表：
//   ra_fatstub 在菜单就崩（像 mca_boot）-> 足迹/布局就足以触发，
//       与核心代码内容无关，方向转向 RA/mesa 侧（尺寸阈值、对齐、加载器）。
//   ra_fatstub 菜单正常              -> 嫌疑回到我们核心的目标代码本身，
//       下一步在核心内部二分（先把 VM 运行时摘掉编一版）。
//
// 内容 = stub_core.c（空核心，一行业务逻辑没有）+ 纯占位段：
//   ro  ~8MB  const 数组（对应我们核心的 rodata 体积）
//   bss  1MB  未初始化数组（对应我们核心的 bss 体积）
//   text  ~64KB 一串 noinline 占位函数（多少给 text 一点分量；
//             完整 8MB text 需要生成源码，第一轮先不追求形似）
// 占位段**只占地方，不执行任何写操作**——初始化数据本身就是变量，不能引入。

#include "stub_core.c"

// ---- 占位 rodata：8MB。绝大多数是 0，但首字节放个 1 防止整段被合并优化掉。
// used + volatile 读防止链接器丢弃（--gc-sections 会删没被引用的段）。
const volatile uint8_t mca_fat_ro[8 * 1024 * 1024] = {1, 0};
__attribute__((used)) const void *mca_fat_ro_anchor(void)
{
    return mca_fat_ro;
}

// ---- 占位 bss：1MB。
volatile uint8_t mca_fat_bss[1024 * 1024];
__attribute__((used)) unsigned mca_fat_bss_touch(void)
{
    // 只读不写：本轮要隔离的是"足迹"，不是行为。
    return mca_fat_bss[0] + mca_fat_bss[1024 * 1024 - 1];
}

// ---- 占位 text：64 个 noinline 函数，每个几百字节，用函数指针数组钉住。
#define FAT_FN(n)                                                             \
    __attribute__((noinline)) static unsigned fat_fn_##n(                     \
        volatile const uint8_t *p, unsigned s)                                \
    {                                                                         \
        unsigned acc = 0;                                                     \
        for (int i = 0; i < 128; i++)                                         \
            acc = acc * 31 + p[(i + s) & 0xff] + n;                           \
        return acc;                                                           \
    }
FAT_FN(0)  FAT_FN(1)  FAT_FN(2)  FAT_FN(3)  FAT_FN(4)  FAT_FN(5)  FAT_FN(6)  FAT_FN(7)
FAT_FN(8)  FAT_FN(9)  FAT_FN(10) FAT_FN(11) FAT_FN(12) FAT_FN(13) FAT_FN(14) FAT_FN(15)
FAT_FN(16) FAT_FN(17) FAT_FN(18) FAT_FN(19) FAT_FN(20) FAT_FN(21) FAT_FN(22) FAT_FN(23)
FAT_FN(24) FAT_FN(25) FAT_FN(26) FAT_FN(27) FAT_FN(28) FAT_FN(29) FAT_FN(30) FAT_FN(31)
FAT_FN(32) FAT_FN(33) FAT_FN(34) FAT_FN(35) FAT_FN(36) FAT_FN(37) FAT_FN(38) FAT_FN(39)
FAT_FN(40) FAT_FN(41) FAT_FN(42) FAT_FN(43) FAT_FN(44) FAT_FN(45) FAT_FN(46) FAT_FN(47)
FAT_FN(48) FAT_FN(49) FAT_FN(50) FAT_FN(51) FAT_FN(52) FAT_FN(53) FAT_FN(54) FAT_FN(55)
FAT_FN(56) FAT_FN(57) FAT_FN(58) FAT_FN(59) FAT_FN(60) FAT_FN(61) FAT_FN(62) FAT_FN(63)

typedef unsigned (*fat_fn_t)(volatile const uint8_t *, unsigned);
__attribute__((used)) static const fat_fn_t mca_fat_fns[64] = {
    fat_fn_0,  fat_fn_1,  fat_fn_2,  fat_fn_3,  fat_fn_4,  fat_fn_5,  fat_fn_6,  fat_fn_7,
    fat_fn_8,  fat_fn_9,  fat_fn_10, fat_fn_11, fat_fn_12, fat_fn_13, fat_fn_14, fat_fn_15,
    fat_fn_16, fat_fn_17, fat_fn_18, fat_fn_19, fat_fn_20, fat_fn_21, fat_fn_22, fat_fn_23,
    fat_fn_24, fat_fn_25, fat_fn_26, fat_fn_27, fat_fn_28, fat_fn_29, fat_fn_30, fat_fn_31,
    fat_fn_32, fat_fn_33, fat_fn_34, fat_fn_35, fat_fn_36, fat_fn_37, fat_fn_38, fat_fn_39,
    fat_fn_40, fat_fn_41, fat_fn_42, fat_fn_43, fat_fn_44, fat_fn_45, fat_fn_46, fat_fn_47,
    fat_fn_48, fat_fn_49, fat_fn_50, fat_fn_51, fat_fn_52, fat_fn_53, fat_fn_54, fat_fn_55,
    fat_fn_56, fat_fn_57, fat_fn_58, fat_fn_59, fat_fn_60, fat_fn_61, fat_fn_62, fat_fn_63,
};

__attribute__((used)) unsigned mca_fat_text_touch(void)
{
    return mca_fat_fns[(mca_fat_bss_touch()) & 63](mca_fat_ro, 7);
}
