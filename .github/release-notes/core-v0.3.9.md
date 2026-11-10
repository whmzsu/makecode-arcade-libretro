# MakeCode Arcade core v0.3.9

## 一句话：不再被三张卡带打死，加载失败也不再假装成功

这一版有**行为改动**，但只落在异常分支上：三个基准用例的末帧哈希与 0.3.8
**逐位相同**。修的是 62 张社区卡带批量冒烟暴露出来的三条问题——两处整数除零
会把整个核心进程直接带死（SIGFPE），以及镜像被 VM 拒绝时前端只看到一片黑屏、
日志里却显示「加载成功」。

**建议升级**：如果你手上有较新的社区卡带（尤其是带 3D / 缩放特效的），0.3.8
按一下 A 就可能直接退出；0.3.9 会继续跑下去。遇到跑不了的卡带，现在也会
明确告诉你原因和段尺寸，而不是让你对着黑屏猜。

## 1. PATCH 13：`blit()` 目标尺寸为 0 时整数除零

`CrateMaker` / `Crate_Dungeon` 的爆炸特效会把内置 `sprite.scale` 设成 0
或逐帧递减到 0/负数，最终走到 `runtime/pxtapp/screen---ext/image.cpp:1092`：

```cpp
int xSrcStep = (wSrc << 16) / wDst;
int ySrcStep = (hSrc << 16) / hDst;   // hDst == 0 → SIGFPE
```

C++ 整数除以 0 是硬件异常，`0xc0000094`（Windows）/ `SIGFPE`（Linux）直接
终结宿主进程，连 panic 横幅都来不及画。现在目标宽或高 ≤ 0 时提前返回
`false`——`blit` 的 `false` 本来就是「未绘制 / 未检出碰撞」，零尺寸也没有
像素可画，语义完全一致。

## 2. PATCH 14：`Math_::idiv()` 除数为 0 时整数除零

`FPS game test (menu test)` 自带的 raycasting 引擎里 `_render_raycasting.ts`
的 `_fov` 没有初值，首帧 `wallHeightInView = (SW << (fpx-1)) / this._fov`
就是 `x/0`（或 `0/0`），落到 `runtime/pxtapp/base/core.cpp` 的
`Math_::idiv(int,int)`。这张卡带**不按键、第 0 帧就崩**。

TS 层对除零期望得到 NaN/Infinity，VM 的整数除法没有浮点可退；现在 `y == 0`
返回 0，是最保守的兜底：不崩宿主，后续帧还能继续跑。

## 3. PATCH 15：镜像校验失败时 `retro_load_game()` 不再谎报成功

`sm64thign` 的黑屏本来是个纯粹的镜像校验问题：`verify.cpp:176` 的
`CHECK(sect->size < 32000, 1014)` 拒绝了一个 109 952 字节的 Function 段，
`vmload.cpp` 在 **VM 线程内** `dmesg("validation error 1014 at 0x1520")`
后直接 return。但 `pxt_vm_start_buffer()` 会立刻 spawn 线程并成功返回，主线程
拿不到这个结果，于是 `retro_load_game()` 返回 true，前端只看到一片黑屏。
围绕「是不是渲染坏了」来回查了三轮才定案。

现在宿主在 spawn 之前用同一条 section 链做一次**同步段级预检**（段链/
对齐、size ≥ 16、size < 32000），违规就：

- `retro_load_game()` 返回 **false**，前端直接报加载失败；
- 日志给出可自查的原因，例如
  `[MCA] pxt64 image rejected: section at 0x1520 size=109952 >= 32000 (VM error 1014); max VM section size is 32000, this cart cannot run on the pxt VM`。

