# PXT32 A 方案实测报告（分支 `probe/pxt32-gc-a`）

> **2026-11-17 状态更新**：本文写作时的"不合并 main、不进 CI"决策已随
> 3DS/Vita 线改变——A 方案随 `probe/pxt32-3ds-vita` **合并进 main**
> （`31d9969`），同日拍板发版矩阵直接收入 32 位（10 主包），CI 设构建回归
> 兜底；3DS/Vita 真机验收仍在进行。`probe/pxt32*` 分支已删除，
> 本文按当时分支状态留档。
>
> **2026-11-18 补注**：第 11 节记录的极简前端（`frontends/ctr`、
> `frontends/vita`）与核心 VPK 已不再构建/分发，源码目录已删除；核心 CIA
> 打包素材迁至 `core/assets/ctr/`，`.info` 统一由 `tools/core.info.in`
> 模板生成（3DS/Vita 包内文件名为 `makecode_arcade_libretro.info`，随核心二进制同名）。
> 第 11 节表格与命令保留作历史记录。
>
> 2026-11-16。承接 `probe/pxt32`（P0/P1）——P1 实测证实 32 位硬阻断是 GC 的
> vtable 高位标记位。本分支尝试 **A 方案：把 FREE/ARRAY/PERMA 标记搬到旁路表**，
> 验证"自己能不能实现 32 位核心"。结论：**技术上跑通了**，但代价是长期 fork，
> 范围决策仍维持"不做 32 位"。本分支不合并 main、不进 CI/发版矩阵。

## 1. 根因与 A 方案设计

上游 `gc.cpp` 在 `PXT32`（`HIGH_SHIFT=28`）下把三个块类型标记编码进对象 vtable
word 的高位：`FREE_MASK=bit31`、`ARRAY_MASK=bit30`、`PERMA_MASK=bit29`，并要求
真 vtable 地址 `< 0x10000000`（Cortex-M flash/RAM 布局）。32 位 Linux/Android 的
`.so` 是 ET_DYN，vtable 落在 `0xf7xxxxxx` —— bit31=1 使每个活对象都被 sweep 判成
free block，`VAR_BLOCK_WORDS` 解出巨大 size 后 `memset` 越界（P1 实测 SIGSEGV）。

**关键简化**（让改动可控）：冲突的只有 bit31/30/29，而
- **MARK 位（bit0）安全** —— vtable 4 字节对齐，`getVTable()` 的 `&~1` 与
  `VAR_BLOCK_WORDS` 的位移都对 bit0 免疫，无需改动；
- **size 的 `(size<<2)` 编码保留** —— 去掉高位 MASK 后 `VAR_BLOCK_WORDS` 仍正确。

因此只需把 FREE/ARRAY/PERMA 三个标记搬到**按 GC word 地址索引的旁路字节表**
（`mcaGcTags`，索引 `(addr-GC_BASE)>>2`，按 2 的幂扩容）。对象内存布局与
`.pxt64` 镜像格式**零改动**。所有标记读写抽象成 `GC_*` 宏：`PXT32` 走旁路表，
`PXT64` 展开成与原来逐字相同的位运算 —— 64 位产物机器码与行为完全不变。

## 2. 额外发现并修复：P1 遗留的重启地址漂移

首轮全量测试发现两张卡带 32 位下重启后卡死在 PANIC 842/841/914 循环。追查
`mca_trace.log`：`gcAllocBlock()` 的 32 位分支用 `mmap(currPtr,...)` **不带
MAP_FIXED**，重启后 `gcAllocPtr` 回绕到 GC_BASE 但上一局页还占着，内核另找
高地址返回（实测第二局块漂到 `0xf7f2xxxx`），导致：
1. 旁路表按 `(addr-GC_BASE)>>2` 索引，高地址算出近 1G 的 idx；
2. `PXT_IS_READONLY` 用 `mca_gc_top` 判区间，漂移块全被当"只读" → GC 不扫描 →
   重启后游戏状态错乱。

修复：`mmap` 加 `MAP_FIXED`。GC_BASE=0x20000000 是 VM 堆专用空洞，MAP_FIXED
丢弃上一局旧映射后重映射，地址严格稳定。修复后 32 位主动 Reset（probe + SELECT）
与 PANIC 后自动重启均干净，无二次 panic。这是 P1 时代就存在、但因 P1 主循环
崩溃而无机会暴露的独立 bug。

## 3. 实测结果（工具机 tool69，x86 ELF32 + 32 位 C 宿主）

### 3.1 64 位零回归（铁律）
- 64 位 core clean 全量重编 + golden 三例（`tools/ci_regress.sh`，Windows DLL）：
  `0556e78f` / `e82844f7` / `0eb7884c` 与 0.3.9 基线**逐位一致**。

### 3.2 golden 三例：32 位 = 64 位，逐位一致
| 用例 | 64 位 | 32 位（A 方案） |
|---|---|---|
| ping 300 帧 | `0556e78f7ce0e828f2dc922d210dc047` | 同 ✓ |
| probe 120 帧 | `e82844f7160449d1767aa69afe788800` | 同 ✓ |
| probe 120 帧连跑 | 稳定 | 3 次均 `e82844f7...` ✓ |

