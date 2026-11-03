#!/usr/bin/env python3
"""make_probe_cartridge.py — 生成"按键探针"卡带（.pxt64）

探针游戏本身没有画面逻辑，只在按键事件发生时往 VM 日志打一行
`PROBE:<键>:down|up`。配合 tools/libretro_host.py --press=A 使用，
就能把「RetroPad 物理键 → MakeCode 按钮」的映射断言成可 grep 的日志，
不用靠看画面猜。

用法：
  python tools/make_probe_cartridge.py [输出路径]
  python tools/libretro_host.py core/makecode_arcade_libretro.dll \\
      out/pxt64/probe.pxt64 60 --press=A

实现上复用 proj_ping 的 pxt_modules 缓存（依赖已装好），只替换 main.ts，
避免每次重新 install。
"""

import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

# png2pxt64 与 pxt 编译链（vm-build/node_modules）已拆到并列仓库
# makecode_arcade_cartconv；从这里导入以复用 pxt 调用环境
# （PXT_FORCE_LOCAL / docker 空壳）。可用 MCA_CONV_REPO 覆盖仓库位置。
CORE_ROOT = os.path.dirname(HERE)
CONV_TOOLS = os.environ.get("MCA_CONV_REPO") or os.path.join(
    os.path.dirname(CORE_ROOT), "makecode_arcade_cartconv", "tools")
sys.path.insert(0, CONV_TOOLS)

import png2pxt64 as P  # noqa: E402

PROBE_TS = """// 按键探针：每个按键把屏幕背景刷成不同颜色。
// 测试端抓末帧比对颜色即可断言"物理键 → MakeCode 按钮"的映射。
// 不用 console.log：VM 里的 console 输出不进 codalLogStore（dmesg），拉不到。
//
// 颜色用 arcade 默认调色板色号（实测得到的 RGB）：
//   1 白 #FFFFFF   2 红 #FF2121   3 粉 #FF93C4   4 橙 #FF8135   5 黄 #FFF609
//   6 青 #249CA3   7 绿 #78DC52   8 蓝 #003FAD   9 淡蓝 #87F2FF
scene.setBackgroundColor(1)   // 白 = 启动成功（同时证明渲染链路是通的）

controller.A.onEvent(ControllerButtonEvent.Pressed, function () {
    scene.setBackgroundColor(2)   // 红
})
controller.B.onEvent(ControllerButtonEvent.Pressed, function () {
    scene.setBackgroundColor(5)   // 黄
})
controller.up.onEvent(ControllerButtonEvent.Pressed, function () {
    scene.setBackgroundColor(7)   // 绿
})
controller.down.onEvent(ControllerButtonEvent.Pressed, function () {
    scene.setBackgroundColor(4)   // 橙
})
controller.left.onEvent(ControllerButtonEvent.Pressed, function () {
    scene.setBackgroundColor(8)   // 蓝
})
controller.right.onEvent(ControllerButtonEvent.Pressed, function () {
    scene.setBackgroundColor(9)   // 淡蓝
})
controller.menu.onEvent(ControllerButtonEvent.Pressed, function () {
    scene.setBackgroundColor(6)   // 青
})
"""

AUDIO_TS = """// 声音探针：一直播 440Hz，同时把背景刷绿。
// 判据：重启后音频采样峰值必须重新 > 0（全零 = dac 丢了，即"重置后没声音"）。
scene.setBackgroundColor(7)   // 绿 = 启动成功
forever(function () {
    music.playTone(440, 500)
    pause(20)
})
"""

PXT_JSON = """{
    "name": "probe",
    "description": "makecode_arcade button probe",
    "dependencies": {
        "device": "*"
    },
    "files": [
        "main.ts"
    ],
    "target": { "name": "probe" }
}
"""

# 忙等探针：按 A 后在 fiber 里跑一段**不调 pause**的循环。
#
# 用途：验证宿主请求快照暂停时，VM 能不能从"解释器内部"被打断。真机上加载
# 存档报 `pause failed -2`（等 5 秒也没停稳）就是这个场景：游戏里有段长循环
# （初始化、紧凑的 CPU 循环），解释器不回到主循环顶，检查点根本轮不到。
# 循环次数要足够大 —— 桌面大约 1 亿次/2 秒，ARM 慢一个数量级。
BUSY_TS = """// 忙等探针：按 A 后跑一亿次自增，中间不调用任何会让出的 API。
scene.setBackgroundColor(1)
controller.A.onEvent(ControllerButtonEvent.Pressed, function () {
    scene.setBackgroundColor(2)
    let t = 0
    while (t < 1000000000) {
        t += 1
    }
    scene.setBackgroundColor(3)
})
"""

