# 低端性能优化：测量基线与热点画像（2026-11-19）

## 阶段定位

功能与平台已收口（core-v0.3.10，19 补丁，10 主包 + 3 平台资产，buildbot 收录准备齐），
进入优化阶段。**本阶段只造尺子、不改核心行为**——所有改动编译期默认关闭，
golden 三例逐位一致。

## 口径（别再用错）

| 指标 | 来源 | 能否作性能依据 |
|---|---|---|
| `--turbo` 墙钟吞吐 | `tools/bench_throughput.sh` | ✅ 唯一可比口径 |
| "实际节拍" 38–50 fps | libretro_host 第 11 节 | ❌ 被 Python 宿主 16ms sleep 封顶 ~50fps，是假象 |
| "游戏真实帧率" | 同上 | ❌ 受卡带逻辑限制，不是设备能力 |

历史数据：`docs/perf-2026-11-04/`、`docs/DEVICE-TEST.md`。
低端目标 = **R36S / RK3326@1.5**：cubeland 满速 65 fps，仅 1.08× 余量，是全系列下限；
RG353M/RG353V（RK3566）146–150 fps（2.4–2.5×），无需动。

## 新增工具

### 1. 分段计时（编译期可选，默认零开销）

- `runtime/pxtapp/mca_profile.h`：宏 + 阶段枚举 + 单调钟封装。默认 `MCA_PROFILE`
  未定义时全部展开成 `((void)0)`，不 include time.h、不定义符号。
- 插桩点：
  - 前端线程 `core/src/libretro_core.cpp`：PULL_SCREEN / PIXEL_CONV / AUDIO /
    VIDEO / FRONT_TOTAL
  - VM 线程 `runtime/pxtapp/core---vm/scheduler.cpp`：VM_EXEC（exec_loop，
    游戏逻辑+绘制）/ VM_PUBLISH
- 构建：`make -f core/Makefile platform=unix MCA_PROFILE=1`
  产物落 `core/build/make/unix-prof/`，**不覆盖**正式 `.so`（正式版 md5 保持
  `0ac38dfb…`，profile 版带 `g_mca_prof` 符号，两者已核对不同）。
- 输出：每 300 帧往 stderr 打一次各段 `us/call`；宿主日志里 grep `MCA-PROF`。
- 验证：默认构建 golden 三例 = `0556e78f / e82844f7 / 0eb7884c` 逐位一致；
  默认版无计时符号。

### 1b. armv7 profile 核心构建（真机用，对象隔离）
- `BUILD_SUFFIX=-prof EXTRA_CXXFLAGS=-DMCA_PROFILE ./tools/build_target.sh armv7a-linux-androideabi21`
  产物落 `core/dist/armv7a-linux-androideabi21-prof/`，`BUILD_SUFFIX` 只改目录名、
  `TARGET_FLAGS` 仍用原始 triple，正式 armv7 对象零污染。
- profile 版与正式版同 900 帧末帧 hash 逐位一致（cubeland `23210fb3…`）：计时不影响画面。
- 78P01 这类 SDK 17 老机需 `tools/android_test/api17_shim.c` 的 LD_PRELOAD 垫片才能
  dlopen API 21 核心（见 `DEVICE-LEDGER.md` §3）。

### 2. 可复现吞吐基线

`tools/bench_throughput.sh <core.so> [frames=1200] [repeats=3] [outdir=out/bench]`

Android 真机无 Python/无 `%N`：C 宿主 `tools/mca_host_android.c` 的 `--wall` 开关
在 STATS 行追加 `wall_ms`/`fps`（单调钟，默认关闭不影响原口径），63 卡 sweep 用此口径。

统一卡带（ping / Blocky / cubeland）、帧数、`--turbo`、重复取中位数，
CSV 附末帧哈希（吞吐再高、画面和 golden 不一致也无意义）。同一脚本可在
tool69 与 R36S 上跑，输出口径一致。`CARTS_DIR=` 可覆盖卡带目录。

## tool69 基线（x86_64，方法学验证用，非目标机）

| 卡带 | 默认构建吞吐 | profile 构建吞吐 | 末帧哈希 |
|---|---|---|---|
| ping | 2564 fps | 2702 fps | `7a7954e8…`（三轮一致） |
| Blocky_Boss_Battle | 1712 fps | 1625 fps | `711241e3…`（三轮一致） |
| arcade-cube-land | 922 fps | 910 fps | `a1c10cd9…`（三轮一致） |

两构型差异在 x86 抖动范围（±5%）：**计时开销可忽略**。数值本身是 x86 高性能机，
只证明方法可复现，不代表低端。

## "卡带 × 阶段"热点画像（tool69，us/call）

