# MakeCode Arcade core for Nintendo Switch — nx v0.3.3

Switch 线的首次独立发布。**这是 Switch 专用的 NRO，不是核心 `.so`。**

## 先说清楚它是什么

Switch 上没法像 PC / Android 那样往 `cores/` 目录丢一个 `.so`——libnx 没有
`dlopen`。所以核心必须编成静态库，和 RetroArch 前端**一起链成一个 NRO**：

```
makecode_arcade_libretro_libnx.nro   ← RetroArch 前端 + MakeCode Arcade 核心
makecode_arcade_libretro.info
安装说明.txt
```

也就是说：**一个核心对应一个 NRO，换核心等于换前端本体**。这与 RetroArch 官方
在线更新器在 Switch 上的做法一致（那边下载的 core 也是一个一个 `.nro`）。
玩别的模拟器要装别的 NRO，两者不冲突。

核心源码与 `core-v0.3.3` 的 6 个平台包**完全同一份**，游戏行为、音画、性能一致。
本包只是把链接形态换成了 `aarch64-none-elf` 静态链。

## 安装

1. 把 `makecode_arcade_libretro_libnx.nro` 与 `makecode_arcade_libretro.info`
   一起拷到 SD 卡：`sdmc:/retroarch/cores/`
2. 卡带（`*.pxt64`）放任意目录，例如 `sdmc:/roms/makecode/`
3. 在 hbmenu 里启动这个 NRO，选卡带即可游戏

**启动方式建议用 title takeover**：按住 `R` 点任意已装游戏进入 hbmenu，
这样拿到的是完整内存（约 3.2GB）。从相册（applet 模式）进也能跑，但该模式
只有 448MB——本核心 RSS 约 11MB，两种模式都够用。

按键：`Start` = 游戏内菜单，`Select` = 重启游戏。方向 / ABXY 在
Quick Menu → Controls 里改。

## 验证到哪一步（请务必读这一段）

**这个包还没有在真机上运行过。** 手上暂时没有 Switch。

已经做到的验证：

- 静态库层：成员数 == 对象数（防同名对象被 ar 覆盖）、AArch64 架构、
  `retro_*` 全局符号 ≥20 个、`retro_init` / `retro_run` / `retro_load_game` /
  `retro_set_environment` 逐个存在。
- NRO 层：链接后回头查 ELF 符号表，确认 `retro_load_game`、
  `pxt_vm_start_buffer`、`mca_tick_init` 三个**只属于本项目**的符号真的进了
  最终可执行体——链接成功不等于归档成员被引用进来了，没这一步的话核心可能
  整个被链接器丢掉而表面上"构建成功"。

但这只证明 ABI 与符号对得上，**不证明运行期正常**。最可能出事的地方是 libnx
那套精简 pthread：本核心的 VM 线程用到 `pthread_cond_timedwait` 与递归互斥量，
如果启动后黑屏或卡在加载，八成在这里。

如果遇到问题，请把现象说清楚（能不能进 RetroArch 菜单 / 加载卡带后是黑屏、
卡住还是崩溃回 hbmenu），有日志就更好。

## 与 6 平台包的关系

| 渠道 | 内容 | 适用 |
|---|---|---|
| `core-v0.3.3` Release | 6 个 `.zip`（linux / windows / android / lakka） | PC、掌机、Android、Lakka |
| 本条 Release（`nx-v0.3.3`） | 1 个 `.zip`（NRO + .info + 说明） | Nintendo Switch |

两条线版本号同步，但是**分别发布**：NRO 要拉 RetroArch 源码 + 装 portlibs，
构建十几分钟，绑在主发版流水线里会拖慢每一次核心发版。

---

非官方第三方项目，与 Microsoft、Nintendo 均无关。Nintendo Switch 是任天堂的商标。
