# MakeCode Arcade libretro Core（路线 B）

[![build](https://github.com/whmzsu/makecode-arcade-libretro/actions/workflows/build.yml/badge.svg)](https://github.com/whmzsu/makecode-arcade-libretro/actions/workflows/build.yml)

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
├── LICENSE                 MIT
├── .github/workflows/build.yml  GitHub Actions：全平台编译回归 + 测试产物
├── .gitlab-ci.yml          libretro 官方 buildbot 收录用（镜像仓库时生效）
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
│   ├── Makefile            标准 libretro 构建（platform=unix/win/win64/osx/android）
│   ├── jni/                Android.mk + Application.mk（官方 CI 的 ndk-build 路径）
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
│   ├── make_local.sh       用 zig 驱动 core/Makefile（本地无目标工具链时）
│   ├── build_target.sh     单目标交叉编译（zig；Android 走 NDK clang）
│   ├── build_matrix.sh     发版矩阵（7 主流目标）/ --all 全架构
│   ├── package_release.sh  打包发行 zip（core + .info + 说明）
│   ├── inspect_binary.py   产物自检（架构/libc 基线/依赖/retro_* 导出，纯标准库）
│   ├── png2pxt64.py        卡带(.png)→字节码(.pxt64) 全自动转换（调 pxt 工具链）
│   ├── make_probe_cartridge.py  探针卡带生成器（按键/生命周期回归用）
│   ├── test_buttons.py     按键映射回归（16 用例）
│   ├── test_lifecycle.py   生命周期回归（换游戏/卸载/重置后音频）
│   ├── libretro_host.py    假前端宿主 + 描述符静态自检（--dry）
│   ├── extract_png_meta.py 独立 PNG 卡带解码验证工具（纯 Python 标准库）
│   ├── make_test_cartridge.py  合成测试卡带生成器（编解码回环自测）
│   ├── vm-build/           pxt 工具链依赖清单（package.json）与 VM 构建脚本
│   └── cartconv/           独立卡带转换器（Node.js，玩家用，详见其 README）
├── png_examples/           真实卡带样本（ping / jumpy / cube-land / falling-duck / destroy-the-castle）
└── test_cartridge.png      合成卡带样本（自测通过）
```

## 里程碑速览

| 里程碑 | 内容 | 状态 |
|---|---|---|
| M0 | core 骨架可编译、可被 RetroArch 加载、显示测试画面 | ✅ 编译验证通过（zig 0.17.0） |
| M1 | PNG 卡带解码（像素解码 + blob 位流 + LZMA + 项目 JSON） | ✅ cart.c 接入完成，3 张真实卡带冒烟通过（source 字节数与 Python 解码/生产日志一致） |
| M2 | vendor pxt C++ 运行时（screen/sprite/引擎）独立构建 | ✅ pxtapp vendored 到 runtime/pxtapp（33 TU 游戏无关，已验证 ping/jumpy/cube-land 共用同一二进制） |
| M3 | pxt VM 字节码解释器接入，跑通第一个游戏 | ✅ **三款游戏全通**：vm_smoke（官方 ABI 直调）+ libretro 假前端路径（RGB565/按键沿/音频） |
| M4 | 输入 / 音频合成 / 60fps 帧模型完整化 | ✅ 宿主接口全部打通：按键描述符/分键重绑、音频合成与峰值回归、帧驱动虚拟时钟（前端一圈一帧，快进=前端多取帧）、panic 横幅、存档目录对接（trace/数据走 `GET_SAVE_DIRECTORY`）、**retro_serialize/unserialize 快照**（v0.2.11，同会话 + 跨会话精确恢复，S1–S7 回归全绿） |
| M5 | 离线转换工具（PNG→bytecode）+ 游戏兼容性测试集 | ✅ tools/png2pxt64.py 全自动转换，三卡带实测通过 |
| M6 | 交叉编译（掌机/桌面/移动）+ 性能验证 | ✅ **7 个主流目标全通并出 zip**（Windows x64/arm64、Linux x64/arm64/armhf、Android arm64/armv7）；✅ **RK3566 + ArkOS 真机验收通过**（v0.2.1，用户反馈运行正常、完全无卡顿；帧率数值待量化）；✅ **Android arm64 验收通过**（v0.2.7，功能与 Linux aarch64 一致） |

## 环境要求与快速开始

| 工具 | 什么时候需要 | 说明 |
|---|---|---|
| bash | 跑所有 .sh 脚本 | Windows 用 Git Bash 即可 |
| GNU make | 用 `core/Makefile` 构建（或官方 CI） | Windows 的 Git Bash 不带 make；可用 NDK 自带的 `prebuilt/windows-x86_64/bin/make.exe`，或直接用 `tools/build_core.sh`（不依赖 make） |
| zig ≥ 0.17 | 编译 core（唯一编译依赖） | `zig cc`/`zig c++` 当编译驱动。默认找 `<项目父目录>/common tools/zig/`，也可用环境变量 `ZIG` 指定可执行文件路径 |
| Python 3 | 测试与打包脚本 | 只用标准库，无需 pip 装包 |
| node + npm | 转换卡带（png2pxt64 / cartconv） | 首次使用先 `cd tools/vm-build && npm install`（锁文件锁定 pxt-arcade@4.2.1 + pxt-core@13.2.1；node_modules 不入库） |
| Android NDK | 仅交叉编译 Android 目标 | r27c 验证过。`ANDROID_NDK_HOME` 环境变量，或放 `<项目父目录>/common tools/android-ndk-r27c` |

即：**只想编译 core 跑测试** → zig 就够；**想转换自己的卡带** → 另装 node
（或直接用 cartconv 便携包，见下）；**只想玩** → 下 release zip + 转好的 .pxt64，
什么都不用装。

## 构建工具链

`core/Makefile` 是**标准 libretro 形态**（支持 `platform=`，官方 buildbot 就用它
构建，仓库根的 `.gitlab-ci.yml` 列出已支持的 CI 目标）。本地交叉编译矩阵用
`tools/build_target.sh`（zig 驱动，代码集合与 Makefile 完全一致）。

```bash
# 本机构建（自动探测 unix / win64 / osx）
make -C core

# 交叉编译：把 zig 当编译器驱动传进去
make -C core platform=unix  CC="zig cc -target x86_64-linux-gnu.2.17" \
                            CXX="zig c++ -target x86_64-linux-gnu.2.17"
make -C core platform=win64 CC="zig cc -target x86_64-windows-gnu" \
                            CXX="zig c++ -target x86_64-windows-gnu"
```

> zig 的路径若带空格，`make CC=...` 会被 shell 拆开。`tools/make_local.sh`
> 已经封装好这件事（生成包装脚本再传给 make）：
>
> ```bash
> tools/make_local.sh            # 本机平台
> tools/make_local.sh win64      # Windows x64
> tools/make_local.sh linux-x64  # Linux x86_64（glibc 2.17 基线）
> tools/make_local.sh clean win64
> ```

Android 走 NDK 的 `ndk-build`（官方 CI 同一条路径）：

```bash
"$ANDROID_NDK_HOME/ndk-build" NDK_PROJECT_PATH=. \
    APP_BUILD_SCRIPT=core/jni/Android.mk \
    NDK_APPLICATION_MK=core/jni/Application.mk \
    APP_ABI=arm64-v8a NDK_OUT=out/ndk/obj NDK_LIBS_OUT=out/ndk/libs
```

`platform` 取值：`unix`（含 `unix-*`）/ `win` / `win64` / `osx` / `android`。
对象文件落在 `core/build/make/<platform>/`，不会污染源码目录。

一键本机构建 + 三个测试程序：

```bash
tools/build_core.sh   # DLL + core_test + vm_smoke + cart_smoke
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
| 非 Windows（源码直跑） | `node tools/cartconv/cartconv.js game.png`（需先完成上面的 `npm install`；首次运行自动准备编译环境，约 30 秒） |

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

> **Android 安装要点**：先解压 zip——「安装或恢复核心」的文件浏览器按平台
> 过滤扩展名，Android 上只列 `.so`（**zip 不出现**），选中解压出的 `.so` 后
> 它才被拷进核心目录；`.info` 不会被带过去，需要单独放到**核心信息目录**
> （`libretro_info_path`，默认为核心目录本身）。
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

pxt 工具链（png2pxt64.py 和 cartconv 转换卡带都依赖）：node + npm。
首次使用先安装依赖：

```bash
cd tools/vm-build && npm install
```

## 持续集成（GitHub Actions）

`.github/workflows/build.yml` 在每次 push / PR 上把所有平台编一遍，作用是
**拦住"源码编不过"的提交**，并把可运行产物挂成 workflow artifact（保留 30 天，
在 Actions 运行页底部下载）。

| 矩阵 | 工具链 | 产物 |
|---|---|---|
| `linux-x64` / `linux-arm64` / `linux-armhf` | Ubuntu 22.04 + gcc 交叉包 | `.so` |
| `windows-x64` | mingw-w64（**-posix** 线程模型，pxt VM 用 pthread） | `.dll` |
| `android-arm64` / `android-armv7` | 跑者自带 NDK + `core/jni/`（与官方模板同一套参数） | `.so` |
| `zig-*`（x64/arm64 Windows + Linux x64/arm64/armhf） | **与本地发版同一工具链**（zig 0.17.0 + `tools/build_target.sh`） | `.so` / `.dll` |
| `macos-universal` | Xcode clang，arm64+x86_64 | `.dylib`（实验，`continue-on-error`） |
| `package` | 汇总发行版工具链产物 → `tools/package_release.sh` | 6 个 zip |
| `release`（只在推 `core-v*` tag 时） | 汇总 `zig-*` + `android-*` 产物 → `package_release.sh` → `gh release create` | **7 个 zip**（含只有 zig 能出的 `windows-arm64`） |

工具链**刻意分两套**，别合并：

- 发行版工具链（gcc / mingw / NDK）证明"源码在官方 buildbot 的构型下编得过"。
  工具链差异本身就是回归价值——`GetTickCount64` 编不过的问题（mingw-gcc 的
  `_WIN32_WINNT` 默认 XP）正是这套 job 第一次抓出来的，zig 的头文件默认够高，
  全换 zig 就会把这类问题藏住。
- `zig-*` job 与本地发版矩阵同一 zig 版本、同一份脚本，验证发版构型在 CI
  上可复现，并补上发行版工具链覆盖不了的两项：**windows-arm64**、
  **glibc 2.17 基线硬断言**（`readelf -V` 最高符号版本 ≤ 2.17，超出即红）。
  Android 本地就是 NDK clang 编，与 android job 同工具链，无需重复。

每个 job 都带断言，不只是"能编过"：

- Linux：`readelf -d` 的 `NEEDED` 只允许 libc/libm/libdl/libpthread/librt/
  libstdc++/libgcc_s（出现别的说明链接方式跑偏），并打印 GLIBC 符号版本
- Windows：导入表里不允许出现 `libstdc++-6.dll` / `libgcc_s_*.dll` /
  `libwinpthread-1.dll`（这几个 DLL 玩家机器上通常没有），并检查
  `retro_init` / `retro_run` 等导出符号在（`--export-all-symbols` 生效检查）
- Android：不允许依赖 `libc++_shared.so`；`LOAD` 段必须是 `0x4000` 对齐
  （16KB 页设备）

两个 CI 文件的分工，别混：

| 文件 | 跑在哪 | 干什么 |
|---|---|---|
| `.github/workflows/build.yml` | 本仓库（GitHub 托管机器） | 编译回归 + 测试用产物 |
| `.gitlab-ci.yml` | libretro 官方 buildbot（镜像仓库） | 收录进 RetroArch Core Downloader 用 |

**发布由 tag 触发**。推 `core-v*` tag 时，`release` job 等 `zig-*` 与
`android-*` 编完，用与本地同一份 `tools/package_release.sh` 打 7 个包，再创建
GitHub Release 并把 zip 挂上去。

- 凭据用仓库内置 `GITHUB_TOKEN`（job 级 `contents: write`，全局仍是只读），
  **不需要 PAT**。Release 对象与附件上传只有 HTTPS API 提供，git/SSH 到不了
  这一层——SSH key 只能推 commit 和 tag。
- 发版前硬断言：包数必须 7、每个包 `.info` 的 `display_version` 必须等于 tag，
  少一个或版本对不上就红，不会发出半套包。
- 说明文本取 `.github/release-notes/<tag>.md`；没写就退回 `--generate-notes`。
- 重跑安全：Release 已存在时走 `upload --clobber` + `edit`，不报错。

真机验收（R1）仍然只能在设备上做，自动化不了。tag 发的包和本地
`tools/build_matrix.sh` 同工具链、同 2.17 基线，构建环境不同可能带来字节差异。

手动触发：仓库 Actions 页 → build → Run workflow。

## 测试（不需要 RetroArch）

```bash
tools/build_core.sh

# 1) 卡带 → 字节码（离线；需要 node 工具链，见「环境要求」）
python tools/png2pxt64.py png_examples/arcade-ping.png out/ping.pxt64

# 2) 假前端走完整 libretro 路径跑 .pxt64（60fps 节拍，出 BMP）
#    存档与 mca_trace.log 默认落在 out/core_test_data/（--data-dir=DIR 可改），
#    不会在项目根目录生成文件
core/core_test.exe out/ping.pxt64 out/ping.bmp 120 --press=LEFT --every=40

# 3) PNG 卡带 + 旁载 .pxt64（同目录同名文件；仅测试程序可走此路径，
#    RetroArch 侧核心只接受 .pxt64）
core/core_test.exe png_examples/arcade-ping.png out/ping2.bmp 60   # 无旁载时显示测试画面

# 4) 回归
core/build/cart_smoke.exe png_examples/*.png        # 卡带解码

# 5) 按键映射回归（探针卡带 + 颜色断言，16 个用例）
python tools/make_probe_cartridge.py                # 生成 out/pxt64/probe.pxt64
python tools/test_buttons.py

# 6) 生命周期回归（三连换游戏 / 关闭游戏后 dlclose / 重置后音频恢复）
python tools/make_probe_cartridge.py --audio        # 生成 out/pxt64/audio_probe.pxt64
python tools/test_lifecycle.py

# 7) 即时存档回归（确定性 / 收敛 / 跨卡带与垃圾拒绝 / 跨进程 / 跨基址 / rewind）
python tools/test_savestate.py
#    并行压测（多份套件同时跑，输出目录必须分开）：
#    MCA_TEST_OUT=out/par1 python tools/test_savestate.py
```

已验证：ping / jumpy / cube-land 三款游戏在**同一份预编译 core 二进制**上
通过 VM 字节码运行并正确渲染（pxtapp C++ 树与游戏无关，
functions.json 逐字节一致）。

## RetroArch 使用

> **核心只运行 `.pxt64` 字节码，文件浏览器也只列出 `.pxt64`。** `.png` 卡带里
> 只有**源码**（meta + source），不是可直接运行的游戏——**必须先用 cartconv
> 转换成 `.pxt64`**（见上文「卡带转换」）。核心不接受 `.png` 作为加载内容。

1. 取 `core/dist/release/makecode_arcade_<platform>.zip`，**先解压**，把其中的
   core 二进制放进 RetroArch 的「核心」目录、`makecode_arcade_libretro.info`
   放进「核心信息」目录（最直接；两者也可以配置为同一目录）。**目录的实际
   位置因平台/发行版而异**，请在 RetroArch `设置 → 目录 → 核心 / 核心信息`
   里查看你设备上的真实路径。也可以用「安装或恢复核心」选中解压出来的
   二进制，但那个文件浏览器只列**当前平台的核心文件扩展名**（Windows `.dll` /
   Linux `.so` / macOS `.dylib`），**zip 不会出现在列表里**，而且 `.info`
   不会被带过去；
2. 加载内容 → 选转换好的 `.pxt64` 文件；
3. 像素格式 RGB565，160x120@60fps，音频 44.1kHz 单声道。
   重置 / 重启 / 换游戏：
   - 重置键（默认 SELECT，可在 Controls 改绑）或前端「重启游戏」= 重开当前游戏，
     VM 状态完全复位，**画面与声音都会恢复**；
   - 「关闭游戏」「换另一张卡带」会真正停掉 VM 线程（`retro_unload_game` /
     `retro_deinit` 里 join），不再出现"一关游戏 RetroArch 就崩"；
   - 每次重置都会写 trace 到存档目录下的 `mca_trace.log`（写不进去会退到
     当前目录 / `/tmp`，实际路径会打进 RetroArch 日志的
     `[MCA] core version ... trace -> ...` 一行），卡住时看它停在哪一步。

## 快进、慢放、即时存档与 panic 提示

- **快进 / 慢放**：不需要核心做任何事。虚拟时钟是**帧驱动**的（前端每取
  一帧，VM 才推进 1/60 秒虚拟时间），所以 RetroArch 的快进（提高 retro_run
  频率）天然就是游戏加速，慢放（降低取帧频率）天然减速，音频是拉动式所以
  一起变速。旧版"把时钟调成 4 倍速"的做法已废弃（会让 forever 丢 tick）。
- **即时存档（Savestate / Rewind）**：v0.2.10 起支持
  `retro_serialize` / `retro_unserialize`，RetroArch 的存档、回带（rewind）、
  run-ahead 都能用。
  **v0.2.11 起存档可以跨会话**：存一档 → 退出 RetroArch（甚至崩溃）→ 重开
  同一个游戏 → 加载 → 从存档那一刻接着玩，画面与原时间线**逐帧一致**。
  实现要点是让地址可复现：GC 堆、卡带镜像、VM 单例都钉在固定地址
  （`runtime/pxtapp/mca_fixed.h`），fiber / event / 指向 core 自身代码的回调
  在恢复时逐个重建并重定位。快照带卡带字节数 + FNV 指纹，拿别的游戏的档
  恢复会被**拒绝**（日志 `-10`），拒绝后游戏照常继续，不会崩前端。
  游戏内随机序列由卡带镜像哈希决定（同一卡带每次开局同一序列），随机状态
  也随快照保存/恢复。
  确定性回归见 `tools/test_savestate.py`（S1 确定性 / S2 同会话收敛 /
  S3 跨卡带拒绝 / S4 垃圾拒绝 / S5 跨进程 / S6 跨模块基址 /
  S7 rewind 热路径）。
- **panic 显示化**：VM 崩溃（panic）时画面顶部会出现红底白字的
  `VM PANIC <code>` 横幅，重启成功后自动消失；详情仍同步进 RetroArch 日志
  （`[MCA-VM] panic code N`）。真机上不用翻日志就能对上故障码。

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

## 许可

本项目以 [MIT](LICENSE) 许可发布。vendored 组件沿用其原始许可：
`core/libretro.h`、`core/third_party/miniz/`、`runtime/pxtapp/`（Microsoft pxt
运行时）为 MIT，`core/third_party/lzma/`（LzmaDec）为公有领域——来源与出处
详见 `runtime/VENDORING.md`。`tools/cartconv/` 依赖的 pxt 工具链经
`npm install` 获取，同为 Microsoft MIT 许可。
