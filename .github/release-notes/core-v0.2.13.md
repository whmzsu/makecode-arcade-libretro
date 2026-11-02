**MakeCode Arcade (unofficial) — libretro core v0.2.13**

> 非官方核心，与 Microsoft 无关联。只播放 `.pxt64` 字节码镜像；
> MakeCode Arcade 的 `.png` 卡带里只有源码、不是可玩游戏，需先用
> [cartconv](https://github.com/whmzsu/makecode-arcade-libretro/releases) 或 `tools/png2pxt64.py` 转换。

## 真机读档的两个残留问题：`pause failed -2` 与"卡带不同"

v0.2.12 之后真机（RK3566 / ArkOS）读档仍会失败，日志两种形态交替出现：

```
[libretro ERROR] [MCA] unserialize: pause failed -2
[ERROR] [State]: 加载状态失败于 ".../cubeland.state5"

[snap] 拒绝恢复：卡带不同（快照 181608 字节/1064f705，当前 181608 字节/e2483410）
```

`mca_trace.log` 给出了直接证据 —— 注意超时那行是 **`spot=3`**：

```
[snap] saved 96704 bytes (heap 65536, fibers 4, events 0)
[snap] pause timeout 5s: spot=3 loops=742 req=742 fibers=4 live=1 online=1 panic=0
```

`spot=3` 表示 VM **已经停在安全点里了**，是宿主那边一直等不到回执。这与 v0.2.12
修的"VM 跑不停"是两回事。

### 一、暂停握手协议有竞态

RetroArch 读档（Load State）之前，会先 `retro_serialize` 存一个"撤销点"，于是两次
`pause` 紧挨着发生。旧协议是"停稳次数 +1"：宿主读一次计数、发请求、等计数涨到 +1。
问题在于 **resume 只是一个电平标志（request=0）**，如果 VM 还没来得及观察到它，
请求就又变回 1，VM 会一直待在内层暂停循环里，**不会再产生第二个回执** —— 宿主等
的那个序号永远到不了，5 秒后放弃。

改成 want/done 双序号：宿主每次 pause 换一个新序号写进 `mcaPauseWant`，VM 停稳后
把该序号写进 `mcaPauseDone`；resume 只需 `want = 0`。VM 的内层循环条件是"want 仍
等于我这次的序号"，所以**换序号必然退出**，不再要求"中间那次 resume 必须被观察到"。

桌面复现（同一份存档，同一帧先存后读，三次）：v0.2.12 的代码 **3 次里 2 次**报
`pause failed -2`，trace 也是 `spot=3`；v0.2.13 三次全过。回归新增 **S9**。

### 二、卡带指纹算在了打补丁之后

`vm_patch_image()` 会把 vtable 指针和内核函数指针（都指向 core 自己的模块）**就地
写回镜像**。指纹原本是拿 patch 之后的 `dataStart..dataEnd` 现算的 —— Linux/Android
的 `.so` 走 ASLR，换个进程这些地址就变，于是**同一个卡带、自己存的档**，重开后被
判成"换了卡带"。Windows 的 DLL 基址固定，所以桌面回归一直没暴露这条。

现在指纹在装载时、**patch 之前**对原始镜像字节算一次（`vmload.cpp` 的
`mca_image_hash()`），存档与校验都用它。启动横幅会打印它：

```
[MCA] core version 0.2.13, cart fingerprint e41ae5c9, trace -> ...
```

回归新增 **S10**：两次独立进程的指纹必须一致，且必须等于"原始镜像字节的 FNV-1a"
（Python 现算）。把计算挪回 patch 之后即可看到判据变红。

> 注意：v0.2.12 及更早存的档，指纹来自打补丁后的镜像，**在 ARM 上无法通过校验**，
> 会被拒绝（`-10`）。请用 v0.2.13 重新存一次档。

### 顺带

`mcaTrace` 的开文件动作移进了互斥区。之前 VM 线程与前端线程同时第一次写 trace 时
会各自 `fopen(..., "w")`，后开的把先写的内容截掉 —— 桌面上的 `[vm] ...` 那几行经常
整个消失，真机排障时也可能丢掉关键行。

### 验证

- `test_savestate` **S1–S10 全绿**（新增 S9 连续存读 / S10 卡带指纹）
- S9、S10 均已用"改回旧行为"的对照构建验证会变红
- `test_buttons` 16/16、`test_lifecycle` 全过
- cubeland（真机那张卡带）桌面跨进程读档：`恢复快照 -> OK`，无拒绝、无超时
- 存档格式未变（v3），但旧档需重存（见上）