| 阶段 | ping | Blocky | cubeland | 随卡带变化 |
|---|---|---|---|---|
| FRONT_TOTAL | ~357 | ~590 | ~1084 | 涨（含等待） |
| PULL_SCREEN（前端等 VM） | 92.5 | 322.5 | 828.9 | **涨 9×** |
| PIXEL_CONV（BGRA→565） | 10.1 | 11.8 | 17.7 | 几乎不变 |
| AUDIO | 0.7 | 1.6 | 1.1 | 恒定 |
| VIDEO（宿主回调） | 226.7 | 221.4 | 189.0 | 恒定，真机不可比 |
| VM_EXEC（游戏逻辑+绘制） | 161.4 | 553.9 | 1156.5 | **涨 7×** |
| VM_PUBLISH | 2.1 | 2.2 | 1.9 | 恒定 |

**结论**：

1. **VM_EXEC 是唯一随游戏复杂度暴涨的核心段**（161→1157 us），就是 R36S 掉帧的
   主因来源。它内部 = 解释执行游戏逻辑 + `image.cpp` 的 blit/draw。
2. **PIXEL_CONV 固定 10–18 us**，与卡带无关，之前文档"代价可忽略"的判断成立，
   **不是优化靶子**。
3. PULL_SCREEN 与 VM_EXEC 是同一份 VM 时间的两种观察（前端等 VM 产帧 ↔ VM 在
   算），不能相加当成两个独立成本。判断瓶颈看 VM_EXEC 即可。
4. VIDEO 段是 Python 宿主回调，真机由前端处理，不可比。

## 78P01 真机画像（armv7 / SDK 17 / MT6592 2×A7@1.14G，us/call，900 帧 report#3）

这是第一份**真 ARM**热点画像（此前 tool69 是 x86）。垫片后跑 profile 核心，
三卡带全 `EXIT=0`、末帧 hash 与正式版逐位一致。

| 阶段 | ping | Blocky | cubeland | 随卡带变化 | 对照 tool69(x86) |
|---|---|---|---|---|---|
| FRONT_TOTAL | 817 | 4171 | 9183 | 涨 11× | 357→1084 |
| PULL_SCREEN（前端等 VM） | 638 | 3976 | 8977 | **涨 14×** | 92→829 |
| PIXEL_CONV（BGRA→565） | 145 | 147 | 156 | **恒定** | 10→18（ARM 高 8–14×，无向量化） |
| AUDIO | 4 | 13 | 5 | 恒定 | 0.7→1.1 |
| VIDEO（宿主回调） | 20 | 24 | 31 | 恒定 | 190–227（C 宿主无前端缩放，不可比） |
| VM_EXEC（逻辑+绘制） | 965 | 6549 | 11594 | **涨 12×** | 161→1157 |
| VM_PUBLISH | 154 | 47 | 25 | 降（竞争） | ~2 |
| VM_EXEC 实际 calls | 530 | 544 | 694 | 900 帧里 VM 只产 530–694 帧 | — |

**结论**：

1. **VM_EXEC 仍是唯一随复杂度暴涨的核心段**，真机上 965→11594 us（0.97→11.6 ms），
   cubeland 单帧就吃掉 11.6 ms——这就是低端掉帧主因，与 tool69 判断一致。
2. **VM 线程跟不上前端**：900 帧前端产出里 VM 只算了 530–694 次（59%–77%），
   PULL_SCREEN 8977 us 是前端空等 VM 的时间，与 VM_EXEC 是同一份时间，不能相加。
3. **PIXEL_CONV 在 ARM 上 ~146 us（比 x86 高一个量级）但仍只占 cubeland 预算 0.8%**，
   确认不是优化靶子；之前"代价可忽略"的判断在真机上依然成立。

## 63 卡全量吞吐 sweep（78P01，600 帧 --wall，2026-11-19）

`tools/mca_host_android.c` 加 `--wall`（默认关，STATS 行追加 `wall_ms`/`fps`），
解决了老 toybox `date` 无 `%N`、秒级精度不够的问题。63 张真实卡带全量跑完
（`out/78p01/sweep63.txt`）：**62 张 rc=0，1 张已知坏卡 `sm64thign` load_game FAIL**。

**最慢 12 张（78P01 = 系列最低端 2×A7）**：

