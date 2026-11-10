# 真机台账（DEVICE-LEDGER）

> 一眼看清有哪些设备、跑什么版本、怎么连。
> 验收过程见 `DEVICE-TEST.md`，性能数据见 `perf-2026-11-04/`，兼容性细节见 `ANDROID-2026-11-08.md`。
>
> **更新规则**：发版后用 `out/therm/softinfo.sh` 重采一遍，或手工改「核心」列；
> 设备新增/退役就加删行，别让表滞后于现实。

## 1. Linux 掌机（9 台）

| 别名 | IP | 型号 | SoC / 内存 | 系统 / 内核 | RA | 核心 | 状态 |
|---|---|---|---|---|---|---|---|
| `cubexx` | 192.168.1.149 | Anbernic RG CubeXX（H700，标识 `rgcubexx-h`） | 4×A55 1.80G / 973MB | muOS 2606.0 (ANDROMEDA) / 4.9.170 | 1.22.2 | **0.3.8** | 在线（19:23 开机后补升） |
| `rgb30c` | 192.168.1.101 | Powkiddy RGB30 | RK3566 1.99G / 967MB | Debian 13 (trixie) / 5.10.226 | 1.22.2 | **0.3.8** | 在线 |
| `rg353m` | 192.168.1.130 | Anbernic RG353M | RK3566 1.99G / **1971MB** | Debian 13 / 5.10.226 | 1.22.2 | 0.3.3 | 关机 |
| `rg353v` | 192.168.1.77 | Anbernic RG353V/VS | RK3566 1.99G / 967MB | Debian 13 / 5.10.226 | 1.22.2 | 0.3.3 | 关机 |
| `rk2023` | 192.168.1.17 | Powkiddy RK2023 | RK3566 1.99G / 964MB | **Ubuntu 19.10** / 4.19.172（glibc 2.30） | **1.19.1** | 0.3.3 | 关机 |
| `rgb30a` | 192.168.1.26 | Powkiddy RGB30 | RK3566 1.99G / 967MB | Debian 13 / 5.10.226 | 1.22.2 | 0.3.3 | 关机 |
| `rgb30b` | 192.168.1.85 | Powkiddy RGB30 | RK3566 1.99G / 967MB | Debian 13 / 5.10.226 | 1.22.2 | 0.3.3 | 关机 |
| `r36sclonea` | 192.168.1.31 | R36S Clone Type2（RG351MP 壳，host `r36s-clone-a`） | RK3326 1.30G / 918MB | Debian 13 / **4.4.189** | 1.22.2 | 0.3.3 | 关机 |
| `r36scloneb` | 192.168.1.137 | R36S Clone Type2（RG351MP 壳，host `r36s-clone-b`） | RK3326 1.30G / 918MB | Debian 13 / **4.4.189** | 1.22.2 | 0.3.3 | 关机 |

系统分布：Debian 13 ×7 / ArkOS ×1（rk2023，实为 Ubuntu 19.10 底）/ muOS ×1。
卡带目录：Debian=`/roms/makecode`、ArkOS=`~/makecode`、muOS=`/mnt/mmc/ROMS/MakeCode Arcade`。

## 2. 其他平台

| 别名 | IP | 设备 | 规格 | 系统 | 核心 | 状态 |
|---|---|---|---|---|---|---|
| （Lakka） | 192.168.1.61 | Nintendo Switch OLED（Mariko） | 6 核 2.295G / 3.9GB | Lakka / L4T 4.9.140 | 0.3.3 `lakka-arm64` | 关机 |
| `r36s`（原） | 192.168.1.81 | 初代 R36S | RK3326 1.30G | Debian | 0.3.3 | **送修** |

- Switch 另有独立的 `nx-v*` 线（Horizon 静态库 + NRO），挂起等上游，产物同为 0.3.3。
- Lakka 的核心在 `/storage/cores/`，卡带在 `/flash/roms/pxt64_collections/`（20 张）。

