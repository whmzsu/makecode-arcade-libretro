自动生成的**诊断产物**，用于定位 Switch 上"加载游戏后黑屏几秒就崩"的问题。不是发版，不是给玩家玩的东西。

## 最终结论（2026-11-07）：根因在系统侧，不在核心

**用户侧决定性发现：官方 RetroArch 前端加载其他核心的游戏同样崩** —— 与我们的核心无关。
用户启动方式为 NSP forwarder（title takeover 全内存），applet 内存额度论也排除。

用符号表（`mca_boot.symbols.txt` + `addr2sym.py`）解析 4 份 crash report，
**所有崩点全在 mesa/nouveau（Switch GL 驱动）的 malloc/free 路径里**，核心函数从未上栈：

| 场景 | 栈（自底向上截取） |
|---|---|
| ozone 菜单画光标 | `_malloc_r` ← `nouveau_mm_allocate` ← `nouveau_buffer_transfer_map` ← `glBufferData` ← `ozone_draw_cursor` |
| swap buffers | `_free_r` ← `nouveau_fence_trigger_work` ← `eglSwapBuffers` ← `gl2_frame` |
| 游戏运行期（旧报告） | dlmalloc 拆链 ← `nouveau_buffer_create` ← `nvc0_launch_grid` |

野指针高位全是 `0x3f…`（double 位模式）—— 堆空闲链表被浮点数据覆盖，是系统级内存被破坏的表现。

**根因：固件 22 + Atmosphère 1.11.1 错配。** 官方 release notes 对照：
1.11.0=22.0.0 基础支持 → 1.11.1=22.1.0 → **1.11.2（2026-06）才补全 22.x** → 1.12.1（2026-10-16）= 22.x 全量 + 23.0.0。
fw 22 发布时 GBAtemp 帖子的症状（自制必崩、forwarder 能进但加载内容报错）与本机逐字吻合。
**解法：升级 Atmosphère 到 1.12.1（fusee 必须同步更新，fusee-primary 已废弃；Hekate 一并升）。**

我们的 NRO（CI libnx 4.12.0）证据链健康：菜单能进、VM 10ms 就绪、Lakka 真机全通过。
教训：**没有符号表时纯反汇编"还原"函数会把 nouveau_buffer_create 误读成"我们的清理循环"** ——
nx-diag 附带 symbols.txt + `addr2sym.py` 是诊断链的固定环节，不要省。

## 已经确认的事实

真机：固件 22.0.0 / Atmosphere 1.11.1，从 hbmenu 启动。

| 观察 | 结果 |
|---|---|
| `ra_plain.nro`（RetroArch 本体，不含核心） | 能启动，正常 |
| `mca_boot.nro` / 发版包（前端 + 我们的核心） | 能启动，VM 10ms 就绪进主循环，但画面黑，几秒后进程崩 |

`mca_trace.log` 停在 `=== core started ===`，再往后一行都没有 —— 说明 VM 起来了、游戏在跑，是**运行期**崩的，不是启动问题。

崩溃报告（Atmosphere）读出来的结论：

```
Result 2168-0002 (Data Abort)
PC = retroarch_switch + 0x7d20c4
 7d20c0  ldr x1, [x6, #0x18]     ; 空闲块 chunk->bk->fd
 7d20c4  str x1, [x0, #0x18]     ; 崩在这，x0 = 0x3f0a8a8b3dc8c8ca（野指针）
```

这段是 newlib 的 `free()` 在拆空闲链表。**堆被人写坏了**（空闲块的 fd 被覆盖），不是 libnx ABI / 链接 / 内存不足。
（后注：该 PC 实为 `_dtoa_r+0x11e4` 处的 dlmalloc 例程，调用方是 nouveau——见上表；当时没有符号表，归属判断部分有误。）

模块归属也是确定的：报告里模块的 text 段 8,286,208 字节，与本 NRO 的 `text_size` 逐字节相等（`ra_plain` 是 8,151,040，差 135,168）。所以崩的确实是"前端 + 我们的核心"这个包。

## 这一版加了什么

**1. `sdmc:/switch/mca_run.log`** —— 运行期 trace，每 30 帧（0.5 秒）一行：

```
[host] stack request=8388608 set=0 get=0 got=... create=0
[vm] thread entry local=0x...
[run] first retro_run (mode=0)
[run] f=30 online=1 thr=1 up=1 panic=0 mode=0 sp=0x...
[run] f=60 online=1 ...
```

