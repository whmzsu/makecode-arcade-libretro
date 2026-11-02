#!/usr/bin/env python3
# test_savestate.py — retro_serialize 快照回归（确定性 / 收敛 / 拒绝路径）
#
# 前提：帧驱动虚拟时钟（v0.2.10）使核心完全确定——同一输入序列 + 同一快照
# ⇒ 精确同一画面。测试全部用 --turbo（不睡帧），确定性由时钟保证。
#
# 场景：
#   S1 确定性：两次完全相同的跑，末帧哈希必须一致
#   S2 收敛：  分叉历史（有无按键）在 240 帧汇合到同一快照，其后 120 帧
#              必须精确同画面；无恢复的对照跑必须不同
#   S3 跨卡带：ping 的快照灌进 jumpy 会话 → 必须拒绝且游戏继续正常
#   S4 垃圾：  随机字节 → 必须拒绝且游戏继续正常
#
# 用法：python tools/test_savestate.py [core路径]   （默认按平台猜）

import os
import random
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
HOST = os.path.join(HERE, "libretro_host.py")
OUT = os.path.join(ROOT, "out", "savestate_test")

if len(sys.argv) > 1:
    CORE = sys.argv[1]
elif os.name == "nt":
    CORE = os.path.join(ROOT, "core", "makecode_arcade_libretro.dll")
else:
    CORE = os.path.join(ROOT, "core", "makecode_arcade_libretro.so")

PING = os.path.join(ROOT, "out", "pxt64", "ping.pxt64")
JUMPY = os.path.join(ROOT, "out", "pxt64", "jumpy.pxt64")


def run(cart, frames, extra=()):
    cmd = [sys.executable, HOST, CORE, cart, str(frames), "--turbo",
           "--data-dir", OUT] + list(extra)
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=ROOT)
    out = (r.stdout or "") + (r.stderr or "")
    hashes = re.findall(r"FINAL-FRAME-HASH ([0-9a-f]{32})", out)
    return (hashes[-1] if hashes else None), out


def main():
    os.makedirs(OUT, exist_ok=True)
    for p in (CORE, PING, JUMPY):
        if not os.path.exists(p):
            print(f"[SKIP] 缺文件：{p}")
            return 0
    state = os.path.join(OUT, "ping_240.bin")
    garbage = os.path.join(OUT, "garbage.bin")
    fails = []

    print("== S1 确定性：相同输入跑两遍，末帧哈希必须一致")
    # 注意 host 的选项是 --press=KEY 等号形式；空格形式会被当成位置参数忽略。
    # 按键选 A：Pong 的方向键只控制上下（up 长按会触发游戏自身的越界 panic，
    # left/right 对玩家桨无效 —— 实测长按 250 帧画面不变），A 会真实改变局面。
    h1, _ = run(PING, 360, ["--press=a", "--press-at=100"])
    h2, _ = run(PING, 360, ["--press=a", "--press-at=100"])
    if h1 and h1 == h2:
        print(f"   OK  {h1}")
    else:
        print(f"   [FAIL] {h1} vs {h2}")
        fails.append("S1 确定性")

    print("== S2 收敛：分叉历史 -> 同一快照 -> 精确同一未来")
    # v1 快照是"同会话"语义：GC 堆里是绝对指针，跨进程恢复必然被 -10 拒绝
    # （跨卡带/跨进程的拒绝路径由 S3 覆盖）。所以这里存/取发生在**同一次运行**：
    #   240 帧存档 → 280 帧读档（读档把游戏拨回 240 帧的状态）
    # 读档后虚拟时钟回到 240 而前端已跑到 280，游戏的 pause 定时器会在第一圈
    # 一次性追平 —— 头 2 帧是追平瞬态，**从第 3 帧起必须与原时间线逐字节重合**
    #（原时间线偏移 = 回退的 40 帧）。
    outS2 = run(PING, 360, ["--press=a", "--press-at=100",
                            f"--save-state-at=240:{state}",
                            f"--load-state-at=280:{state}",
                            "--hash-every=1"])[1]
    hashes = dict()
    for line in outS2.splitlines():
        m = re.match(r"FRAME-HASH (\d+) ([0-9a-f]{32})", line)
        if m:
            hashes[int(m.group(1))] = m.group(2)
    okSave = "快照已存" in outS2 and "恢复快照" in outS2 and "-> OK" in outS2
    bad = [i for i in range(283, 361)
           if i in hashes and (i - 40) in hashes and hashes[i] != hashes[i - 40]]
    if okSave and not bad:
        print(f"   OK  恢复后与原时间线逐帧重合（283..360 共 {360-283+1} 帧，偏移 40）")
    else:
        print(f"   [FAIL] 存取={'OK' if okSave else '失败'}，不重合帧数={len(bad)}"
              + (f"，例如 {bad[:3]}" if bad else ""))
        fails.append("S2 收敛")
    # 对照：同样按键、不读档的整跑末帧必须与"无按键"跑不同。
    # （注意不能拿上面那次的末帧比 —— 读档把虚拟时钟拨回了 40 帧，末帧对应的
    #   是虚拟 320 而不是 360。）
    hA, _ = run(PING, 360, ["--press=a", "--press-at=100"])
    hC, _ = run(PING, 360, [])
    if hC and hA and hC != hA:
        print(f"   OK  对照（无按键）确实不同：{hC} vs 按键 {hA}")
    else:
        print(f"   [FAIL] 对照哈希与按键跑相同（按键可能没生效）：{hC}")
        fails.append("S2 对照")

    print("== S3 跨卡带：ping 快照灌进 jumpy → 必须拒绝且游戏继续")
    hD, outD = run(JUMPY, 240, [f"--load-state-at=100:{state}"])
    rejected = "恢复快照" in outD and "[FAIL]" in outD
    if rejected and hD:
        print(f"   OK  拒绝且存活：{hD}")
    else:
        print("   [FAIL] 应拒绝且游戏应继续正常")
        fails.append("S3 跨卡带")

    print("== S4 垃圾数据：必须拒绝且游戏继续")
    with open(garbage, "wb") as fh:
        fh.write(bytes(random.randrange(256) for _ in range(4096)))
    hE, outE = run(PING, 120, [f"--load-state-at=60:{garbage}"])
    rejected = "恢复快照" in outE and "[FAIL]" in outE
    if rejected and hE:
        print(f"   OK  拒绝且存活：{hE}")
    else:
        print("   [FAIL] 应拒绝且游戏应继续正常")
        fails.append("S4 垃圾")

    print()
    if fails:
        print(f"[FAIL] {len(fails)} 项：{', '.join(fails)}")
        return 1
    print("[OK] 全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