## 3. Android 掌机（3 台）

| 别名 | adb id | 设备 | 系统 | 屏幕 | 核心 | 用途 |
|---|---|---|---|---|---|---|
| G Cloud | `2429TNR40628` | 罗技 G Cloud `GR0006` | Android 11 / SDK 30 | 1080×1920 @320 | 0.3.5 | 无 root，RA 里能跑；shell 宿主必 906 |
| `rtwo` | `ZY22GCKKB4` | Motorola XT2301-5（骁龙 8 Gen 2） | **Android 16 / SDK 36** | — | 0.3.5 | RA 1.21.0 是 `targetSdk=28` 才跑得起来 |
| `oxygen` | `b2aec2db` | Xiaomi Mi Max 2（骁龙 625） | **Android 7.1.1 / SDK 25** | — | **0.3.8** | **常规长测机**：3.7GB / data 43GB |

合计 **14 台**：9 台 Linux 掌机 + 1 台 Switch(Lakka) + 1 台送修 + 3 台 Android。

2026-11-09 用 0.3.7 的 `android-arm64` 核心（md5 `8e3e37c383cd290a72cf34f901aa5935`，
455 136 B）在 Mi Max 2 上跑 21 张卡带 ×600 帧：**21/21 与桌面 x86_64 基线逐位一致**，
单个卡带 0–3 s。这条链路是改核心后的首选快速回归，G Cloud / Moto 那两台仍停在 0.3.5。

> 0.3.8 的 `android-arm64`（md5 `ed2a6a56f0e1a1ef410541892ba7dca7`，455 136 B）：体积与
> 0.3.7 相同，但补上了 `DF_1_NODELETE`（0.3.7 的 Android 包一直缺这个标志），md5 因此不同。
> 2026-11-10 用它在 Mi Max 2 上跑 21 卡 ×600 帧：**21/21 与桌面基线逐位一致**。

## 4. 版本登记（linux-arm64 核心）

| 版本 | 核心 MD5 | 大小 | 备注 |
|---|---|---|---|
| 0.3.8 | `eb4eb21716d77ecaabd77357b5ce1ea7` | 732 496 B | **首个 strip 版本**（原 5.0 MB）+ 独立 `-debuginfo.zip`；补 Android/musl 的 `DF_1_NODELETE`。qemu 跑 golden 三例与 0.3.7 逐位一致 |
| 0.3.7 | `a3e8b64dc602fc98de331a7a1c1dc003` | 5 007 984 B | 修 PATCH 11/12 + 退出 139（`DF_1_NODELETE`）；两机实测退出码 0 |
| 0.3.6 | — | — | 零行为变化（工程债），未在真机铺开 |
| 0.3.5 | `5e8e2b51bc1b055e4d6c1266a9f9fb90` | 5 008 656 B | libretro 契约修复 + 移除 PNG 解码 |
| 0.3.4 | `7bd2bfe0392c44fec0593107e039e157` | 5 170 520 B | — |
| 0.3.3 | `9da97c82daa676fe93bb4d61634f7dbf` | — | 7 台关机机 + Switch Lakka 仍在此版 |

## 5. 容易忽略的差异（踩过的坑）

**目录与命名**
- **`.info` 位置**：只有 muOS 把核心与 `.info` 分开放（核心 `/opt/muos/share/core`，
  `.info` `/opt/muos/share/emulator/retroarch/info/`），换核心极易漏改。其余系统两者同目录。
- **Android 的 `.info` 主文件名必须与 `.so` 完全一致**，放在 RA 的「核心信息」目录；
  换完要删同目录 `core_info.cache`，否则前端仍显示旧版本。该缓存是 8 字节 `#RZIPv1#` 头
  + zlib 压缩 JSON，可用 `zlib.decompress` 解开核对。
- 换核心后 `.bak` 必须移出核心目录，否则被前端 glob 成重复条目。