BUSY_JSON = """{
    "name": "busy",
    "description": "makecode_arcade busy-loop probe",
    "dependencies": {
        "device": "*"
    },
    "files": [
        "main.ts"
    ],
    "target": { "name": "busy" }
}
"""

# panic 探针：跑 0.5 秒后主动 panic，模拟"游戏内部崩了"。
#
# 用途：验证 VM 线程猝死后的自愈链路 ——
#   control.panic → soft_panic → systemReset → 清理 → pthread_exit
# 这条路径**不走** multiStart() 的 return，所以 mcaVmExited 必须自己置位，
# 否则宿主永远以为 VM 还活着、前端永不重启：真机表现就是"画面冻在最后一帧，
# 进程不崩也不退"（v0.2.14 修的就是它）。
PANIC_TS = """// panic 探针：启动 0.5 秒后主动 panic，然后由宿主自动重启。
scene.setBackgroundColor(6)   // 青 = 启动成功
forever(function () {
    pause(500)
    control.panic(7)
})
"""

PANIC_JSON = """{
    "name": "panic",
    "description": "makecode_arcade panic probe",
    "dependencies": {
        "device": "*"
    },
    "files": [
        "main.ts"
    ],
    "target": { "name": "panic" }
}
"""


def main():
    argv = [a for a in sys.argv[1:] if not a.startswith("--")]
    audio = "--audio" in sys.argv
    busy = "--busy" in sys.argv
    panic = "--panic" in sys.argv
    if panic:
        kind, ts, js, default = "panic", PANIC_TS, PANIC_JSON, "panic.pxt64"
    elif busy:
        kind, ts, js, default = "busy", BUSY_TS, BUSY_JSON, "busy.pxt64"
    elif audio:
        kind, ts, js, default = "audio", AUDIO_TS, PXT_JSON, "audio_probe.pxt64"
    else:
        kind, ts, js, default = "probe", PROBE_TS, PXT_JSON, "probe.pxt64"
    out = argv[0] if argv else os.path.join(CORE_ROOT, "out", "pxt64", default)
    if not os.path.isfile(P.PXT_JS):
        raise SystemExit(f"pxt core not found: {P.PXT_JS}\n先运行 tools/vm-build/setup_toolchain.sh")

    src = os.path.join(P.TARGET, "proj_ping")
    proj = os.path.join(P.TARGET, "proj_" + ("audio_probe" if kind == "audio" else kind))
    if not os.path.isdir(os.path.join(src, "pxt_modules")):
        raise SystemExit(f"{src}/pxt_modules 不存在：先在 makecode_arcade_cartconv 里跑一次转换把依赖装上")

    if not os.path.isdir(os.path.join(proj, "pxt_modules")):
        print(f"[1/3] 从 proj_ping 复制依赖缓存 -> {proj}")
        if os.path.isdir(proj):
            shutil.rmtree(proj)
        shutil.copytree(src, proj)
    else:
        print(f"[1/3] 复用已有工程 {proj}")

    with open(os.path.join(proj, "main.ts"), "w", encoding="utf8", newline="\n") as f:
        f.write(ts)
    with open(os.path.join(proj, "pxt.json"), "w", encoding="utf8", newline="\n") as f:
        f.write(js)
    print("[2/3] 写入%s main.ts" %
          {"busy": "忙等探针", "audio": "声音探针", "panic": "panic 探针"}.get(kind, "按键探针"))

    proc = P.run_pxt(["build", "--hwvariant", "vm"], proj)
    image = os.path.join(proj, "built", "binary.pxt64")
    if not os.path.isfile(image) and P.ensure_docker_stub(proj):
        print("      补齐 docker 占位产物后重试")
        proc = P.run_pxt(["build", "--hwvariant", "vm"], proj)
    if not os.path.isfile(image):
        sys.stderr.write((proc.stdout or "")[-3000:] + "\n" + (proc.stderr or "")[-3000:])
        raise SystemExit("[3/3] pxt build 失败")

    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    shutil.copyfile(image, out)
    print(f"[3/3] 探针卡带 -> {out} ({os.path.getsize(out)} 字节)")


if __name__ == "__main__":
    main()
