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

### 2. 可复现吞吐基线

`tools/bench_throughput.sh <core.so> [frames=1200] [repeats=3] [outdir=out/bench]`

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

## 下一步（需把尺子搬到目标机）

tool69 是 x86，**热点排序要按 R36S 的真实数字重测**才有意义——ARM 无 x86 的
自动向量化，`image.cpp` 各 bpp 分支的相对成本可能不同。步骤：

1. R36S 上装 `out/core-prof.so` + 用 `bench_throughput.sh` 同口径跑，出真实热点画像。
2. 若确认 VM_EXEC 内部 blit 占大头 → 定点优化 `image.cpp` 的 `drawImageCore`
   （覆盖率仅 18%、863 行未执行，上游自带 "expanded version 5% faster" 注释）。
3. 每步优化后：golden 三例逐位一致 + 全卡带 sweep 逐位比对 + 吞吐基线对比。

## 已知限制

- 3DS/Vita 是静态链接（无 dlopen）、无 Python 宿主、`--turbo` 口径不适用，
  本轮测量链路**不覆盖**这两平台。若需覆盖，得接平台 tick API
  （`svcGetSystemTick` / `sceKernelGetProcessTimeWide`）重编 cia/vpk，属另一个工作量。
- tool69 的 VIDEO 段含跨进程开销，仅作方法学占位。