P1 在此处是 SIGSEGV（主循环第一帧崩）；A 方案 + MAP_FIXED 后完全跑通。

### 3.3 63 张真实卡带全量（600 帧 + A 键，32/64 同 C host 口径对比）
- **61 张**：32 位与 64 位末帧 hash **逐位一致**（含 Mouse_Adventure 382KB、
  Stack_the_Goats、FPS_game_test_menu_test、Strawberry_Slam 等大卡带）。
- **sm64thign**：32/64 均 `load_game FAIL`（非 pxt64 镜像，已知坏卡）——行为一致。
- **Horizontally_Spinning_Rat**：64 位稳定（`efe46461...`），32 位在墙钟 16ms
  节拍的 C host 下偶发丢帧（0.5~1 distinct 帧差异），60 帧稳定、120 帧 3 次
  2 同 1 异、240/480 帧 3 次全同。属该极慢卡带（600 帧需 >200s）在 C host
  调度节拍下的测试口径抖动，非 GC 旁路表损坏（golden 与其余 61 张全稳）。
  受限于 C host 无 `--turbo`，未能完全归因，记为遗留观察项。

### 3.4 重启回归
probe + SELECT 主动 Reset：32 位 2 次 RESET、零 PANIC、末帧 `e82844f7...` = 64 位。
两张原本 PANIC 980 后重启卡死的卡带，修复后 32/64 逐位一致。

## 4. 结论
- **A 方案技术上成立**：32 位 Linux 宿主能跑通任意卡带，golden 三例与 61/63
  张真实卡带的末帧与 64 位逐位一致；顺带修掉 P1 的重启地址漂移。
- **代价仍是长期 fork**：`gc.cpp` 的标记读写被重写成旁路表，上游 runtime 每次
  更新都要重新 rebase 这套改动；32 位 number boxing（`NumberBoxes` 段）语义与
  64 位本就不同，不能指望所有卡带逐位一致；Linux x86/armhf、ARM Android、
  Win32 32 位已实测（见第 5–7 节），其余 ABI 仍未逐一验证。
- **范围决策不变**：15 台设备全 64 位、收益近零，不恢复 32 位发版。本分支作为
  "能不能自己做"的证据保留，不合并 main。

## 5. 追加：Android ARM32 真机验证（2026-11-16）

用 NDK r27c 的 `--target=armv7a-linux-androideabi21` 重编 A 方案 core 与
`tools/mca_host_android.c` 的 32 位无头宿主，产物均经 `llvm-readelf` 确认是
`ELF32 / ARM`：

- core：`core/dist/armv7a-linux-androideabi21/makecode_arcade_libretro.so`
  711536 B，md5 `065bb41e1d6d487ec166c291816c1eb8`
- host：`out/android/mca_host32` 20044 B，md5 `25e24a3699217a397c6e3f0dfe792d26`

两台在线设备都运行 `armeabi-v7a` 核心（系统主 ABI 仍是 `arm64-v8a`）：

| 设备 | serial | SDK / Android | 结果 |
|---|---|---|---|
| MI MAX 2 | `b2aec2db` | 25 / 7.1.1 | 21/21 + golden 三例全部一致，`rc=0` |
| MI MAX（hydrogen） | `13c2be8e` | 24 / 7.0 | 21/21 + golden 三例全部一致，`rc=0` |

- **21 卡 × 600 帧**：两台都与 `tools/fixtures/android21-baseline.txt` 逐位一致，
  `rc=0`；结果分别存于 `out/android_test/android32-b2aec2db.txt` /
  `android32-13c2be8e.txt`。
- **golden 三例**：`0556e78f…`（ping 300）、`e82844f7…`（probe 120）、
  `0eb7884c…`（probe 120 + A 从第 4 帧按住）三台口径全部一致；第三例要用
  `--tap=A@4 --tap-dur=100000` 对齐 Python 宿主的 `--press=A --release=100000`
  默认第 4 帧按下。
- **重启路径**：probe 注入 `SELECT@60` 跑 240 帧，两台均 `rc=0`、无 `PANIC`；
  未观察到核心 `TARGET RESET` 日志，因此这里只作为“无崩溃”的补充观察，
  不替代 x86 上已完成的主动 Reset 逐位验证。
- 32 位下卡带 buffer 落在普通高地址（实测 `0xeb24cd00`）也能跑通，印证 A 方案
  不再依赖 vtable/指针低 28 位假设。该 host 只是测试工具，不在发版路径。

结论：A 方案的 32 位支持在 **ARM32 Android + 真实 bionic** 上同样成立；但范围
决策仍不变，分支不合并、不进 CI/发版矩阵。

## 6. 追加：Win32 实测（2026-11-16）

本机只有 64 位 Python，无法用 `tools/libretro_host.py` 加载 32 位 DLL；因此新增
`tools/win32_shim/`（`dlfcn.h` / `dlfcn_shim.c` / `sys/mman.h`），用
`LoadLibraryA` / `GetProcAddress` 顶替 `dlopen` / `dlsym`，直接把现有的
`tools/mca_host_android.c` 编成 32 位 PE 宿主。该兼容层只用于测试，不在发版路径。

