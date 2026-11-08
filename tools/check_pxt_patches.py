#!/usr/bin/env python3
"""check_pxt_patches.py — 校验 runtime/pxtapp 里 9 个 pxt 上游补丁是否都在

## 为什么需要这个脚本

runtime/pxtapp 是 vendored 的 pxt 运行时（MIT，来源见 runtime/VENDORING.md）。
我们为了让它在 libretro 前端里活下来改了 9 处：换游戏、重置、卸载、重启后
音频、以及"同一输入的两次运行必须逐帧一致"。

这些改动**直接落在源码里**（没有独立 patch 文件，构建时也不再重放），所以
只有一个隐形的约束在保护它们：没人会把整棵树换成上游版本。偏偏升级 pxt 时
最自然的动作就是"重新生成 pxtapp"——那一刻 9 个补丁全部消失，而代码照样编
得过、`contract_test` 照样全绿，退化要到真机上换游戏才暴露（而且是崩溃，不是
报错）。这个脚本把"补丁还在"变成一条可执行的断言。

## 用法

    python tools/check_pxt_patches.py           # 校验；缺哪条报哪条
    python tools/check_pxt_patches.py --list    # 只列清单，不检查
    python tools/check_pxt_patches.py --strict  # 额外检查有没有未登记的 PATCH 编号

退出码：0 全在 / 1 有缺失 / 2 环境问题（树不存在）

## 判定方式

每条补丁 = 若干条「文件 + 正则」正向断言（必须出现），加上可选的反向断言
（上游写错的东西不该再出现）。正向断言一律挑**补丁独有**的代码特征，不用
泛泛的函数名——`pxt_vm_online` 这种名字可能上游日后自己加了，判不出来。
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
PXTAPP = os.path.join(ROOT, "runtime", "pxtapp")

# ── 补丁清单 ────────────────────────────────────────────────────────────
# 编号沿用开发过程中的说法（PATCH 4 已被 PATCH 10 取代，所以不是 1..9 连续）：
# 上游 initRandomSeed 的 fd=-1 崩溃在 PATCH 10 换成"确定性种子"时一起消失了，
# 不再需要单独的修补。RETIRED 里保留编号占位，免得日后有人翻出旧文档对不上。
PATCHES = [
    {
        "no": 1,
        "title": "find_src_map() 64 位指针截断",
        "impact": "镜像尾部 SRCM 源映射找不到，VM panic 时没有函数名/行号",
        "checks": [
            # 掩码必须在 uintptr_t 上做；上游是 (uint32_t)vmImg->dataEnd
            ("core---vm/vm.cpp", r"\(uintptr_t\)vmImg->dataEnd\s*&\s*~\(uintptr_t\)0xf"),
            ("core---vm/vm.cpp", r"0x4d435253"),          # SRCM 魔数仍在扫
        ],
        "forbid": [
            ("core---vm/vm.cpp", r"\(uint32_t\)vmImg->dataEnd"),
        ],
    },
    {
        "no": 2,
        "title": "pointers.cpp 生成物的 aspxt:: 笔误",
        "impact": "编不过（codegen 把 pxt::asRefCollection 拼成了 aspxt::）",
        "checks": [
            ("pointers.cpp", r"asRefCollection\(ctx->"),
        ],
        "forbid": [
            ("pointers.cpp", r"aspxt::"),
        ],
    },
    {
        "no": 3,
        "title": "VM 线程栈大小显式指定",
        "impact": "pthread_create(attr=NULL) 取宿主 exe 的栈预留；RetroArch 只有 2MB，"
                  "VM 初始化阶段栈溢出、进程无声消失",
        "checks": [
            ("core---vm/vmload.cpp", r"#define MCA_VM_STACK_SIZE"),
            ("core---vm/vmload.cpp", r"pthread_attr_setstacksize\(&vm_attr,\s*MCA_VM_STACK_SIZE\)"),
            ("core---vm/vmload.cpp", r"pthread_create\(&vm_thread,\s*&vm_attr,"),
        ],
        "forbid": [
            # 退回 attr=NULL 就等于补丁没了（上游写法）
            ("core---vm/vmload.cpp", r"pthread_create\(&vm_thread,\s*NULL,"),
        ],
    },
    {
        "no": 5,
        "title": "宿主可请求重开这一局（reset 键 / 前端 Restart）",
        "impact": "重置键无效；systemReset 的清理序列没人触发，重开必崩",
        "checks": [
            # 宿主线程置位、VM 线程在主循环里看到就退出
            ("core---vm/scheduler.cpp", r"volatile int mcaResetRequest = 0"),
            ("core---vm/scheduler.cpp", r"if \(mcaResetRequest\)"),
            ("core---vm/scheduler.cpp", r"MCA PATCH 5"),
            ("core---vm/vmload.cpp", r"mcaResetRequest = 1"),
            ("core---vm/vmload.cpp", r"MCA PATCH 5（本项目）：宿主侧的"),
        ],
        "forbid": [],
    },
    {
        "no": 6,
        "title": "GC 块清单/globals 在重启时清空",
        "impact": "重启后第一次 gc() 拿上一局的 globals 去 mark 已复用内存 → oops(41) 递归",
        "checks": [
            ("base/gc.cpp", r"void gcForgetAllBlocks\(\)"),
            ("base/gc.cpp", r"firstBlock\s*=\s*NULL"),
            ("base/gc.cpp", r"globals\s*=\s*NULL"),
            ("pxtbase.h", r"gcForgetAllBlocks\(\)"),
            ("core---vm/scheduler.cpp", r"gcForgetAllBlocks\(\)"),
        ],
        "forbid": [],
    },
    {
        "no": 7,
        "title": "取帧等待有上限 + VM 在线标志",
        "impact": "WDisplay::update 无超时等前端取帧 × 前端重置期间不拉帧 = 死锁；"
                  "离线间隙前端还会访问已释放的单例",
        "checks": [
            ("screen---ext/screen.cpp", r"DLLEXPORT void pxt_screen_shutdown"),
            ("screen---ext/screen.cpp", r"MCA PATCH 7"),
            ("core---vm/scheduler.cpp", r"DLLEXPORT void pxt_vm_set_online"),
            ("core---vm/vmload.cpp", r"pxt_vm_set_online\(0\)"),
        ],
        "forbid": [],
    },
    {
        "no": 8,
        "title": "unload/deinit 里真正停掉 VM 线程",
        "impact": "前端 dlclose 后 VM 线程还在跑已 unmap 的代码段 → 关游戏即崩前端",
        "checks": [
            ("core---vm/vmload.cpp", r"DLLEXPORT int pxt_vm_stop\(\)"),
            ("core---vm/vmload.cpp", r"pthread_join\(vm_thread,\s*&dummy\)"),
            ("core---vm/vmload.cpp", r"MCA PATCH 8"),
        ],
        "forbid": [],
    },
    {
        "no": 9,
        "title": "清理时把 static 单例指针置 NULL",
        "impact": "GC 堆整体作废但 static 指针还在指旧地址 → 重启后彻底没声音，"
                  "且旧指针被复用时按错误类型写内存",
        "checks": [
            ("mixer---ext/melody.cpp", r"instWSynthesizer\s*=\s*NULL"),
            ("mixer---ext/melody.cpp", r"MCA PATCH 9"),
            ("mixer---ext/sound.cpp", r"dac\s*=\s*NULL"),
            ("mixer---ext/sound.cpp", r"DLLEXPORT void pxt_audio_shutdown"),
        ],
        "forbid": [],
    },
    {
        "no": 10,
        "title": "确定性随机种子 + 虚拟时钟",
        "impact": "种子取墙钟/栈地址 → 同一卡带两次运行第 1 帧就分叉，"
                  "存档、回放、golden hash 回归全部失效",
        "checks": [
            ("core---vm/platform.cpp", r"MCA PATCH 10"),
            ("core---vm/platform.cpp", r"infoHeader->hexHash"),
            ("core---vm/vmload.cpp", r"seedRandomFromImage\(\)"),
            ("core---vm/scheduler.cpp", r"MCA PATCH 10"),
        ],
        "forbid": [
            # 墙钟种子（PATCH 10 之前的老做法）
            ("core---vm/platform.cpp", r"GetTickCount64\(\)\s*\^"),
        ],
    },
]

# 已退役：编号留着，防止旧文档/旧日志里的 PATCH 4 对不上号
RETIRED = {
    4: "initRandomSeed 对 fd=-1 调 read()（Windows 上 __fastfail 崩进程）——"
       "整个函数在 PATCH 10 里换成\"种子 = 镜像 hexHash\"，崩溃点一并消失",
}


def read(rel):
    path = os.path.join(PXTAPP, rel)
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return None


def main():
    argv = sys.argv[1:]
    if "--list" in argv:
        print(f"{len(PATCHES)} 个在册补丁（runtime/pxtapp，vendored pxt 运行时）：\n")
        for p in PATCHES:
            print(f"  PATCH {p['no']:<2}  {p['title']}")
            print(f"            后果：{p['impact']}")
        print("\n已退役：")
        for no, why in sorted(RETIRED.items()):
            print(f"  PATCH {no:<2}  {why}")
        return 0

    if not os.path.isdir(PXTAPP):
        print(f"找不到 {PXTAPP}", file=sys.stderr)
        return 2

    cache = {}
    strict = "--strict" in argv
    missing = 0

    print("PATCH  状态  补丁")
    print("-" * 72)
    for p in PATCHES:
        problems = []
        for rel, pat in p["checks"]:
            if rel not in cache:
                cache[rel] = read(rel)
            text = cache[rel]
            if text is None:
                problems.append(f"{rel}: 文件不存在")
            elif not re.search(pat, text):
                problems.append(f"{rel}: 缺少特征 /{pat}/")
        for rel, pat in p["forbid"]:
            if rel not in cache:
                cache[rel] = read(rel)
            text = cache[rel]
            if text is not None and re.search(pat, text):
                problems.append(f"{rel}: 仍残留上游写法 /{pat}/")

        if problems:
            missing += 1
            print(f"{p['no']:<6} 缺失  {p['title']}")
            for msg in problems:
                print(f"        · {msg}")
            # 失败原因平时在折叠日志里看不到，转成注解（公开仓库可匿名读）
            print(f"::error file=runtime/pxtapp::PATCH {p['no']} 缺失——{p['title']}"
                  f"（后果：{p['impact']}）")
        else:
            print(f"{p['no']:<6} 在    {p['title']}")

    # --strict：树上出现了没登记的编号，说明有人加了补丁却没进这张表，
    # 下次重放时它不会被想起——这正是这类隐形约束最典型的死法
    if strict:
        known = {p["no"] for p in PATCHES} | set(RETIRED)
        seen = set()
        for dirpath, _dirs, files in os.walk(PXTAPP):
            for fn in files:
                if not fn.endswith((".cpp", ".h")):
                    continue
                rel = os.path.relpath(os.path.join(dirpath, fn), PXTAPP)
                if rel not in cache:
                    cache[rel] = read(rel)
                for no in re.findall(r"MCA PATCH (\d+)", cache[rel] or ""):
                    seen.add(int(no))
        extra = sorted(seen - known)
        if extra:
            print()
            for no in extra:
                print(f"::error file=runtime/pxtapp::树上出现未登记的 MCA PATCH {no}"
                      f"，请补进 tools/check_pxt_patches.py 的 PATCHES")
            missing += len(extra)

    print("-" * 72)
    if missing:
        print(f"{missing} 处缺失。补丁内容见 docs/M3-VM-HOST.md"
              f"（PATCH 1~5 的重放脚本在 cartconv 仓库 vm-build/build_vmapp.sh，"
              f"6~10 只有文档描述）。")
        return 1
    print(f"全部 {len(PATCHES)} 个补丁在位。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
