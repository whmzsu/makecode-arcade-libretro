#!/usr/bin/env python3
"""test_lifecycle.py — 内容生命周期回归（换游戏 / 关闭游戏 / 重置后音频）

这三个场景都要求前端在**卸载或重启**时把 VM 线程干净地收掉，否则前端会崩：

  * 关闭游戏：retro_unload_game → retro_deinit → dlclose(核心库)。
    核心库被 unmap 后 VM 线程若还在跑里面的代码 = SIGSEGV。
  * 换游戏：unload → load 另一张卡带，第二张必须能正常出画面。
  * 重置：GC 堆整体作废，单例指针（instWSynthesizer）必须一起丢掉，
    否则重启后 dac 一直是 NULL —— 现象是"reset 之后没声音"。

用法：
  python tools/test_lifecycle.py [--core PATH]

判据都是硬断言（退出码非 0 即失败）：
  1. 换游戏后第二张卡带要产出新帧；
  2. 关闭游戏后 dlclose 不崩（进程必须正常退出）；
  3. 重置后音频采样峰值必须重新 > 0，且 WSynthesizer 被重新构造。
"""

import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
HOST = os.path.join(HERE, "libretro_host.py")

DEFAULT_CORE = {
    "win32": "core/dist/x86_64-windows-gnu/makecode_arcade_libretro.dll",
    "linux": "core/dist/x86_64-linux-gnu/makecode_arcade_libretro.so",
    "darwin": "core/dist/x86_64-apple-darwin/makecode_arcade_libretro.dylib",
}

TRACE_CANDIDATES = [
    # libretro_host.py 默认把 saves/trace 落到 out/ 下（不再污染项目根目录）
    os.path.join(ROOT, "out", "libretro_host_saves", "makecode_arcade", "mca_trace.log"),
    os.path.join(ROOT, "libretro_host_saves", "makecode_arcade", "mca_trace.log"),
    os.path.join(ROOT, "mca_trace.log"),
    "/tmp/mca_trace.log",
]


def core_path():
    for i, a in enumerate(sys.argv[1:]):
        if a == "--core" and i + 2 <= len(sys.argv) - 1:
            return sys.argv[i + 2]
        if a.startswith("--core="):
            return a.split("=", 1)[1]
    rel = DEFAULT_CORE.get(sys.platform)
    if not rel:
        raise SystemExit(f"未知平台 {sys.platform}，请用 --core 指定核心库")
    return os.path.join(ROOT, rel)


def run(core, args, timeout=180):
    cmd = [sys.executable, HOST, core] + args
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout,
                       cwd=ROOT)
    return p.returncode, (p.stdout or "") + (p.stderr or "")


def reset_trace():
    for f in TRACE_CANDIDATES:
        try:
            os.remove(f)
        except OSError:
            pass


def read_trace():
    for f in TRACE_CANDIDATES:
        if os.path.isfile(f):
            with open(f, encoding="utf8", errors="replace") as fh:
                return fh.read()
    return ""


def audio_windows(out):
    """心跳行里的 audio_peak(近30帧)=N —— 静音窗口是 0。"""
    return [int(m) for m in re.findall(r"audio_peak\(近30帧\)=(\d+)", out)]


