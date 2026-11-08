# MakeCode Arcade core v0.3.5

## 一句话：把 libretro 契约上的三处欠账补上，再把「只认 .pxt64」贯彻到底

**游戏运行时、VM、音画输出、卡带兼容性一行都没改**——21 张语料 × 600 帧与 0.3.3/0.3.4
逐张末帧哈希全一致。这一版改的是**核心与前端之间的约定**，以及把早已废弃的 PNG 解码
链路整条删掉。

## 三处契约修复

### 1. 换卡带不再「谎报成功」

```cpp
// 0.3.4 及以前：打一条 WARN 就 return true，前端以为换好了，实际还在跑旧游戏
if (runtime_available()) {
    log_printf(RETRO_LOG_WARN, "[MCA] VM already running; reload the core...\n");
    return true;
}
// 0.3.5：先停干净，再装新镜像
if (runtime_available()) vm_stop();
```

`vm_host` 的 `vm_stop()` 本来就能把 VM 线程收干净（卸载/关闭游戏一直走这条），
文档也写着「同一进程内 unload→load 已支持」，唯独 `retro_load_game` 里那句提前返回
把这条路堵死了。修复后实测三连换卡带（ping→cubeland→jumpy→ping）每张都真正重新
装载镜像（148832 → 181608 → … 字节），各自出帧正常，最后 `dlclose` 不崩。

### 2. 像素格式协商提前，失败有回退

以前 `SET_PIXEL_FORMAT` 是在 `retro_load_game` 里才发的，失败只打一条 WARN，
渲染路径却无条件按 RGB565 写——前端按默认 0RGB1555 解读，颜色就全错。

现在：

- 协商放在 `retro_set_environment`（前端视频驱动已就绪），`load_game` 时再兜一次；
- 前端不接受 565 时，**真的按 0RGB1555 输出**（`out_color()` 统一转换），
  包括 panic 横幅和测试画面；
- 回填值也会校验（个别前端返回 true 却改格式）。

### 3. 只认 .pxt64，PNG 处理全部删除

`.info` 与 `get_system_info` 一直只声明 `pxt64`，前端按扩展名过滤，`.png` 卡带
根本到不了核心。所以核心侧那套 PNG 入口（`try_sidecar_pxt64()` 旁载、PNG→彩条分支、
`arcade_png_load_project()`）是纯废料：

```text
删除：core/src/{png_loader.cpp,h, cart.c,h, decompress.c,h}
      core/third_party/{miniz/*, lzma/*}
      core/tests/cart_smoke.cpp
      libretro_core.cpp 里的 PNG 分支与 sidecar 逻辑
```

代价从 5 处构建清单（Makefile / Android.mk / build_core.sh / build_target.sh /
build_musl.sh）一起摘掉。**aarch64 发行产物 5197360 → 5034216 字节**，源码净减
约 8700 行。非 `.pxt64` 现在只在通用镜像校验处失败（`not a valid .pxt64 image`），
核心不再为 PNG 输出任何专门提示。

顺带删掉了 `SET_SUPPORT_NO_GAME(false)` 这次无效调用（传 false 等于没声明）。

## 新增：假前端契约自检（CI 里跑两遍）

0.3.4 修的 `SET_CONTROLLER_INFO` 缺终止哨兵，是「只有干净 gcc 构型才炸」的未定义
行为——靠真机撞太贵。新增 `core/tests/contract_test.cpp`，用严格假前端把契约走一遍：

- `get_system_info`：`valid_extensions` 只能等于 `pxt64`，`need_fullpath/block_extract` 为 false；
- 像素格式必须在**首次 `load_game` 之前**协商；`--reject-rgb565` 时验证退回 0RGB1555；
- `SET_INPUT_DESCRIPTORS` / `SET_CONTROLLER_INFO` / `SET_VARIABLES` 三个数组必须能在
  界内找到终止哨兵（缺哨兵 = 越界读）；
- 喂 PNG 魔数、垃圾字节、`NULL`：`load_game` 必须返回 false；
- 失败后 `retro_run` 仍要按 16bpp 出帧（尺寸 160×120、pitch 320）。

接入方式：

```bash
make -C core platform=unix contract-test                            # buildbot 同构型（gcc/make）
make -C core platform=unix SANITIZE=undefined contract-test          # UBSan 构型，对象独立目录
```

两条都已加进 `.github/workflows/build.yml` 的 `makefile-compat` job。本地实测
gcc 13 与 `-fsanitize=undefined -fno-sanitize-recover` 下各 28 项断言全过。

## 验证

- **21 张语料 × 600 帧**：真机 0.3.3 与 0.3.5 **逐张末帧哈希全一致**，
  `ping` = `f93e4597…`、`arcade-cube-land` = `23210fb3…`；判定 ok 20 / panic 1
  （Strawberry_Slam 的 `PANIC 980` 仍是已知游戏自身 bug）。
- **架构等价**：x86_64 与 aarch64 产物 `ping` 末帧哈希同为 `f93e4597…`。
- **换卡带**：三连切换每张都重新装载镜像并出帧，`unload → dlclose` 正常退出。
- **发版矩阵**：5 个 zig 目标（win-x64/win-arm64/linux-x64/linux-arm64/android-arm64）
  + 4 个发行版工具链构型（gcc x64/arm64、MinGW、ndk-build）全绿。
- **Lakka/musl 全静态**：musl.cc `aarch64-linux-musl-cross`（GCC 11.2.1）编出零 `NEEDED`
  的 1956064 字节 DSO，与 glibc 版 **21 张语料逐张末帧哈希完全一致**（`ping` = `f93e4597…`）。
- **发行包体检**：6 个包逐个体检 PASS——架构、`.info` 版本与源码版本一致、
  `NEEDED` 白名单（lakka 包为 0）、glibc 2.17 基线、25 个 `retro_*` 导出、包内无 `.pdb`。

## 已知限制（未变）

- 32 位前端不支持（PXT64 镜像在 32 位 VM 下无法执行）。
- 即时存档 / rewind / 快进 / 金手指 / Netplay 不提供（v0.3.0 起的范围收敛）。
- Strawberry_Slam 在固定 tick 触发 `PANIC 980`，游戏自身 bug，核心侧不修。
- Switch NX 是独立发布线（`nx-v*`），本版不发 NX 包。

## 升级建议

行为与 0.3.4 完全相同（哈希一致），所以**不是必须升级**；但从 0.3.5 起：

- 换卡带（不重载 core 直接 load 第二张）才真正可用；
- 拒绝 RGB565 的前端不会再出现配色错误；
- 自己用 gcc/make 从源码编的用户，产物小了约 160 KB、少 5 个编译单元。

---

MakeCode Arcade core 是非官方的第三方项目，与 Microsoft、Nintendo 均无关。
