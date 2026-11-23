# MCA 3DS / Vita 真机测试与验收（核心形态）

> 本文档描述 3DS / Vita 核心形态的安装、真机验收与本地构建流程。
> 文中提到的「真机测试包」（cia/self/info/测试卡带）是本地工作目录的产物，
> **不随仓库分发**；正式玩家请直接从 [Releases](https://github.com/whmzsu/makecode-arcade-libretro/releases)
> 下载 `makecode_arcade_ctr.zip` / `makecode_arcade_vita.zip`。
> 玩家向安装步骤见 [玩家指南](玩家指南.md) / [Player Guide](Player-Guide.md)。

> 这里只提供**核心形态**：core 静态链进 RetroArch 完整前端的安装文件
> （3DS `.cia` / Vita `.self`）加一份 `.info`。极简前端与核心 VPK 线已放下，
> 不再分发。
>
> 源码分支 `probe/pxt32-3ds-vita` 已并入 main（`31d9969`）。测试包一律
> **tool69 本地编译**（`$MCA_BIN/build_frontend_bundles.sh`，见下文「构建」）；
> GitHub CI（`3ds-vita.yml`）是回归与发版路径——推 `core-v*` tag 时由
> `build.yml` 通过 `workflow_call` 调用并把产物交回 `release` job 统一上传。
> 平时 push / 手动 dispatch 只出 workflow artifact。Switch 的本地构建与
> 真机边界见 [Switch 构建与验收](SWITCH-BUILD.md)。
>
> **真机结论**：3DS 核心形态在 new3ds 验收通过，按键/菜单/卡带装载全绿，
> 画面性能明显优于早先的 CPU 缩放前端；老款 3DS 手头无真机，但在 new3ds
> 上关闭 RA 电源设置中的 New 3DS clock/cache 模拟 old3ds 后，jumpy 明显
> 卡顿——old3ds 仅 268MHz、无 L2 cache，属硬件算力下限，同份 CIA 在
> new3ds 上正常，不是构建或代码缺陷。old3ds 只保证能跑、不保证流畅。
> Vita 核心形态此前 `self + info` 并入已装 RA 时报 `C2-12828-1`，已定位为
> 核心 `mcaTrace()` 缓存 `FILE*` 在 RA 换 content 后悬垂（PATCH 16，v0.3.9
> 修复），**真机复验通过**（0.3.10 连续开关内容颜色与稳定性均正常）。
> GPU 硬缩放实验因真机黑屏已放弃，源码已回退封存。

## 测试包内容（本地产物，供对照 md5）

| 文件 | 说明 |
|---|---|
| `3DS/1-安装/makecode_arcade_libretro.cia` | **3DS 核心文件**：RA 完整前端 + mca core 静态链接，0.3.11 本地构建 5 447 104 B（md5 `a061403f…`，UniqueId `0xF1CA1`），FBI 安装 |
| `3DS/1-安装/makecode_arcade_libretro.info` | 核心元数据，给 RA 的 Load Core 列表 / `.pxt64` 关联用（3DS/Vita 通用） |
| `Vita/1-安装/makecode_arcade_libretro.self` | **Vita 核心文件**：fSELF，0.3.11 本地构建 4 684 144 B（md5 `f76f4f6f…`），并入官方 RA 的 app 目录 |
| `Vita/1-安装/makecode_arcade_libretro.info` | 同上，放 RA 的 `info/` 目录 |
| `*/2-卡带/*.pxt64` | 4 张测试卡带（两平台各一份）：probe / ping / jumpy / Mouse_Adventure |

## 3DS 安装（先装官方 RetroArch）

1. 先装官方 3DS RetroArch（官网 / buildbot 的 `.cia`，FBI 安装），确认能进主界面。
2. 把 4 张 `.pxt64` 卡带拷到 SD 卡**任意你找得到的位置**（比如新建
   `sdmc:/mca/` 放进去——示例路径，不是必须）。
3. 把 `makecode_arcade_libretro.cia` 复制到 **`sdmc:/retroarch/cores/`**，把
   `makecode_arcade_libretro.info` 放到 **`sdmc:/retroarch/cores/info/`**，重启 RA。
4. **Load Core → MakeCode Arcade → Load Content → 浏览到卡带位置选 `.pxt64`** 开玩。

> 也可直接用 FBI 装 `makecode_arcade_libretro.cia` 得到独立「MakeCode Arcade」图标
> （UniqueId `0xF1CA1`，内含 RA），点开即玩——两种方式都行，推荐统一在官方
> RA 里切核心。new3ds 已实测通过。

## Vita 安装（先装官方 RetroArch）

Vita 走同一条路：**官方 RA + 并入核心**（静态模型下换核即 exec 重启进对应
SELF，无动态加载）。

1. 先装官方 Vita RetroArch（`RETROVITA.vpk`，VitaShell 安装），确认能进主界面。
2. 把 4 张 `.pxt64` 卡带放到**任意可写位置**（示例：新建 `ux0:/data/mca/`）。
3. VitaShell 开权限后，把 `makecode_arcade_libretro.self` 复制到官方 RA 的 app
   目录（如 `ux0:/app/RETROVITA/`）。
4. 把 `makecode_arcade_libretro.info` 放到同目录的 `info/` 下。
5. 启动 RA → Load Core → MakeCode Arcade → Load Content 浏览到卡带位置选 `.pxt64`。
6. **把显示比例设为 4:3**（Vita 必须做）：Settings → Video → Scaling →
   Aspect Ratio 选 `4:3`；再进游戏，Quick Menu → Overrides →
   Save Core Overrides，让这条只对本核心生效。

> **为什么 Vita 要手动设比例**：这版 RA 的 `Core provided` 不采纳核心报的
> 4:3，会横向拉满 960×544，人物变形。同一颗核心在 3DS 与桌面 RA 1.18 上
> 都正常（桌面实测 `Geometry: 160x120, Aspect: 1.333` → 按 480×360 出图）。
> **真机交叉验证：这台 Vita 上换成其他核心、同样选 `Core provided`，比例一样
> 拉满全屏** —— 与本核心无关，是前端该版对 `Core provided` 的实现缺陷。
> 所以核心侧修不了、也不该改：
> `aspect_ratio` 报 `0.0f` 与报 `4.0f/3.0f` 在 RA 里走同一段换算，
> `SET_GEOMETRY` 也只在值发生变化时才重算 viewport。
>
> 该 self 自带我们构建的 RA 前端（tip `d7cc9bc`），与官方 stable 主包
> 版本号可能不同——静态模型下不影响运行。
>
> 若加载卡带后立刻报 `C2-12828-1`，那是 PATCH 16 之前的行为（每局第一帧
> 在 `pthread_mutex_unlock` 里读地址 `0xc`）。请用当前测试包复验，并把
> 结果（含 psp2core dump，如有）反馈回来。

## 验收要点

- probe 卡带逐键变色：A→红、B→黄、十字→绿橙蓝淡蓝、START→青、
  SELECT→回白底（重启成功）。3DS 用 A/B/十字/START/SELECT；Vita 用
  CROSS/CIRCLE/十字/START/SELECT。
- `arcade-ping` 画面正常即基本通过；`arcade-jumpy-platformer`、
  `Mouse_Adventure`（最重）看帧率。
- 出问题取证：trace 日志写在 **RA 的存档目录**（「设置 → 目录 → 存档」里
  看实际路径）下的 `mca_trace.log`，拷回来；Vita 崩溃会在 `ux0:` 生成
  `psp2core-*.psp2dmp`，一并带回。

## 构建（本地优先，GitHub 只兜底）

核心形态 = RA 完整前端 + core 静态库，统一在 tool69 本地编（先 `source /opt/mca/env.sh`）：
源码 `$MCA_BUILD_WORKSPACE/{ctr,vita}`，RA 树
`$MCA_TOOLCHAIN/relay/retroarch`（pin `d7cc9bc`）。
工具链 `DEVKITPRO=$MCA_TOOLCHAIN/relay/extract/ctr/devkitpro`、
`VITASDK=$MCA_TOOLCHAIN/relay/extract/vita/vitasdk`。并行度取
`$MCA_BUILD_JOBS`（默认 6，留 2 核给系统；临时降载用
`MCA_BUILD_JOBS=4 build_frontend_bundles.sh ...`）。

先编 core 静态库，再把 `.a` 拷进 RA 树改名 `libretro_ctr.a` /
`libretro_vita.a`，**先 clean 再链**（旧 `.o` 带过期特性宏会报
`preempt_init`/`rvz_*`/`sceNet*` 一堆 undefined）。CIA 素材用
`core/assets/ctr/` 下的 icon/banner/wav：

```
JOBS="${MCA_BUILD_JOBS:-6}"
# 3DS core
export DEVKITPRO=$MCA_TOOLCHAIN/relay/extract/ctr/devkitpro
export DEVKITARM=$DEVKITPRO/devkitARM
export PATH=$DEVKITARM/bin:$DEVKITPRO/tools/bin:$PATH
make -C $MCA_BUILD_WORKSPACE/ctr/core platform=ctr -j"$JOBS"
# 拷 makecode_arcade_libretro_ctr.a → RA 树 libretro_ctr.a
cd $MCA_TOOLCHAIN/relay/retroarch
make -f Makefile.ctr clean
make -f Makefile.ctr -j"$JOBS" HAVE_RETRONFS=0 HAVE_CHEEVOS_RVZ=0
make -f Makefile.ctr retroarch_3ds.cia HAVE_RETRONFS=0 HAVE_CHEEVOS_RVZ=0 \
  APP_TITLE="MakeCode Arcade" APP_PRODUCT_CODE="CTR-P-MCA2" \
  APP_UNIQUE_ID=0xF1CA1 APP_ICON=core/assets/ctr/icon48.png \
  APP_BANNER=core/assets/ctr/banner256.png APP_AUDIO=core/assets/ctr/banner.wav
cp retroarch_3ds.cia makecode_arcade_libretro.cia

# Vita core（VPK 线已放下，只出 self + info）
export VITASDK=$MCA_TOOLCHAIN/relay/extract/vita/vitasdk
export PATH=$VITASDK/bin:$PATH
make -C $MCA_BUILD_WORKSPACE/vita/core platform=vita -j"$JOBS"
# 拷 makecode_arcade_libretro_vita.a → RA 树 libretro_vita.a
cd $MCA_TOOLCHAIN/relay/retroarch
make -f Makefile.vita clean
make -f Makefile.vita retroarch_vita.self -j"$JOBS"
cp retroarch_vita.self makecode_arcade_libretro.self
```

3DS CIA 定制身份前先删 `.bnr/.icn/.smdh/.cia`（make 按 mtime 跳过）。

上述三步已整合进 `$MCA_BIN/build_frontend_bundles.sh [tag]`：它会用 `$MCA_SRC`
刷新 `build/workspace/{ctr,vita,nx}`，依次编出 CIA / SELF / NRO，按 CI 资产名
打到 `release/local/<tag>/`。本地完整发版 = `package_release.sh` 的 12+5 标准包
再加这 3 个前端整包，共 20 个 zip。

## 运行态测试（模拟器冒烟，非真机替代）

前端整包不像标准 `.so` 那样能跑 golden frame hash。工具机上有三条兜底，
都**只证明包能被识别并进入运行态，不跑卡带、不出 frame hash、不替代真机验收**：

**自 2026-11-23 起纳入常规本地测试**：本地构建/发版流程在标准 `.so` 的
golden hash + 回归夹具之外，固定追加跑这三条冒烟（每条约 1.5 分钟），
作为前端整包的最低门槛；脚本输出与日志按约定落在 `$MCA_REPORTS/`。

| 脚本 | 平台 | 做法 | 断言 |
|---|---|---|---|
| `$MCA_BIN/vita3k_smoke.sh [tag]` | Vita | 官方 RA Vita VPK（1.22.2）装进 Vita3K，导入我们的 self + info | RA 识别 core、进 Running、无崩溃 |
| `$MCA_BIN/azahar_smoke.sh [tag]` | 3DS | Azahar（Citra 后继）`-i` 安装我们的 CIA，再启动已安装 title | title `000400000F1CA100` 加载、RA 写出 `retroarch.cfg`、无崩溃 |
| `$MCA_BIN/eden_smoke.sh [tag]` | Switch | Eden（Yuzu 系）带 prod.keys 加载我们的 NRO | `Loading RetroArch`、OpenGL shader 编译、RA 写出 `retroarch.cfg`、无崩溃 |

3DS 侧环境要点：AppImage 2126.2 放在 `$MCA_TOOLCHAIN/emulators/azahar/`；
它只带 xcb/wayland Qt 插件（无 offscreen），必须 `xvfb-run` + llvmpipe 软件渲染，
用户目录用 `XDG_DATA_HOME` 隔离。CIA 装到 `sdmc/.../title/00040000/0f1ca100/`；
RA 真启动的证据是 `sdmc/retroarch/retroarch.cfg` 落盘（不只是停在 loader）。
模拟器的 stub 未实现告警与 1×1 纹理告警属已知噪声，只记不判失败。
实测：0.3.11 CIA（5 447 104 B）安装并运行 75s，RA 配置落盘、无崩溃信号。

Switch 侧环境要点：Eden v0.2.1 AppImage 放在 `$MCA_TOOLCHAIN/emulators/eden/`；
它是 Yuzu 系，**必须有 prod.keys**（否则弹 "Derivation Components Missing" 卡死），
key 放在 `$MCA_TOOLCHAIN/emulators/keys/`（21.2.0，用户自有 dump），脚本拷进隔离
用户目录的 `eden/keys/`。无头渲染三个坑：Xvfb 下 Vulkan 无 present queue，
必须把 `qt-config.ini` 的 `backend` 两处（`backend=` 与 `backend\default=`）都改成
OpenGL；llvmpipe 只报 GL 4.5，需 `MESA_GL_VERSION_OVERRIDE=4.6` +
`MESA_GLSL_VERSION_OVERRIDE=460` 才过 Eden 的 4.6 检查。RA 真启动的证据同样是
`sdmc/retroarch/retroarch.cfg` 落盘；llvmpipe 的 shader 指令/未映射写告警属已知噪声。
实测：0.3.11 NRO（16,627,110 B）加载并运行 75s，RA 配置落盘、无崩溃。
注意这**绕过了 fw21/22 mesa `nv50_ir` 真机问题**（见 `docs/SWITCH-BUILD.md`），
模拟器通过不代表真机通过。

Vita 侧的 4:3 比例与 `C2-12828-1`、3DS 侧的 old3ds 算力、Switch 侧的
fw21/22 `nv50_ir` 都是真机专属。详细环境与证据见
`$MCA_REPORTS/vita3k-smoke-report-20261123.md`、
`$MCA_REPORTS/azahar-smoke-20261123-122355/` 及 `$MCA_REPORTS/eden-smoke-*/`。

## 已知边界

- **只认 `.pxt64`**：官方编译器只产 pxt64，PNG 不处理也不提示；玩家侧
  用 `tools/png2pxt64.py` 自己转。
- 只做基本功能；不做存档/加载、快进快退、核心选项、金手指、Netplay。
- **Vita 要手动把比例设成 4:3**（见「Vita 安装」第 6 步）：该版 RA 的
  `Core provided` 会拉满全屏（真机上其他核心同样如此，前端通病）；
  3DS 与桌面 RA 不需要这一步。
- 老款 3DS 无真机，仅用 new3ds 关 clock/cache 模拟验证过：能跑但明显卡顿
  （268MHz、无 L2 cache，硬件算力下限），只保证能跑、不保证流畅；想要流畅
  请用 new3ds 或更高性能设备。GPU 硬缩放线已放弃（真机黑屏）。
