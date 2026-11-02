**MakeCode Arcade (unofficial) — libretro core v0.2.14**

修的是真机「加载存档卡死」这条链路，以及一条让跨会话存档永久失效的区间计算 bug。
**旧档（≤ v0.2.13 存的跨会话档）仍然不可用，需要用本版重新存一次**，原因见下。

---

## 1. 加载存档后卡死 / 崩溃

真机日志停在：

```
[snap] loaded (heap 65536, fibers 3, events 0)
[vm] cleanup 0/7 output offline
[vm] cleanup 1/6 gcFreeze
[vm] cleanup 2/6 reset callbacks        ← 再没有下文，画面冻住、进程不崩不退
```

三个独立问题叠在一起：

**(a) panic 之后的退出路径绕过了线程收尾。** `systemReset()` 末端是 `pthread_exit()`，
它**不走** `multiStart()` 的 return，于是 `mcaVmExited` 永远不置位 —— 宿主据此认为
VM 还活着（`pxt_vm_running() == true`），前端永不重启：画面停在最后一帧，进程既
不崩也不退。桌面（Mingw）上更直接：从 fiber 里 `pthread_exit` 会 0xC0000005。

现在 panic 不再 `pthread_exit`：`target_panic` / `soft_panic` 只置 `panicCode`，
解释器的 `if (panicCode) break;` 结束当前 fiber，控制权回到主循环，那里走
`hostRestartCleanup()` 后**正常 return**，线程从 `multiStart` 干净收尾。

**(b) 前端只认"玩家按了重置"。** VM 线程自己 panic 掉时 `g_reset_pending` 仍是
false，于是没人问"线程还在吗"。新增自愈：检测到 VM 线程猝死（`true → false` 跳变）
就自动用原始镜像重启；一次装载内最多 3 次，避免坏卡带变成每帧重启的死循环。

**(c) 前端线程上的 panic 会跑清理序列。** `systemReset()` 从 `retro_run` 线程调进来
等于把 RetroArch 的 render 线程注销；而且它此刻可能正握着 `irqMutex`（`pxt_get_audio_samples`
→ `target_disable_irq`），清理再进去抢就是自死锁 —— 真机日志精确停在
`cleanup 2/6`，那里调的 `stopPlaying()` 第一件事就是 `target_disable_irq()`。
现在前端线程的 panic 只登记码值 + 请求 VM 线程重启；`irqMutex` 也改成显式初始化的
**递归锁**，同线程重入不再自锁。

## 2. `拒绝恢复：本平台取不到模块区间`

```
[snap] 拒绝恢复：本平台取不到模块区间，跨会话恢复不安全
[snap] module relocate: old=0 size=0 cur=0 hits=0+0
```

`/proc/self/maps` 的解析器**就地**把每行 `\n` 改成 0 来切行，于是同一份缓冲区只能
遍历一次；而算区间天生要两遍（先找出本模块的路径，再按路径聚合所有映射段）。第二遍
读到的是一份报废缓冲区（只剩第一行 vdso），`g_modBase` 恒为 0 → 快照里的
`modBase/modSize` 全是 0 → 跨会话恢复被拒（`-13`），重定位也永远是 `hits=0+0`。

现在改成**只读**解析（逐行指针推进，路径拷进调用方缓冲），想看几遍看几遍。新增
`mcaModuleMapSelfTest()`：内嵌 maps 样本，任何平台都能跑，关键断言是"同一份文本
连续解析两遍结果一致"——就地改写型实现必挂。`tools/test_modrange.py` 通过 ctypes
调用它，并断言真实分支取到了非零区间（Linux CI 上等于覆盖了真机那条路径）。

> 桌面一直走 Windows 的 `VirtualQuery` 分支，这段代码从没被执行过，所以 S1–S10
> 全绿也挡不住它。

## 3. 诊断增强（下次真机出问题能一次定位）

`mca_trace.log` 新增：

| 行 | 说明 |
|---|---|
| `[mod] module range base=… size=… (ok/UNAVAILABLE)` | 模块区间到底取到没有 |
| `[vm] PANIC n on vm/frontend thread` | panic 码 + 哪条线程 |
| `[vm] cleanup begin on vm/frontend thread` | 清理发生在谁身上 |
| `[vm]   reset fn #i <addr> enter/done` | 逐个 reset 回调，卡在哪个一看便知 |
| `[snap] vm was paused N ms (host side slow?)` | 超过 2 秒的暂停（宿主侧问题） |
| `[snap] vm resumed after load (loops=… req=…)` | 读档后 VM 真的回到主循环 |
| `[host] vm thread gone unexpectedly -> auto restart (n/3)` | 自愈触发 |

## 4. 回归

- 新增 `tools/make_probe_cartridge.py --panic` 探针卡带（`control.panic(7)`），
  `test_lifecycle` 第 4 段断言：进程退出码 0、panic 走主循环收尾、自动重启 3 次后
  达到上限停止 —— 修复前这一项在桌面上直接 0xC0000005。
- 新增 `tools/test_modrange.py`（maps 解析自测 + 真实区间非零）。
- `test_savestate` S1–S10 全绿；`test_buttons` 16/16；`test_lifecycle` 5/5。
- 发版矩阵 7/7；7 个 zip 的 `.info` 均为 0.2.14。

## 5. 升级注意

- **≤ v0.2.13 存的跨会话档不能用**：它们的 `modBase/modSize` 是 0（第 2 条那个 bug），
  在 ARM 上无法重定位，恢复会被拒（`-13`）。请用 v0.2.14 重新存一次。
- 同一次运行内的存/读不受影响（不需要模块区间）。
- 启动横幅可自查：

  ```
  [MCA] core version 0.2.14, cart fingerprint e41ae5c9, trace -> ...
  ```

---

**非 Microsoft 官方产品。** MakeCode Arcade 是 Microsoft 的商标；本核心是第三方
libretro 移植，与 Microsoft 无关。
