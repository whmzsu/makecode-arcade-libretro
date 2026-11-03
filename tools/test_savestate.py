#!/usr/bin/env python3
# test_savestate.py — retro_serialize 快照回归（确定性 / 收敛 / 拒绝路径）
#
# 前提：帧驱动虚拟时钟（v0.2.10）使核心完全确定——同一输入序列 + 同一快照
# ⇒ 精确同一画面。测试全部用 --turbo（不睡帧），确定性由时钟保证。
#
# 场景：
#   S1 确定性：两次完全相同的跑，末帧哈希必须一致
#   S2 收敛：  分叉历史（有无按键）在 240 帧汇合到同一快照，其后 120 帧
#              必须精确同画面；无恢复的对照跑必须不同（同会话 rewind）
#   S3 跨卡带：ping 的快照灌进 jumpy 会话 → 必须拒绝且游戏继续正常
#   S4 垃圾：  随机字节 → 必须拒绝且游戏继续正常
#   S5 跨进程：A 进程存档 → 退出 → B 进程加载（v0.2.11 的核心能力）
#              恢复后第一帧起必须与原时间线逐帧重合
#   S6 跨基址：S5 的存档改写"保存时的模块基址"（模拟动态加载器换了地址），
#              恢复必须照样逐帧重合 —— 堆里有指向 core 自身代码的指针，
#              不做重定位的话这里会崩（真机上是"加载存档后 RetroArch 闪退"）
#   S7 rewind：同一存档每 20 帧读一次，压恢复路径的内存管理
#   S8 忙等：  游戏里有段不调 pause 的长循环，此时读档必须成功
#              （v0.2.12：真机 `pause failed -2` 的根因是解释器里没有让出点，
#                宿主等 5 秒也停不稳；桌面用 10 亿次自增复现）
#   S9 连续存/读：存档紧接着读档（RetroArch 读档前存撤销点的时序）
#              （v0.2.13：真机 `spot=3` 超时的根因是暂停握手协议有竞态）
#   S10 卡带指纹：必须是"原始镜像字节"的 FNV-1a，跨进程一致
#              （v0.2.13：旧实现算在 patch 之后，ASLR 一变就误判成换了卡带）
#
# 头部字段偏移（S6 要改写存档，格式定义见 scheduler.cpp 的 McaSnapHeader）：
#   magic@0 version@4 heapUsed@8 ... modBase@56 modSize@64 imgBytes@72 imgHash@76
#   McaGcState@80（120 字节）→ GC 堆内容从 @200 开始
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
# 输出目录可用 MCA_TEST_OUT 覆盖 —— 并行跑多份套件做竞态压力测试时，两份
# 套件不能共用同一个存档文件（否则互相覆盖，失败是假的）。
OUT = os.environ.get("MCA_TEST_OUT") or os.path.join(ROOT, "out", "savestate_test")

# Windows 控制台默认 cp1252/GBK：场景名含中文时 print 直接 UnicodeEncodeError
# （windows-11-arm CI runner 上实踩）。测试输出本就不进管道协议，统一 UTF-8。
if os.name == "nt" and hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")

if len(sys.argv) > 1:
    CORE = sys.argv[1]
elif os.name == "nt":
    CORE = os.path.join(ROOT, "core", "makecode_arcade_libretro.dll")
else:
    CORE = os.path.join(ROOT, "core", "makecode_arcade_libretro.so")

def _cart(name):
    """优先用本地转换产物（out/pxt64/）；没有就退回仓库内置夹具
    （tools/fixtures/）。CI 的 ARM runner 上没有 pxt 编译环境，无法
    现场从 png_examples 生成，所以三张测试卡带直接入库作夹具。"""
    local = os.path.join(ROOT, "out", "pxt64", name)
    if os.path.exists(local):
        return local
    return os.path.join(HERE, "fixtures", name)


PING = _cart("ping.pxt64")
BUSY = _cart("busy.pxt64")
JUMPY = _cart("jumpy.pxt64")


