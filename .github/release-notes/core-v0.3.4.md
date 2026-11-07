# MakeCode Arcade core v0.3.4

## 一句话：只修一个「大多数设备没暴露、但确实存在」的崩溃隐患

0.3.4 与 0.3.3 之间，**游戏运行时、VM、音画输出、性能、卡带兼容性一行都没改**。
改的是核心初始化时给前端的一小段元数据：

```cpp
// 0.3.3 及以前
static struct retro_controller_info infos[1] = {};
// 0.3.4
static struct retro_controller_info infos[2] = {};
infos[1].types = nullptr;   // 补上终止哨兵
```

`RETRO_ENVIRONMENT_SET_CONTROLLER_INFO` 约定前端**按 port 顺序一直读到 `types == nullptr`
才停**，不接受额外的数组长度参数。只给一个元素，前端就会去读不存在的 `infos[1].types`
——典型的越界读，读到什么完全取决于那一刻栈上/静态区里剩下什么。

### 为什么以前没炸，为什么必须修

| 构型 | 表现 |
|---|---|
| 已发布的 zig 发行包（glibc 2.17 基线） | 那块内存恰好是 0，前端读到 NULL 就正常停下；**运气好，不是正确** |
| gcc 12 干净源码构建（Linux x86_64） | 残留是 `0x65646f43656b614d`，也就是小端字符串 `"MakeCode"`，被当指针 `strlen` → **稳定 SIGSEGV，核心加载即崩** |

这次是在新搭的常驻验证节点上，用 debootstrap 建了一个只装 `g++/make` 的干净
Debian rootfs 编源码时才撞出来的。zig、clang 与几种 ARM 设备都没暴露，说明问题不在
某个平台，而在**未定义行为对编译器布局的依赖**。换前端、换编译器、换优化等级都可能
让它从「没事」变成「必崩」；而官方 buildbot 收录用的正是 gcc/make 构型，所以这个修复
必须在收录推进前发出去。

**给前端开发者的提示**：如果你自己写 libretro 前端，请不要因为 0.3.3 能跑就把
`SET_CONTROLLER_INFO` 当成定长数组处理——规范里它就是 NULL 终止的。

### 行为确实没变（逐帧验证过）

- 21 张语料 × 600 帧：真机 0.3.3 与修复版 **逐张末帧哈希全一致**，
  `ping` = `f93e4597…`、`arcade-cube-land` = `23210fb3…`，与历史基准对齐。
  唯一异常仍是已知的 Strawberry_Slam `PANIC 980`（游戏自身 bug，跨平台一致）。
- 修复后四路构型语义等价：zig x86_64 / zig aarch64 / gcc 干净 x86_64 / Windows `.dll`，
  `ping` 末帧哈希同为 `f93e4597…`。
- `cubeland` 连跑 20000 帧，RSS 稳定在约 16 MB，22 次采样无增长。
- 真 RetroArch 前端层（宿主 1.18 与 arm64 chroot 1.14）：`.info` 被识别、核心加载、
  内容载入、持续出帧到 16 万+ tick；tick 41683 的已知 panic 后核心自动重载、宿主存活。

## 同时修掉的测试工具缺陷（不进发行包）

- `tools/libretro_host.py`：把 `retro_log_printf_t` 的第一个**变参**声明成 `c_char_p`，
  ctypes 转 Python 对象时会无条件 `strlen` 它；在 Linux x86_64 / Python 3.12 下同样会
  读到垃圾指针而崩。已改为只声明固定的两个参数（日志里的 `%s` 显示占位符，
  画面与哈希判据不受影响）。这是测试宿主的锅，不是核心的。
- `tools/sweep_carts.py`：新增 `--turbo`，批量回归跳过 27fps 节拍 sleep；默认行为不变。
  加上后 qemu-user 里 21 张卡带全量回归约 2.5 分钟。

## 验证与打包

- 发版矩阵 5 个 zig 目标 + 4 个发行版工具链构型（gcc x64/arm64、MinGW、ndk-build）全绿。
- 6 个包「装一遍再验」：架构、`.info` 版本与 tag 一致、ELF `NEEDED` 白名单、
  glibc 2.17 基线、`retro_*` 导出数、Android 16KB 段对齐、包内无 `.pdb`，全部 PASS。
- 干净 buildbot 构型单独验过：Debian bookworm + g++12 + make 编出 373 KB `.so`、
  25 个 `retro_*` 导出，依赖全在白名单内；它的 **glibc 基线是 2.34**，与发版 zig 包的
  2.17 基线不同——这正是两套 CI 分开跑、不互相替代的原因。

## 已知限制

- 32 位前端仍不支持（PXT64 镜像在 32 位 VM 下无法执行，见 v0.3.1 说明）。
- 即时存档 / 快进仍不提供（见 v0.3.0 说明）。
- Strawberry_Slam 在固定 tick 触发 `PANIC 980`，属游戏自身 bug，核心侧不修。
- **Switch NX 是独立发布线**（`nx-v*` tag），本版不发 NX 包，现有 NX 产物仍是 0.3.3。
  它编的是同一份核心源码，因此同样带着上面那个哨兵缺陷；下次 NX 发版会一并带上修复。

## 升级建议

- **自己用 gcc/make 从源码编译的用户**：建议尽快升级，旧的干净构型在 Linux x86_64 上会崩。
- **用现成发行包的用户**：0.3.3 大概率一直跑得好好的，不是紧急升级；但这是未定义行为，
  换前端或换版本后没有保证，建议顺手换掉。
- Linux / Windows / Android 的游戏表现与 0.3.3 完全相同，升级不会带来画质、帧率或
  兼容性变化。

---

MakeCode Arcade core 是非官方的第三方项目，与 Microsoft、Nintendo 均无关。
