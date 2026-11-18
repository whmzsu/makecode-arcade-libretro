# 交叉编译

结论：**能，除了 Android 需要 NDK 自带的 clang，其余目标都不需要任何交叉工具链**。

整个 core（pxt VM 内核 33 个 C++ 编译单元 + 引擎原语 + 卡带解码 + libretro 接口层）
是纯 C/C++，无汇编、无平台专有 API、无外部依赖（miniz/LzmaDec 都 vendor 进来了）。
zig 自带 libc 头与 glibc/musl stub，换 `-target` 就能出对应架构的产物。

## 发版矩阵（主流目标，全部 64 位）

```bash
tools/build_matrix.sh     # 默认只跑下面这 5 个
tools/package_release.sh  # 打包成可直接放进 RetroArch 的 zip
```

> **发版矩阵含 32 位**（2026-11-17 起，9 主流目标）。32 位的硬阻断原本是
> GC 的 vtable 高位标记位——A 方案（GC 标记搬旁路表）进 main 后，**PXT64
> 布局在 32 位平台上已可用**（3DS/Vita 线同一路径）。发版矩阵 2026-11-17
> 起收入 4 个 32 位目标（Win x86 / Linux x86/armhf / Android armv7），
> 与 64 位走同一条 PXT64 镜像路径。3DS 核心形态已在 new3ds 真机验收通过；
> Vita 核心形态此前 `C2-12828-1` 崩溃已定位并修复、待复验（见文末）；
> 老款 3DS 暂无设备未验。根因与决策过程见 `docs/PXT32-A-SCHEME.md` 与
> README「32 位与 64 位」，真机结论见文末「3DS / Vita 真机验收」小节。

| 目标三元组 | 平台 | 格式 | libc 基线 | 产物 | 备注 |
|---|---|---|---|---|---|
| `x86_64-windows-gnu` | Windows x64 | PE | UCRT | 670 KB | 无第三方运行时 |
| `aarch64-windows-gnu` | Windows ARM64 | PE | UCRT | 605 KB | 无第三方运行时 |
| `x86_64-linux-gnu.2.17` | Linux x64 | ELF | glibc **2.16** | 4.6 MB | |
| `aarch64-linux-gnu.2.17` | Linux ARM64（RK3566/RK3326） | ELF | glibc **2.17** | 4.9 MB | 掌机主力 |
| `aarch64-linux-android21` | Android ARM64 | ELF | bionic API 21 | 933 KB | 16KB 页对齐 |

5 个目标的 14 个 `retro_*` 符号全部导出，动态依赖只有 libc/libm/libdl(+ld.so)。
Linux 产物明显更大是因为带着完整符号表与 libc++ 静态部分；Windows 侧 PE 只有导出表。

> Windows 产物依赖 UCRT（`api-ms-win-crt-*`，Win10 起系统自带）——与 RetroArch
> 自身的依赖一致，不需要额外的 VC 运行库安装。ARM64 版对应 Windows on ARM
> 的 RetroArch；x64 版在 ARM64 Windows 上走 x64 仿真也能用。

## 其它用法

```bash
# 单个目标（增量构建，对象缓存在 core/build/<triple>/）
tools/build_target.sh aarch64-linux-gnu.2.17
tools/build_target.sh aarch64-linux-android21
tools/build_target.sh native

# 全架构（验证"可移植性"用，含 musl/riscv/loongarch/ppc/s390x/大端/macOS/x32…）
tools/build_matrix.sh --all

# 跑完 --all 后回到发版状态：顺手删掉不在本次目标列表里的 dist/build 目录
tools/build_matrix.sh --prune         # 主流 7 个 + 清理其余
# 清理只动产物目录：dist/release/（打好的 zip）与 build/vm_objs（本机缓存）不受影响。

# 打包
tools/package_release.sh              # 只打主流 7 个
tools/package_release.sh --all        # dist 下所有已编目标（包名带完整三元组，不覆盖）
tools/package_release.sh aarch64-linux-gnu.2.17   # 指定目标
```

产物布局：

```
core/dist/<triple>/makecode_arcade_libretro.{so|dll|dylib}
core/dist/<triple>/build.log
core/dist/MATRIX.md                       # 汇总表 + 二进制自检（自动生成）
core/dist/release/makecode_arcade_<platform>.zip
```

发行包内容：`makecode_arcade_libretro.{so,dll}` + `makecode_arcade_libretro.info`
+ `README.txt`（安装说明）。`.info` 与二进制同名，RetroArch 的「加载核心」列表里
才会显示成 "MakeCode Arcade" 而不是裸文件名。

## 二进制自检