def run(cart, frames, extra=()):
    """跑一次前端。返回 (末帧哈希, 完整输出, 退出码)。"""
    cmd = [sys.executable, HOST, CORE, cart, str(frames), "--turbo",
           "--data-dir", OUT] + list(extra)
    r = subprocess.run(cmd, capture_output=True, text=True, cwd=ROOT)
    out = (r.stdout or "") + (r.stderr or "")
    # core trace（mca_trace.log）每个进程启动都截断重建——不在这里收进 out，
    # 前一个场景的 [snap]/[fix] 现场就永久丢了（CI 上无日志可查，armhf 排障
    # 时踩过）。路径用 glob：--data-dir 在 host 里没生效时也能找到。
    import glob
    for tf in glob.glob(os.path.join(ROOT, "out", "**", "mca_trace.log"),
                        recursive=True):
        try:
            with open(tf, encoding="utf-8", errors="replace") as fh:
                for ln in fh:
                    if any(k in ln for k in ("[snap]", "[fix]", "[heap]",
                                             "[img]", "PANIC", "panic",
                                             "reject")):
                        out += "[trace] " + ln
        except OSError:
            pass
    hashes = re.findall(r"FINAL-FRAME-HASH ([0-9a-f]{32})", out)
    return (hashes[-1] if hashes else None), out, r.returncode


def frame_hashes(out):
    """把 --hash-every=1 的 FRAME-HASH 行解析成 {帧号: 哈希}。"""
    d = {}
    for line in out.splitlines():
        m = re.match(r"FRAME-HASH (\d+) ([0-9a-f]+)", line.strip())
        if m:
            d[int(m.group(1))] = m.group(2)
    return d


def compare_after_load(hA, hB, loadAt, offset, last):
    """恢复后逐帧比对：B 的第 k 帧对应 A 的第 k+offset 帧。
    恢复发生在第 loadAt+1 次 retro_run 之后，所以从 loadAt+2 开始比。"""
    return [k for k in range(loadAt + 2, last + 1)
            if k in hB and (k + offset) in hA and hB[k] != hA[k + offset]]


def save_load_lines(out, limit=6):
    """从 host 输出里抽存/取相关的诊断行（CI 上失败时唯一的信息来源）。"""
    keys = ("快照已存", "恢复快照", "retro_serialize", "retro_unserialize",
            "pause failed", "pause reject", "PANIC", "panic", "vm thread",
            "[trace]")
    return [ln.strip() for ln in out.splitlines()
            if any(k in ln for k in keys)][:limit]