- core：`core/dist/x86-windows-gnu/makecode_arcade_libretro.dll`
  PE32 / I386，733696 B，md5 `e2a2c58a5972a890e76f897aff00454a`
- host：`out/mca_host32.exe` PE32 / I386，284160 B，md5
  `23c10e50f8d9a7f98e2a950fb8560502`

结果：

- **golden 三例全部逐位一致**：`0556e78f…` / `e82844f7…` / `0eb7884c…`
  （第三例仍用 `--tap=A@4 --tap-dur=100000` 对齐 Python 宿主口径）。
- **21 卡 × 600 帧**全部与 `tools/fixtures/android21-baseline.txt` 逐位一致，
  `rc=0`；结果在 `out/win32_test/carts21.txt`，golden 在
  `out/win32_test/golden.txt`。
- 实测日志：镜像在 `0x00e5xxxx`、`number_vt=0x63667f98`、`GC block @ 0x050e1020`，
  高地址指针不再被误判，说明 A 方案在 Win32 同样摆脱了 vtable 低 28 位假设。
- 观察项：`platform.h` 对 `__MINGW32__` 走 PXT64 式 `GC_BASE 0x2000000000`，
  在 32 位 `uintptr_t` 下截成 0；旁路表从 0 开始索引，按当前堆地址会增长到
  `2^25` 字节量级。功能正确，但有额外内存开销，若未来真要做 Win32 发版，
  这里值得单独优化。

Win32 32 位 core 至此已能跑通标准回归；范围决策仍不变，不合并、不进 CI/发版矩阵。

## 7. 追加：32 位矩阵补测（2026-11-16 晚）

按“先 Linux armhf，再 Win32/Android 全量与长稳，再 RESET/PANIC 夹具”的顺序补测。

### 7.1 Linux ARM32（armhf）

Zig `arm-linux-gnueabihf.2.17` 全量重编：

- core：`core/dist/arm-linux-gnueabihf.2.17/makecode_arcade_libretro.so`
  4372432 B，md5 `cf048347248982521180746bd896eee0`
- host：`out/mca_host_armhf` 1134644 B，md5 `2bddfe5b2a38501d904e39d9a073b7b5`
- tool69 上用 `qemu-arm-static -L $MCA_SYSROOT_ARMHF` 运行（env.sh 变量，
  指向 `/opt/mca/toolchain/sysroot-armhf`）；sysroot 从 Debian bookworm armhf
  的 libc6/libgcc-s1/libstdc++6 解包，不进仓库。
- 2026-11-28 起这六例（golden 三例 + `--reject-rgb565` 三例）已固化为
  `tools/tool69/armhf_smoke.sh`（部署在 tool69 `$MCA_BIN/`），`--build-core`
  可先重编 ELF32 core；CI 的 64 位 Python 宿主载不了 32 位 core，只能在
  tool69 上跑。

结果：

- golden 三例与 21 卡 × 600 帧全部逐位一致、rc=0
  （`out/armhf_test/carts21.txt`）。
- 全量 64 用例（63 卡 + ping）对比 64 位基线：61 一致；
  `sm64thign` 两边都是 `FAIL`（已知坏卡）；
  `Horizontally_Spinning_Rat` 是既有墙钟节拍抖动卡；
  `unique_cube` 在 qemu-armhf 下三次三个不同 hash，而 x86 32/64 各三次稳定
  `8c40c32e…`。这更像 qemu/极慢卡的计时口径问题，未归因到 GC 旁路表，记为观察项。

### 7.2 Win32 / Android 全量与长稳

- Win32：全量 64 用例中 63 个逐位一致，`sm64thign` 两边 FAIL
  （`out/win32_test/carts63.txt`）。
- Android 两台：各 62 个逐位一致；`sm64thign` 两边 FAIL；
  `Horizontally_Spinning_Rat` 又一次出现墙钟节拍抖动
  （`out/android_test/android32-*-63.txt`）。`unique_cube` 在两台 Android 上都一致。
- 长稳：四个环境（armhf qemu、Win32、Mi Max 2、hydrogen）跑
  `arcade-jumpy-platformer` 18000 帧，末帧均为 `1c0f2dd46f99e3754ecdd0cb966ee3f6`；
  `Space_Destroyer` 6000 帧 ×2，末帧均为 `a0a25e59d3cbaeef8dad890e2d95d13a`。

### 7.3 PATCH 夹具与 RESET/PANIC

64 位基线（`out/armhf_test/fixture-baseline64.txt`）：

```text
crash-scale.pxt64       9ebf768b67950bb5673487d0d43e5bb8 rc=0
crash-idiv.pxt64        032c69da01ab186c77fc96cae52c86ed rc=0
oversize-section.pxt64  FAIL rc=5
reset-stress            e82844f7160449d1767aa69afe788800 rc=0
```

- armhf qemu、Win32、两台 Android：三条 PATCH 夹具全部与基线一致；
  `oversize-section.pxt64` 按预期 `load_game -> FAIL`，不能当测试失败。
