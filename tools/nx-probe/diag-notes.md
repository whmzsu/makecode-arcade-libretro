自动生成的**诊断产物**，用于定位 Switch 上 NRO 一启动就崩的问题。不是发版，不是给玩家玩的东西。

## 怎么用

两个文件都放 `sdmc:/switch/`，然后按住 `R` 点任意已装游戏进 hbmenu，依次启动。

### 1. `probe.nro`（先跑这个）

纯 libnx 控制台程序，不建线程、不链第三方库。启动后屏幕上会打印：

- 构建时用的 libnx 版本
- 你机器的固件版本
- applet 模式（相册 = 限内存 / title takeover = 全内存）
- 本进程的可用内存与已用内存

**判定**：

- 能看见这段字 → 工具链、libnx 与固件 ABI、hbmenu 启动路径都正常，问题在 RetroArch 或核心
- 也崩了 → 与我们的代码无关，是 libnx 与固件的 ABI 不兼容

### 2. `ra_plain.nro`（控制组）

RetroArch 本体，**不含**本项目的核心（`HAVE_STATIC_DUMMY=1` 构建）。

- 它能进菜单、我们的 NRO 不能 → 问题在核心
- 它一样崩 → 问题在 RetroArch 前端本身（官方 1.22.x 在新固件上的已知状况）

## 备注

`ra_plain.nro` 是"尽力而为"产物，CI 里编不出来就不会出现在这里。
