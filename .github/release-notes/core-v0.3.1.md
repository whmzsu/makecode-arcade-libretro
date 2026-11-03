# MakeCode Arcade core v0.3.1

## 只支持 64 位前端（本版最重要的一条）

**32 位目标已整体移除**：不再构建、不再发布。本版起：

- 发行包只出 64 位：Windows x64/ARM64、Linux x64/ARM64、Android ARM64
  （共 5 个包）。`linux-armhf` / `android-armv7` **不再出现**。
- GitHub Actions 与 buildbot 列表里的 32 位 job 全部删除，
  `core/jni/Application.mk` 只列 `arm64-v8a`，本地 `tools/build_matrix.sh`
  的 32 位条目（armhf / armv7 / x86 / x32 / riscv32 / armeb / i686-android）
  一并清掉。
- 核心侧**不加任何运行时判断**：既然不再产出 32 位构建，代码里就不留
  针对它的分支。

### 为什么（不是"慢一点"，是根本跑不起来）

卡带里装的是 **PXT64** 字节码镜像。官方 pxt 编译器（`pxt-core` 的 arcade
target）**没有产出 32 位镜像的开关**——它唯一能适配 32 位目标的地方，是把
镜像就地补丁成目标原生布局，而这个函数
（`runtime/pxtapp/core---vm/verify.cpp` 的 `vm_patch_image`）的 `PXT32` 分支
至今是上游留下的：

```c
#ifdef PXT32
    if (sect.type == SectionType::NumberBoxes) {
        return (state->error = "TODO: NumberBoxes");   // 从未实现
    }
#endif
```

调用方 `vmStartCore` 还不检查返回值，于是镜像原样装载 → boxed number 的
vtable 是垃圾 → 校验报 `1064` → VM 线程几毫秒就退出。

真机实测（RK3566 / RK3326 各一台，`retroarch32` + armhf 核心）：前端能列出
核心、能加载、日志还打 `VM started`，但 trace 是 `vm ready (online=0 live=0)`，
画面全黑。

补上游这个坑不在本项目范围内：PXT64 → 32 位原生布局的转换是编译器侧的
职责，核心侧硬补等于自造一套 ABI；而且收益近零——两台真机的 64 位前端
都跑得动。**除非上游把 `vm_patch_image` 的 `TODO: NumberBoxes` 补完，
32 位前端一律不支持。**

## 其它变化

- `.info` 的 `notes` 加上 64 位声明——buildbot 收录后，玩家在 RetroArch 的
  Core Downloader 里就能看到，不用等装完才发现。
- README 新增「只支持 64 位前端」一节（技术原因 + 真机现象 + 处置方式），
  `docs/DEVICE-TEST.md` 补上 2026-11-03 两台真机的全量验收记录。

## 验证

- 本地（Windows x64，zig）：全量重编通过；ping 冒烟 300 帧、按键回归 16/16、
  生命周期回归（重置 / 换游戏 / dlclose / panic 自愈）全过。
- CI：64 位编译矩阵 + miniretro 冒烟 + makefile-compat 三平台（buildbot 同路径）。
- 真机：RK3566 与 RK3326 各一台，v0.3.0 功能全绿（性能数据见 README /
  DEVICE-TEST）。

## 升级建议

直接换 64 位包即可，无需改动卡带或配置。旧 `.pxt64` 完全兼容。

---

MakeCode Arcade core 是非官方的第三方项目，与 Microsoft 无关。
