#!/usr/bin/env python3
"""test_buttons.py — 按键映射自动化回归

原理：out/pxt64/probe.pxt64（tools/make_probe_cartridge.py 生成）会把每个
按键事件的屏幕背景刷成固定颜色。本脚本用 libretro_host.py 逐个按键跑一遍，
抓末帧比对中心像素颜色，从而断言「RetroPad 物理键 → MakeCode 按钮」的映射。

比看画面可靠：颜色是离散的，能精确到"按 A 变成了按 B 的颜色"这种错位。

用法：
  python tools/test_buttons.py [--core 路径] [--probe 路径] [--frames N]

退出码：0 全部通过；1 有用例失败；2 缺产物
"""

import argparse
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# arcade 默认调色板（实测 RGB，见 tools/make_probe_cartridge.py 注释）
COLORS = {
    "white": (255, 255, 255),   # 1 启动底色
    "red": (255, 33, 33),       # 2
    "pink": (255, 147, 196),    # 3
    "orange": (255, 129, 53),   # 4
    "yellow": (255, 246, 9),    # 5
    "teal": (36, 156, 163),     # 6
    "green": (120, 220, 82),    # 7
    "blue": (0, 63, 173),       # 8
    "ltblue": (135, 242, 255),  # 9
}

# 探针里各键对应的颜色
KEY_COLOR = {
    "A": "red", "B": "yellow", "UP": "green", "DOWN": "orange",
    "LEFT": "blue", "RIGHT": "ltblue", "START": "teal", "SELECT": "teal",
}

# (说明, 按键, 额外参数, 期望颜色)
# 映射是写死的：A/B/方向/START=游戏内菜单/SELECT=重置，换键由玩家在前端改。
# 本核心**没有核心选项**，所以这里不能再有 --opt= 用例。
# 注意：单独的"SELECT 后回白底"判据偏弱 —— 画面冻住不动也是白底。真正的
# 重启证明是"重置后再按键还能变色"（重置后 A 应刷红）那条用例。
# 额外参数里可用 "FRAMES=n" 覆盖默认帧数（长时序用例需要）。
CASES = [
    ("无输入（启动底色）",           None,     [],                              "white"),
    ("A -> MakeCode A",              "A",      [],                              "red"),
    ("B -> MakeCode B",              "B",      [],                              "yellow"),
    ("UP -> MakeCode Up",            "UP",     [],                              "green"),
    ("DOWN -> MakeCode Down",        "DOWN",   [],                              "orange"),
    ("LEFT -> MakeCode Left",        "LEFT",   [],                              "blue"),
    ("RIGHT -> MakeCode Right",      "RIGHT",  [],                              "ltblue"),
    ("START -> Game Menu",           "START",  [],                              "teal"),
    ("SELECT -> Reset（重启回白底）", "SELECT", [],                              "white"),
    ("未映射键 X 无效",               "X",      [],                              "white"),
    ("未映射键 Y 无效",               "Y",      [],                              "white"),
    ("未映射键 L 无效",               "L",      [],                              "white"),
    ("位掩码路径: 按 B",              "B",      ["--bitmask"],                   "yellow"),
    ("位掩码: SELECT 重启",           "SELECT", ["--bitmask"],                   "white"),
    # 重启真·存活证明：先按 SELECT 重启（70 帧），再按 A（120 帧）——
    # 如果 VM 没有重启成功（重置后冻住），A 不会生效，画面仍是白底。
    ("重置后 A 仍生效（VM 存活）",    "SELECT", ["--press-at=70", "FRAMES=170",
                                                 "--seq=A@120"],                 "red"),
    ("重置后连按两次重置仍存活",       "SELECT", ["--press-at=40,80", "FRAMES=200",
                                                 "--seq=A@140"],                 "red"),
]


def center_rgb(bmp_path):
    with open(bmp_path, "rb") as f:
        d = f.read()
    off = struct.unpack_from("<I", d, 10)[0]
    w, h = struct.unpack_from("<ii", d, 18)
    bpp = struct.unpack_from("<H", d, 28)[0]
    row = ((w * bpp // 8) + 3) // 4 * 4
    i = off + (abs(h) // 2) * row + (w // 2) * (bpp // 8)
    return (d[i + 2], d[i + 1], d[i])      # BMP 里是 BGR


def nearest(rgb):
    best, dist = None, 1 << 30
    for name, ref in COLORS.items():
        dd = sum(abs(rgb[i] - ref[i]) for i in range(3))
        if dd < dist:
            best, dist = name, dd
    return best, dist


def run_case(core, probe, frames, press, extra, tmpdir, idx):
    extra = list(extra)
    # 允许用例用 FRAMES=n 覆盖全局帧数（多键时序用例需要更长的时间轴）
    for i, e in enumerate(list(extra)):
        if e.startswith("FRAMES="):
            frames = int(e.split("=", 1)[1])
            extra.pop(i)
            break
    bmp = os.path.join(tmpdir, f"case{idx}.bmp")
    cmd = [sys.executable, os.path.join(HERE, "libretro_host.py"),
           core, probe, str(frames), f"--bmp={bmp}"] + extra
    if press:
        cmd.append(f"--press={press}")
    proc = subprocess.run(cmd, capture_output=True, text=True, encoding="utf8",
                          errors="replace")
    if not os.path.isfile(bmp):
        return None, (proc.stdout or "")[-800:] + (proc.stderr or "")[-800:]
    return center_rgb(bmp), None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--core", default=os.path.join(ROOT, "core", "makecode_arcade_libretro.dll"))
    ap.add_argument("--probe", default=os.path.join(ROOT, "out", "pxt64", "probe.pxt64"))
    ap.add_argument("--frames", type=int, default=40)
    args = ap.parse_args()

    for p, hint in [(args.core, "先跑 tools/build_core.sh"),
                    (args.probe, "先跑 python tools/make_probe_cartridge.py")]:
        if not os.path.isfile(p):
            print(f"缺少 {p}\n  {hint}")
            return 2

    print(f"core  : {args.core}")
    print(f"probe : {args.probe}")
    print(f"{'用例':<28} {'期望':<8} {'实际':<8} 结果")
    print("-" * 66)

    fails = 0
    with tempfile.TemporaryDirectory() as tmpdir:
        for i, (desc, press, extra, expect) in enumerate(CASES):
            rgb, err = run_case(args.core, args.probe, args.frames, press, extra, tmpdir, i)
            if err is not None:
                print(f"{desc:<28} {expect:<8} {'--':<8} FAIL（跑不起来）")
                print(err)
                fails += 1
                continue
            got, dist = nearest(rgb)
            ok = got == expect
            mark = "ok" if ok else "FAIL"
            print(f"{desc:<28} {expect:<8} {got:<8} {mark}"
                  f"{'' if ok else f'   rgb={rgb} 距离={dist}'}")
            if not ok:
                fails += 1

    print("-" * 66)
    total = len(CASES)
    print(f"{total - fails}/{total} 通过")
    return 0 if fails == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