def main():
    core = core_path()
    if not os.path.isfile(core):
        raise SystemExit(f"核心不存在: {core}")
    probe = os.path.join(ROOT, "out", "pxt64", "probe.pxt64")
    audio = os.path.join(ROOT, "out", "pxt64", "audio_probe.pxt64")
    ping = os.path.join(ROOT, "out", "pxt64", "ping.pxt64")
    cubeland = os.path.join(ROOT, "out", "pxt64", "cubeland.pxt64")
    jumpy = os.path.join(ROOT, "out", "pxt64", "jumpy.pxt64")
    for f in (ping, cubeland, jumpy, audio):
        if not os.path.isfile(f):
            raise SystemExit(f"缺卡带: {f}（先跑 tools/png2pxt64.py / make_probe_cartridge.py --audio）")

    fails = []

    # ---- 1. 换游戏：ping -> cubeland -> jumpy -> ping，再 dlclose ----
    # 连续换多张才会暴露"上一次没停干净"的问题（真机上玩家会反复切游戏）
    reset_trace()
    code, out = run(core, [ping, "60",
                           f"--switch={cubeland},{jumpy},{ping}", "--dlclose"])
    got = [int(x) for x in re.findall(r"第 \d+ 张卡带产出 (\d+) 帧", out)]
    ok = code == 0 and len(got) == 3 and all(g > 0 for g in got)
    print(f"[{'ok' if ok else 'FAIL'}] 换游戏 3 连（ping→cubeland→jumpy→ping）："
          f"各张产出 {got} 帧，退出码 {code}")
    if not ok:
        fails.append("换游戏")
        print(out[-1500:])

    # ---- 2. 关闭游戏：跑一会 -> unload/deinit -> dlclose ----
    code, out = run(core, [ping, "45", "--dlclose"])
    ok = code == 0 and "dlclose OK" in out
    print(f"[{'ok' if ok else 'FAIL'}] 关闭游戏（unload+deinit+dlclose）：退出码 {code}")
    if not ok:
        fails.append("关闭游戏")
        print(out[-1500:])

    # ---- 3. 重置后音频：两次重置，每次之后峰值必须重新 > 0 ----
    reset_trace()
    code, out = run(core, [audio, "240", "--press=SELECT", "--press-at=60,140"])
    peaks = audio_windows(out)
    trace = read_trace()
    dac_count = trace.count("ExtDAC created")
    # 第 1 个窗口在启动阶段，之后每个窗口都必须有声
    silent = [p for p in peaks[1:] if p == 0]
    ok = code == 0 and len(peaks) >= 6 and not silent and dac_count >= 3
    print(f"[{'ok' if ok else 'FAIL'}] 重置后音频：峰值序列 {peaks}，"
          f"ExtDAC 重建 {dac_count} 次（应为 3 = 启动 + 2 次重置）")
    if not ok:
        fails.append("重置后音频")
        print(out[-1500:])

    # ---- 4. panic 自愈：VM 线程 panic 后前端必须把游戏重新拉起来 ----
    #
    # 真机上的"加载存档卡死"就是这条链断掉：panic → systemReset → pthread_exit
    # 绕过 multiStart 的收尾，mcaVmExited 永不置位 → 前端以为 VM 还活着 →
    # 画面冻在最后一帧，进程不崩也不退。桌面（Mingw）上则是直接 0xC0000005。
    #
    # 判据：
    #   * 进程退出码 0（不崩）；
    #   * trace 里出现 "after exec_loop -> cleanup and exit"（panic 走主循环收尾，
    #     不是 pthread_exit）；
    #   * 出现 "auto restart (1/3)" + "starting fresh vm"（前端确实重启了）；
    #   * 出现 "auto restart limit reached"（连续崩溃有上限，不会无限重启）。
    panic_cart = os.path.join(ROOT, "out", "pxt64", "panic.pxt64")
    if os.path.isfile(panic_cart):
        reset_trace()
        code, out = run(core, [panic_cart, "400", "--turbo"])
        trace = read_trace()
        ok = (code == 0
              and "after exec_loop -> cleanup and exit" in trace
              and "auto restart (1/3)" in trace
              and "starting fresh vm" in trace
              and "auto restart limit reached" in trace)
        print(f"[{'ok' if ok else 'FAIL'}] panic 自愈：退出码 {code}，"
              f"自动重启 {trace.count('starting fresh vm')} 次")
        if not ok:
            fails.append("panic 自愈")
            print(out[-1500:])
            print(trace[-2000:])
    else:
        print("[skip] panic 自愈（缺 out/pxt64/panic.pxt64，"
              "先跑 tools/make_probe_cartridge.py --panic）")

    # ---- 5. 描述符/选项自检（顺带）----
    code, out = run(core, [ping, "--dry"])
    ok = code == 0 and "8 条描述符 / 0 个选项" in out
    print(f"[{'ok' if ok else 'FAIL'}] 输入配置自检：退出码 {code}")
    if not ok:
        fails.append("输入自检")

    print("-" * 60)
    if fails:
        print("失败：" + "、".join(fails))
        return 1
    print("生命周期回归全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
