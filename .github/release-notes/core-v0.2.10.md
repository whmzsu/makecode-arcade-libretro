**MakeCode Arcade (unofficial) — libretro core v0.2.10**

> 非官方核心，与 Microsoft 无关联。只播放 `.pxt64` 字节码镜像；
> MakeCode Arcade 的 `.png` 卡带里只有源码、不是可玩游戏，需先用
> [cartconv](https://github.com/whmzsu/makecode-arcade-libretro/releases) 或 `tools/png2pxt64.py` 转换。

## 本版重点：即时存档 + 完全确定

**`retro_serialize` / `retro_unserialize` / `retro_serialize_size` 全部接入。**
RetroArch 的即时存档、Rewind（逐帧回带）、Run-Ahead 都能用了。快照是全状态的：
GC 堆、fiber、事件队列、共享操作数栈、单例指针、随机状态一次搬完；`serialize_size`
恒定，前端可以按它一次性分配环形缓冲（单份约 1.6 MB）。

语义是**同会话精确恢复**：恢复后画面与原时间线逐帧重合。v1 不支持跨进程/跨卡带
恢复（GC 堆里是绝对地址），这类快照会被拒绝并返回失败，游戏继续跑、不会崩。

为了让快照真正可复现，这一版把核心做成了**完全确定**的：同一卡带 + 同一输入序列，
两次独立进程跑 300 帧的末帧画面逐字节一致。为此修掉五个非确定源：

1. 取帧等待用的 `pthread_cond_timedwait` 在 zig-mingw 下直接返回 EINVAL —— 前端
   实际上没等，300 帧里有 288 次读的是上一帧。改为跨平台事件原语。
2. `retro_load_game` 异步返回，装载期间前端白拉的黑屏帧不计圈数，导致「前端第 N
   帧」与「VM 第 N 圈」之间留下由墙钟决定的偏移。改为同步等到主循环就绪。
3. 取帧请求条件化 → 改为无条件请求 + 序号严格配对（第 N 次取帧 = 第 N 圈 = 第 N 次发布）。
4. pxt 的 settings 模块会把持久化的「运行次数」混进随机种子（每次 +1），于是两次
   运行第 2 帧起画面就分叉。种子改为只由卡带镜像哈希决定。
5. 屏幕缓冲未清零，头几帧取决于堆里的残留数据。

**虚拟时钟随之改为「前端请求驱动、一圈一帧」**：快进与慢放成为前端取帧频率的
自然结果，不再靠缩放时钟（缩放会让 `forever` 循环丢 tick）。慢放 = 前端拉帧慢，
游戏自动压到消费速度，音频跟帧走。

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
- 高分等 settings 仍持久化在存档目录，但不再影响随机序列。
- v0.2.10 尚未在真机（RK3566 / Android）验收，验收结论会补在仓库 `docs/DEVICE-TEST.md`。

## 回归

`test_savestate`（确定性 / 同会话收敛 / 跨卡带拒绝 / 垃圾数据拒绝）、
`test_buttons`（16 个按键用例）、`test_lifecycle`（换游戏 / 关闭 / 重置后音频）全过；
CI 13/13（发行版 gcc + zig 0.17.0 双工具链交叉验证）。
