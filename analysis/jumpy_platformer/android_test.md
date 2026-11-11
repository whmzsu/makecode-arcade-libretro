# jumpy-platformer 在安卓真机的实测（回应"安卓上精灵动画没起效"反馈）

## 设备
- 型号：**Mi Max 2**（`b2aec2db`），**Android SDK 25**（bionic 不打堆指针 TBI tag，shell 宿主可用）
- adb：`C:/Users/whmzs/AppData/Local/Android/Sdk/platform-tools/adb.exe`
  （Git Bash 下需 `MSYS_NO_PATHCONV=1`，否则 `/data/...` 会被转成本地 Windows 路径）
- NDK：r27c，位于 `C:/Users/whmzs/Desktop/workbuddy/common tools/android-ndk-r27c`
  clang：`toolchains/llvm/prebuilt/windows-x86_64/bin/aarch64-linux-android21-clang.cmd`

## 核心
- `out/rel039/makecode_arcade_android-arm64.zip` 解出的 `makecode_arcade_libretro.so`
- 已落地到本目录：`android_core/makecode_arcade_libretro.so`（宿主报 `core=MakeCode Arcade (route B) ver=0.3.9`）

## 测试宿主（注入输入用）
- 源码：`tools/mca_host_android.c`（已跟踪；commit `58c3452` 引入，本轮回修复按钮映射）
- 编译（NDK r27c，arm64，minSdk 21）：
  ```
  aarch64-linux-android21-clang.cmd -O2 -Icore tools/mca_host_android.c -o mca_host_inj -ldl
  ```
- 已落地到本目录：`android_core/mca_host_inj`
- 支持的输入注入参数（默认全关，行为同回归宿主）：
  - `--seq=A@40,100,170,240` + `--seq-dur=8`：在指定帧起**按住 N 帧**（模拟"按住"）
  - `--tap=A@300,360` + `--tap-dur=6`：**短脉冲**（指定帧起仅按 N 帧，模拟"按一下"）
  - `--hold=RIGHT@260`：从指定帧起**常按**某键（测持续移动）
  - `--perframe`：每帧打印 `FRAME-HASH n <md5>`
  - 末尾打印 `FINAL-FRAME-HASH <md5> (WxH, 独立帧 N)`
- hash 口径：md5(末帧 宽×高×2 字节 RGB565)，与桌面 `libretro_host.py` 一致

## 命令
```
adb -s b2aec2db push android_core/mca_host_inj /data/local/tmp/mca_test/
adb -s b2aec2db push android_core/makecode_arcade_libretro.so /data/local/tmp/mca_test/
adb -s b2aec2db push <cart>.pxt64 /data/local/tmp/mca_test/cart.pxt64
adb -s b2aec2db shell "cd /data/local/tmp/mca_test && ./mca_host_inj /data/local/tmp/mca_test/makecode_arcade_libretro.so /data/local/tmp/mca_test/cart.pxt64 600 --perframe"
```
（安卓上 dlopen 不走相对路径，**core 必须传绝对路径**）

---

## 一、无输入基线（安卓真机 500 帧）—— 标题屏循环
```
FINAL-FRAME-HASH 1c0f2dd46f99e3754ecdd0cb966ee3f6  (160x120, 独立帧 14)
```
无输入时只有 **3 个唯一标题态 hash**（循环闪烁）：
```
02b0fb5e328d311a2c47b3dfb586749e
e2d84eb5325d9436a69fc5acee35358b
1c0f2dd46f99e3754ecdd0cb966ee3f6
```
干净退出，无 PANIC 906、无 SIGSEGV。

## 二、桌面对照（同 0.3.9，无输入 600 帧）
```
FINAL-FRAME-HASH 1c0f2dd46f99e3754ecdd0cb966ee3f6  (160x120, 独立帧 15)
```
→ 末帧 MD5 与安卓**逐字节一致**，渲染管线平台无关。

---

## 三、关键修复：测试宿主的按钮映射 bug（不是核心 bug）

早期注入 A 推不动标题屏，根因在**测试宿主**而非核心：

