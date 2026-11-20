# MakeCode Arcade core v0.3.11

## 一句话：统一命名 + macOS 正式进发版矩阵，画面一位没变

这一版是**发版形态**版本，没有行为改动：正常卡带的末帧哈希与 0.3.10
**逐位相同**（golden 三例 `0556e78f` / `e82844f7` / `0eb7884c`）。

## 1. 资产：12 主包（macOS 首次进 Release）+ 5 符号包 + 3 平台 zip

| 组 | 内容 |
|---|---|
| 64 位（8） | `windows-x64` `windows-arm64` `linux-x64` `linux-arm64` `macos-x64` `macos-arm64` `android-arm64` `lakka-arm64` |
| 32 位（4） | `windows-x86` `linux-x86` `linux-armhf` `android-armv7` |
| 符号包（5） | 五个 ELF 平台的 `-debuginfo.zip` |
| 平台资产（3） | `makecode_arcade_ctr.zip`（3DS）/ `makecode_arcade_vita.zip`（Vita）/ `makecode_arcade_switch.zip`（Switch） |

**macOS（`macos-x64` / `macos-arm64`）首次随 Release 发布**，且不是"编过就算"：
两个包都在 GitHub 真实 macOS runner 上 dlopen 实跑——golden 三例末帧哈希与
Linux 逐位一致、`--reinit=2` 三轮重载协商正常、`SET_PIXEL_FORMAT` 次数符合预期。

## 2. 3DS / Vita / Switch 核心文件统一命名

自本版起，平台 zip 内的核心文件从旧短名 `mca_libretro.*` 统一为
`makecode_arcade_libretro.*`（`.cia` / `.self` / `.nro` / `.info` 同步更名）。
功能无变化，但**老用户必须先装新版、再删旧文件**（顺序反了 RetroArch 会因
记录的上次核心路径缺失而打不开）：

- **Vita**：装新 `ux0:/data/retroarch/cores/` 后，删除旧 `mca_libretro.self`
  与 `mca_libretro.info`，否则 Load Core 出现重复项；若上次用的就是旧核心导致
  RA 卡死，删 `ux0:/data/retroarch/config/MakeCode Arcade/` 后重启即恢复
- **3DS**：按标题 ID 覆盖安装即可，旧的 `.cia` 建议删除

详见下方「资产怎么选」中的更名说明。

## 3. 更正 v0.3.10 的一处说法

0.3.10 说明里写过「32 位与 64 位的末帧哈希本来就不相同，不跨位宽比对」——
**这个说法不成立**，源于当时对首测时序差异的误判。真机实测：32 位设备
（android-armv7）与 64 位设备（android-arm64 / linux-arm64）跑同一张卡带
300 帧，末帧哈希完全相同。跨位宽可比反而是好事：**一套 golden 基线就能同时
守住两套指针布局**，32 位回归不再需要独立基线。

## 4. 官方收录（buildbot）准备

仓库侧为提交 libretro 官方收录做的工程准备全部落地：

- macOS 两条收录 job（真实 runner 双架构验证，转硬断言）
- 3DS / Vita 静态收录 job
- 5 个桌面 job 显式注入 platform（修正同 runner 连续编译的"对象串味假绿"）
- `buildbot-sim`：win-i686 / linux-i686 / linux-aarch64 三条官方矩阵目标在
  GitHub 侧先行模拟（独立 runner + `file` 架构断言）
- `tools/verify_github_assets.py`：核对 Release 上玩家真正下到的 zip

## 5. 核心源码：零行为改动

唯一的源码改动是分段性能计时插桩（`MCA_PROFILE`），**默认关闭、零开销**，
不影响任何发布物。发版包内 19 个补丁全部在位（`tools/check_pxt_patches.py`
校验通过）。
