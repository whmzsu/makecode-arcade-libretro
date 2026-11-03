// 异常路径回归样本（v0.3.2 起用）。
//
// 目的：VM 的 JS try/catch 走 op_try → restoreVMExceptionState →
// longjmp(FiberContext::loopjmp)。这条链在 mingw 目标上曾经直接把宿主进程
// 带崩（STATUS_BAD_STACK 0xC0000028，见 docs/DEVICE-TEST.md 的 v0.3.2 一节）。
// 冒烟时把它编成 .pxt64 跑一遍：不崩、有画面变化 = 异常回跳路径完好。
//
// 重建方法：把本文件拷成 cartconv 编译树里任意 proj_* 副本的 main.ts
// （例如 proj_TicTacTwo_，保留它的 pxt.json，把 name 随手改掉），然后
//   python tools/fetch_shared.py 之外直接用：
//     from png2pxt64 import run_pxt
//     run_pxt(["install", "--hwvariant", "vm"], proj_dir)
//     run_pxt(["build",  "--hwvariant", "vm"], proj_dir)
//   产物在 proj_dir/built/binary.pxt64。
//
// 预期行为：catch 生效（n=42），背景色每 250ms 轮换 —— 画面持续变化说明
// 异常抛出后 VM 恢复正常运行，而不是停在 catch 里。
let n = 0
try {
    throw "boom"
} catch (e) {
    n = 42
}
scene.setBackgroundColor(1)
game.onUpdateInterval(250, function () {
    n += 1
    scene.setBackgroundColor(n % 15 + 1)
})
