# PXT32 A 方案实测报告（分支 `probe/pxt32-gc-a`）

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
  64 位本就不同，不能指望所有卡带逐位一致；ARM/Android 各 ABI 未逐一验证。
- **范围决策不变**：15 台设备全 64 位、收益近零，不恢复 32 位发版。本分支作为
  "能不能自己做"的证据保留，不合并 main。