```bash
python tools/inspect_binary.py            # 扫 core/dist/*/
python tools/inspect_binary.py <某个产物>
```

纯标准库解析 ELF / PE / Mach-O，不需要 objdump/readelf，输出：

- 格式、位宽、机器架构；
- **libc 基线**：glibc 版本号（掌机固件兼容性看这个）/ `bionic API nn`（读
  `.note.android.ident`）/ musl；
- `retro_*` 必需符号是否齐全；
- `DT_NEEDED` 动态依赖列表（插件型 core 越少越好）。

```
target                         fmt    bits    arch         libc               size  symbols
x86_64-linux-gnu.2.17          ELF    64-bit  x86_64       2.16            4688864  OK
aarch64-linux-android21        ELF    64-bit  AArch64      bionic API21     955592  OK

动态依赖（DT_NEEDED；插件型 core 越少越好）
  aarch64-linux-gnu.2.17         libm.so.6 libc.so.6 libpthread.so.0 libdl.so.2
  aarch64-linux-android21        libm.so libdl.so libc.so
```

## Windows：zig 与 gcc（官方 buildbot / GitHub Actions）

zig 编 `x86_64-windows-gnu` 时默认把 `_WIN32_WINNT` 定在 `0x0a00`（Win10）；
**mingw-gcc 不定**，走 `_mingw.h` 里的 XP 默认（`0x0502`）。这个差别在
`runtime/pxtapp/core---vm/platform.cpp` 上会直接炸：

```
platform.cpp:21:18: error: 'GetTickCount64' was not declared in this scope
```

`GetTickCount64` 是 Vista+ 的 API。本地一直用 zig 编，这个坑看不到；官方
buildbot 的 `windows-x64-mingw` 模板和 GitHub Actions 的 windows job 都是
mingw-gcc，一上就红。

处理方式：`core/src/win32_baseline.h` + 编译时 `-include`（`core/Makefile` 与
`tools/build_target.sh` 的 Windows 分支都加了）。头里用 `#ifndef` 兜底：

| 编译器 | `_WIN32_WINNT` | 结果 |
|---|---|---|
| mingw-gcc | 未定义 → 垫片给 `0x0601`（Win7） | 编得过 |
| zig | 已给 `0x0a00` → 垫片不生效 | 保持 Win10 基线，零告警 |

不要改成命令行 `-D_WIN32_WINNT=0x0601`：在 zig 下每个编译单元会报
`-Wmacro-redefined`，而且把 zig 的基线从 Win10 降下来。

## Android：为什么走 NDK clang 而不是 zig

zig 能解析 `-target aarch64-linux-android`，也接受 `--sysroot` 指向 NDK，但会撞上
**zig 自带 libc++ 与 NDK bionic 头版本不匹配**：

```
zig/lib/libcxx/include/math.h:393:31: error: use of undeclared identifier 'FP_NAN'
```

zig 的 libc++ 用 `#include_next <math.h>` 找 C 数学头，而 bionic 的数学宏布局与其
假设不符。硬修需要同时替换 zig 的 libc++，得不偿失。

所以 Android 目标改用 **NDK 自带的 clang**（官方唯一支持的路径，18.0.3 / r27c）：

| 项 | 取值 |
|---|---|
| 编译器 | `$NDK/toolchains/llvm/prebuilt/<host>/bin/clang++.exe` |
| 目标 | `--target=aarch64-linux-android21`（64 位）；armeabi-v7a 走 `armv7a-linux-androideabi21`（32 位，2026-11-17 起同入发版矩阵） |
| sysroot | clang 自动按自身路径定位，无需显式传 |
| NDK 位置 | `$ANDROID_NDK_HOME`，缺省回落到 `../common tools/android-ndk-r27c` |

两个必要的链接参数：

1. **`-static-libstdc++`**：默认会依赖 `libc++_shared.so`。RetroArch 加载的是第三方
   `.so`，不同 App 的 STL 版本不一致会崩；静态链进去后只依赖 `libc/libm/libdl`。
2. **`-Wl,-z,max-page-size=16384`**：Android 15 起有 16KB 页设备，4KB 对齐的 `.so`
   直接 `dlopen` 失败。加了这个参数后所有 LOAD 段对齐为 `0x4000`（16KB 页设备按
   4KB 向下兼容，反之不行）。

API 级别即三元组末尾数字；NDK r27 的下限是 21（也是 RetroArch Android 的最低要求）。

将来若要支持 x86_64 Android 模拟器，把 `x86_64-linux-android21` 加进
`build_matrix.sh --all` 即可（脚本已支持，NDK 也有对应 sysroot）。x86（i686）
Android 暂不构建（收录需求出现时再加）。

