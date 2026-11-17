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

**二、3DS / Vita / Switch 整包（不需要装 RetroArch，装完主菜单直接有图标）**

| 文件 | 给谁用 |
|---|---|
| `mca_ctr.3dsx` | 3DS，从 Homebrew Launcher 启动 |
| `mca_ctr.cia` | 3DS，FBI 安装到主菜单（推荐） |
| `mca_libretro.cia` | 3DS，想要**完整 RetroArch**（设置菜单等）的人；与极简包可共存 |
| `mca_vita.vpk` | PS Vita 极简包（推荐，启动最快） |
| `mca_libretro.vpk` | PS Vita，完整 RetroArch 形态（独立图标，与已装 RA 并存） |
| `mca_libretro.self` + `mca_libretro.info` | PS Vita，把核心并入**已装**的 RetroArch |
| `makecode_arcade_switch.zip` | Nintendo Switch（NRO，放 `sdmc:/switch/`） |

> 名字带 `mca_ctr` / `mca_vita` 的是**极简前端**（无菜单、启动最快）；
> 名字带 `mca_libretro` 的是**完整 RetroArch 形态**。两者装哪个都行，也可共存。

**三、卡带**

所有形态都只认 `.pxt64` 游戏文件（见上面的玩家指南，官方 IDE 直接导出，
或用 [cartconv](https://github.com/whmzsu/makecode-arcade-cartconv) 批量转换）。
3DS 卡带放 `sdmc:/mca/`，Vita 放 `ux0:/data/mca/`，Switch 放 `sdmc:/roms/makecode/`。

> 32 位说明：核心的 32 位包（x86/armhf/armv7）走与 64 位同一条 PXT64 镜像
> 路径（GC 标记旁路表方案），63 张卡带本地逐位一致；3DS/Vita 真机验收仍在
> 进行中，遇到问题欢迎提 issue。

---

## 文档

- 玩家安装与游玩指南（小白版）：
  [中文](https://github.com/whmzsu/makecode-arcade-libretro/blob/main/docs/%E7%8E%A9%E5%AE%B6%E6%8C%87%E5%8D%97.md) ·
  [English](https://github.com/whmzsu/makecode-arcade-libretro/blob/main/docs/Player-Guide.md)