**只有 2 台常开**
- `cubexx` / `rgb30c` 是唯一两台保持联网的；其余关机或送修，要动先请用户开机。
- 2026-11-10 08:24~19:23 `cubexx`（.149）短暂关机：无 ARP、22 端口超时，同网段扫 22 端口只剩
  .1（路由）/ .69（工具机）/ .84（另一台 dropbear，root 密码不对，不是 cubexx）/
  .101（rgb30c）/ .196 —— **判设备关机或掉网，不是改 IP**。19:23 开机后已补升 0.3.8 并实测通过。

**账号与登录**
- `rgb30c` 的 SSH 用户是 **`ark`**（root 公钥未装，sudo 免密）；其余 Linux 掌机多用 `ark`。
- 换系统后 host key 变了 → 先 `ssh-keygen -R <ip>`，否则 `Host key verification failed`。
- R36S Clone 恢复镜像后 `/home/ark` 是 777，sshd `StrictModes` 拒公钥 → `chmod 755 ~`。
- 全新镜像默认 ssh disabled、家目录 700、`fluidsynth`/`mpris-proxy` 全局自启。
- Switch/Lakka 只开密码认证（root/root），Windows 下需要原生 askpass exe（`zig cc` 编 20 行
  C 程序打印 `$ASKPASS_PW`），cmd 脚本会静默失败。

**部署操作（cubexx 是 BusyBox）**
- 无 `tar -z` → 用 `gzip -dc x.tgz | tar x -C dir` 或**直接 `scp` 传文件**（2026-11-10 补升
  0.3.8 用的是 scp，最省事）；无 `stat` → 用 `wc -c`；无 `install` → 用 `cp` + `chmod`。
- 掌机 **`/tmp` 是 noexec**，可执行文件要放家目录。
- `ssh host 'bash -s' -- args` 里带空格的路径会被拆开 → 用 glob 或写死在脚本内。

**环境特性**
- 待机 `powersave` 288MHz，吞吐虚低约 6 倍；测性能前切 `performance`，测完复原。
- RK3566 的 DDR 常驻 324MHz，`echo performance > /sys/class/devfreq/dmc/governor`
  可再涨 7~14%（RK3326 只有 2~3%，重启不保留）。
- `rk2023` 是唯一非 Debian 13 的 Linux 机（Ubuntu 19.10 / glibc 2.30 / RA 1.19.1），
  兼容性上最像「外来户」，新特性优先在它上面试。
- **R36S Clone 的 DTB 必须**是 `rk3326-r36s-type2-panel1-with-amp-linux.dtb`，
  否则喇叭无声、耳机正常。

**Android**
- adb 在 `$LOCALAPPDATA/Android/Sdk/platform-tools/adb.exe`（不在 PATH）；Git Bash 里
  必须 `export MSYS_NO_PATHCONV=1`，否则远端路径被改写成 Windows 路径。
- `adb push` 覆盖已有文件会掉执行位，之后要 `chmod 755`。
- 核心目录在私有区（`/data/user/0/com.retroarch.aarch64/cores/`）无 root 写不进，
  只能走 RA 的「Install or Restore a Core」。
- **SDK < 30 才有逐位可比的哈希**：Mi Max 2（SDK 25）shell 宿主与桌面一致；
  SDK ≥ 30 的 shell 域进程必撞 TBI（top byte `0xb4`），pxt NaN-boxing 的 `isDouble`
  全误判 → `PANIC 906`，那种设备只能走 RA 或 `targetSdk=28` 的测试 APK。

## 6. 判「问题与核心无关」的门槛

必须做**同配置只换核心**的版本对照，不能只看日志停在哪。
0.3.3~0.3.5 曾在两台 muOS/Debian 机上被误判成「RA 固有 139」，
直到拿 0.3.5 与 0.3.7 对照才定位到核心侧的 static 析构 + `dlclose`
（详见 `DEVICE-TEST.md` 末节与 `MEMORY-lessons.md`）。