def rewrite_module_base(src, dst, delta):
    """把快照改写成"在基址 modBase-delta 上存"的等价物（模拟动态加载器换地址）。

    返回平移的指针个数；0 表示存档里没有模块指针（S6 就没意义了）。
    格式见文件头的偏移说明，version 变了就说明这里要跟着改。
    """
    import struct
    with open(src, "rb") as fh:
        b = bytearray(fh.read())
    version, heap_used = struct.unpack_from("<II", b, 4)
    if version != 3:
        raise SystemExit(f"[FAIL] 快照版本 {version} != 3，test_savestate 的偏移需要更新")
    mod_base = struct.unpack_from("<Q", b, 56)[0]
    mod_size = struct.unpack_from("<Q", b, 64)[0]
    heap_off = 200
    n = 0
    for off in range(heap_off, heap_off + heap_used, 8):
        v = struct.unpack_from("<Q", b, off)[0]
        if mod_base <= v < mod_base + mod_size:
            struct.pack_into("<Q", b, off, v - delta)
            n += 1
    struct.pack_into("<Q", b, 56, mod_base - delta)
    with open(dst, "wb") as fh:
        fh.write(b)
    return n


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
    h1 = run(PING, 360, ["--press=a", "--press-at=100"])[0]
    h2 = run(PING, 360, ["--press=a", "--press-at=100"])[0]
    if h1 and h1 == h2:
        print(f"   OK  {h1}")
    else:
        print(f"   [FAIL] {h1} vs {h2}")
        fails.append("S1 确定性")

    print("== S2 收敛（同会话 rewind）：分叉历史 -> 同一快照 -> 精确同一未来")
    # 这是 RetroArch 的 rewind / runahead 路径：存/取发生在**同一次运行**内。
    #   240 帧存档 → 280 帧读档（读档把游戏拨回 240 帧的状态）
    # 读档后虚拟时钟回到 240 而前端已跑到 280，游戏的 pause 定时器会在第一圈
    # 一次性追平 —— 头 1 帧是追平瞬态，**其后每一帧都必须与原时间线逐字节重合**
    #（原时间线偏移 = 回退的 40 帧）。
    #
    # 参照物必须是**另一次不读档的整跑**，不能拿同一次输出里 k-40 帧当参照：
    # k-40 一旦落进回放区，参照物本身就变成"更早的回放帧"，比较的语义就成了
    # A[k-40] vs A[k-80]（两次相隔 40 帧的真实画面，当然不同）。早先版本正是
    # 这样写的，于是从 320 帧起必然报"39 帧不重合"，看起来像核心分叉，实则
    # 是比对窗口越界 —— 当时 HEAD 基线也复现（600 帧时报 278 帧，恰等于
    # 600-322+1），就是最直接的反证。
    saveAt, loadAt = 240, 280
    off = saveAt - loadAt                      # 负值 = 读档把时间拨回
    outA = run(PING, 360, ["--press=a", "--press-at=100", "--hash-every=1"])[1]
    outS2 = run(PING, 360, ["--press=a", "--press-at=100",
                            f"--save-state-at={saveAt}:{state}",
                            f"--load-state-at={loadAt}:{state}",
                            "--hash-every=1"])[1]
    hA2 = frame_hashes(outA)
    hS2 = frame_hashes(outS2)
    okSave = "快照已存" in outS2 and "恢复快照" in outS2 and "-> OK" in outS2
    # 回放区从 loadAt+2 起；B[k] 对应 A[k + off]（off 为负，即往前 40 帧）
    bad = [k for k in range(loadAt + 2, 361)
           if k in hS2 and (k + off) in hA2 and hS2[k] != hA2[k + off]]
    if okSave and not bad:
        print(f"   OK  恢复后与原时间线逐帧重合（{loadAt+2}..360 共 {360-loadAt-1} 帧，"
              f"偏移 {-off}）")
    else:
        print(f"   [FAIL] 存取={'OK' if okSave else '失败'}，不重合帧数={len(bad)}"
              + (f"，例如 {bad[:3]}" if bad else ""))
        for ln in save_load_lines(outS2):
            print(f"      | {ln}")
        fails.append("S2 收敛")
    # 对照：同样按键、不读档的整跑末帧必须与"无按键"跑不同。
    hC = run(PING, 360, [])[0]
    hA = hA2.get(360)
    if hC and hA and hC != hA:
        print(f"   OK  对照（无按键）确实不同：{hC} vs 按键 {hA}")
    else:
        print(f"   [FAIL] 对照哈希与按键跑相同（按键可能没生效）：{hC}")
        fails.append("S2 对照")

    print("== S3 跨卡带：ping 快照灌进 jumpy → 必须拒绝且游戏继续")
    hD, outD, rcD = run(JUMPY, 240, [f"--load-state-at=100:{state}"])
    rejected = "恢复快照" in outD and "[FAIL]" in outD
    if rejected and hD:
        print(f"   OK  拒绝且存活：{hD}")
    else:
        print("   [FAIL] 应拒绝且游戏应继续正常")
        fails.append("S3 跨卡带")

    print("== S4 垃圾数据：必须拒绝且游戏继续")
    with open(garbage, "wb") as fh:
        fh.write(bytes(random.randrange(256) for _ in range(4096)))
    hE, outE, rcE = run(PING, 120, [f"--load-state-at=60:{garbage}"])
    rejected = "恢复快照" in outE and "[FAIL]" in outE
    if rejected and hE:
        print(f"   OK  拒绝且存活：{hE}")
    else:
        print("   [FAIL] 应拒绝且游戏应继续正常")
        for ln in save_load_lines(outE):
            print(f"      | {ln}")
        fails.append("S4 垃圾")

    print("== S5 跨进程：A 存档 -> 退出 -> B 加载 -> 与原时间线逐帧重合")
    # v0.2.11 的核心能力：RetroArch 退出/崩溃之后再进同一个游戏加载存档。
    # 做法：A 进程跑到 120 帧存档（中途按过 A 键制造局面），B 进程自己在第 40
    # 帧读这个档 —— 读档后 B 的局面 = A 的 120 帧局面，于是 B 的第 k 帧必须
    # 等于 A 的第 k+80 帧，且从读档后的**第一帧**起就要对上（不是"两三帧后
    # 才收敛"，那说明画面缓冲之类没进快照）。
    crossState = os.path.join(OUT, "ping_cross.bin")
    saveAt, loadAt = 120, 40
    off = saveAt - loadAt
    outA = run(PING, 240, ["--press=a", "--press-at=100",
                           f"--save-state-at={saveAt}:{crossState}",
                           "--hash-every=1"])[1]
    hB, outB, rcB = run(PING, 240, [f"--load-state-at={loadAt}:{crossState}",
                                    "--hash-every=1"])
    hA = frame_hashes(outA)
    hBf = frame_hashes(outB)
    okSave = "快照已存" in outA
    okLoad = "恢复快照" in outB and "-> OK" in outB
    bad = compare_after_load(hA, hBf, loadAt, off, 240)
    if okSave and okLoad and rcB == 0 and not bad:
        print(f"   OK  跨进程恢复后逐帧重合（{loadAt+2}..240 共 {240-loadAt-1} 帧，偏移 {off}）")
    else:
        print(f"   [FAIL] 存={'OK' if okSave else '失败'} 取={'OK' if okLoad else '失败'}"
              f" 退出码={rcB} 不重合帧数={len(bad)}" + (f"，例如 {bad[:3]}" if bad else ""))
        for ln in save_load_lines(outA) + save_load_lines(outB):
            print(f"      | {ln}")
        fails.append("S5 跨进程")

    print("== S6 跨基址：同一快照把\"保存时的模块基址\"挪走 -> 必须照样重合")
    # 堆里存着指向 core 自身代码的指针（RefAction 的内核回调、fiber 的 wakeFn）。
    # 动态加载器给的基址每个进程都可能不同，恢复时必须按差值重定位。真机上
    # 缺这一步的表现就是"加载存档后 RetroArch 直接崩"。这里用改写存档的方式
    # 模拟换基址：所有落在 [modBase, modBase+modSize) 的值整体下移 delta，
    # modBase 也下移 delta —— 等价于"这份档是在另一个基址上存的"。
    rebased = os.path.join(OUT, "ping_rebased.bin")
    delta = 0x10000
    nshift = rewrite_module_base(crossState, rebased, delta)
    hC, outC, rcC = run(PING, 240, [f"--load-state-at={loadAt}:{rebased}",
                                    "--hash-every=1"])
    hCf = frame_hashes(outC)
    okLoad = "恢复快照" in outC and "-> OK" in outC
    bad = compare_after_load(hA, hCf, loadAt, off, 240)
    if nshift and okLoad and rcC == 0 and not bad:
        print(f"   OK  重定位 {nshift} 个模块指针后逐帧重合")
    else:
        print(f"   [FAIL] 重定位 {nshift} 个 取={'OK' if okLoad else '失败'}"
              f" 退出码={rcC} 不重合帧数={len(bad)}" + (f"，例如 {bad[:3]}" if bad else ""))
        fails.append("S6 跨基址")

    print("== S7 rewind 热路径：同一存档反复回退（每 20 帧读一次）")
    # RetroArch 的 rewind 是"每帧存、按住就每帧读"，本核心的恢复路径会被连续
    # 调用成百上千次。这里压缩成"存一次、每 20 帧读一次"，重点压恢复流程的
    # 内存管理：早期版本在同进程时把 fiber 释放后**原地写回**，那块内存已经
    # 回到分配器手里 —— 游戏下一次新建 fiber 就盖住它，表现为"读档几十帧后
    # 画面才走岔"（S2 里约 1/8 概率，压力更低时几乎必现）。所以这轮必须比。
    rewindState = os.path.join(OUT, "ping_rewind.bin")
    base, step, loads = 120, 20, (140, 160, 180)
    hRa, outRa, _ = run(PING, 220, ["--press=a", "--press-at=100", "--hash-every=1"])
    hR = frame_hashes(outRa)
    args = ["--press=a", "--press-at=100", "--hash-every=1",
            f"--save-state-at={base}:{rewindState}"]
    for L in loads:
        args.append(f"--load-state-at={L}:{rewindState}")
    outRb = run(PING, 220, args)[1]
    hRb = frame_hashes(outRb)
    okSave = "快照已存" in outRb and outRb.count("-> OK") >= len(loads)
    bad = []
    for L in loads:
        for t in range(2, step):            # 读到下一次读档之前
            k = L + t
            if k in hRb and (base + t) in hR and hRb[k] != hR[base + t]:
                bad.append(k)
    if okSave and not bad:
        n = sum(1 for L in loads for t in range(2, step) if L + t in hRb)
        print(f"   OK  {len(loads)} 次回退共 {n} 帧全部与原时间线重合")
    else:
        print(f"   [FAIL] 存={'OK' if okSave else '失败'}，不重合帧数={len(bad)}"
              + (f"，例如 {bad[:3]}" if bad else ""))
        fails.append("S7 rewind")

    print("== S8 忙等：游戏卡在不调 pause 的长循环里，读档必须成功")
    # 真机报过 `unserialize: pause failed -2`（等 5 秒也没停稳）。根因不是握手，
    # 是 VM 根本没机会看请求：fiber 里一段不调 pause 的循环会让解释器一直跑到
    # 它自然让出为止，主循环顶的检查点轮不到。桌面上 10 亿次自增 ≈ 20 秒，
    # 足够把"宿主 5 秒预算"撞穿。
    #
    # 判定：读档必须返回 OK，且日志里不能出现 pause failed（对照见 v0.2.11：
    # 同一个卡带、同一个时机，必现 -2）。
    if not os.path.isfile(BUSY):
        subprocess.run([sys.executable, os.path.join(HERE, "make_probe_cartridge.py"),
                        "--busy", BUSY], cwd=ROOT, check=False)
    if not os.path.isfile(BUSY):
        print("   [SKIP] 忙等探针卡带生成失败（需要 pxt 工具链）")
    else:
        busyState = os.path.join(OUT, "busy_state.bin")
        _, outB, rcB = run(BUSY, 50, ["--press=a", "--press-at=20",
                                      f"--save-state-at=10:{busyState}",
                                      f"--load-state-at=40:{busyState}"])
        okLoad = "-> OK" in outB
        if okLoad and "pause failed" not in outB and rcB == 0:
            print("   OK  忙等中断点生效：VM 在解释器内部被打断并停稳")
        else:
            why = "pause failed" if "pause failed" in outB else f"读档{'OK' if okLoad else '失败'}"
            print(f"   [FAIL] {why}，退出码={rcB}")
            fails.append("S8 忙等")

    print("== S9 连续存/读：存档紧接着读档（RetroArch 读档前存撤销点的时序）")
    # 真机 trace 是 `pause timeout 5s: spot=3` —— VM 明明停在安全点上，宿主却在
    # 干等。根因是旧的"停稳次数 +1"协议：上一次 resume（request=0）还没被 VM
    # 观察到就又被置成请求，VM 一直待在暂停循环里不出来、也不再产生第二个 ack。
    # RetroArch 读档前会先 retro_serialize 存一个撤销点，两次 pause 紧挨着，
    # 正好踩这个窗口（v0.2.12 之前桌面测不出，因为存取都隔了几十帧）。
    btState = os.path.join(OUT, "backtoback.bin")
    btArgs = []
    for f in (120, 160, 200):
        btArgs += [f"--save-state-at={f}:{btState}",
                   f"--load-state-at={f}:{btState}"]
    _, outBt, rcBt = run(PING, 240, btArgs)
    nSave = outBt.count("快照已存")
    nLoad = outBt.count("恢复快照")
    nOk = sum(1 for L in outBt.splitlines() if "恢复快照" in L and "-> OK" in L)
    if nSave == 3 and nLoad == 3 and nOk == 3 and "pause failed" not in outBt and rcBt == 0:
        print("   OK  3 次紧密存/读全部成功（无 pause failed）")
    else:
        print(f"   [FAIL] 存 {nSave}/3、读 {nLoad}/3、成功 {nOk}/3"
              + ("，出现 pause failed" if "pause failed" in outBt else "")
              + f"，退出码={rcBt}")
        fails.append("S9 连续存/读")

    print("== S10 卡带指纹：只取决于原始镜像字节（打补丁之前），跨进程一致")
    # 旧实现拿 vm_patch_image() **之后**的镜像算指纹，而 patch 会把 vtable 和
    # 内核函数指针写进镜像 —— Linux/Android 的 .so 走 ASLR，换个进程指纹就变，
    # 跨会话读档被误判成"换了卡带"（真机 `拒绝恢复：卡带不同 ... 181608 字节`）。
    # Windows 的 DLL 基址固定，所以这条以前一直没暴露。
    # 判据：两次独立进程的指纹相同，且等于"原始镜像字节的 FNV-1a"（Python 现算）。
    def cart_id(out):
        a = re.search(r"image (\d+) bytes, cart", out)
        b = re.search(r"cart fingerprint ([0-9a-f]{8})", out)
        return (int(a.group(1)), b.group(1)) if a and b else (None, None)

    id1 = cart_id(run(PING, 12, [])[1])
    id2 = cart_id(run(PING, 12, [])[1])
    if not id1[0]:
        print("   [FAIL] 启动横幅里没找到 image/fingerprint（横幅被改过？）")
        fails.append("S10 指纹")
    else:
        raw = open(PING, "rb").read()[: id1[0]]
        h = 2166136261
        for x in raw:
            h = ((h ^ x) * 16777619) & 0xFFFFFFFF
        exp = "%08x" % h
        if id1 == id2 and id1[1] == exp:
            print(f"   OK  两次进程都是 {id1[1]}（= 原始镜像 {id1[0]} 字节的 FNV-1a）")
        else:
            print(f"   [FAIL] 两次={id1[1]}/{id2[1]}，原始镜像应为 {exp}")
            fails.append("S10 指纹")

    print()
    if fails:
        print(f"[FAIL] {len(fails)} 项：{', '.join(fails)}")
        return 1
    print("[OK] 全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