- 单次 SELECT Reset 在所有 32 位环境均无 PANIC。
- 21 次连续 SELECT 的 hash 跨宿主不稳定：armhf/部分宿主是 `e82844f7…`，
  Win32 与 Mi Max 2 出现 `02b0fb5e…`；Win64 自己也随 tap 数量出现同样两种值。
  这是测试宿主的 RESET 节拍口径问题，不是 32 位专属崩溃；只记为观察项，
  不据此宣称“高频 RESET 逐位稳定”。

### 7.4 32 位 Android RetroArch

两台设备都列出 `armeabi-v7a,armeabi`，libretro buildbot 也提供
`RetroArch_ra32.apk`（本次下载 md5 `70fab3634f0249914523e29b31e5ff7e`），
所以硬件/ROM 层面可以用 32 位 RetroArch。当前两机只装了
`com.retroarch.aarch64`。本次尝试 `adb install` 与 `pm install` 都被 MIUI 的
`INSTALL_FAILED_USER_RESTRICTED` 拦截，需要在设备上开启“允许通过 USB 安装”
并确认弹窗后才能实测前端加载；这一步保持未完成时，不声称 RA 前端已验证。

### 7.5 仍未覆盖

32 位 RetroArch 前端加载、CI/发版矩阵，仍未做或不在这条探针分支范围内。

## 8. 追加：8 键映射 / 坏镜像 / 像素降级 / 路径边界（2026-11-16 晚）

### 8.1 标准 8 键逐键映射

宿主：`--tap=<KEY>@4 --tap-dur=100000`，probe 卡带 120 帧，颜色即映射结果。
x86-32、armhf qemu、Win32 三个 32 位环境全部与 64 位基线逐位一致
（`out/input32/`）：

| 键 | 末帧 hash | 键 | 末帧 hash |
|---|---|---|---|
| A | `0eb7884c…` | RIGHT | `5ea71e47…` |
| B | `aab0c6e8…` | START | `3658205f…` |
| UP | `4b7aded7…` | SELECT | `e82844f7…` |
| DOWN | `e37d92b3…` | LEFT | `6208cdcc…` |

SELECT 在 probe 上触发 Reset，所以末帧等同无输入——这是预期，不按失败处理。

### 8.2 坏镜像边界

构造 9 个坏镜像（`out/badimg/`）：空文件、16 字节、512 字节垃圾、PNG 改名、
首段 size 非 8 对齐、首段 size 超大、probe 截断一半、截掉尾部 8 字节、尾部多 1 字节。
x86-32、armhf qemu、Win32 三个 32 位环境与 64 位基线行为一致
（`out/input32/badimg-*.txt`）：

- 空文件：宿主读不到内容，`rc=4`；其余结构坏镜像：`load_game -> FAIL, rc=5`。
- 截断到 section 边界（`truncated` / `trunc-tail` / `unaligned`）核心按 section 链
  吃掉有效部分正常出帧，hash 与 64 位一致——核心本来就是“按链截断”语义，
  不把这三条判成失败。
- 全程无段错误、无越界，坏镜像不会让 32 位 core 崩。

### 8.3 RGB565 → 0RGB1555 降级

给 C 宿主补了 `--reject-rgb565`（`tools/mca_host_android.c`，仅测试宿主），
模拟前端拒绝 RGB565。核心打 `frontend rejected RGB565; output 0RGB1555` 后
正常出帧。四环境 golden 三例逐位一致（`out/pixfmt/reject565-*.txt`）：

```text
ping 300      aec6c1c80bc75cdcab7a26b527b88136
probe 120     af451361f744bd6e9763e921352d8fee
probe A@4     181cc2bb2313931d367e3bf0553246b7
```

### 8.4 路径 / 权限边界

正常目录、多级不存在目录、路径带空格、中文路径（Win32 与 Linux 都测）：
四个 32 位环境末帧都是 `e82844f7…`，无异常。
把 data-dir 指向一个普通文件时，核心进入 VM 后 `FAILURE: can't write file`：
x86-32 Linux / armhf `rc=134`（abort），Win32 `rc=127`。这是核心写存档失败的
既有行为，32 位与 64 位同口径（Win64 同样 abort），不算 A 方案回归。

### 8.5 覆盖范围更新

至此原先列出的 P0/P1 功能项已全部跑完，只剩前端级 32 位 RetroArch
（需设备侧确认 USB 安装）和 CI / 发版矩阵（属于范围决策，不在探针分支做）。

---

## 9. 追加：3DS/Vita 移植起步 —— GC 堆基址解耦（分支 `probe/pxt32-3ds-vita`）

> 2026-11-16 晚。承接前面的性能评估（两台掌机算力都够，门槛在移植形态）：
> 从 `probe/pxt32-gc-a` 派生本分支，第一步解耦 A 方案的命门——**GC 堆被
> `mmap(MAP_FIXED)` 钉死在编译期常量 `GC_BASE=0x20000000`**。3DS/Vita 是静态
> 链接（`libretro_ctr.a` / `libretro_vita.a`），没有这套 mmap，地址也可能被占。

### 9.1 改动：基址从编译期常量改成运行时变量

