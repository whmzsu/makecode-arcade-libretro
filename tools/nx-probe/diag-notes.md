自动生成的**诊断产物**，用于定位 Switch 上 NRO 一启动就崩的问题。不是发版，不是给玩家玩的东西。

已经在真机上确认的事实（固件 22.0.0 / Atmosphere 1.11.1，从 hbmenu 启动）：

| 文件 | 结果 |
|---|---|
| `ra_plain.nro`（RetroArch 本体，不含任何核心） | 能启动 |
| `probe.nro`（我们裸 gcc 编的纯 libnx 程序） | 起不来 |
| `makecode_arcade_libretro_libnx.nro`（前端 + 我们的核心） | 起不来 |

`ra_plain` 和它们由同一个容器、同一个 libnx 编出来，所以"libnx 与固件 ABI 不兼容"这条不成立。现在要拆的是剩下的两种可能。

## 请按顺序跑，每一步都告诉我结果

全部放 `sdmc:/switch/`，按住 `R` 点任意已装游戏进 hbmenu（title takeover，全内存）。

### 1. `probe_nacp.nro`（先跑这个）

和起不来的 `probe.nro` **用的是同一个 ELF**，唯一差别是 `elf2nro` 打包时多带了一个 NACP（标题/作者/版本）。头部对比显示 `probe.nro` 是三个 NRO 里唯一没有 NACP 的（文件末尾 0 字节额外数据，另两个都多 34214 字节）。

- **能显示文字** → 起不来的原因是我打包姿势少了元数据，跟核心无关；`probe.nro` 的失败就不能当作"平台不兼容"的证据
- **也起不来** → 与 NACP 无关，问题在更底层

### 2. `ra_stubcore.nro`（对照：RetroArch + 空核心）

核心是 `tools/nx-probe/stub_core.c`，25 个 libretro 入口全部实现，一行业务逻辑都没有：不建线程、不分配大内存、不读文件。

- **也起不来** → "静态核心链接/注册"这条路本身有问题，跟我们的代码无关
- **能进菜单** → 机制没问题，问题在我们核心的目标代码

### 3. `mca_boot.nro`（我们的核心 + 开机 trace）

正式核心，多了开机埋点。真机上会往

```
sdmc:/switch/mca_boot.log
```

按顺序写里程碑（写一行 flush 一行；崩在下一行之前，所以最后一行就是最后到达的位置）：

```
[boot] ctor              我们的目标代码被加载并执行了
[boot] set_environment   RetroArch 开始和核心握手
[boot] get_system_info   前端读核心信息
[boot] retro_init enter / done
[boot] load_game enter   开始加载卡带
[boot] vm started        VM 起来了
[boot] first retro_run   第一帧渲染
```

把这个文件的内容发出来就能直接定位死在哪一步。**文件不存在**本身也是结论：说明崩在我们第一行 trace 之前，也就是加载器或进程初始化阶段。

## 最有价值的一样东西

```
sdmc:/atmosphere/crash_reports/
```

按修改时间找最新的那份（`.log`），里面有崩溃模块名、模块内偏移和结果码。配合 `mca_boot.symbols.txt` 可以直接对到函数名。如果这个目录是空的，说明 `crash_report` 系统模块被关了。

## 附带文件

- `mca_boot.symbols.txt` — 核心 NRO 的符号表（`nm -n`），崩溃偏移靠它对到函数
- `*-build.log` — 构建日志，编不出来时看这个
