# MakeCode Arcade libretro Core（路线 B）

把 MakeCode Arcade 游戏作为"卡带"在 RetroArch 中运行，主线技术路线为
**pxt VM 字节码 + C++ 运行时**（路线 B），目标平台包括 PC 与低端 ARM 掌机
（RK3326 / RK3566 等）。

## 与既有两个 core 的关系

本项目相关姊妹仓库（同为 whmzsu 所有，均已在生产使用）：

| 仓库 | 引擎 | 定位 | 实测性能 |
|---|---|---|---|
| [mcarcade-core](https://github.com/whmzsu/mcarcade-core) | QuickJS 解释执行官方 sim | 兼容基准（语义=浏览器） | 桌面 ping 5ms/帧、cube-land 10ms/帧；RK3566 约 3~4 倍慢，重游戏掉帧 |
| [mcarcade-libretro](https://github.com/whmzsu/mcarcade-libretro) | Hermes AOT 字节码 + JIT | 性能升级尝试 | 桌面与 QuickJS 相近；aarch64 JIT 收益待真机验证 |
| 本项目 | **pxt VM 字节码 + 原生 C++ 引擎**（无 JS 引擎） | 低端掌机专用，路线 B | ✅ RK3566 真机流畅无卡顿；目标 60fps 单核 <15%，常驻 <16MB |

三个 core 共享同一套 C 基础设施（cart.c 卡带解析 / decompress.c LZMA /
cache.c 编译缓存 / 内嵌 pxtc 编译器）。路线 B 的价值在于**彻底去掉
JS 引擎层**——JS 模拟层（DOM mock、promise 微任务、GC、JS 逐帧逻辑）
正是弱机上 step 耗时的根源。

## 目录结构

```
makecode_arcade/
├── README.md               本文件
├── docs/
│   ├── ROUTE-B-DESIGN.md   路线 B 总体设计（架构 / 数据流 / 里程碑 / 风险）
│   ├── PNG-FORMAT.md       .png 卡带（隐写打包）格式规格
│   ├── M2-RUNTIME-NOTES.md pxt 运行时考古笔记（官方宿主 8 函数 ABI）
│   ├── M3-VM-HOST.md       VM 宿主接入、验收记录与已知限制
│   ├── CROSS-COMPILE.md    交叉编译（目标矩阵 / 用法 / 踩坑）
│   ├── DEVICE-TEST.md      真机验收记录（RK3566 + ArkOS，含故障复盘）
│   └── INPUT.md            按键与选项（映射、描述符、验证方法）
├── core/                   libretro core（C/C++）
│   ├── libretro.h          官方头文件（来自 libretro-common，MIT）
│   ├── Makefile            构建（支持 aarch64 交叉编译）
│   ├── src/
│   │   ├── libretro_core.cpp   libretro 入口 + 帧循环 + 视频/音频/输入接线
│   │   ├── vm_host.cpp         pxt VM 宿主封装（镜像装载/按键沿/拉帧拉音频）
│   │   ├── png_loader.h/.cpp   卡带解码 C++ 封装
│   │   ├── cart.c/.h           卡带解析（vendored 自 mcarcade-core，MIT）
│   │   ├── decompress.c/.h     LZMA-alone 解压（vendored，MIT）
│   │   └── platform.h          pxt VM 宿主 ABI（对齐官方 sdlmain.cpp）
│   ├── tests/              假前端 core_test / VM 冒烟 vm_smoke / 解码冒烟 cart_smoke
│   └── third_party/            miniz（tinfl）+ LzmaDec（vendored）
├── runtime/
│   ├── pxtapp/             pxt 官方 VM 内核+引擎原语（vendored，已打 3 个补丁，游戏无关）
│   └── VENDORING.md
├── tools/
│   ├── build_core.sh       一键构建：DLL + 三个测试程序
│   ├── build_target.sh     单目标交叉编译（zig；Android 走 NDK clang）
│   ├── build_matrix.sh     发版矩阵（7 主流目标）/ --all 全架构
│   ├── package_release.sh  打包发行 zip（core + .info + 说明）
│   ├── inspect_binary.py   产物自检（架构/libc 基线/依赖/retro_* 导出，纯标准库）
│   ├── png2pxt64.py        卡带(.png)→字节码(.pxt64) 全自动离线转换
│   ├── vm-build/           pxt 工具链（node_modules）与 VM 构建脚本
│   ├── extract_png_meta.py 独立 PNG 卡带解码验证工具（纯 Python 标准库）
│   └── make_test_cartridge.py  合成测试卡带生成器（编解码回环自测）
├── test_cartridge.png      合成卡带样本（自测通过）
└── refs/                   参考资料（不进 core）
    ├── jan4984-lib.rs      Rust+V8 原型的 PNG 解码与 libretro 胶水
    ├── jan4984-binding.js  原型的模拟器 API 绑定清单
    ├── whmzsu-mcarcade-core-README.md       姊妹核心 QuickJS 版说明
    ├── whmzsu-mcarcade-libretro-README.md   姊妹核心 Hermes 版说明
    └── whmzsu-libretro/    姊妹核心源码快照（cart/cache/compiler/core 等）
```

## 里程碑速览

| 里程碑 | 内容 | 状态 |
|---|---|---|
| M0 | core 骨架可编译、可被 RetroArch 加载、显示测试画面 | ✅ 编译验证通过（zig 0.17.0） |
| M1 | PNG 卡带解码（像素解码 + blob 位流 + LZMA + 项目 JSON） | ✅ cart.c 接入完成，3 张真实卡带冒烟通过（source 字节数与 Python 解码/生产日志一致） |
| M2 | vendor pxt C++ 运行时（screen/sprite/引擎）独立构建 | ✅ pxtapp vendored 到 runtime/pxtapp（33 TU 游戏无关，已验证 ping/jumpy/cube-land 共用同一二进制） |
| M3 | pxt VM 字节码解释器接入，跑通第一个游戏 | ✅ **三款游戏全通**：vm_smoke（官方 ABI 直调）+ libretro 假前端路径（RGB565/按键沿/音频） |
| M4 | 输入 / 音频合成 / 60fps 帧模型完整化 | 宿主接口全部打通；剩余：菜单键映射细化、panic 恢复策略、存档目录对接前端 |
| M5 | 离线转换工具（PNG→bytecode）+ 游戏兼容性测试集 | ✅ tools/png2pxt64.py 全自动转换，三卡带实测通过 |
| M6 | 交叉编译（掌机/桌面/移动）+ 性能验证 | ✅ **7 个主流目标全通并出 zip**（Windows x64/arm64、Linux x64/arm64/armhf、Android arm64/armv7）；✅ **RK3566 + ArkOS 真机验收通过**（v0.2.1，用户反馈运行正常、完全无卡顿；帧率数值待量化）；✅ **Android arm64 验收通过**（v0.2.7，功能与 Linux aarch64 一致） |

## 构建工具链

本机统一用 **zig**（`zig cc`/`zig c++` 即 C/C++ 编译驱动，与姊妹 core
一致），位于 `../common tools/zig/zig.exe`（0.17.0）。Android 目标用
**NDK 自带 clang**（`../common tools/android-ndk-r27c`，zig 的 libc++ 与
bionic 头不兼容）。

```bash
tools/build_core.sh   # 本机：DLL + core_test + vm_smoke + cart_smoke
```

### 卡带转换（给玩家用）

```bash
tools/cartconv/package_cartconv.sh   # 打免安装便携包（约 85MB）
```

产物 `dist/cartconv/makecode-cartconv-<ver>-windows-x64.zip`：自带 node 运行时和
pxt 编译链，玩家解压后把 `.png` 拖到 `cartconv.exe` 上即可，单个/多个/整个
目录都支持。`.png` 卡带里只有源码，必须现场编译成 `.pxt64`，所以这一步绕不开。
当前版本 **1.1.0**，压缩包 83 MB。

| 场景 | 命令 |
|---|---|
| 拖拽 | 把 `.png` 或文件夹拖到 `cartconv.exe` |
| 单张 | `cartconv.exe game.png` |
| 批量 | `cartconv.exe carts\ -o out\` |

单张约 10 秒（旧脚本 29 秒）。实现细节与踩坑见 `tools/cartconv/README.md`。

**支持范围**：已在 4 张真实卡带上验证（bpp=1/2、blocks/TS、含中文、带 animation /
color-coded-tilemap 依赖）。工具在编译前会预检，遇到转不了的情况直接说明原因，
不会产出坏文件：

| 情况 | 表现 |
|---|---|
| 依赖社区扩展 `gh:user/repo` | 编译前拦下（离线无法下载） |
| 源码含二进制资源 | 编译前拦下（当前只支持纯文本工程） |
| 本地没有的官方扩展 | `install` 后核对，报出缺哪个 |
| 卡带比编译器新 | 警告但继续编译 |

### 交叉编译与发版

纯 C/C++、无平台专有依赖，zig 自带 libc 头，换 `-target` 即可：

```bash
tools/build_matrix.sh     # 发版矩阵：7 个主流目标（约 30 秒）
tools/package_release.sh  # 打包成可直接放进 RetroArch 的 zip
```

| 平台 | 产物 |
|---|---|
| Windows x64 / ARM64 | `makecode_arcade_windows-x64.zip` / `-arm64.zip` |
| Linux x64 / ARM64 / ARMhf | `makecode_arcade_linux-x64.zip` / `-arm64.zip` / `-armhf.zip` |
| Android ARM64 / ARMv7 | `makecode_arcade_android-arm64.zip` / `-armv7.zip` |

每个 zip 内含 core 二进制 + `.info` + 安装说明。Linux 目标固定用 glibc 2.17
基线（覆盖 2012 年后的掌机固件）；Android 静态链 libc++ 并做 16KB 页对齐
（Android 15）。

> **Android 安装要点**：RetroArch 的「安装或恢复核心」只提取 `.so`，`.info`
> 需要单独放到**核心信息目录**（`libretro_info_path`，默认为核心目录本身）。
> 核心目录必须留在 App 私有空间——Android 10 起 `/storage/emulated/0` 是
> noexec，core 放那里能识别但加载必失败。完整步骤见
> `docs/DEVICE-TEST.md` 第五轮。

```bash
tools/build_matrix.sh --all                    # 全架构验证（26/27 通过）
tools/build_matrix.sh --prune                  # 回到发版 7 目标，并删掉其余目标的 dist/build 目录
python tools/inspect_binary.py                 # 架构 + libc 基线 + 依赖 + retro_* 导出
```

产物在 `core/dist/<triple>/`，汇总表 `core/dist/MATRIX.md`。工作区默认只保留
发版 7 个目标的产物（`--all` 验证完用 `--prune` 收拾，`dist/release/` 的 zip 不受影响）。
细节与坑（glibc 基线、Android NDK、32 位 MIPS、大端字节序）见
**docs/CROSS-COMPILE.md**。

pxt 工具链（仅重新生成字节码/升级 pxt 版本时需要）：node + npm，
已装于 `tools/vm-build/node_modules`（pxt-arcade@4.2.1 + pxt-core@13.2.1）。

## 测试（不需要 RetroArch）

```bash
tools/build_core.sh

# 1) 卡带 → 字节码（离线，一次性）
python tools/png2pxt64.py png_examples/arcade-ping.png out/ping.pxt64

# 2) 假前端走完整 libretro 路径跑 .pxt64（60fps 节拍，出 BMP）
core/core_test.exe out/ping.pxt64 out/ping.bmp 120 --press=LEFT --every=40

# 3) PNG 卡带 + 旁载 .pxt64（同目录同名文件，模拟真实使用）
core/core_test.exe png_examples/arcade-ping.png out/ping2.bmp 60   # 无旁载时显示测试画面

# 4) 回归
core/build/cart_smoke.exe png_examples/*.png        # 卡带解码

# 5) 按键映射回归（探针卡带 + 颜色断言，16 个用例）
python tools/make_probe_cartridge.py                # 生成 out/pxt64/probe.pxt64
python tools/test_buttons.py

# 6) 生命周期回归（三连换游戏 / 关闭游戏后 dlclose / 重置后音频恢复）
python tools/make_probe_cartridge.py --audio        # 生成 out/pxt64/audio_probe.pxt64
python tools/test_lifecycle.py
```

已验证：ping / jumpy / cube-land 三款游戏在**同一份预编译 core 二进制**上
通过 VM 字节码运行并正确渲染（pxtapp C++ 树与游戏无关，
functions.json 逐字节一致）。

## RetroArch 使用

1. 取 `core/dist/release/makecode_arcade_<platform>.zip`，把其中的 core 二进制
   与 `makecode_arcade_libretro.info` 一起放进 RetroArch 的 cores 目录；
2. 卡带用离线转换产物 `.pxt64`（或把 `.pxt64` 放在 `.png` 同目录同名，
   core 会自动旁载）；
3. 像素格式 RGB565，160x120@60fps，音频 44.1kHz 单声道。
   重置 / 重启 / 换游戏：
   - 重置键（默认 SELECT，可在 Controls 改绑）或前端「重启游戏」= 重开当前游戏，
     VM 状态完全复位，**画面与声音都会恢复**；
   - 「关闭游戏」「换另一张卡带」会真正停掉 VM 线程（`retro_unload_game` /
     `retro_deinit` 里 join），不再出现"一关游戏 RetroArch 就崩"；
   - 每次重置都会写 trace 到存档目录下的 `mca_trace.log`（写不进去会退到
     当前目录 / `/tmp`，实际路径会打进 RetroArch 日志的
     `[MCA] core version ... trace -> ...` 一行），卡住时看它停在哪一步。

## 按键

默认映射（RetroPad → MakeCode）：

| RetroPad | MakeCode | 说明 |
|---|---|---|
| A | A | 跳跃 / 确认 |
| B | B | 返回 / 取消 |
| 十字键 ↑↓←→ | 方向 | |
| START | Game Menu | 游戏内菜单（真机右侧的菜单小键） |
| SELECT | Reset | 真机右侧的重置小键，按一下重开当前游戏 |

**核心不带任何选项**（快捷菜单 → 选项里是空的）。所有按键调整都在
快捷菜单 → 控制（Controls）里做，这一页由 core 的 `SET_INPUT_DESCRIPTORS`
提供 8 条带语义的标签（`A -> MakeCode A (jump / OK)`、
`Start -> Game Menu (in-game menu key)`、`Select -> Reset (restart game)` 等）：

- 掌机 A/B 位置和 MakeCode 相反 → 把 "MakeCode A" 绑到物理 B 键，反之亦然
- 想换个键开游戏菜单 / 重开游戏 → 把对应那条绑到别的物理键

> 注意区分：`Start -> Game Menu` 是**游戏自己的**菜单；RetroArch 另有一个
> 热键 `Menu Toggle`（呼出前端快速菜单）。两者撞键时到 设置 → 输入 → 热键
> 里把 `Menu Toggle` 改掉或设为 none。

自动化回归：

- `python tools/test_buttons.py` —— 按键。用探针卡带把每个键刷成固定背景色，
  抓末帧比对颜色，16 个用例覆盖 8 条映射、未映射键不误触发、位掩码快速路径，
  以及「重置后再按键仍变色」的重启存活证明（只看"回白底"分不清重启成功和
  画面冻住）。
- `python tools/test_lifecycle.py` —— 生命周期。三连换游戏（unload → load →
  ...）、关闭游戏后 `dlclose` 核心库不崩、重置后音频采样峰值必须重新 > 0
  （`make_probe_cartridge.py --audio` 生成的发声探针卡带），外加描述符自检。
- `libretro_host.py --dry` —— 描述符静态自检（必须恰好 1 条 Game Menu /
  1 条 Reset、同一个键不得重复、不得发核心选项）。

设计说明（含重置、卸载与重启后音频的实现）见 `docs/INPUT.md`。

详见 docs/ROUTE-B-DESIGN.md。
