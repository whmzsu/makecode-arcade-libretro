# MCA Switch（RetroArch NX）构建与真机验收

> 本文档覆盖 Switch 的**产物形态、CI 发版路径、tool69 本地构建、安装与真机验收**。
> 玩家向安装步骤见 [玩家指南](玩家指南.md) / [Player Guide](Player-Guide.md)；
> 3DS/Vita 见 [3DS/Vita 真机测试与验收](3DS-VITA-TEST.md)，交叉编译通用坑见
> [CROSS-COMPILE.md](CROSS-COMPILE.md)。正式玩家请从
> [Releases](https://github.com/whmzsu/makecode-arcade-libretro/releases)
> 下载 `makecode_arcade_switch.zip`，本文的本地产物不随仓库分发。

## 一、产物形态：为什么是 NRO 而不是 `.so`

Switch **没有 `dlopen`**，核心不能像桌面那样丢一个 `.so` 进 `cores/` 目录。
这里的出厂形态是**静态库** `makecode_arcade_libretro_libnx.a`（devkitA64 +
`-fPIE`），由 RetroArch 的 `Makefile.libnx` 连同前端一起链成 NRO：

- **一个 NRO 内含且仅含一个核心**，这正是官方在线更新器分发核心的形态；
- `tools/package_release.sh` **不参与**这条线（它只打标准 libretro core 包）；
- `.info` 可选、放同一目录即可；更新核心 = 换掉这个 NRO，`retroarch.cfg` 保留。

归档有个坑：GNU `ar` 只存基名，而 `runtime/pxtapp` 里 `base/control.cpp` 与
`core---vm/control.cpp` 同名，直接归档会让后者静默覆盖前者（符号到链接期才报
缺失）。Makefile 先把对象拷成扁平唯一名再归档，并对成员数/对象数做断言。

## 二、CI 发版路径（三个触发方式，别混）

真实分工以 `.github/workflows/build.yml` 为准（`build.yml:1216-1255`）：

| 触发 | 行为 |
|---|---|
| 推 `core-v*` tag | `build.yml` 通过 `workflow_call` 调用 `nx.yml`（job `switch-core`），产物 artifact `nx-switch-nro` 交回 `build.yml` 的 `release` job，**与桌面/移动包一次 `gh release create` 上传全部 20 个 zip**；nx.yml 自己的 `core-release-upload` 在 `workflow_call` 下自动跳过 |
| 推 `nx-v*` tag | nx.yml 的 `release` job 单独建 **NRO-only Release**（版本号与核心同步，如 `nx-v0.3.3` ↔ `core-v0.3.3`），排障用 |
| 手动 dispatch | 只跑回归、产物挂 workflow artifact；指定 `upload_tag` 可排障补传（`core-release-upload`） |

被调 workflow 的上传 job 在 `workflow_call` 下显式排除，避免两条流水线各自
轮询同一个 Release 造成竞态与空等。

`nx.yml` 的两级构建：

```yaml
core-libnx: devkitpro/devkita64 容器 → make -C core platform=libnx
            校验 .a 架构 + retro_* 全局符号
nro:        拉 RetroArch v1.22.2 + dkp-pacman 装 portlibs（ffmpeg/vpx/opus/
            png/zlib/bzip2/lzma/expat/mesa/libdrm_nouveau）→ make -f Makefile.libnx
```

## 三、本地构建（tool69，推荐）

先 `source /opt/mca/env.sh`。相关路径：

| 变量 | 路径 | 用途 |
|---|---|---|
| `MCA_SRC` | `/opt/mca/src` | 唯一源码树（从此刷新工作区） |
| `MCA_BUILD_WORKSPACE/nx` | `work/build/workspace/nx` | NX 构建工作区（脚本自动 rsync） |
| `MCA_TOOLCHAIN/relay/extract/nx/devkitpro` | 工具链 | devkitA64 / `aarch64-none-elf-*` |
| `MCA_TOOLCHAIN/relay/retroarch-nx-v1.22.2` | RA_NX 树 | pin 的 RetroArch NX 源码（**不是** 3DS/Vita 用的 `relay/retroarch`） |
| `MCA_RELEASE_LOCAL/<tag>` | `work/release/local/<tag>` | 正式产物落地目录 |
| `MCA_BUILD_JOBS` | 默认 `6` | 并行度，留 2 核给系统；可临时 `MCA_BUILD_JOBS=4` 降载 |

一条命令完成三套前端整包（3DS CIA / Vita SELF / Switch NRO）：

```bash
"$MCA_BIN/build_frontend_bundles.sh" core-v0.3.11
```

脚本对 Switch 依次做：

1. `rsync` 刷新 `build/workspace/nx`，`export DEVKITPRO=.../extract/nx/devkitpro`；
2. `make -C $MCA_BUILD_WORKSPACE/nx/core platform=libnx -j"$JOBS"` → `.a`；
3. 把 `.a` 拷进 RA_NX 树改名 `libretro_libnx.a`，`make -f Makefile.libnx clean`
   后 `make -f Makefile.libnx -j"$JOBS"`（**先 clean 再链**，旧 `.o` 带过期宏）；
4. 断言：`aarch64-none-elf-nm retroarch_switch.elf` 中
   `retro_load_game` / `pxt_vm_start_buffer` / `mca_tick_init` 三个 T 符号必须齐
   （3/3，少一个即失败）；NRO 大小 > 1,000,000 B；
5. 打包为 `makecode_arcade_switch.zip`，内含
   `core/makecode_arcade_libretro_libnx.nro` + `makecode_arcade_libretro.info`
   + `安装说明.txt`（同源 `tools/nx-install-notes.txt`）。

日志落 `$MCA_REPORTS/frontend-build-<时间戳>/build.log`，分阶段计时。
本地完整发版 = `package_release.sh` 的 12 主包 + 5 debuginfo，再加
CIA / SELF / NRO 三个前端整包，共 **20 个 zip**，与 CI 数量对齐。

0.3.11 本地实测（`-j6`）：NRO 16,627,110 B，md5 `86106ddb…`；三包 `.info`
哈希全同（三平台共用一份模板）。`-j6` 的 NRO 阶段约 77s，相比 `-j2` 的
131s 提速 1.7×；与 `-j2` 产物逐位一致。

## 四、安装与真机验收

1. 把 `makecode_arcade_libretro_libnx.nro` 拷到 SD 卡 **`sdmc:/switch/`**
   （是 `switch/`，**不是** `retroarch/cores/`）；`.info` 可选，同目录即可。
2. 卡带 `.pxt64` 随便放（示例 `sdmc:/roms/makecode/`）。
3. **按住 R 点任意已装游戏**进 hbmenu → 选这个 NRO（title takeover，拿完整内存）。
   不要从相册图标进——applet 模式内存只有几百 MB，前端不一定撑得住。
   hbmenu 里按 R+方向键切到「应用模式」启动效果相同。
4. 游戏内 Start = 菜单，Select = 重启；方向/ABXY 可在
   Quick Menu → Controls 改。
5. 验收：probe 卡带逐键变色（A→红、B→黄、十字→绿橙蓝淡蓝、START→青、
   SELECT→回白底＝重启成功）；`arcade-ping` 画面正常即基本通过；
   `arcade-jumpy-platformer` / `Mouse_Adventure` 看帧率。
6. 取证：trace 日志在 RA 存档目录下的 `mca_trace.log`。

## 五、已知边界与故障取证

> **fw21/22 前端崩溃（上游未修复，与核心无关）**：固件 21/22 + 大气层
> 1.11~1.12 下，title takeover 模式加载**任何核心**都会崩——crash report 指向
> mesa/nouveau 的 `nv50_ir` 着色器编译器读野指针；官方 1.21.0 / 1.22.2 裸前端
> 同样崩。上游 [libretro/RetroArch#18905](https://github.com/libretro/RetroArch/issues/18905)。
> applet 模式下 NRO 跑在 hbloader 进程里，RA 1.21.0 一启动连累整机重启。
> 核心从未出现在崩溃栈上（VM 装载、GC、主循环均正常）。固件 20 及以下不受
> 影响；等上游修复前，fw21/22 请先降级固件或换设备。

- 只认 `.pxt64`；玩家侧用 `tools/png2pxt64.py` 自己转。
- 只做基本功能；不做存档/加载、快进快退、核心选项、金手指、Netplay。
- 启动诊断与符号定位素材在 `tools/nx-probe/`（含 `diag-notes.md`）。
- NX 线当前挂起在上述上游前端 bug，核心本身可正常构建；不要反复重估
  "AMS 版本错配"等已被推翻的结论。

## 六、相关文件

| 文件 | 用途 |
|---|---|
| `.github/workflows/nx.yml` | CI 两级构建 + `nx-v*` 独立发版 + 手动补传 |
| `.github/workflows/build.yml` | `core-v*` 统一调用与 20 包一次上传 |
| `tools/tool69/build_frontend_bundles.sh` | tool69 本地三套前端整包构建 |
| `tools/nx-install-notes.txt` | 随包分发的安装说明原文 |
| `tools/nx-probe/` | 启动诊断与符号定位 |
| `docs/3DS-VITA-TEST.md` | 3DS/Vita 的同类构建与验收文档 |
| `docs/TOOLHOST-README.md` | 工具机目录布局与 `$MCA_*` 变量定义 |
