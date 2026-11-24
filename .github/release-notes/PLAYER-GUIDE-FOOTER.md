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
| `makecode_arcade_macos-x64.zip` | macOS Intel（x64，.dylib） |
| `makecode_arcade_macos-arm64.zip` | macOS Apple Silicon（M 系列，.dylib） |
| `makecode_arcade_android-arm64.zip` | Android 64 位（绝大多数手机/掌机） |
| `makecode_arcade_android-armv7.zip` | Android 32 位（老设备） |
| `makecode_arcade_lakka-arm64.zip` | Lakka / LibreELEC（musl 系统） |

`*-debuginfo.zip` 是对应 Linux/Lakka 包的调试符号，只有要把崩溃地址翻译回
源码行时才需要，玩家跳过。

**二、3DS / Vita / Switch 静态核心文件（先装官方 RetroArch，再并入核心）**

| 文件 | 给谁用 |
|---|---|
| `makecode_arcade_ctr.zip` | 3DS 核心（内含 `makecode_arcade_libretro.cia` + `makecode_arcade_libretro.info`）：解压后把 cia 复制进官方 RA 的 `cores/`、info 放 `cores/info/`，new3ds 实测通过 |
| `makecode_arcade_vita.zip` | PS Vita 核心（内含 `makecode_arcade_libretro.self` + `makecode_arcade_libretro.info`）：解压后并入官方 RA 的 app 目录 + `info/` |
| `makecode_arcade_switch.zip` | Nintendo Switch 核心（内含 `makecode_arcade_libretro_libnx.nro` + `makecode_arcade_libretro.info`）：解压后 NRO 放官方 RA 的 `sdmc:/retroarch/cores/`、info 放 `sdmc:/retroarch/info/` |

> **3DS / Vita 都建议先装官方 RetroArch**（从 RetroArch 官网 / buildbot 下载），
> 再把对应 zip 解压、按上面说明把里面的核心文件并入，和其他平台一样在 RA 里
> 选核心、开游戏。极简前端（`mca_ctr.*` / `mca_vita.vpk`）与核心 VPK 已不再提供；
> info 不再单独作为资产，已打进各平台 zip。
>
> **3DS**：解压 `makecode_arcade_ctr.zip`，把 `makecode_arcade_libretro.cia` 放到
> `sdmc:/retroarch/cores/`、`makecode_arcade_libretro.info` 放到 `sdmc:/retroarch/cores/info/`，
> 重启 RA，Load Core 即出现 MakeCode Arcade。（也可直接用 FBI 装 CIA 得到独立图标入口。）
>
> **Vita**：解压 `makecode_arcade_vita.zip`，VitaShell 开权限，把 `makecode_arcade_libretro.self`
> 放官方 RA 的 app 目录、`makecode_arcade_libretro.info` 放其 `info/` 下。此前版本进游戏报
> `C2-12828-1` 已定位修复，请用最新版 self 复验；若仍崩附 `psp2core-*.psp2dmp`。
> 画面被拉满全屏、改成 4:3 也不生效的，是早期存过的 core override 在覆盖全局设置：
> 删掉 `ux0:/data/retroarch/config/MakeCode Arcade/` 整个目录重启 RA 即可（前端设置问题，与核心无关）。
>
> **Switch**：解压 `makecode_arcade_switch.zip`，把 `makecode_arcade_libretro_libnx.nro`
> 放 `sdmc:/retroarch/cores/`、`makecode_arcade_libretro.info` 放 `sdmc:/retroarch/info/`
> （Switch 默认 info 目录，注意与 3DS 的 `cores/info/` 不同）。启动官方 RA → Load Core
> → MakeCode Arcade → Load Content 选卡带。（NRO 放 `sdmc:/switch/` 由 hbmenu 直接
> 启动也可，title takeover 拿完整内存；RA 的默认目录固定在 `sdmc:/retroarch/`，与
> NRO 实际位置无关。不要从相册图标进——applet 模式内存只有几百 MB。）
>
> **更名说明**：自本版起，3DS/Vita 的核心文件从旧短名 `mca_libretro.cia/.self`
> 统一更名为 `makecode_arcade_libretro.cia/.self`（与 PC/Android/Switch 一致，
> info 同步更名）。功能无变化；**Vita 老用户请删除旧文件** `mca_libretro.self`
> 与 `mca_libretro.info`（否则 Load Core 列表会出现重复项），3DS 按标题 ID
> 安装、无需处理。
>
> **⚠️ Vita 删旧文件的顺序很重要**：RetroArch 启动时会加载"上次使用的核心"，
> 若旧文件被删、而 RA 记住的仍是旧路径，**RA 会打不开**。正确做法是先装新核心 →
> 在 RA 里 Load Core 选 `makecode_arcade_libretro.self` → 正常退出 RA → 再删旧文件。
> 已经踩到这个坑的话，用 VitaShell 把 `makecode_arcade_libretro.self` 复制一份
> 改名为 `mca_libretro.self`（`info/` 下同样复制 `makecode_arcade_libretro.info`
> 为 `mca_libretro.info`）放回原位，RA 即可启动；进去切到新核心、正常退出后，
> 再删掉这两个改名副本。仍打不开时，编辑 `ux0:/data/retroarch/retroarch.cfg`，
> 把 `libretro_path` 清空或改为新核心完整路径。

**三、卡带**

所有形态都只认 `.pxt64` 游戏文件（见上面的玩家指南，官方 IDE 直接导出，
或用 [cartconv](https://github.com/whmzsu/makecode-arcade-cartconv) 批量转换）。
卡带放哪都行（RA 文件浏览器能浏览到即可）；
`sdmc:/mca/`、`ux0:/data/mca/`、`sdmc:/roms/makecode/` 只是示例路径，不是必须。

> 32 位说明：核心的 32 位包（x86/armhf/armv7）走与 64 位同一条 PXT64 镜像
> 路径（GC 标记旁路表方案），63 张卡带本地逐位一致；3DS 核心形态已在
> new3ds 真机验收通过；老款 3DS 无真机，但模拟时明显卡顿（硬件算力下限），
> 只保证能跑、不保证流畅，想要流畅请用 new3ds；Vita 核心形态待复验。
> 遇到问题欢迎提 issue。

---

## 文档

- 玩家安装与游玩指南（小白版）：
  [中文](https://github.com/whmzsu/makecode-arcade-libretro/blob/main/docs/%E7%8E%A9%E5%AE%B6%E6%8C%87%E5%8D%97.md) ·
  [English](https://github.com/whmzsu/makecode-arcade-libretro/blob/main/docs/Player-Guide.md)