| 卡带 | fps | 600帧耗时 | distinct |
|---|---|---|---|
| unique_cube | 9.7 | 62.1s | 267 |
| Horizontally_Spinning_Rat | 9.9 | 60.8s | 15 |
| TEXTURED_BROHANN_CUBE | 9.9 | 60.8s | 2 |
| 3D_dinosaur_model | 17.2 | 34.8s | 218 |
| 3D_dinosaur_model_resolution_increase | 17.3 | 34.7s | 175 |
| low-poly_picachu_at_SUPER_HIGH_FPS | 26.2 | 22.9s | 2 |
| arcade-cube-land | 105.3 | 5.7s | 15 |
| stereoscopic_3d | 106.3 | 5.6s | 2 |
| Cave_Explorer | 109.1 | 5.5s | 220 |
| Spin | 156.9 | 3.8s | 299 |
| Hot_Air_Balloon | 167.1 | 3.6s | 15 |
| **arcade-jumpy-platformer** | **168.2** | **3.6s** | 15 |

**jupmy 结论**：用户直觉它卡，但实测 **168 fps = 60fps 目标的 2.8× 余量，不卡**；
3 次重跑 hash 全一致（`1c0f2dd4…`）、fps 168–173 稳定。它在 63 张里排第 12 慢，
属于中下，但绝对吞吐远超实时需求。它慢于 ping(1154fps) 是因为 dist=15 的场景
每帧都有绘制，而 ping 大量帧逻辑空转。

**真正 <60fps 的 6 张**（实机 60fps 节拍下会掉帧）：unique_cube、
Horizontally_Spinning_Rat、TEXTURED_BROHANN_CUBE、两张 3D_dinosaur_model、
low-poly_picachu。全是 3D/软件光栅类重卡。注意：`unique_cube` 三次三个不同 hash
（`5fc5…`/`b41a…`/`44a0…`），`Horizontally_Spinning_Rat` 历史也有墙钟抖动——
这两张的 hash 不可用于逐位比对，但 fps 稳定（9.7/9.9），吞吐数据可用。

## 方向修正：drawImageCore 不是瓶颈（2026-11-19 实测）

此前依据"覆盖率仅 18%、863 行未执行、上游注释 5% faster"推断 `image.cpp` 的
`drawImageCore` 是 VM_EXEC 大头。**加了 DRAW_IMAGE 子段实测后推翻该推断**：

| 卡带 | DRAW_IMAGE us/call | calls / 300帧 | 每帧 blit 耗时 | VM_EXEC 每帧 | blit 占比 |
|---|---|---|---|---|---|
| ping | 30.2 | 4876 | ~0.5 ms | 0.98 ms | 高（但绝对值可忽略） |
| Blocky | 3.8 | 15037 | ~0.19 ms | 6.5 ms | **~3%** |
| cubeland | 8.4 | 4410 | ~0.12 ms | 11.7 ms | **~1%** |

`drawImageCore` 已是高度展开的优化循环，**每次调用仅几微秒**，不是靶子。
VM_EXEC 的 99% 在 **PXT 解释器主循环**（`core---vm/vm.cpp:599 exec_loop()` 的
opcode 函数指针分派 `opcodes[op](ctx, arg)`）——cubeland 逻辑重、每帧执行大量字节码。
之前"覆盖率低=值得优化"的推理是错的：覆盖率低是因为这段本来就快。

`mca_profile.h` 新增 `MCA_PF_DRAW_IMAGE` 段常驻（默认零开销），以后每轮优化都能
直接看 blit 占比，不用再猜。`DRAW_IMAGE` 插桩见 image.cpp 的 `drawImageCoreImpl` 包装。

## 下一步（靶子转向解释器，待决策）

VM_EXEC 的大头是解释器 opcode 分派，可选方向（**均未动手，需先评估风险/收益**）：

1. **解释器分派优化**：`exec_loop` 的 `opcodes[op](ctx, arg)` 间接调用在 ARM 上
   分支预测代价高。可试 computed-goto / 内联高频 opcode，但涉及 PXT ABI，
   逐位一致验证成本高。**收益未知，风险高**。
2. **不改解释器，改 GC/分配**：若热点在对象分配（GC sweep），有更安全的切入点。
   需先加 GC 子段实测再定。
3. **接受现状**：低端机 cubeland ~104fps 已远超 60fps 目标（1.7× 余量），
   78P01 是系列最低端；R36S 口径 65fps/1.08× 才是真瓶颈，但那台关机无法测。
   若目标是"够用"，可能不必为 1.08× 的边际机型承担核心改动风险。

通用验证铁律不变：golden 三例逐位一致 + 全卡带 sweep + 吞吐基线对比
（78P01 口径 ping ≥1200 / Blocky ~343 / cubeland ~104 fps）。

## 已知限制

- 3DS/Vita 是静态链接（无 dlopen）、无 Python 宿主、`--turbo` 口径不适用，
  本轮测量链路**不覆盖**这两平台。若需覆盖，得接平台 tick API
  （`svcGetSystemTick` / `sceKernelGetProcessTimeWide`）重编 cia/vpk，属另一个工作量。
- tool69 的 VIDEO 段含跨进程开销，仅作方法学占位。
