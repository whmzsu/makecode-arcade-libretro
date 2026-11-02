# MakeCode Arcade —— libretro 核心（Hermes JIT 版）

在 RetroArch 里玩 MakeCode Arcade 的卡带。这是 `makecode-arcade-core`（QuickJS 版）的
**性能升级版**：游戏运行时从 QuickJS 换成 Meta 的 **Hermes** 引擎——
`binary.js` 先预编译成 Hermes 字节码（AOT），再由 Hermes 的高速解释器执行，
支持 JIT 的平台（x64 / arm64）还会在运行时做热点 JIT。

```
RetroArch ── makecode_arcade_libretro.{dll,so} ──┬─ src/core.cpp         libretro 接口 + Hermes 宿主（jsi）+ 字节码缓存
                                                 ├─ src/cart.c           PNG 卡带解析（低位隐写 + LZMA）【复用旧核心】
                                                 ├─ src/compiler.c       内嵌 pxt 编译器（临时 QuickJS，png → binary.js）【复用】
                                                 ├─ src/cache.c          编译缓存（MCC1 容器，binary.js 与 HBC 共用）【复用】
                                                 ├─ pxt_bundle.h         官方三层 sim + DOM/WebAudio mock + glue（构建时生成）
                                                 ├─ pxt_compiler_bundle.h  pxt 编译器载荷（构建时生成，需 pxtc 环境）
                                                 └─ game.js / game.png   由前端作为"内容"载入
```

## 与旧核心的关系

| 层 | 旧核心 makecode-arcade-core | 本核心 makecode_arcade_libretro |
|---|---|---|
| 游戏执行 | QuickJS 解释执行 | **Hermes**（AOT 字节码 + 高速解释器 + 自适应 JIT） |
| binary.js 加载 | 文本 eval | **预编译 HBC 字节码 + 落盘缓存**（二次加载零编译） |
| .png 内嵌编译 | 临时 QuickJS 跑 pxtc | 同一条路（原样复用，产物逐字节一致） |
| 编译缓存 | `<存档目录>/compile-cache/` | 同目录；HBC 缓存（`hbc*.bin`）并存 |
| 精灵 C 快路 / 零拷贝取帧 / 音频 | 有 | 原样保留，行为逐字节对齐 |

核心名称：`makecode_arcade_libretro`（与旧核心 `arcade_libretro` 可共存）。

## 构建

依赖：CMake 3.22+、Ninja、MSVC 2022（Windows）/ clang（Linux/macOS）、Python 3、
Node + pxtc 环境（仅构建期；`node tools/setup_pxtc.js`，建议 `PXTBUILD=E:/pxtbuild`）。
Hermes / QuickJS / miniz / lzma 全部 vendored 在 `third_party/`。

```bash
# Windows
cmake -B build -G Ninja
cmake --build build                     # → build/makecode_arcade_libretro.dll

# 自检（假前端跑帧出 PPM）
./build/core_test tests/data/ping.js out.ppm 90
./build/core_test tests/data/ping.png out.ppm 90    # 走内嵌编译器那条路

# 交叉编译（Android）
cmake -B build-android -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-21
cmake --build build-android
```

可选参数：`-DPXTBUILD=<pxtc 环境目录>`（默认环境变量 `PXTBUILD` 或 `E:/pxtbuild`）、
`-DHERMES_JIT=0|1|2`（默认 1=平台支持就开）。

## 验证结果（2026-11-01，Windows x64，MSVC Release）

| 项目 | 结果 |
|---|---|
| 画面正确性 | ping.js 90 帧 / ping.png（内嵌编译）输出与 QuickJS 旧核心**逐字节一致**（非黑像素 389=389） |
| 精灵 C 快路 | 生效（blit 计数与旧核心同量级，fb=0） |
| 两级缓存 | 编译缓存（binary.js）+ 字节码缓存（HBC）均命中，二次加载零编译，输出一致 |
| 每帧耗时 | 桌面 x64 上两引擎相近（±10%，测量噪声大）；AOT+JIT 的真实收益待 Linux/ARM 真机验证 |

**Windows 上的 JIT 说明**：Hermes 上游刻意在 Windows x64 禁用 JIT（x86-64 后端用了 GCC 专属
语法与 setjmp，从未适配 MSVC，见 `third_party/hermes/include/hermes/VM/JIT/Config.h` 内注记）。
Windows 构建 = **AOT 字节码 + Hermes 高速解释器**；Linux x64 / aarch64 / Android 会按
`HERMESVM_ALLOW_JIT=1` 自动启用真 JIT（这些才是本核心的主战场——RK3566 类弱 ARM 设备）。

```bash
# 两边都钉死虚拟时钟，量出的耗时才可比
ARCADE_FIXED_DT=16 ARCADE_PERF=1 ./core_test tests/data/ping.png out.ppm 300 --pace
# 第二次加载应看到：编译缓存命中 + bytecode cache hit
```

## 已知边界

- 存档状态不支持（与旧核心相同，JS 闭包无法序列化）。
- 依赖 `github:` 扩展的卡带仍需 PC 端 `cart_build.js` 转换（内嵌编译器不联网）。
- Hermes JIT 在个别平台不可用时自动退回纯解释（AOT 字节码收益仍在）。

## 许可

本仓库代码 MIT。第三方：Hermes (MIT)、QuickJS (MIT)、miniz (MIT)、LZMA SDK (public domain)、
libretro.h (MIT)、pxt-* (MIT)。详见 `third_party/hermes/LICENSE`。