- `platform.h`：新增 `extern uint8_t *mca_gc_base;`（放分支外，Win32 走 PXT32
  旁路表的 gc.cpp 也可见）；PXT32 的 `PXT_IS_READONLY` 下界从 `GC_BASE` 改成
  `mca_gc_base`。PXT64 / `__MINGW32__` 分支一律不碰。
- `scheduler.cpp`：定义 `mca_gc_base = (uint8_t *)GC_BASE`（**默认值不变**，
  故既有平台零回归）；`gcAllocPtr` 前移到文件顶部，重启回绕改用 `mca_gc_base`。
- `gc.cpp`：旁路表索引与下界检查 `(addr - GC_BASE)` → `(addr - mca_gc_base)`。
- 新增导出函数 `mca_vm_set_gc_heap(void *base, unsigned size)`：静态链接平台在
  `pxt_vm_start*` 之前调用，指定 VM 堆区间；之后 `gcAllocBlock` 在该区间内
  bump 分配，**完全不调用 mmap**。未调用时 `mca_gc_heap_limit=0`，原 MAP_FIXED
  路径一字不变。Win32 PE 下额外加 `__declspec(dllexport)` 才能进导出表。

### 9.2 验证：注入任意堆地址，末帧逐位一致

给测试宿主加 `--gc-heap=SIZE`（`tools/mca_host_android.c`，仅测试、不在发版路径）：
在 `retro_init` 前 dlsym `mca_vm_set_gc_heap`，注入一块普通 `calloc` 堆（地址随
ASLR 变化，刻意避开 `0x20000000`）。用 32 位 Linux（纯 PXT32 分支，与 3DS/Vita
同构，工具机 tool69）验证：

- golden 三例：注入 64MB 堆后 `0556e78f…` / `e82844f7…` / `0eb7884c…`
  （含 A@4 按键），与基线逐位一致。
- probe 注入 3 次 + 换 128MB 堆：`e82844f7…` 全一致、稳定。
- 21 卡 × 600 帧注入堆对比 `android21-baseline.txt`：**21/21 逐位一致**，
  `bad=0`。
- 64 位 Windows core 全量重建后 golden 三例仍逐位一致（PXT64 零回归）。
- 32 位 Win32 core 全量重建 golden 三例一致；但**不要用它验证注入堆**：
  `__MINGW32__` 下 `gcAllocBlock` 走 xmalloc 分支、不读注入堆，改基址只会让
  旁路表与堆错位（实测 hash 不稳）。注入路径必须用纯 PXT32 平台验证。

### 9.3 结论与后续

GC 堆基址解耦成立：A 方案不再依赖固定地址，注入任意合法堆后末帧不变——
3DS/Vita 绕开 mmap 的核心机制已具备。距离可运行还差（按优先级）：
1. 静态链接形态：`libretro_ctr.a` / `libretro_vita.a` 两个 Makefile 目标 +
   devkitARM/vitaSDK 工具链（tool69 需装 devkitPro）。
2. 平台接入：在各自 `retro_init` 前后调 `mca_vm_set_gc_heap` 指向平台合法堆
   （3DS 线性内存 / Vita `sceKernelAllocMemBlock`），并确认堆容量够 21 卡最大
   （Mouse_Adventure 382KB，64MB 堆绰绰有余）。
3. pthread 栈：core 用 `pthread_create` 起 VM/worker 线程，3DS 栈尤其紧，
   devkitARM/newlib 语义要实测。
4. 前端实测：静态 core 链进 RetroArch 3DS/Vita 版，跑 golden + 8 键。

分支不合并 main、不进 CI/发版矩阵；范围决策仍是"不做 32 位发版"。

### 9.4 追加：静态库构建目标 + 平台接入（同日晚）

上面清单的第 1、2 项已实现（代码层面；真工具链实编受环境阻塞，见下）：

**Makefile 目标**（`core/Makefile`，复用 libnx 的 `STATIC_LINKING` 归档规则）：
- `platform=ctr` → `libretro_ctr.a`，devkitARM `arm-none-eabi-*`，
  `-march=armv6k -mtune=mpcore -mfloat-abi=hard -mfpu=vfp -DARM11 -D_3DS -D_PXT_CTR`
- `platform=vita` → `libretro_vita.a`，vitaSDK `arm-vita-eabi-*`，
  `-march=armv7-a -mtune=cortex-a9 -mfloat-abi=hard -mfpu=neon -DVITA -D_PXT_VITA`
- 两者都要求环境（`DEVKITPRO` / `VITASDK`），缺则明确报错；产物是静态库，
  39 个对象扁平化归档（防 base/control.cpp 与 core---vm/control.cpp 重名覆盖），
  带成员数自检——与 libnx 完全同构。

**平台接入**（不污染公共代码，仿 nx_diag 模式）：
- 新增 `core/src/platform_gc_heap.h`：非目标平台把
  `MCA_PLATFORM_GC_HEAP_INIT()` 展开成 `(void)0`（零符号）；3DS/Vita 分别调
  `mcaCtrInitGcHeap()` / `mcaVitaInitGcHeap()`。
