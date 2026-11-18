# MakeCode Arcade core v0.3.10

## 一句话：一套包打通 10 个平台，并修掉真机「关掉再开颜色就变」的顽固偏色

这一版的重点在**发版形态**和**真机稳定性**：桌面发行包从 6 个扩到 **10 个
（64 位 6 + 32 位 4）**，新增 3DS / Vita / Switch 三条平台资产，并附带 5 个
`-debuginfo` 符号包。行为侧修了 5 条真机缺陷，其中 **PATCH 20 是玩家能直接
看到的那个**——在 Vita 上「关闭内容→重新打开」后精灵颜色整体偏掉，关掉
RetroArch 再进又正常。

正常卡带画面一位没变：三个基准用例的末帧哈希与 0.3.9 **逐位相同**。

## 1. 发版形态：10 个主包 + 5 个符号包 + 3 个平台资产

| 组 | 内容 |
|---|---|
| 64 位（6） | `windows-x64` `windows-arm64` `linux-x64` `linux-arm64` `android-arm64` `lakka-arm64` |
| **32 位（4，新增）** | `windows-x86` `linux-x86` `linux-armhf` `android-armv7` |
| 符号包（5） | 五个 ELF 平台的 `-debuginfo.zip`（靠 `--build-id=sha1` 与主包配对，可 `addr2line` 直接定位） |
| 平台资产（3） | `makecode_arcade_ctr.zip`（CIA + info）、`makecode_arcade_vita.zip`（SELF + info）、`makecode_arcade_switch.zip`（NRO + info） |

**32 位是怎么跑起来的**：不是把 VM 改成 32 位指针（`PXT32` 那套 GC 高位标记
和 32 位 vtable 地址根本冲突，见 `docs/` 调查记录），而是让 **PXT64 镜像布局
跑在 32 位平台上**，GC 标记走旁路表。32 位包已实测：四个目标全部编过、PE/ELF
机器类型逐位核对、32 位宿主两轮加载末帧哈希一致。32 位与 64 位的末帧哈希
**本来就不相同**（两套指针布局各自的渲染路径），各自稳定即可，不跨位宽比对。

## 2. PATCH 20：像素格式协商结果必须随 `retro_init` 作废（真机偏色真因）

核心用 `-Wl,-z,nodelete` 链接（防「退出即 139」），副作用是 **`.so` 不随内容
卸载，所有静态变量跨会话存活**。而 `g_pixel_format_ok` 一旦协商成功就永久
短路：

1. 前端「关闭内容」会重建视频驱动，像素格式回落到 libretro 默认的
   `0RGB1555`；
2. 再「打开内容」时核心看到标志仍为 true，不再发 `SET_PIXEL_FORMAT`；
3. 于是**核心按 RGB565 写、前端按 1555 解**。

色彩数学与真机照片完全吻合：565 的绿 `0x78DC52` 按 1555 读成橙黄，棕
`0x91463D` 读成暗青绿（就是照片里的「绿头发」）；白、黑、纯红这类饱和色
偏移不明显，所以看起来像「只有个别精灵变色，而且每次都变同一套」。3DS 不
重建视频驱动，从未暴露。

现在 `retro_init` / `retro_deinit` 都清空协商结果，`retro_load_game` 重新协商。
桌面 RA 日志里一次内容加载会出现**两次** `SET_PIXEL_FORMAT: RGB565`——这就是
「不重协商就出错」的实证。

## 3. PATCH 16–19：真机链路上的四条独立加固

这四条都是**真实缺陷**，但都不是上面那个偏色的成因（各自单独验证过：装了也
不修偏色）。写在这里是为了后来者不再重复排查。

- **PATCH 16**｜Vita 每局第一帧必崩（报 `C2-12828-1`）。`WDisplay` 的
  `screenMutex` / `eventMutex` 是静态 pthread 对象，newlib 下不保证零值等价于
  `PTHREAD_MUTEX_INITIALIZER`，`unlock` 直接读地址 `0xc`。改为显式
  `pthread_mutex_init`。
- **PATCH 17**｜调色板撕裂。`setPalette` 写 16 色表与渲染线程读之间没有同步，
  一帧之内可能读到半新半旧。改为 `WDisplay` 持有 `pubPalette` 快照，写与发布
  同锁，前端只读快照。
- **PATCH 18**｜3DS / Vita 的 GC 堆注入改为幂等。一次内容加载会走两轮
  `retro_init`，原实现每轮都 mmap 一块新堆，第二轮换基址后 VM 里所有旧指针
  失效。现在只在首次注入，之后复用同一地址。
- **PATCH 19**｜注入的 GC 堆必须清零。GC 语义假设「新分配到的对象字段为 0」，
  不清零会读到上一轮残留。

## 4. 验证

- **画面未变**：`tools/ci_regress.sh` 三个基准用例与 0.3.9 基线逐位一致

  ```text
  ping 300 帧      0556e78f7ce0e828f2dc922d210dc047
  probe 120 帧     e82844f7160449d1767aa69afe788800
  probe 120 帧按A  0eb7884cb40cef0cb549946af3a942ab
  ```

- **「关闭再打开」路径**：`tools/libretro_host.py --reinit=2`（unload → deinit →
  再 init → load 同一卡带）连续三轮末帧哈希一致，这是 PATCH 20 的回归断言。
  CI 的 golden job 只做单次加载，测不到这条。
- **32 位**：`tools/reopen32.c`（32 位极简 libretro 宿主）两轮加载末帧一致。
- **契约测试**：`core/tests/contract_test` 全绿，含前端拒绝 RGB565 时的
  0RGB1555 降级路径。
- **补丁表**：`tools/check_pxt_patches.py --strict` 从 14 条扩到 **19 条**。
- **发版包体检**：`tools/verify_release.py` 是 release job 的硬门槛——10 主包
  + 5 符号包 + 3 平台资产，NODELETE 标志、PE/ELF 机器类型、Android 16 KB 页
  对齐逐项断言，FAIL 就不建 Release。

## 已知限制

- **Vita 要手动把显示比例设成 4:3**：该版 RetroArch 的 `Core provided` 会横向
  拉满 960×544。真机交叉验证过——**换成其他核心、同样选 `Core provided` 也是
  拉满**，是前端实现缺陷，与本核心无关。设置一次 + Save Core Overrides 即可，
  3DS 与桌面 RA 不需要。
- 即时存档 / rewind / 快进 / 金手指 / Netplay 不提供（v0.3.0 起的范围收敛）。
- 核心选项仍为**零**（刻意设计，见 `docs/INPUT.md`）。
- `PXT32` 指针模式（32 位 vtable 地址 + GC 高位标记）仍不可用；32 位平台走的
  是 PXT64 布局 + GC 旁路表。
- 单个 VM 段 ≥ 32000 字节的卡带无法运行（会明确报 `error 1014`，不再黑屏）。
- Windows 的「退出 139」等价问题（PE 无 `NODELETE` 标志）未处理，取决于前端
  是否 `FreeLibrary`。
- `Strawberry_Slam` 的 `PANIC 980` 是游戏脚本读未初始化数据，且依赖输入序列。

## 升级建议

**建议升级**。Vita 玩家会直接受益于 PATCH 16/18/20（不再第一帧崩溃、二次加载
不再画面错乱与偏色）；桌面玩家行为不变，但 32 位设备与 3DS / Switch 用户现在
有了官方包。

---

MakeCode Arcade core 是非官方的第三方项目，与 Microsoft、Nintendo 均无关。
