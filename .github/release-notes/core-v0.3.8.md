# MakeCode Arcade core v0.3.8

## 一句话：画面一行没动，包小了一大半，符号还能找回来

这一版**没有任何行为改动**：三个基准用例的末帧哈希与 0.3.7 逐位相同。改的全是
「出厂形态」——发行包一律 strip，同时另发 `<平台>-debuginfo.zip` 保留符号，用
build-id 把两边绑在一起。顺手补上了 0.3.7 漏掉的 Android 链接选项。

**建议升级**：同样的核心，`linux-arm64` 从 5.0 MB 降到 732 KB，六个包总量
4.1 MB → 1.5 MB。对固件仓库（muOS / Knulli / ArkOS 都要求 strip 产物）和
SD 卡空间有限的手持设备来说，这一版才是「能直接收录」的形态。

## 1. 发行包 strip：linux-arm64 5 007 984 → 732 312 B

此前三个 Linux 发布件把 `.debug_info` 直接发给了玩家，而 Android 被 NDK 自动
strip、Windows 本就不带符号表——同版本六个包的状态并不统一（曾经被固件仓库
拒收，理由就是「带调试信息的核心」）。

| 平台 | strip 前 | strip 后 | zip |
|---|---|---|---|
| linux-arm64 | 5 007 984 B | **732 312 B**（−85%） | 1 380 KB → **287 KB** |
| linux-x64 | 4 621 152 B | **733 032 B**（−84%） | 1 347 KB → **299 KB** |
| lakka-arm64 | 1 956 104 B | **593 840 B**（−70%） | 731 KB → **239 KB** |
| android-arm64 | 455 136 B | 455 136 B（NDK 已 strip） | 不变 |
| windows-x64 / arm64 | 615 424 / 689 152 B | 同左（PE 无符号表） | 不变 |

strip 工具按目标架构挑：GNU 交叉 binutils 优先，其次 `llvm-strip`，最后才试
宿主 `strip`——宿主 binutils 多为单目标，遇到 aarch64 会直接报格式错误。所以
打包脚本对每个候选**实际执行**并用 `readelf -h` 验产物，不看 `which`。
发版环境（`MCA_REQUIRE_STRIP=1`）里 strip 失败直接红，不允许静默发出 5 MB 的包。

## 2. 调试包：`<平台>-debuginfo.zip` × 3

strip 掉的符号没有丢，三个 ELF 平台各多一个调试包（只有 `.so` 原件，没有
`.info`——它不是给玩家装的）。**主包与调试包靠 build-id 配对**，所以本版把
build-id 从 `none` 统一改成 `--build-id=sha1`：

```bash
# 玩家报的崩溃地址（-e 指定主包里的 .so，符号取自调试包）
aarch64-linux-gnu-addr2line -f -C -e makecode_arcade_linux-arm64-debuginfo.so 0x0001a2b4
# 先确认两边 build-id 一致（都在 .note.gnu.build-id 里）
readelf -n makecode_arcade_libretro.so | grep 'Build ID'
```

build-id 是文件内容的哈希，同样输入仍得同样结果，不影响可复现构建。SHA-1 是
20 字节固定长度，不影响产物尺寸。

## 3. 补上 0.3.7 漏掉的 Android 链接选项

0.3.7 修「退出 RetroArch 即 139」靠的是链接期 `-Wl,-z,nodelete`，当时改了
`core/Makefile` 与 `tools/build_target.sh`，但 **`core/jni/Android.mk` 是第四个
链接入口**——Android 发行包走 `ndk-build`，前三个都管不到它，于是 Android 包
仍然没有 `DF_1_NODELETE`。本版补上，并在两处设了断言防回归：

- CI 的 `android-arm64` job 编完就 `readelf -d` 查 `FLAGS_1 ... NODELETE`；
- `tools/verify_release.py` 对**所有 ELF 包**查同一个标志（Windows 是 PE，跳过）。

## 4. 验证

- **末帧哈希**：三个基准用例与 0.3.7 逐位相同（strip 不改变指令）

  ```text
  ping 300 帧     0556e78f7ce0e828f2dc922d210dc047
  probe 120 帧    e82844f7160449d1767aa69afe788800
  probe 120 帧按A 0eb7884cb40cef0cb549946af3a942ab
  ```

- **发行包体检**：`tools/verify_release.py` 6 个主包 + 3 个调试包全 PASS
  （已 strip、架构/机器类型正确、glibc ≤ 2.17、无 `libc++_shared`、
  16KB 页对齐、debug-info 与 build-id 配对）。
- **CI**：`package` job 末尾会把打好的包解开 dlopen 跑 60 帧；`golden-hash`
  守像素；`pxt-patches` 校验 11 条补丁。

## 已知限制（未变）

- 32 位前端不支持（PXT64 镜像在 32 位 VM 下无法执行）。
- 即时存档 / rewind / 快进 / 金手指 / Netplay 不提供（v0.3.0 起的范围收敛）。
- Strawberry_Slam 在固定 tick 触发 `PANIC 980`，游戏自身 bug，核心侧不修。
- Windows 的「退出 139」等价问题（PE 无 `NODELETE` 标志）未处理，取决于前端
  是否 `FreeLibrary`。
- Switch NX 是独立发布线（`nx-v*`），本版不发 NX 包。

## 升级建议

**建议升级**，尤其是给固件仓库提交/自带核心的场景：同样的画面、同样的兼容性，
包体积小 4 倍。若你从不看崩溃回溯，可以只下主包；要报 crash 就同时下对应平台
的 `-debuginfo.zip`。

---

MakeCode Arcade core 是非官方的第三方项目，与 Microsoft、Nintendo 均无关。
