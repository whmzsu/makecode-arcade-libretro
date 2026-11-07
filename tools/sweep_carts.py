"""sweep_carts.py — 把一批 .pxt64 逐张灌进内核跑固定帧数，汇总冒烟指标

为什么要有它：单张手点只能证明"这张能跑"，判断不了"这一批是不是都正常"。
核心最容易出的问题恰恰是**个别卡带**触发（堆不够、某个 API 没实现、线程没退），
所以需要一张表：谁没出画面、谁只出一帧、谁崩了。

用法：
  python tools/sweep_carts.py                                   # 默认 out/testcarts
  python tools/sweep_carts.py out/testcarts png_examples/*.pxt64 -n 600
  python tools/sweep_carts.py --core <core.dll|.so> carts/ --press A

判定（宁可报可疑，也不要静默放过）：
  ok        帧数跑满、独立画面 ≥ 3、非黑峰值 ≥ 1%
  static    独立画面 < 3（多半停在标题/菜单，或 VM 卡住不出帧）
  blank     非黑峰值 < 1%（黑屏）
  short     实际帧数 < 请求帧数（VM 提前退出/崩溃）
  panic     VM 打了 panic 但宿主进程活着、统计行照常（v0.3.2 起 panic 不再带崩
            前端；这种卡带通常是游戏自身的 bug，值得单独看是谁踩的）
  crash     宿主进程非零退出（真的崩了），或没有统计行
"""

from __future__ import annotations

import argparse
import glob
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
HOST = os.path.join(HERE, "libretro_host.py")

STAT_RE = re.compile(
    r"统计：video=(\d+) 帧（其中独立画面 (\d+) 帧，游戏真实帧率 ≈ ([\d.]+) fps）"
    r".{0,3}画面非黑峰值 (\d+)%", re.S)
CRASH_RE = re.compile(r"panic|Segmentation fault|access violation|Traceback", re.I)


def parse_stats(out: str) -> tuple[int, int, float, int] | None:
    """统计行里的逗号全角/半角混用（"fps）, 画面非黑"），分段取更稳。"""
    vid = re.search(r"video=(\d+) 帧", out)
    dis = re.search(r"独立画面 (\d+) 帧", out)
    fps = re.search(r"真实帧率 ≈ ([\d.]+) fps", out)
    blk = re.search(r"非黑峰值 (\d+)%", out)
    if not (vid and dis and blk):
        return None
    return (int(vid.group(1)), int(dis.group(1)),
            float(fps.group(1)) if fps else 0.0, int(blk.group(1)))


def pick_core(explicit: str | None) -> str:
    """默认取发行包里的核心：优先用"发出去的那个"，而不是本地随手编的。"""
    if explicit:
        return explicit
    for pat in ("core/dist/release/.stage/*/makecode_arcade_libretro.*",
                "out/**/makecode_arcade_libretro.*"):
        for path in glob.glob(os.path.join(HERE, "..", pat), recursive=True):
            if path.endswith((".dll", ".so", ".dylib")):
                return os.path.abspath(path)
    raise SystemExit("找不到核心产物，用 --core 指定")