- `core/src/platform_gc_heap_{ctr,vita}.cpp`：各自 `malloc(64MB)` 连续堆后调
  `mca_vm_set_gc_heap()` 注入。用 newlib malloc 而非 libctru/vitaSDK 专有 API，
  减少 SDK 耦合（分配失败只报日志、不 panic，让后续路径可见失败）。
- `core/src/libretro_core.cpp` 的 `retro_init` 开头调一次
  `MCA_PLATFORM_GC_HEAP_INIT()`；两个 cpp 加入 `sources.mk`（空 TU，5 入口共用）。

**验证**：
- 64 位 core 全量重建 + golden 三例逐位一致；两个新 cpp 在非目标平台编成空 TU，
  产物里查不到平台 hook 符号（零回归）。
- tool69 用 stub 工具链（软链系统 gcc）跑 `make -n`：ctr/vita 两条路径的编译器
  路径、ABI flag、39 对象归档命令、产物名、环境守卫全部正确；实编两个 hook cpp
  确认 `T mcaCtrInitGcHeap()` / `T mcaVitaInitGcHeap()` 定义符号在、
  未定义引用 `mca_vm_set_gc_heap` 由 scheduler.cpp 提供、无宏时为空 TU。

**真工具链阻塞（environment，非代码）**：tool69 无 devkitPro/vitaSDK；
`apt.devkitpro.org` 整域被 Cloudflare 403（连 Release 都拿不到）；GitHub release
大文件下载仅 ~65KB/s（65MB 安装包要 17 分钟，且那个 .pkg 是 macOS 格式用不上）；
Docker Hub 拉取超时；ghcr.io 可达但没有 devkitpro 官方镜像。故**没法在本环境用真
devkitARM/vitaSDK 实编**——静态库目标的正确性靠 stub 工具链的 make -n + hook
cpp 实编验证到结构层，真正的 SDK 头/链接需在有工具链的机器补做。

**剩余**（清单第 3、4 项）：pthread 栈适配（3DS 栈紧，devkitARM/newlib 语义实测）、
前端实测（静态 core 链进 RetroArch 3DS/Vita 版跑 golden + 8 键）。这两项都
强依赖真工具链/真机，本环境无法推进。

### 9.5 追加：真工具链实编打通（GitHub Actions 容器，2026-11-16 深夜）

9.4 末尾记录的"真 SDK 实编"阻塞已解除——绕法是 **GitHub Actions 官方容器**：
本机与 tool69 都访问不了 apt.devkitpro.org（Cloudflare 403），但 GitHub 的
runner 没这个限制。新增 `.github/workflows/3ds-vita.yml`（仅 workflow_dispatch，
不进常规 CI、不进发版矩阵）：

- `libretro_ctr.a`：容器 `devkitpro/devkitarm:latest`，**138 925 B，全绿**
- `libretro_vita.a`：容器 `vitasdk/vitasdk:latest`（注意 vitaSDK 不归
  devkitPro 发布，`devkitpro/vitasdk` 镜像不存在），**134 840 B，全绿**
- 校验（沿用 nx.yml 模式 + 平台 hook 断言）：ELF32/ARM、39 对象 = 39 归档成员
  （防同名覆盖）、25 个 retro_* 全局符号、4 个关键 retro 符号、
  `mcaCtrInitGcHeap` / `mcaVitaInitGcHeap` / `mca_vm_set_gc_heap` 必须在库内。

注意 workflow 文件必须存在于默认分支才能被 `gh workflow run` 发现——已在
main 单独 cherry-pick 该 yml（`0b1ed6b`，仅一个 dispatch-only CI 文件，
构建矩阵未动）；分支本体 `probe/pxt32-3ds-vita` 依旧不合 main。
**（2026-11-17 追注：该分支后来已完整合并进 main，`31d9969`，分支已删。）**

**首轮实编抓到 5 个只有真工具链才能暴露的问题**（全部已修，逐条零回归）：
1. `core.cpp` `getConfig`：`vmImg->configData` 是 `int32_t*`，devkitARM 的
   newlib 里 `int32_t = long int`，隐式转 `int*` 报错 → 显式转换（32 位下
   int/long 同宽，ABI 无差）。
2. `scheduler.cpp`：`sys/mman.h` 与 mmap 调用只排除了 PXT64 → 加排除
   `_PXT_CTR`/`_PXT_VITA`；裸机 gcAllocBlock 的 mmap else 分支改为明确
   panic（预置堆是唯一入口，预置缺失失败可见）。
3. `scheduler.cpp` `afterProgramPage`：定义用 `uint32_t` 与 pxtbase.h 的
   `unsigned` 声明冲突（同 1 的 long int 问题）→ 定义改回 unsigned。
4. newlib 默认特性可见性不声明 `strdup`（vmcache.cpp）与 `usleep`
   （vmload.cpp）→ 裸机平台补 extern "C" 声明（实现由 newlib/libctru
   提供，静态库阶段无需链接）。
5. `platform_gc_heap.h` hook 声明没包 extern "C" → 符号是 C++ 修饰名，
   nm 断言查不到 → 补 extern "C"。