每行写完之后还会做一次 `malloc(24)` + `free()` 的**堆探针**。也就是说：如果日志最后一行是 `f=1230`，那崩溃就发生在第 1230 帧之后的这次探针里 —— **帧号直接给你**。

日志用裸 `open/write/close` 写，不走 stdio：堆坏了以后 `fprintf` 自己可能要 malloc，写不出去。

`stack request=8388608 ... got=?` 这一行是专门查"VM 线程真的拿到 8MB 栈了吗"——libnx/newlib 有可能把它悄悄限成 128KB，那样 VM 深递归就会踩穿相邻堆，症状和现在完全一致。

**2. `sdmc:/switch/mca_mode.txt`** —— 切通路口径，用记事本建一个纯文本，内容写一个数字：

| 内容 | 行为 |
|---|---|
| `0` 或没有这个文件 | 正常（默认） |
| `1` | 不往前端推视频帧（画面全黑） |
| `2` | 不往前端推音频 |
| `3` | 完全不拉 VM 数据（黑帧 + 静音），只跑 VM 自己 |

同一份包跑四次，就能判断堆是死在 **video 回调 / audio 回调 / pxt 取帧拷贝** 哪一条上，还是与这三条都无关（那就是 VM 线程自身）。

## 请这么跑

全部放 `sdmc:/switch/`，按住 `R` 点任意已装游戏进 hbmenu。

1. **先跑一次 mode 0**（不放 `mca_mode.txt` 就是 0），等它自己崩掉。
2. 回传 `sdmc:/switch/mca_run.log`（**整个文件**，不只是最后几行）+ 新的崩溃报告。
3. 如果还有余力，再把 `mca_mode.txt` 依次写成 `1` / `2` / `3`，每次跑完记一句"崩了没崩、大约几秒"，并保留当次的 `mca_run.log`（会被追加，不是覆盖，所以在行首记一下当前批次也行）。

## 崩溃报告在哪

```
sdmc:/atmosphere/crash_reports/
```

按修改时间取最新那份 `.log`。重点看 `PC` / `LR` 的 `retroarch_switch + 0x...` 偏移——配上本页的 `mca_boot.symbols.txt` 能直接翻成函数名。

## 附带文件

- `mca_boot.nro` — 我们的核心 + 诊断打点（`-DMCA_NX_DIAG=1`，正式发版包不带这些打点）
- `ra_stubcore.nro` — RetroArch + 空核心（只验证静态核心机制）
- `probe_nacp.nro` — 纯 libnx 控制台程序（带 NACP 的版本）
- `ra_plain.nro` — RetroArch 本体，不含核心
- `*.symbols.txt` — 对应 NRO 的 `nm -n` 符号表，崩溃偏移靠它对到函数
- `*-build.log` — 构建日志

---

## 最终定案（2026-11-07 凌晨）

机器：固件 22.0.0 + Atmosphère 1.12.1（用户已从 1.11.1 升级）。

**这不是我们核心的问题。**证据链：

1. crash report 里的栈从未出现核心函数；VM 侧 mca_trace 一路跑到
   "core started"（镜像装载/GC/initRuntime/主循环全通，10ms 就绪）。
2. **官方 RetroArch 1.21.0 与 1.22.2（不含任何第三方核心）裸前端同样崩**，
   用户配置重置（retroarch.cfg 重生 + 无着色器缓存）后依旧。
3. 崩点在 mesa/nouveau 的 **nv50_ir 着色器编译器**（1.22.2 符号已确认：
   NVC0LegalizePostRA::findFirstUses 读哈希表野指针）。1.21.0 与 1.22.2
   寄存器签名逐位一致（X5=0xfffffffe, X6=0x21），野值每次不同（符号扩展
   负值垃圾）。
4. **连非 RA 的自制 GUI（tencent-switcher-gui）也崩，野值特征相同**；
   melonDS 正常（软件渲染，不走 GL 着色器管线）。
5. 启动方式矩阵：title takeover = 菜单✓、加载核心崩（Application 进程崩）；
   applet = NRO 跑在 hbloader 进程里，RA 1.21.0 一启动整个 hbloader 崩
   （Program ID 010000000000100d）→ 整机重启。

上游：libretro/RetroArch#18905（open，fw21.2 起有人报，fw22.5 复现）；
Atmosphere-NX/Atmosphere#2717（同错误码的 applet 崩溃讨论）。

**NRO text 大小对照表**（判 crash report 归属用）：
我们的 NRO=8,286,208 / 官方1.22.2=8,134,656 / 官方1.21.0≈7,741,440（个别
报告 7,737,344，段对齐差一页）/ tencent-switcher-gui=4,984,832。
