**MakeCode Arcade (unofficial) — libretro core v0.2.11**

> 非官方核心，与 Microsoft 无关联。只播放 `.pxt64` 字节码镜像；
> MakeCode Arcade 的 `.png` 卡带里只有源码、不是可玩游戏，需先用
> [cartconv](https://github.com/whmzsu/makecode-arcade-libretro/releases) 或 `tools/png2pxt64.py` 转换。

## 本版重点：存档跨会话 —— 退出 RetroArch 之后还能接着玩

v0.2.10 的存档只在自己那次运行里有效，退出 RetroArch（或崩一次）再读档就会被
拒绝，界面上还常只是"加载成功但游戏照旧"。这一版把它做通了：

**存一档 → 退出 RetroArch（甚至崩溃）→ 重开同一个游戏 → 加载 → 从存档那一刻
继续玩，画面与原时间线逐帧一致。**

做法是让地址可复现，而不是重新实现序列化：

| 对象 | 处理 |
|---|---|
| GC 堆 / 卡带镜像 / VM 单例 | 钉在固定地址（`GC_BASE`、`+1MB`、`+5MB`），换进程地址不变 |
| fiber / 事件 | 恢复时重建，`sp` 与栈副本按字节偏移还原，`pending` 链重接 |
| 指向 core 自身代码的指针（`RefAction` 回调、fiber 的 `wakeFn`） | 快照记下保存时的模块区间，恢复时按差值整体平移 |

安全边界也补齐了：

- 快照带**卡带字节数 + FNV-1a 指纹**。拿别的游戏的档恢复会被拒绝（日志 `-10`
  一行说明），游戏照常继续、不会崩前端；垃圾数据同理。
- 平台取不到模块加载区间时（目前 macOS），跨会话恢复直接拒绝（`-13`），退回
  v0.2.10 的"同会话专用"，绝不带着野回调跑。
- 核心版本变了，旧档同样拒绝——升级核心后请重新存档。

## 顺带修掉的两个真问题

1. **暂停握手抢跑**（v0.2.10 就有）：`pxt_state_pause()` 只看暂停标志，而这个
   标志的清除晚于置位，连续存/取时宿主会读到上一次的残留值，于是搬到的是 VM
   正在改的半截状态。真机上表现为"读档后立刻 panic / 几十帧后画面走岔"。现在
   等"一个**新的**停稳事件"。
2. **恢复时往已释放的内存里写活对象**：同进程恢复为了省事，把 fiber 释放后原地
   写回，那块内存已回到分配器手里 —— 游戏下一次新建 fiber 就盖住它，表现为
   "读档后几十帧画面才分叉"（约 1/8 概率，压测更明显）。现在一律重建，`stackCopy`
   同样重建。

## 下载哪个

| 你的设备 | 选 |
|---|---|
| RK3566 / ArkOS / 各类 Linux 掌机（aarch64） | `linux-arm64` |
| 同上但 32 位系统（armv7/armhf） | `linux-armhf` |
| Android 手机/平板 + RetroArch（arm64-v8a） | `android-arm64` |
| 老 32 位 Android 设备 | `android-armv7` |
| Windows x64 / arm64 桌面 | `windows-x64` / `windows-arm64` |
| Linux 桌面 x86_64 | `linux-x64` |

Linux 包按 **glibc 2.17** 基线编译（覆盖 2012 年后几乎所有掌机固件），无额外运行库依赖。

**Android 注意**：core 目录保持默认（App 私有），**核心信息目录要单独指到一个可写
的外部路径**（`libretro_info_path`）——`/storage/emulated/0` 是 noexec，`.so` 放那里
能识别但加载必失败；而「安装或恢复核心」只拷 `.so`、会丢弃 `.info`，所以 `.info`
必须手动放进那个目录。包内 `README.txt` 有完整步骤。

## 已知现象（不是核心缺陷）

- Pong 模板长按 **UP** 会触发游戏自身的 `panic(1980)`（精灵越界），蓝屏并写日志，
  属游戏层行为。
- 单份快照约 1.6 MB；Rewind 的环形缓冲默认存几十份，掌机上的内存占用待实测。
- v0.2.11 尚未在真机（RK3566 / Android）验收，结论会补在仓库 `docs/DEVICE-TEST.md`。

## 回归

`test_savestate` S1–S7（确定性 / 同会话收敛 / 跨卡带拒绝 / 垃圾拒绝 / **跨进程** /
**跨模块基址** / **rewind 热路径**）全绿，并发三份压测亦全绿；
`test_buttons`（16 个按键用例）、`test_lifecycle`（换游戏 / 关闭 / 重置后音频）全过。
