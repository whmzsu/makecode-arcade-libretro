# MakeCode Arcade core v0.3.6

## 一句话：零行为变化 —— 这一版只动构建与验证的基础设施

游戏运行时、VM、音画输出、卡带兼容性**一行都没改**。三张基准卡带的末帧哈希与 0.3.5
逐位相同，`contract_test` 两种构型全过。改的是「源码清单有几份」「CI 验到哪一层」
「NX 诊断的取证链要不要一直自动跑」这三件工程债。

**如果你的 0.3.5 用着正常，不需要升级。**

## 1. 源文件清单收敛到一处

以前 5 个构建入口各自列一遍要编哪些源文件：

```text
core/Makefile · core/jni/Android.mk · tools/build_core.sh
tools/build_target.sh · tools/build_musl.sh
```

v0.3.5 删 PNG 解码链路那轮就漏改了两处，靠 CI 链接失败才发现。反方向更糟：漏列一个源
不会报错，只会静默少符号，等到真机某个功能不响应才回头查。

现在唯一真源是 `core/sources.mk`（三行「名字 := 空格分隔值」），shell 侧由
`tools/lib/sources.sh` 用 sed 读同一份文件。顺带把链接时的 `"$OBJV"/*.o` 换成由清单
推出的对象数组 —— 老构建目录里躺着的 `cart.o` / `png_loader.o` / `LzmaDec.o` 不会
再被 glob 静默链进产物。

## 2. CI 补两处「没人验过」的洞

### 末帧哈希回归（`golden-hash` job）

契约测试只能断言「核心有没有按 libretro 规范说话」，抓不住「契约全绿但画面画错了」。
新增 `tools/ci_regress.sh`：跑卡带 → 比 `FINAL-FRAME-HASH` → 不一致就 exit 1。三个用例：

| 用例 | 验的是 |
|---|---|
| `ping` 300 帧，无输入 | 静态场景 + 定时器路径 |
| `probe` 120 帧，无输入 | 纯色渲染基线 |
| `probe` 120 帧，按住 A | RetroPad A → MakeCode A 的按键映射 |

`probe.pxt64` 由仓库内 `tools/make_probe_cartridge.py` 现场生成，自有卡带，无版权问题。
基线在 `tools/fixtures/golden-frames.txt`，`--record` 重生成时只替换正文、保留注释头；
`--bmp-dir` 落末帧，失败时 CI 会把三张 BMP 当 artifact 传上来，方便直接看「变成什么样」。

### 发行包自验（`package` job 末尾新增一步）

以前打包完就上传，**玩家下载到的那个 zip 没人跑过**——v0.3.1 的 Windows 包里只有
`.pdb` 没有 `.dll` 就是这么漏出去的。现在打包后会解包、`dlopen`、`load_game`、跑 60 帧，
真正装不上就红在发版之前。

### pxt 补丁校验（`pxt-patches` job）

vendored 的 pxt 树里 9 个必要补丁（`MCA PATCH 1/2/3/5~10`）以前只靠
`docs/M3-VM-HOST.md` 的人工步骤重放，换机器、重 clone 后没有任何东西能确认它们还在。
新增 `tools/check_pxt_patches.py` 表驱动校验：每条补丁声明「哪个文件 + 什么正则」，
失配时报出编号与后果；`--strict` 还会抓树上未登记的补丁编号。

## 3. NX 诊断收窄

- **实现拆出**：129 行开机 trace 从 `libretro_core.cpp` 搬到 `core/src/nx_diag.{h,cpp}`，
  接口层只剩 10 处 `MCA_NX_BOOT(...)`，该文件内已无 `__SWITCH__`。非 Switch 平台编成
  空 TU（零符号），所以能直接列进清单、不用加平台条件。
- **自动触发取消**：`nx-diag.yml` 只保留 `workflow_dispatch`。原先每次 push 命中
  `core/src/**`、`runtime/**`、`core/Makefile` 就自动跑两个 job（装 portlibs + 克隆
  RetroArch + 编 4 个 NRO，十几分钟 ×2）并覆盖预发布页。NX 线是**挂起**在等上游
  [libretro/RetroArch#18905](https://github.com/libretro/RetroArch/issues/18905)
  （根因在系统侧 mesa `nv50_ir`，核心从未出现在任何崩溃栈上），恢复前不会有新证据。
- **删掉启动时的 `__attribute__((constructor))`**：它唯一的作用是证明「本文件的目标文件
  真被链进 NRO」，而这件事 `nx.yml` 已用符号断言回答（`retro_load_game` /
  `pxt_vm_start_buffer` / `mca_tick_init` 必须在 NRO 的 ELF 里）。正式 NX 包因此少一次
  每次启动写 SD 卡。

## 4. 文档漂移

- `tools/nx-probe/diag-notes.md` 前后两段结论互相矛盾：开头说「根因 = Atmosphère 1.11.1
  版本错配，升级 1.12.1 即可」，文末说「机器已是 1.12.1，仍崩，根因是 mesa」。后者才对
  （升级后复测同症状）。已删掉被推翻的那段，降级成一条「已作废的中间判断」留作教训 ——
  它刚好说明**崩溃签名逐位一致 ≠ 找到根因**。
- `docs/M3-VM-HOST.md` 修掉两处：`.png` 同名 `.pxt64` 自动旁载（v0.3.5 已删）、
  「9 个补丁固化进 build_vmapp.sh」（早就不对）。

## 验证

- **5 个构建入口逐个实跑**（工具机 gcc 13.3 / Ubuntu 24.04 / NDK r27）：`make
  platform=unix`、`ndk-build -j4`（37 个 `.o` → 623496 B）、`build_core.sh`、
  `build_musl.sh`（零 `NEEDED`、25 个 `retro_*`）、`build_target.sh`
  → `aarch64-linux-gnu.2.17` = **5034672 字节**（0.3.5 是 5034216，**+456 字节**，
  是空 TU 的段/对齐差，不是代码变化）。`libnx` / `win64` 分支用 `make -n` 干跑确认
  清单都含 `nx_diag.cpp`。
- **跨构型像素等价**：三个基准用例在 zig/x86_64-windows、gcc13/x86_64-linux、
  zig2.17/aarch64-linux（qemu chroot，用新产物跑的）三处**逐位相同**：

  ```text
  ping 300 帧     0556e78f7ce0e828f2dc922d210dc047
  probe 120 帧    e82844f7160449d1767aa69afe788800
  probe 120 帧按A 0eb7884cb40cef0cb549946af3a942ab
  ```

- **契约自检**：`contract_test` 常规与 `--reject-rgb565` 两种构型各 27 项断言全过。

## 勘误（0.3.5 说明）

0.3.5 的说明里写「各 28 项断言全过」，实为 **27 项**（按 `contract_test` 实际输出的
`ok` 行计，两种构型都是 27）。数字写错，结论不变：全部通过。

## 已知限制（未变）

- 32 位前端不支持（PXT64 镜像在 32 位 VM 下无法执行）。
- 即时存档 / rewind / 快进 / 金手指 / Netplay 不提供（v0.3.0 起的范围收敛）。
- Strawberry_Slam 在固定 tick 触发 `PANIC 980`，游戏自身 bug，核心侧不修。
- Switch NX 是独立发布线（`nx-v*`），本版不发 NX 包。

## 升级建议

**不是必须升级** —— 行为与 0.3.5 完全相同（末帧哈希逐位一致）。只有自己用
make / gcc / ndk-build 从源码编的用户会看到区别：清单少了一处维护点，产物多 456 字节。

---

MakeCode Arcade core 是非官方的第三方项目，与 Microsoft、Nintendo 均无关。