## 全架构验证（`--all`）

`tools/build_matrix.sh --all`：21 个目标全部通过（64 位 17 + 32 位 4；
2026-11-17 起主流矩阵含 32 位）。

> 下表是 v0.3.0 期间的历史实测记录，当时还包含 32 位目标（arm32/armeb/x86/
> x32/riscv32）。这些杂项条目后来从 `--all` 移除；2026-11-17 起主流矩阵重新
> 收入 4 个 32 位目标（x86/armhf/armv7 等），但 armeb/x32/riscv32 这类
> 冷门布局仍不构建。保留这张表只是为了记下当时各架构的实际产物
> 尺寸与基线。

| 目标 | 架构 | 格式 | libc 基线 | 产物大小 |
|---|---|---|---|---|
| `aarch64-linux-gnu(.2.17)` | AArch64 | ELF | 2.29 / **2.17** | 4.9 MB |
| `aarch64_be-linux-gnu` | AArch64 大端 | ELF | 2.29 | 5.0 MB |
| `x86_64-linux-gnu(.2.17)` | x86_64 | ELF | 2.29 / **2.16** | 4.7 MB |
| `aarch64/x86_64-linux-musl` | 双架构 musl | ELF | 无 glibc 依赖 | 4.4–5.0 MB |
| `riscv64` | RISC-V 64 | ELF | 2.27 | 6.5 MB |
| `loongarch64` | 龙芯 | ELF | 2.36 | 6.1 MB |
| `mips64` / `mips64el` | MIPS64 | ELF | 2.29 | 5.2 / 5.1 MB |
| `powerpc64le` | PowerPC64 LE | ELF | 2.29 | 5.2 MB |
| `s390x` | S/390 | ELF | 2.29 | 5.2 MB |
| `x86_64-macos` / `aarch64-macos` | Intel / Apple Silicon | Mach-O | 系统自带 | 699 / 753 KB |

历史（已移除的 32 位条目）：`arm-linux-gnueabihf(.2.17)` 4.3 MB、
`arm-linux-gnueabi` 4.4 MB、`armeb-linux-gnueabi` 4.4 MB、
`arm-linux-musleabihf` 4.4 MB、`x86-linux-gnu` 3.9 MB、
`x86_64-linux-gnux32` 4.0 MB、`riscv32-linux-gnu` 5.5 MB。

## glibc 基线：掌机兼容性的关键

zig 默认 glibc 基线与新发行版对齐（32 位 ARM 甚至要求 **2.34**，Ubuntu 22.04 级），
老掌机固件（glibc 2.27~2.33）直接装不上。三元组里钉版本即可：

| 三元组 | 默认 | 钉版本后 |
|---|---|---|
| `aarch64-linux-gnu` | 2.29 | `aarch64-linux-gnu.2.17` → **2.17** |
| `x86_64-linux-gnu` | 2.29 | `x86_64-linux-gnu.2.17` → **2.16** |

2.17 发布于 2012 年，覆盖几乎所有在跑的 ARM 固件（ArkOS / JelOS / Batocera /
EmuELEC 都远高于它）。**发版矩阵已固定使用 `.2.17` 变体。**

musl 目标（`*-linux-musl`）不依赖 glibc 版本，适合 Alpine 类固件或静态部署。

## 实现细节（build_target.sh）

- 每个 triple 独立对象目录 `core/build/<triple>/`，增量构建、可并行；
- 非 Windows 目标加 `-fPIC` 并链接 `-shared -Wl,-soname,...`；
- Windows 目标链接 `-shared -Wl,--export-all-symbols`（PE 不导符号前端会认不出 core）；
- macOS 用 `-dynamiclib`，产物 `.dylib`；
- Android 见上文三参数（NDK clang + `-static-libstdc++` + 16KB 页对齐）；
- 编译选项与本地构建完全一致（`-fno-rtti -fno-exceptions` 等，pxt VM 的要求）。

## 已知不可行 / 需额外条件

| 目标 | 原因 | 出路 |
|---|---|---|
| `mips-linux-gnu` / `mipsel-linux-gnu`（32 位） | zig 0.17 未提供 32 位 MIPS 的 glibc/musl 头 | 用 `mips64-*`，或旧版 zig / 厂商工具链 |
| `m68k-linux-gnu` | LLVM 无 m68k 后端 | 只能上 GCC 交叉工具链 |
| `sparc64-linux-gnu` | **zig/LLVM 的 SPARC 后端自身崩溃**：`vm_host.cpp` 任意优化级别编出 segfault；即使跳过也报 `bad addralign on INIT_ARRAY shdr` | 换 GCC 交叉工具链或等 zig 修复；SPARC 上基本没有 RetroArch 用户 |

