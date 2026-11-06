# MakeCode Arcade core v0.3.3

## 这一版是两个新平台的接入，游戏本身没有任何变化

0.3.2 到 0.3.3 之间，**游戏运行时、VM、音画输出、性能、卡带兼容性一行都没改**。
这一版做的事是让同一个核心能装到两种此前装不上的系统上：

| 平台 | 本版之前 | 本版之后 |
|---|---|---|
| Lakka / LibreELEC（aarch64） | 装不上 | **新增 musl 全静态包** |
| Nintendo Switch（libnx） | 不支持 | **新增静态核心 + NRO 打包链路** |

如果你的设备是 Linux / Windows / Android，**这一版与 0.3.2 行为完全一致，没有升级理由**。
已经能跑的设备不必动。

## Lakka / LibreELEC：为什么必须静态链接

Lakka 与 LibreELEC 用的是 **musl libc**，而此前发布的 `.so` 全部是 glibc 动态链接的——
放到 Lakka 上 RetroArch 会直接报 "无法加载核心"，因为 rootfs 里根本没有 `libc.so`
（只有 `ld-musl-aarch64.so.1`）。

修法是把这个包做成**零依赖的全静态 DSO**：`readelf -d` 看不到任何 `NEEDED` 条目，
libc 与 libstdc++ 全部链进 `.so` 本体。Lakka 官方 buildbot 产的核心也是这么做的。

技术上走过一条弯路，记在这里供参照：zig 的 musl 交叉编译**做不出**可用的全静态
DSO——`-shared` 会留下 `NEEDED libc.so`，`-static -shared` 顺序反了会产出 ar 归档，
`-nostdlib` 手工链静态库时 zig 的 musl 树缺 `dup3.c` / `mprotect.c`，且它的
`libc.a` 是 exe 专用非 PIC，塞进 DSO 只能靠 `-Wl,-z,notext` 硬来（结果是 TEXTREL，
在强制 W^X 的系统上加载即失败）。最终改用 dockcross 的
`linux-arm64-musl` + GNU ld 的 `-shared -static`，与 buildbot 同构型。

**包名**：`makecode_arcade_libretro_lakka-arm64.zip`
**装法**：`.so` 与 `.info` 一起放到 `/storage/cores/`，重启 RetroArch。

## Nintendo Switch：独立发布，不在本页的 6 个包内

Switch 的形态很特殊：**libnx 没有 `dlopen`**，核心不能是独立 `.so`。它必须编成
静态库 `libretro_libnx.a`，和 RetroArch 前端源码一起链成一个 `retroarch_switch.nro`。
也就是说 **一个核心对应一个 NRO**，换核心等于换前端本体——这与"把核心放进前端
cores 目录"的常规玩法不是一回事。

因此 Switch 产物**不进本页的 6 个包**，由独立的 NX 流水线产出
（artifact 名 `nx-switch-nro`），内容为：

```
makecode_arcade_libretro_libnx.nro
makecode_arcade_libretro.info
```

装法：两个文件一起放 `sdmc:/retroarch/cores/`，卡带放 `sdmc:/roms/makecode/`。
NRO 采用 title takeover（覆盖相册/任意已装 title 启动）。

**请注意这一条**：Switch 链路目前只做到"编译 + 链接 + 符号验证"，**尚未在真机运行过**。
CI 里做了实证校验（确认 `retro_load_game`、`pxt_vm_start_buffer`、`mca_tick_init`
这几个符号真的出现在最终 NRO 的 ELF 里，而不是链接时被静默丢弃），但这只证明
ABI 与符号对得上，不证明运行期正常。最可能出问题的地方是 libnx 那套精简 pthread
与本核心 VM 线程使用的 `pthread_cond_timedwait` / 递归互斥量。首次上真机若出现
黑屏或卡住，属于预期内的未验证风险。

## 顺带修掉的一个构建期真 bug

把 pxt 运行时编成静态库时发现：`runtime/pxtapp` 里 `base/control.cpp` 与
`core---vm/control.cpp` **同名**（分别定义 `millis`/`reset` 与 `raiseEvent`/`_ramSize`，
符号互补不重叠）。GNU ar 归档时只保留基名、丢弃目录，两个 `control.o` 进同一个
`.a` 时后者会**静默覆盖**前者，符号就此消失，要等到链接期才以 `undefined symbol`
形式炸出来，且看起来与文件名毫无关系。

修法是在归档前把对象拷成路径扁平的唯一名（`o_<完整路径下划线化>`），归档后立刻
断言成员数等于对象数，不一致就直接失败。**这个问题只影响静态链接路径（即 Switch），
所有动态 `.so` / `.dll` / `.dylib` 包不受影响**——它们是把带完整路径的对象列表
直接交给链接器，从不经过 ar。

## 验证到哪一步

- **核心行为回归**：与 0.3.2 同一份源码树，运行时零改动，20 张语料的行为基线不变。
- **Lakka 包**：全程用 dockcross 构建，CI 里跑 900 帧 miniretro 冒烟
  （`ubuntu-24.04-arm`），并断言产物 `NEEDED` 为空、导出符号 ≥20 个。
- **Switch 包**：静态库与 NRO 两段都做了校验——库成员数、AArch64 架构、
  `retro_*` 全局符号数量、关键符号逐个存在，NRO 里再查一遍核心符号是否真的链进去。
- **尚未做**：两个新平台都**没有实际硬件运行**（手上暂时没有 Switch 与 Lakka 设备）。
  已在 0.3.2 验证过的 8 台 Linux ARM 掌机、以及 Android 目标是稳定的。

## 已知限制

- Switch 与 Lakka 均为首次支持，未经真机验证。报障请附上前端日志。
- 32 位前端仍不支持（PXT64 镜像在 32 位 VM 下无法执行，原因见 v0.3.1 说明）。
- 即时存档 / 快进仍不提供（原因见 v0.3.0 说明）。

## 升级建议

- **Linux / Windows / Android 用户**：无功能变化，可以不升级。
- **Lakka / LibreELEC 用户**：这是目前唯一能用的版本，取 `lakka-arm64` 包。
- **Switch 用户**：可以试，但请预期这是未经真机验证的首次支持。

---

MakeCode Arcade core 是非官方的第三方项目，与 Microsoft、Nintendo 均无关。