def run_one(core: str, cart: str, frames: int, press: str | None,
            press_at: int, release: int | None, data_dir: str,
            turbo: bool = False) -> dict:
    cmd = [sys.executable, HOST, core, cart, str(frames), f"--data-dir={data_dir}"]
    if turbo:
        # qemu-user / 批量回归下按真实 27fps 节拍 sleep 会把一轮拖到十几分钟；
        # turbo 只是跳过节拍等待，不改变 VM 出帧语义（FINAL-FRAME-HASH 一致）。
        cmd.append("--turbo")
    if press:
        # 一律用 --opt=value：Git Bash 下 "--press-at 60" 这种空格形式会被
        # 宿主自己的参数解析漏掉，静默退回默认值（实测按在第 4 帧而不是 60）
        cmd += [f"--press={press}", f"--press-at={press_at}"]
        if release:
            cmd.append(f"--release={release}")
    proc = subprocess.run(cmd, capture_output=True, text=True,
                          encoding="utf8", errors="replace", timeout=300)
    out = proc.stdout + proc.stderr
    m = parse_stats(out)
    row = {
        "cart": os.path.basename(cart),
        "path": os.path.abspath(cart),
        "bytes": os.path.getsize(cart),
        "exit": proc.returncode,
        "frames": m[0] if m else 0,
        "distinct": m[1] if m else 0,
        "fps": m[2] if m else 0.0,
        "nonblack_pct": m[3] if m else 0,
        "hash": (re.search(r"FINAL-FRAME-HASH (\w+)", out) or [None, None])[1],
    }
    if proc.returncode != 0:
        row["verdict"] = "crash"
        row["note"] = f"宿主退出码 {proc.returncode:#x}"
    elif not m:
        row["verdict"] = "crash"
        row["note"] = "没有统计行（宿主中途退出）"
    elif CRASH_RE.search(out):
        # VM 打了 panic 但宿主活下来了 —— 与"把前端带崩"是两件事，分开计。
        pm = re.search(r"PANIC (\d+)", out)
        row["verdict"] = "panic"
        row["note"] = (f"VM panic {pm.group(1)}，宿主存活" if pm else "VM panic，宿主存活")
    elif row["frames"] < frames:
        row["verdict"] = "short"
        row["note"] = f"{row['frames']}/{frames} 帧"
    elif row["nonblack_pct"] < 1:
        row["verdict"] = "blank"
        row["note"] = f"非黑 {row['nonblack_pct']}%"
    elif row["distinct"] < 3:
        row["verdict"] = "static"
        row["note"] = f"独立画面 {row['distinct']}"
    else:
        row["verdict"] = "ok"
    return row


def main() -> int:
    ap = argparse.ArgumentParser(description="批量卡带冒烟")
    ap.add_argument("paths", nargs="*", help="卡带文件或目录（默认 out/testcarts）")
    ap.add_argument("--core", help="核心 .dll/.so（默认取发行包）")
    ap.add_argument("-n", "--frames", type=int, default=600, help="每张跑多少帧（默认 600）")
    ap.add_argument("--press", help="模拟按键（如 A / START），配合 --press-at")
    ap.add_argument("--press-at", type=int, default=60)
    ap.add_argument("--release", type=int, help="第 N 帧松开")
    ap.add_argument("--data-dir", default=os.path.join(HERE, "..", "out", "sweep_saves"))
    ap.add_argument("--turbo", action="store_true",
                    help="帧循环不 sleep（qemu-user / 批量回归用；默认保持真实节拍）")
    ap.add_argument("-o", "--json", default=os.path.join(HERE, "..", "out", "sweep.json"))
    args = ap.parse_args()

    carts: list[str] = []
    for p in (args.paths or [os.path.join(HERE, "..", "out", "testcarts")]):
        if os.path.isdir(p):
            carts += sorted(glob.glob(os.path.join(p, "*.pxt64")))
        else:
            carts += sorted(glob.glob(p))
    if not carts:
        raise SystemExit("没有匹配到 .pxt64")

    core = os.path.abspath(pick_core(args.core))
    # 一律用绝对路径：宿主会切到存档目录，相对路径在子进程里会失效
    carts = [os.path.abspath(c) for c in carts]
    args.data_dir = os.path.abspath(args.data_dir)
    os.makedirs(args.data_dir, exist_ok=True)
    print(f"核心: {core}\n卡带: {len(carts)} 张，每张 {args.frames} 帧"
          f"{f'，第 {args.press_at} 帧按 {args.press}' if args.press else ''}\n")

    rows = []
    for c in carts:
        r = run_one(core, c, args.frames, args.press, args.press_at, args.release,
                    args.data_dir, args.turbo)
        rows.append(r)
        print(f"  {r['verdict']:7s} {r['cart'][:38]:40s} "
              f"帧 {r['frames']:4d}  独立 {r['distinct']:4d}  "
              f"非黑 {r['nonblack_pct']:3d}%  {r['fps']:5.1f}fps"
              f"  {r.get('note','')}")

    with open(args.json, "w", encoding="utf8") as f:
        json.dump(rows, f, indent=1, ensure_ascii=False)

    bad = [r for r in rows if r["verdict"] != "ok"]
    print(f"\n合计 {len(rows)} 张：ok {len(rows) - len(bad)}，异常 {len(bad)}"
          f"  -> {os.path.abspath(args.json)}")
    for r in bad:
        print(f"  ! {r['verdict']:7s} {r['cart']}  {r.get('note','')}")
    return 0 if not bad else 1


if __name__ == "__main__":
    sys.exit(main())
