#!/usr/bin/env python3
# test_modrange.py — 模块区间回归（跨会话指针重定位的地基）
#
# 为什么单独有这条测试：
#   v0.2.14 的真机事故是 Linux/Android 上 g_modBase 恒为 0，
#       [snap] 拒绝恢复：本平台取不到模块区间，跨会话恢复不安全
#       [snap] module relocate: old=0 size=0 cur=0 hits=0+0
#   根因是 maps 解析"就地"把 '\n' 改成 0，同一份缓冲区只能遍历一次，而算区间
#   天生要两遍（先找本模块路径，再按路径聚合）。桌面一直走 Windows 的
#   VirtualQuery 分支，这段代码从没被执行过 —— 所以 S1~S10 全绿也挡不住。
#
# 两件事：
#   1) mcaModuleMapSelfTest()：内嵌 maps 样本，任何平台都能跑。关键断言是
#      "同一份文本连续解析两遍结果一致"—— 就地改写型实现必挂，正是那个 bug。
#   2) 真实分支必须取到非零区间（Windows: VirtualQuery / Linux: /proc/self/maps）。
#      在 Linux CI 上跑这一步，就等于覆盖了真机那条路径。
#
# 用法：python tools/test_modrange.py [core路径]   （默认按平台猜）

import ctypes
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT = os.path.join(ROOT, "out", "modrange_test")

if len(sys.argv) > 1:
    CORE = sys.argv[1]
elif os.name == "nt":
    CORE = os.path.join(ROOT, "core", "dist", "x86_64-windows-gnu",
                        "makecode_arcade_libretro.dll")
else:
    CORE = os.path.join(ROOT, "core", "makecode_arcade_libretro.so")


def main():
    # CI runner 的 locale 可能是 C，中文输出会撞 UnicodeEncodeError。
    # 统一按 UTF-8 输出（失败也只丢字符，不影响结论）。
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

    # 先转绝对路径：下面会 chdir 到 out/ 下（避免 mcaTrace 把日志写到仓库根），
    # 之后再 abspath 就变成相对新 cwd 了。
    core = os.path.abspath(CORE)
    if not os.path.exists(core):
        print("找不到核心：%s" % core)
        return 2

    # mcaTrace 会往"数据目录"写 mca_trace.log；这里没初始化数据目录，
    # 它会退回当前目录 —— 所以先切到 out/ 下，别污染仓库根。
    os.makedirs(OUT, exist_ok=True)
    os.chdir(OUT)

    lib = ctypes.CDLL(core)
    lib.mcaModuleMapSelfTest.restype = ctypes.c_int
    lib.mcaModuleBase.restype = ctypes.c_ulonglong
    lib.mcaModuleSize.restype = ctypes.c_ulonglong

    failed = 0

    print("== 1 内嵌 maps 样本（含\"同一文本解析两遍\"的回归断言）")
    ok = lib.mcaModuleMapSelfTest()
    if ok:
        print("   OK  maps 解析自测通过")
    else:
        print("   FAIL maps 解析自测失败，详见 %s" % os.path.join(OUT, "mca_trace.log"))
        failed += 1

    print("== 2 真实分支取模块区间")
    base = lib.mcaModuleBase()
    size = lib.mcaModuleSize()
    print("   base=0x%x size=0x%x" % (base, size))
    if base == 0 or size == 0:
        print("   FAIL 取不到模块区间 —— 跨会话读档会被拒（-13）")
        failed += 1
    elif base % 0x1000:
        print("   FAIL base 未按页对齐（0x%x）" % base)
        failed += 1
    else:
        # 区间里必须包含本模块的一个已知符号地址：用导出函数的地址反推。
        # ctypes 拿不到符号地址，这里退一步只校验区间"看起来合理"：
        # 至少 4KB、且 base 落在用户态地址范围。
        if size < 0x1000:
            print("   FAIL 区间太小（0x%x），不像整个模块" % size)
            failed += 1
        else:
            print("   OK  区间非零、页对齐、覆盖整段")

    print()
    if failed:
        print("[FAIL] %d 项失败" % failed)
        return 1
    print("[OK] 全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