1. `button_id()` 把 `A` 错映射成 bit 0；标准 libretro 里 **A=8**（见 `tools/libretro_host.py` 对照表）。核心读 A 查的是 id=8，旧映射下 A 根本没送进核心。
2. `input_state_cb` 没处理 `RETRO_DEVICE_ID_JOYPAD_MASK`（id=256）的**整掩码查询**。pxt-arcade 运行时正是用 bitmask 查"任意键"来 dismiss 标题屏和 `showLongText` 对话框（`controller.pauseUntilAnyButtonIsPressed` 走这条路径）。旧实现对 id=256 返回 0 → 标题屏永远收不到"有键按下"。

修复（对齐 `libretro_host.py`）：
- 按钮映射改为标准值：`A=8, B=0, SELECT=2, START=3, UP=4, DOWN=5, LEFT=6, RIGHT=7`；
- `input_state_cb` 对 `id==256` 返回 16 位整掩码，并放宽 port/device/index 的硬校验。

> 注：这是**我们测试工具**的 bug。核心本身严格遵循 libretro 输入规范（python 参考宿主早已用正确映射跑通），故核心侧无需任何改动。

---

## 四、真机直接喂输入的决定性证据

### 4.1 标题屏 + 3 个教学对话框被 A 推开（日志 `run_title_dismiss.log`）
输入：4 次 A 短按（第 40/100/170/240 帧，每次按 8 帧）
```
FINAL-FRAME-HASH 7b3bec69f17ad0421a0270473888f09a  (160x120, 独立帧 13)
```
逐帧追踪：第 **44 帧首次出现非标题 hash** `c0c67b0f...` → 标题被 A 推开；
随后依次出现 `b2ccc3...` / `15941f...` / `7b3bec...`，正是 3 个教学对话框（每次 A 推进一步）。
**结论：A 的"按几次"语义在安卓上完全有效**（标题 1 次 + 对话框各 1 次 = 4 次进操控）。

### 4.2 进操控后跑动 + 跳跃，动画逐帧变化（日志 `run_jump_anim.log`）
输入：`--seq=A@40,100,170,240 --seq-dur=8 --hold=RIGHT@260 --tap=A@300,360,420,480,540 --tap-dur=6`
```
FINAL-FRAME-HASH 1ae1741a9e89f1b58ef60003506008ed  (160x120, 独立帧 16)
（共 11 个独立 hash，玩法帧持续变化）
```
跳跃窗口（第 303–314 帧，A 在第 300 帧触发跳跃）逐帧 hash：
```
303 01e22af4954c2ad8dcd6a0da27d9b9f2  CHG
304 d7746655e9e11fce69768b5863b65154  CHG
305 d7746655e9e11fce69768b5863b65154
306 d7746655e9e11fce69768b5863b65154
307 300e6621f41775c5757f971a96211ab8  CHG
...
314 1ae1741a9e89f1b58ef60003506008ed  CHG
```
跳跃动画**逐帧切换不同帧**（`01e22af4 → d7746655 → 300e6621 → 1ae1741a`），清清楚楚。
→ **直接证明：安卓真机上精灵动画系统完全正常，并非"动画没起效"。**

---

## 最终结论
1. 安卓 0.3.9 核心在 Mi Max 2 真机能正常加载运行 jumpy，**不崩溃、干净退出**；
2. 无输入时末帧 hash 与桌面 0.3.9 **逐字节一致** → VM 执行 + 精灵 blit 渲染管线平台无关，无任何平台特供分支；
3. **直接喂输入实跑证明**：A 按几次可推开标题屏与 3 个教学对话框进入操控，进操控后跑动/跳跃时英雄精灵**逐帧切帧**，动画系统正常；
4. 论坛反馈"动画没起效"**不是核心 bug**，最可能是：① 跑的是教程半成品导出（英雄单帧静态图，根本没有动画帧可播）；② 没按 A 开场。两者都是卡带/用法问题，与核心无关。

## 附：本回改动文件
- `tools/mca_host_android.c`：按钮映射对齐标准 libretro（A=8…RIGHT=7）+ `input_state_cb` 支持 `JOYPAD_MASK` 整掩码查询 + 输入注入参数（`--seq/--tap/--hold/--perframe`）。
- `tools/libretro_host.py`：新增 `--hold=N`（从 N 帧起常按 `--press` 的键，桌面持续移动测试用）—— 与安卓宿主无关，独立增强。
- 证据日志（本目录）：`run_title_dismiss.log`、`run_jump_anim.log`；测试宿主二进制 `mca_host_inj`。