**没有放宽 32000**：这是上游 pxt VM 给小栈 MCU 设的保守上限，改了就偏离上游；
各真机（掌机 / Android / Lakka musl）的栈大小未逐一审计，且超过函数内 16 位
跳转的 128 KB 量级后还有别的风险。诊断实验里临时放宽到 262144 确实能让原版
出画（与 cartconv 侧把大数组拆成 13 个小函数的 `split.pxt64` 末帧逐位一致），
但收益只有一张卡带，转换侧已有「预检 + `known_bad` + 改游戏源码」的完整方案，
所以这一版只让它**明确失败**，不偷偷改 VM 语义。

## 4. 回归与验证

- **画面未变**：`tools/ci_regress.sh` 三个基准用例与 0.3.8 基线逐位一致

  ```text
  ping 300 帧     0556e78f7ce0e828f2dc922d210dc047
  probe 120 帧    e82844f7160449d1767aa69afe788800
  probe 120 帧按A 0eb7884cb40cef0cb549946af3a942ab
  ```

- **契约测试**：两种像素格式路径均全绿，新增两条断言——超限段镜像必须
  `load_game` 返回 false，且前端日志里要出现 `1014` 与 `32000`。
- **回归夹具**（`tools/fixtures/`，CI 新增 `crash-regression` job）：
  `crash-scale.pxt64` / `crash-idiv.pxt64` 各跑满 600 帧且宿主存活（0.3.8
  是进程直接死）；`oversize-section.pxt64` 必须 `load_game -> FAIL`。
- **63 张社区卡带全量复跑**（0.3.9 win-x64，`A@60 + LEFT@100,140,…,540`，600 帧）：

  | | 0.3.8 | 0.3.9 |
  |---|---|---|
  | 正常出画 | 57 | **59** |
  | 进程级崩溃 | **3** | **0** |
  | VM panic（宿主存活） | 0 | 1 |
  | 明确加载失败 | 0 | 1 |
  | 预期静态 demo | 2 | 2 |

  `CrateMaker` / `Crate_Dungeon` 从 crash 转 ok；`FPS_game_test_menu_test`
  除零被兜住后走到游戏自身的 `PANIC 980`（核心自动重启、宿主存活、600 帧
  跑满、末帧 `032c69da…`），不再是进程级 SIGFPE；`sm64thign` 从黑屏变成
  `load_game -> FAIL`。**其余 59 张末帧哈希与 0.3.8 逐位一致**，没有一张
  正常卡带画面发生变化。

- **补丁表**：`tools/check_pxt_patches.py` 从 11 条扩到 **14 条**，
  `--strict` 校验 PATCH 13/14/15 都在位。
- **发版包体检**：`tools/verify_release.py` 仍是 release job 的硬门槛，
  6 个主包 + 3 个调试包全 PASS 才建 Release。

## 已知限制（未变 / 修订）

- 32 位前端不支持（PXT64 镜像在 32 位 VM 下无法执行）。
- 即时存档 / rewind / 快进 / 金手指 / Netplay 不提供（v0.3.0 起的范围收敛）。
- 核心选项仍为**零**（刻意设计，见 `docs/INPUT.md`）。
- `Strawberry_Slam` 的 `PANIC 980` 是游戏脚本读未初始化数据，且**依赖输入序列**
  （A 口径与四方向口径触发，`A@60 + LEFT` 口径 600 帧不触发），不是必然 panic。
- 单个 VM 段 ≥ 32000 字节的卡带仍无法运行，但现在会明确报 `error 1014`
  而不是黑屏。要救只能改游戏源码（拆大数组字面量）或换非 VM 前端。
- Windows 的「退出 139」等价问题（PE 无 `NODELETE` 标志）未处理，取决于前端
  是否 `FreeLibrary`。
- Switch NX 是独立发布线（`nx-v*`），本版不发 NX 包。

## 升级建议

**建议升级**，尤其是收录社区卡带较多的固件/整合包：0.3.8 上有 3 张卡带能让
RetroArch 直接退出，0.3.9 只是「继续跑」或「明确报失败」，正常卡带画面一位
都没变。

---

MakeCode Arcade core 是非官方的第三方项目，与 Microsoft、Nintendo 均无关。
