# 路线 B 总体设计：pxt VM 字节码 + C++ 运行时的 libretro core

日期：2026-11-01　状态：定稿（骨架阶段）

## 0. 目标与非目标

**目标**

- RetroArch 加载 MakeCode Arcade 游戏（.png 卡带），60fps 完整体验。
- 面向低端 ARM 掌机（RK3326：4×A35@1.3GHz/1GB；RK3566：4×A55@1.8GHz）
  做到 core 常驻内存 < 16MB、60fps 时单核占用 < 15%。
- 纯 C/C++ 静态编译，无 JIT 依赖（规避 W^X 与交叉编译复杂度）。

**非目标**

- 不做实体掌机 .uf2 的整机模拟（那是路线 D，长期可选）。
- MVP 阶段不做 in-core TS 编译。

## 1. 为什么是路线 B

参照系：游戏在 120MHz Cortex-M4 真机上跑满 60fps → 任何解释方案在
A35/A55 上都有 20~100 倍算力余量。低端设备的真实瓶颈是**内存、启动
时间、功耗**，而非速度：

| 方案 | 内存 | 交叉编译 | 结论 |
|---|---|---|---|
| A. 内嵌 JS 引擎 | V8 ~100MB / QuickJS 慢 10-30x | 差（V8） | 仅适合 PC 端 MVP 验证 |
| **B. pxt VM + C++ 运行时** | **5~15MB** | **无痛** | **主线** |
| C. WASM + wasm3 | 3~10MB | 无痛 | 备选，构建链不确定 |
| D. 整机模拟 M4 | 10~30MB | 一般 | 模拟开销大，长期可选 |

pxt 的 bytecode/VM 目标与 libSDL 原生版（iOS/Android/桌面）同源，
已被官方在树莓派 Zero（aarch64 相近算力）上验证过。

## 1.5 既有 QuickJS / Hermes core 的实测性能（路线 B 的立项依据）

姊妹仓库 mcarcade-core（QuickJS）与 mcarcade-libretro（Hermes）提供了
真实性能数据（详见 refs/ 下两份 README）：

- **桌面 x64**：ping 5.0ms/帧、jumpy 8.0ms/帧、cube-land 10ms/帧
  （16.67ms 预算），优化已做完（精灵 blit C 化、音频段表游标、零拷贝
  取帧、无头渲染裁剪）——JS 引擎侧可挤的水分基本挤干了。
- **RK3566（4×A55@1.8GHz）**：step 约为桌面 3~4 倍 → 重游戏
  20~40ms/帧，60fps 失守。这正是路线 B 要解决的场景。
- **Hermes 版**：AOT 字节码 + JIT，桌面收益不明显；aarch64 真 JIT
  （Linux/Android）尚未真机验证，且 JIT 需 W^X 映射，掌机固件上
  不保证可用。即使 Hermes 解释器比 QuickJS 快数倍，**JS 模拟层的
  结构性开销（DOM/WebAudio mock、promise 微任务链、GC、JS 逐帧
  对象分配）依然存在**——这是路线 B 与它们的本质区别：路线 B 的
  引擎是原生 C++，同一套引擎在 120MHz MCU 上就能跑满帧。
- **编译路径差异**：两者跑的是官方 sim 的 binary.js；路线 B 需要
  pxt VM 字节码（M5 转换器），编译思路可复用内嵌 pxtc 的工程经验
  （内存 VFS、Buffer 替身、microtask 抽干等，见 core 仓库 README 5.1）。

**结论**：QuickJS/Hermes core 继续作为"兼容基准 + 桌面/强机产品"，
路线 B core 面向 RK3326/3566 级掌机，三者共享 C 基础设施
（cart/decompress/cache），语义以 QuickJS 版的逐字节画面输出为基准。

## 2. 总体架构

```
                .png 卡带（源码+资源，隐写）
                        │ tools/pxt-convert（Node 端离线转换，M5）
                        ▼
                .mka 字节码包（游戏 bytecode + 资源清单）
                        │
┌─────────────── libretro core ─────────────────┐
│  retro_load_game                              │
│   └─ 卡带装载器（.png 直读 或 .mka）           │
│  retro_run（每帧步进 16.639ms 虚拟时间）        │
│   ├─ pxt VM        字节码解释器（pxt 官方）     │
│   ├─ 游戏引擎 C++  sprite/physics/scene/music  │
│   ├─ fiber 调度器   pxt 运行时按帧步进改造       │
│   └─ HAL shim      ──→ video_cb(160x120)      │
│                     ──→ audio_batch(int16)     │
│                     ──→ input_state(RetroPad)  │
└───────────────────────────────────────────────┘
```