64 位宿主 golden（ping 300 帧 `0556e78f…`）在改动后逐位一致——以上修复
对既有平台零行为变化。原 9.4 清单第 3、4 项中，pthread 的**编译期**风险
已随实编通过解除（头文件、类型都在），**链接/运行期**栈语义与前端实测
仍需真机（3DS 栈紧、Vita memblock 接入），是本分支剩余的全部工作。

## 10. 追加：pthread 栈语义定案 + VM 栈/GC 堆定容（2026-11-17）

### 10.1 pthread 语义（从真工具链二进制反汇编核实，非文档推断）

| 平台 | 实现层 | NULL attr 默认栈 | setstacksize |
|---|---|---|---|
| 3DS | devkitARM `libsysbase.a` 的 pthread 层 → `__syscall_thread_create`（libctru.a syscalls.o）→ libctru `threadCreate` | 32KB（0x8000） | 忠实传递（attr+4 → threadCreate stackSize；attr+0 必须 NULL） |
| Vita | vitasdk `libpthread.a`（PTE/pthread-embedded，vita_osal.o）→ `sceKernelCreateThread` | 0 → **最低 4KB** | 忠实传递（attr+8，`max(stack,4096)`） |

mutex/cond 两平台都有真实现；`PTHREAD_MUTEX_INITIALIZER` 可用（codalemu 的
irqMutex 我们本就显式 init）。libretro 路径上**唯一的线程**是 VM 线程
（`spinThread`，显式 attr；NULL attr 的 `vmStartFromUser` 只属于官方 sim 宿主，
重置路径 `vm_request_reset` 只置标志、重启走 `pxt_vm_start_buffer → spinThread`）。

**结论：pthread 不是 3DS/Vita 的阻塞项，内存预算才是。**

### 10.2 实测定容（2026-11-17，tool69）

方法：x86-32 zig 构建（PXT32，与 3DS/Vita 同指针宽度）+ 32 位 C 宿主，
64 卡 × 600 帧 × 6 档 `MCA_VM_STACK_SIZE`（8MB/2MB/512K/256K/128K/64K），
每组**强制清 build/dist 全量重编**（`needs_build` 按 mtime 跳过、换宏不触发
重编译——首轮 6 组因此全部跑了同一个 8MB 产物，已用 `so_sizes` + 反汇编
`mov r1, #0x40000` 复核修正）；GC 高水位用新增诊断 `-DMCA_GC_HWM_LOG=1`
（scheduler.cpp bump 点单调累计，VM 线程退出打 `[vm] gc heap hwm=… KB`）。

结果：
- **VM 栈真实需求 < 64KB**：64KB 档全部卡带末帧与 8MB 档逐位一致、rc=0；
  sm64thign（已知坏卡 rc=5）与 Horizontally_Spinning_Rat（已知墙钟抖动）
  在所有档位表现一致，与本变量无关。
- **GC 堆高水位最大 128KB**（stereoscopic_3d / low-poly_picachu），
  中位数 64KB；Mouse_Adventure 补测 64KB（旧记录"382KB"口径不同，作废）。
  hwm 与栈尺寸完全无关（自洽）。

### 10.3 落地参数（`-D` 可覆盖）

| 参数 | 值 | 余量 | 位置 |
|---|---|---|---|
| VM 线程栈 | 256KB | 4× | `core/Makefile` ctr/vita 目标 `-DMCA_VM_STACK_SIZE=262144` |
| GC 堆 | 2MB | 16× | `platform_gc_heap_{ctr,vita}.cpp` `MCA_GC_HEAP_SIZE` 默认值（原 64MB） |

OLD3DS 应用内存 ~64MB：2MB 堆 + 256KB 栈 + ~1MB 代码段，给前端留足空间。
堆越界走 `gc heap exhausted` 明确 panic，不会静默写坏。Vita（512MB）与 3DS
同口径，便于问题对照。

附带改动：`tools/build_target.sh` 支持环境注入 `EXTRA_CXXFLAGS/EXTRA_CFLAGS`；
GC 高水位诊断（`MCA_GC_HWM_LOG`）保留为常设诊断开关。真工具链重建验证：
ctr.a 482382 B / vita.a 455364 B，39 对象、25 retro_*、注入函数与 hook 符号
齐全，`vmload.o` 内栈常量 `#0x40000` 落位；64 位默认构建 golden 三例
（`0556e78f` / `e82844f7` / `0eb7884c`）逐位一致，零回归。

### 10.4 剩余（需真机/前端）

静态库链进 RA 3DS/Vita 前端跑 golden + 8 键；3DS 真机为 OLD3DS 时重点看
内存是否够（前端自身分配）。若某卡带 2MB 堆不足，先开 HWM 诊断实测再调
`MCA_GC_HEAP_SIZE`。

## 11. 追加：真机测试包（最小前端 .3dsx / .vpk，2026-11-17）

不走整编 RetroArch，core 以静态库链进自写的**最小 libretro 前端**，先把
「真机 + 真平台 ABI + 视频/音频/输入全链路」在 homebrew 形态下验掉：

