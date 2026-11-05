"""分析 muOS 稳定性长跑 CSV，输出每段汇总 + 泄漏斜率 + 卡顿窗口。

用法: python tools/muos/analyze_stab.py stab.csv [stab.log] [logdir]
CSV 列: ts,phase,event,rss_kb,hwm_kb,cputicks,temp_c,load1,progress

注意：设备时钟可能在测试中途被校时（本机实测从 23:57 跳回 22:09），
CSV 的 ts 列因此不可用于算时长。时长优先取 <phase>.log 里宿主脚本自己
报的 "X fps 实际节拍"（它用单调时钟），取不到才退回"样本数 × 采样间隔"。
"""

import os
import re
import statistics
import sys
from collections import OrderedDict

SAMPLE_SEC = 10


def load(path):
    rows = []
    with open(path) as f:
        head = f.readline()
        if "phase" not in head:
            raise SystemExit("unexpected header: " + head)
        for line in f:
            p = line.rstrip("\n").split(",")
            if len(p) < 9:
                continue
            rows.append(p)
    return rows


def to_int(v, default=0):
    try:
        return int(v)
    except (TypeError, ValueError):
        return default


def phase_fps(logdir, ph):
    """从宿主日志取真实节拍（单调时钟，不受系统校时影响）。"""
    if not logdir:
        return None
    path = os.path.join(logdir, ph + ".log")
    if not os.path.exists(path):
        return None
    txt = open(path, errors="replace").read()
    m = re.findall(r"([\d.]+) fps 实际节拍", txt)
    return float(m[-1]) if m else None


def main():
    csv_path = sys.argv[1]
    stab_log = sys.argv[2] if len(sys.argv) > 2 else None
    logdir = sys.argv[3] if len(sys.argv) > 3 else os.path.dirname(csv_path)
    rows = load(csv_path)

    phases = OrderedDict()
    for r in rows:
        phases.setdefault(r[1], []).append(r)

    print("%-10s %8s %8s %8s %9s %8s %7s %7s  %s" % (
        "phase", "frames", "dur_s", "avg_fps", "rss_MB", "slope", "hwm_MB", "temp", "src"))
    print("-" * 79)

    total_frames = 0
    total_dur = 0
    problems = []

    for ph, rs in phases.items():
        samples = []
        frames = 0
        for r in rs:
            ev = r[2]
            if ev.startswith("end_rc_"):
                rc = ev.split("end_rc_")[1]
                if rc != "0":
                    problems.append("%s: rc=%s" % (ph, rc))
            elif ev == "sample":
                samples.append(r)
                frames = max(frames, to_int(r[8]))

        fps0 = phase_fps(logdir, ph)
        if fps0:
            dur = frames / fps0
            src = "log"
        else:
            dur = len(samples) * SAMPLE_SEC
            fps0 = frames / dur if dur else 0
            src = "est"

        fps = frames / dur if dur else 0
        rss = [to_int(s[3]) for s in samples if to_int(s[3]) > 0]
        hwm = [to_int(s[4]) for s in samples if to_int(s[4]) > 0]
        temp = [to_int(s[6]) for s in samples if to_int(s[6]) > 0]

        if rss and dur:
            # 用后 80% 样本做线性拟合，避开启动爬坡
            n0 = int(len(rss) * 0.2)
            seg = rss[n0:] or rss
            if len(seg) >= 2:
                slope = (seg[-1] - seg[0]) / len(seg) * 6  # 每 10s 采样 -> KB/min
            else:
                slope = 0
            slope_mb = slope / 1024
        else:
            slope_mb = 0

        print("%-10s %8d %8.0f %8.1f %9.1f %8.2f %7.1f %7d  %s" % (
            ph, frames, dur, fps,
            (rss[-1] / 1024) if rss else 0, slope_mb,
            (max(hwm) / 1024) if hwm else 0, max(temp) if temp else 0, src))

        total_frames += frames
        total_dur += dur or 0

        # 卡顿：progress 增量的中位数 vs 最小窗口（排除起止边界样本）
        deltas = []
        for a, b in zip(samples, samples[1:]):
            d = to_int(b[8]) - to_int(a[8])
            if d >= 0:
                deltas.append(d)
        if len(deltas) >= 3:
            mid = statistics.median(deltas)
            worst = min(deltas)
            if mid > 0 and worst < mid * 0.4:
                problems.append("%s: 卡顿窗口 %d 帧 (中位 %d，时长可能被 suspend 打断)"
                                % (ph, worst, mid))
            if worst < 0:
                problems.append("%s: progress 回退（进程重启？）" % ph)

    print("-" * 79)
    print("TOTAL %d 帧 / %d 秒 (%.1f 分钟)，均值 %.1f fps" % (
        total_frames, total_dur, total_dur / 60, total_frames / total_dur if total_dur else 0))

    if len(sys.argv) > 2:
        log = open(sys.argv[2]).read()
        for key in ("PANIC", "Traceback", "core dumped"):
            if key in log:
                print("[log] 出现 %s" % key)
        print("[log] governor:", "OK" if "performance" in log else "?")
        if "governor restored" not in log:
            print("[log] 警告：governor 未还原（脚本可能被中断）")

    if problems:
        print("\n需要关注：")
        for p in problems:
            print(" -", p)
    else:
        print("\n无异常：无 rc!=0、无卡顿窗口、progress 单调。")


if __name__ == "__main__":
    main()