关键决策：

1. **ROM 双格式**：MVP 同时接受 .png（core 内解码到 JSON）与 .mka
   （预转换字节码）。字节码编译器（pxt toolchain 需要 Node）放离线
   转换器，core 保持零重型依赖。
2. **帧模型**：pxt 运行时是事件驱动 + fiber 协作调度（codal 风格）。
   core 把它改造成"每帧一步"：`retro_run` → advance timers(16.639ms)
   → 恢复/让出 fiber → 渲染 → 输出音频。这是路线 B 最大的一处运行时
   改造，M3 的核心工作。
3. **显示管线**：引擎内部保持 160x120 4bpp 索引色 + 调色板（与真机
   一致），HAL 层查表转 RGB565 后交 libretro，零拷贝语义。

## 3. 模块划分（core/src）

| 模块 | 职责 | 来源 |
|---|---|---|
| libretro_core.cpp | libretro 入口、帧循环、音视频输入接线 | 自写（骨架已完成） |
| cart.c / decompress.c | PNG 卡带解析 + LZMA（**已 vendor**） | 姊妹 core mcarcade-core（MIT） |
| png_loader.cpp | cart.c 的 C++ 封装 | 自写（已完成） |
| platform.h | HAL 接口（screen/sound/input/timer） | 自写（接口已定义） |
| pxtvm/ | pxt VM 字节码解释器 | vendor 自 microsoft/pxt（M3） |
| engine/ | pxt-arcade C++ 引擎（screen/sprite/sound） | vendor（M2，具体 repo 见 VENDORING.md） |
| convert/（tools） | PNG→bytecode 离线转换器 | 依赖 pxt toolchain（Node），可复用姊妹 core 内嵌 pxtc 的工程经验 |

## 4. 里程碑与验收标准

- **M0 骨架**（当前）：core 编译通过；RetroArch 手动加载显示动态测试画面；
  日志接口、像素格式协商（RGB565 优先）、RetroPad 轮询就绪。
  （注意：本机暂无 C 编译器，骨架尚未编译验证。）
- **M1 PNG 解码**（✅ 代码就位）：vendor 姊妹 core 的 cart.c/decompress.c
  并接入 retro_load_game；tools/extract_png_meta.py 用 ≥3 个真实卡带
  验证输出与姊妹 core 一致（source 文件表内容逐字节相同）。
- **M2 运行时 vendor**：引擎 C++ 代码在 core 构建体系内独立编译通过，
  用一个单元测试把静态场景渲染到 framebuffer 存 PNG 对拍。
- **M3 VM 接入**：手动用 pxt 编一个小游戏得到字节码，VM 解释执行、
  fiber 按帧步进、画面与官方模拟器逐帧目测一致。
- **M4 完整外设**：RetroPad → controller API 映射表；Sound 合成器输出
  int16 音频（44100Hz 立体声）；游戏节奏（splash/计时器）与真机一致。
- **M5 转换器 + 兼容性**：`tools/pxt-convert game.png` 一键产出 .mka；
  建立官方示例游戏回归集（≥10 款）。
- **M6 上掌机**：aarch64 交叉编译产物在 RK3326 实机 60fps、内存达标。

## 5. 风险与对策

| 风险 | 影响 | 对策 |
|---|---|---|
| fiber 调度与帧模型耦合深 | M3 工作量失控 | 先通"单 fiber 每帧跑满"的简化模型，再补协作式让出 |
| pxt VM 与 pxt 版本耦合 | 字节码格式漂移 | 锁定 pxt tag；转换器与 core 同源同版本 |
| LZMA 依赖体积 | 掌机包体 | 已随 cart.c vendor LzmaDec.c（几 KB，public domain），无需完整 liblzma |
| 音频合成时序 | 卡顿/破音 | 音频按样本时钟独立于视频步进，缓冲 2~3 帧 |
| pxt C++ 引擎 repo 边界不明 | M2 返工 | VENDORING.md 先做代码考古，确认后再 vendor |

## 6. 许可

core 自写代码：MIT。vendor 的 pxt / pxt-arcade / libretro-common 均为
MIT，需在 runtime/VENDORING.md 维护来源与版本清单。
