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

import png2pxt64 as P  # noqa: E402  复用 pxt 调用环境（PXT_FORCE_LOCAL / docker 空壳）

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


def main():
    argv = [a for a in sys.argv[1:] if not a.startswith("--")]
    audio = "--audio" in sys.argv
    if audio:
        out = argv[0] if argv else os.path.join(P.ROOT, "out", "pxt64", "audio_probe.pxt64")
    else:
        out = argv[0] if argv else os.path.join(P.ROOT, "out", "pxt64", "probe.pxt64")
    if not os.path.isfile(P.PXT_JS):
        raise SystemExit(f"pxt core not found: {P.PXT_JS}\n先运行 tools/vm-build/setup_toolchain.sh")

    src = os.path.join(P.TARGET, "proj_ping")
    proj = os.path.join(P.TARGET, "proj_audio_probe" if audio else "proj_probe")
    if not os.path.isdir(os.path.join(src, "pxt_modules")):
        raise SystemExit(f"{src}/pxt_modules 不存在：先跑一次 png2pxt64.py 把依赖装上")

    if not os.path.isdir(os.path.join(proj, "pxt_modules")):
        print(f"[1/3] 从 proj_ping 复制依赖缓存 -> {proj}")
        if os.path.isdir(proj):
            shutil.rmtree(proj)
        shutil.copytree(src, proj)
    else:
        print(f"[1/3] 复用已有工程 {proj}")

    with open(os.path.join(proj, "main.ts"), "w", encoding="utf8", newline="\n") as f:
        f.write(AUDIO_TS if audio else PROBE_TS)
    with open(os.path.join(proj, "pxt.json"), "w", encoding="utf8", newline="\n") as f:
        f.write(PXT_JSON)
    print("[2/3] 写入%s main.ts" % ("声音探针" if audio else "按键探针"))

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