> 注：32 位 MIPS 掌机（部分老 RG 系列）如果确实要支持，最省事的路线是
> 找厂商 SDK 的 GCC 交叉工具链编 `core/src` + `runtime/pxtapp`（源码是纯 C++，
> 换工具链不改代码），而不是依赖 zig。

### 一个容易踩的坑：`-shared` 允许未定义符号

ELF 共享库默认允许未定义符号（惰性绑定），所以**缺对象文件时链接仍会"成功"**，
产出看似正常的残缺 `.so`（sparc64 曾留下一个 62MB 的假货）。本项目里
`powerpc64le` 也撞到过类似情况：libc++ 的 `to_wstring` 引用了 glibc 2.32+ 才有的
`__vsnprintfieee128`——我们的代码从不调用宽字符串转换，动态链接下无害，但若开启
`-Wl,--no-undefined` 就会误报失败。

因此脚本的策略是：**不加 `--no-undefined`**（保持动态链接语义），而由
`build_matrix.sh` 在构建失败时删掉残缺产物 + 自检只统计成功目标，避免"假成功"。

## 与平台无关性有关的代码约束（给后续维护者）

要保持"全架构可编"这个属性，改代码时注意：

1. 不要引入平台专有 API；需要平台能力时走 `platform.h` 的抽象或 `#ifdef` 收敛在一处；
2. 不要用 `long` 假设位宽（32 位 ARM/MIPS 上 `long` 是 32 位），指针算术用 `uintptr_t`；
3. 浮点用 `float/double`，不假设 x87；
4. 字节序：`.pxt64` 镜像与卡带解码都按小端读写，大端目标（`aarch64_be`、`armeb`、
   `mips64`）能编过但目前**未做镜像字节序转换**——真要在大端设备上跑，需要在
   `vm_host.cpp` 里加一次 section 头/字面量的字节序翻转；
5. 时间/线程：只用 pthread 与 `std::chrono`，别用 Win32 专有 API；
6. Android 走 NDK clang，**不要**在代码里加 `__ANDROID__` 特判除非确实必要——目前
   一个都不需要。

## 3DS / Vita 真机验收与核心形态构建（2026-11-18）

这两个平台**只出核心形态**：core 静态库（32 位）链进 RetroArch 完整前端，
出厂为 `mca_libretro.cia`（3DS）或 `mca_libretro.self` + `.info`（Vita）。
极简前端与核心 VPK 线已放下、不再构建分发。均在 tool69 本地编译
（devkitARM / vitasdk，RA 树 pin `d7cc9bc`）。结论：

- **3DS 核心形态**：new3ds 真机验收通过——按键、菜单、卡带装载全绿，
  画面性能明显优于早先的 CPU 缩放前端。老款 3DS 手头无设备，暂未验。
- **3DS 安装**：建议先装官方 3DS RA，把核心 `.cia` 复制到
  `sdmc:/retroarch/cores/`、`.info` 放到 `sdmc:/retroarch/cores/info/`，重启 RA
  即在 Load Core 列表出现 MakeCode Arcade；也可直接用 FBI 装 CIA 得到独立图标入口。
- **Vita 核心形态**：先装官方 Vita RA，`mca_libretro.self` + `.info` 并入其
  app 目录 / `info/`。此前真机在
  `Validation OK → start main loop` 后报 `C2-12828-1`；两份 psp2core dump
  取证定位为 `mcaTrace()` 缓存的 `FILE*` 在 RA 换 content 重置 stdio 后悬垂，
  `_svfprintf_r` 写野指针。2026-11-18 修复（Vita 分支直写 stderr、不 fopen
  不缓存，见 `runtime/pxtapp/core---vm/scheduler.cpp`）并重编 self
  （md5 `3c751257…`），**待真机复验**。
- **GPU 硬缩放实验**：曾试 citro3d 顶屏 GPU 缩放，四轮修复（2 幂纹理、
  tiled 上传、VBlank、permutation）后真机仍黑屏，**已放弃**，源码回退
  （实验源码只在本地工作目录封存，不随仓库分发）。

**重编 Vita self 的坑**：改 `mcaTrace()` 后须重编 core `.a` → 换进 RA 树
`libretro_vita.a` → `make -f Makefile.vita clean` → 重链 self。注意
`va_list ap` 在 `#if`+`return` 下同作用域会重声明编译失败，必须用 `#else`
包住原路径。测试包、md5、安装步骤见 `docs/3DS-VITA-TEST.md`
（测试包本身是本地产物，不随仓库分发）。