| 包 | 平台/ABI | 产物 | 大小 |
|---|---|---|---|
| `frontends/ctr` | 3DS armv6k hard-float vfp（devkitARM） | `mca_ctr.3dsx` | 329 668 B |
| `frontends/vita` | Vita armv7-a hard-float NEON（vitaSDK） | `mca_vita.vpk` | 234 414 B |

前端职责（两边同构，各一文件 C）：环境回调（RGB565、bitmask、save/system
目录指到卡带目录）、video 直写帧缓冲（3DS 顶部屏 RGB565 2x 居中 + 底屏
console 状态；Vita `sceDisplaySetFrameBuf` RGBA 4x 居中——vitasdk snapshot
无 vita2d/SDL，绕开 GXM）、音频（3DS ndsp 双缓冲环 / Vita sceAudioOut
1024 帧块阻塞出队，无音频时回退 VBlank 节奏）、8 键映射对齐 docs/INPUT.md
（3DS A/B/十字/START/SELECT 物理键直映；Vita CROSS=A、CIRCLE=B）、
L/R 肩键切卡带（3DS Y 退出；卡带目录 `sdmc:/mca` / `ux0:/data/mca`）。

实编踩到的平台坑（均已修）：
1. **`pxt::sleep_ms` ABI 错位**：pxtbase.h 声明 `unsigned`（mangle 'j'）、
   scheduler.cpp 定义 `uint32_t`——devkitARM(newlib) 的 uint32_t=long int
   （mangle 'm'），静态库内 undefined reference。定义统一改 `unsigned`。
2. **libctru 新版 API**：`hidKeysHeld()` 无参、`gspWaitForVBlank()`（不是
   VSync）、`ndspWaveBuf` 无 nbytes 字段（nsamples 按 halfword 计，立体声
   =帧数×2）。
3. **vitasdk**：`sceDisplaySetFrameBuf` 走 `SceDisplayFrameBuf` 结构；
   `sceAudioOutOutput(port, buf)` 固定端口块长（开端口时定 1024）；
   `vita-pack-vpk -b` 直收 velf；无 SceNewlib_stub（newlib 粘合在 libc）；
   链接一律用 g++（libstdc++/__aeabi_atexit）。

tool69 真工具链复验：两包零警告重建；64 位默认构建 golden 三例
（`0556e78f`/`e82844f7`/`0eb7884c`）逐位一致。安装/验收步骤见
`docs/3DS-VITA-TEST.md`（probe 卡带逐键变色 + golden 基线）。

**剩余（真机）**：3DS 画面旋转宏如不对改 `blit_frame()` 一处；Vita
`SetFrameBuf` 若个别固件不亮需换 GXM/vita2d（届时 CI 加 libvita2d job）。

### 11.1 CIA 打包（2026-11-17 补）

CI ctr job 装 makerom/bannertool（**不在 devkitPro pacman 源**，从 GitHub
release 取：makerom v0.18.4 官方、bannertool 用 diasurgical fork 1.2.0——
原 Steveice10 仓库已 404）打 `mca_ctr.cia`（346 048 B，UniqueId 0xF1CA0/
CTR-P-MCA1，Logo Homebrew），artifact `ctr-testpack`（3dsx+cia）。

图标/banner 由 `tools/gen_cia_assets.py` 生成（纯 Python PNG，无 PIL）：
紫底白字 MCA + 1s 静音 wav（makebanner 的 `-a` 在 fork 里必填）。

rsf 三轮实测踩平：①缺 `AccessControlInfo/CoreVersion` 报 ExHeader Not
Found；②`EnableL2Memory` 键 makerom 不认；③还缺 `Priority` 与
`SystemCallAccess`。最终按 PKSM assets/app.rsf 结构重写：全用户态 SVC 表 +
本前端所需服务（fs:USER/gsp::Gpu/gsp::Lcd/hid:USER/dsp::DSP/APT:U/cfg:u）
+ 对应 Dependency 模块。bannertool makesmdh 参数按语言命名（-es/-el/-ep）。

### 11.2 暂停态卡带列表菜单（2026-11-17 补）

两个前端加同一套交互：**START+SELECT 同按进出**（两键同 held 且至少一键
新按下才算，避免先后按误触发）。菜单态**不跑 `retro_run`**，VM 状态原地
保留，退出即续玩；列表按文件名字典序，当前卡带标 `*`，光标 `>`，22 行
滚动窗口。装载复用 `load_cart()`（换卡带先 `retro_unload_game`）。

- 3DS：底屏 console 渲染（`consoleClear` + printf）。
- Vita：内嵌 `frontends/vita/font8x8.h`（dhepper/font8x8，Public Domain，
  仅 ASCII 可打印区 96 字形 × 8 字节）直绘 `g_fb`，深底 + 光标高亮条，
  退出菜单 `blit_frame()` 恢复游戏画面。

CI：vita job 补前端构建（`make -C frontends/vita` 默认目标即 vpk）+
`vita-testpack` artifact。commit `31c0fab`（run `43998990453` 全绿）。
包更新：3dsx 330 788 B（`995e00bc…`）、cia 347 072 B（`ee94c4f3…`）、
vpk 236 332 B（`bc89373b…`）。
