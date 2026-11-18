---

## 资产怎么选（每组拿一个就够，别都下）

**一、RetroArch 核心（装进已有 RetroArch 的核心目录，解压后放 .so/.dll）**

| 文件 | 给谁用 |
|---|---|
| `makecode_arcade_windows-x64.zip` | Windows 64 位（绝大多数 Windows 电脑） |
| `makecode_arcade_windows-arm64.zip` | Windows on ARM（Surface 等） |
| `makecode_arcade_windows-x86.zip` | 老 32 位 Windows（少见） |
| `makecode_arcade_linux-x64.zip` | Linux/掌机 64 位 x86 |
| `makecode_arcade_linux-arm64.zip` | Linux 掌机 64 位 ARM（RK3566/RK3326 等） |
| `makecode_arcade_linux-x86.zip` | Linux 32 位 x86 |
| `makecode_arcade_linux-armhf.zip` | Linux 32 位 ARM（retroarch32 机器） |
| `makecode_arcade_android-arm64.zip` | Android 64 位（绝大多数手机/掌机） |
| `makecode_arcade_android-armv7.zip` | Android 32 位（老设备） |
| `makecode_arcade_lakka-arm64.zip` | Lakka / LibreELEC（musl 系统） |

`*-debuginfo.zip` 是对应 Linux/Lakka 包的调试符号，只有要把崩溃地址翻译回
源码行时才需要，玩家跳过。

**二、3DS / Vita / Switch 静态核心文件（这两个平台只出核心形态）**

| 文件 | 给谁用 |
|---|---|
| `mca_libretro.cia` | 3DS 核心文件：完整 RetroArch，FBI 安装，new3ds 实测通过 |
| `mca_libretro.info` | 3DS/Vita 核心元数据，跨平台通用（见下方 3DS 列表说明） |
| `mca_libretro.self` + `mca_libretro.info` | PS Vita 核心文件：并入**已装**的 RetroArch |
| `makecode_arcade_switch.zip` | Nintendo Switch（NRO，放 `sdmc:/switch/`） |

> 3DS / Vita **只提供核心形态**（`mca_libretro.*` + `.info`）：极简前端
> （`mca_ctr.*` / `mca_vita.vpk`）与核心 VPK 已不再提供。
>
> **3DS 核心形态安装要点**：光装 `mca_libretro.cia` 只有一个独立图标；要让
> RA 的 Load Core 列表识别，再把同一 `.cia` 拷到 `sdmc:/retroarch/cores/`、
> 把 `mca_libretro.info` 拷到 `sdmc:/retroarch/cores/info/`，重启 RA。
> 静态核心加不进另一个官方 RA 构建，须从「MakeCode Arcade」图标启动。
>
> **Vita 核心安装**：VitaShell 开权限，把 `mca_libretro.self` 放已装 RA 的
> app 目录、`mca_libretro.info` 放其 `info/` 下。此前版本进游戏报
> `C2-12828-1` 已定位修复，请用最新版 self 复验；若仍崩附 `psp2core-*.psp2dmp`。

**三、卡带**

所有形态都只认 `.pxt64` 游戏文件（见上面的玩家指南，官方 IDE 直接导出，
或用 [cartconv](https://github.com/whmzsu/makecode-arcade-cartconv) 批量转换）。
3DS 卡带放 `sdmc:/mca/`，Vita 放 `ux0:/data/mca/`，Switch 放 `sdmc:/roms/makecode/`。

> 32 位说明：核心的 32 位包（x86/armhf/armv7）走与 64 位同一条 PXT64 镜像
> 路径（GC 标记旁路表方案），63 张卡带本地逐位一致；3DS 核心形态已在
> new3ds 真机验收通过，老款 3DS 无设备未验，Vita 核心形态待复验。
> 遇到问题欢迎提 issue。

---

## 文档

- 玩家安装与游玩指南（小白版）：
  [中文](https://github.com/whmzsu/makecode-arcade-libretro/blob/main/docs/%E7%8E%A9%E5%AE%B6%E6%8C%87%E5%8D%97.md) ·
  [English](https://github.com/whmzsu/makecode-arcade-libretro/blob/main/docs/Player-Guide.md)
