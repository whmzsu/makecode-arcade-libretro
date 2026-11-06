自动生成的**诊断产物**，用于定位 Switch 上 NRO 一启动就崩的问题。不是发版，不是给玩家玩的东西。

## 怎么用

全部放 `sdmc:/switch/`，按住 `R` 点任意已装游戏进 hbmenu，依次启动。

### 1. `probe.nro`（纯 libnx 探针）

控制台程序，不建线程、不链第三方库。屏幕上打印构建期 libnx 版本、固件版本、applet 模式、内存。

- 能看见这段字 → 工具链、ABI、hbmenu 启动路径都正常
- 也崩 → libnx 与固件的 ABI 不兼容（与我们的代码无关）

### 2. `ra_plain.nro`（对照一：RetroArch 本体）

不含任何核心（`HAVE_STATIC_DUMMY=1`）。能进菜单 = 前端在这台机器上没问题。

### 3. `ra_stubcore.nro`（对照二：RetroArch + **空核心**）

核心是 `tools/nx-probe/stub_core.c`，只实现 libretro 入口，一行业务逻辑都没有。

- **也崩** → “静态核心链接/注册”这条路本身有问题，跟我们的代码无关
- **能进菜单** → 机制没问题，问题在我们核心的目标代码

### 4. `mca_boot.nro`（我们的核心 + 开机 trace）

正式核心，多了开机埋点。真机上会往

```
sdmc:/switch/mca_boot.log
```

按顺序写里程碑（写一行 flush 一行，崩在下一行之前，最后一行就是最后到达的位置）：

```
[boot] ctor              我们的目标代码被加载并执行了
[boot] set_environment   RetroArch 开始和核心握手
[boot] get_system_info   前端读核心信息
[boot] retro_init enter / done
[boot] load_game enter   开始加载卡带
[boot] vm started        VM 起来了
[boot] first retro_run   第一帧渲染
```

把这个文件的内容发出来，就能直接定位死在哪一步。**文件不存在**本身也是结论：说明崩在我们第一行 trace 之前（加载器或进程初始化阶段）。

## 附带文件

- `mca_boot.symbols.txt` — 核心 NRO 的符号表。崩溃报告里的模块内偏移可以靠它对到函数名。
- `*-build.log` — 构建日志，编不出来时看这个。
