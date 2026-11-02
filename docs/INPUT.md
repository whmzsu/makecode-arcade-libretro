# 按键与选项（输入端设计）

## 一、问题：为什么"RetroArch 里配不了按键"

libretro 的输入端分三层，缺任何一层都会让玩家觉得"按键没法配"：

| 层 | 由谁决定 | 缺失时的表现 |
|---|---|---|
| 物理键 → RetroPad 键（如"手柄 X 键 = RetroPad A"） | 前端（RetroArch 输入设置） | 玩家在设置里手动绑，总能用 |
| RetroPad 键 → 游戏功能（如"RetroPad A = 跳跃"） | **core**（`SET_INPUT_DESCRIPTORS`） | 快捷菜单 → 控制 页空白或只显示通用键名，玩家不知道哪个键干什么 |
| 行为开关（A/B 交换、菜单键用哪个） | 仍然前端（Controls 页重绑） | 不需要 —— v0.2.4 起核心不再提供任何选项 |

v0.2.1 之前本项目只做了"读按键"，后两层的声明一个都没发 —— 所以前端虽然
照样能把按键事件传进来，但界面上没有任何可配置项，等于"配不了"。

> v0.2.4 定稿：**映射写死、只用前端 Controls 页改键**，核心一个选项都不发。
> 默认布局对齐真机：**RetroPad A/B = MakeCode A/B，START = 游戏内菜单，
> SELECT = 重置**（真机右侧的两个小键）。
>
> v0.2.3 曾把 A/B 交换、菜单键、重置键做成三个核心选项。实测问题：同一件事
> 有两种改法互相打架 —— 选项改成 `both` 后描述符会发出两条 `Menu`、`Reset`
> 那条被挤掉，玩家在 Controls 页只看到两个 menu、找不到 reset。选项已全部删除。

## 二、实现

### 1. 输入描述符（`SET_INPUT_DESCRIPTORS`，load_game 时发）

```cpp
struct { unsigned id; const char* desc; } kDesc[] = { ... };   // 固定表，8 条
static struct retro_input_descriptor d[kCount + 1];            // +1 = terminator
```

标签写「RetroPad 键 → 游戏内功能」而不是 `Button A`：

```
port=0 id=  8 A       -> A -> MakeCode A (jump / OK)
port=0 id=  0 B       -> B -> MakeCode B (back / cancel)
port=0 id=  4 UP      -> D-Pad Up -> MakeCode Up
port=0 id=  5 DOWN    -> D-Pad Down -> MakeCode Down
port=0 id=  6 LEFT    -> D-Pad Left -> MakeCode Left
port=0 id=  7 RIGHT   -> D-Pad Right -> MakeCode Right
port=0 id=  3 START   -> Start -> Game Menu (in-game menu key)
port=0 id=  2 SELECT  -> Select -> Reset (restart game)
```

两条硬规则：

1. **一个 RetroPad 键只能有一条描述符**。曾经因为"菜单键"可以配成 Start+Select
   而发出两条同名 `Menu` 行，Controls 页就成了"两个 menu、没有 reset"。
2. START 那条必须写 **Game Menu**（游戏内菜单），因为 RetroArch 自己还有一个
   **Menu Toggle**（呼出前端快速菜单）—— 两者同名会让玩家分不清哪个是哪个。

**坑**：数组必须给 terminator 留位（N+1）。参考项目写 `[8]` 存 7 条 + 空项，
最后一条写到了数组外，踩掉相邻静态变量 —— 表现是 `video_cb` 变 NULL、画面
永远出不来。本项目按表长度动态留位。

### 2. 控制器类型（`SET_CONTROLLER_INFO`）

声明端口 0 只有一种设备（`RETRO_DEVICE_JOYPAD`，标签 "MakeCode Arcade
Gamepad"），Controls 页的 Device Type 才有正确选项。`retro_set_controller_port_device`
记录实际设备，若是 `RETRO_DEVICE_NONE` 就整帧不读按键。

### 3. 核心选项：没有

v0.2.4 起 `SET_VARIABLES` 只发一个空表（`{{nullptr,nullptr}}`），前端选项页
明确显示 0 项。所有输入相关设置都走 Controls 页：

| 想要的效果 | 做法（都在 Controls 页） |
|---|---|
| 掌机 A/B 位置和 MakeCode 相反 | 把 "MakeCode A" 绑到物理 B 键，反之亦然 |
| 换一个键当游戏内菜单 | 把 "Start -> Game Menu" 绑到别的物理键 |
| 换一个键当重置 | 把 "Select -> Reset" 绑到别的物理键 |

为什么删掉选项：选项改的是"RetroPad 键 → 功能"，Controls 页改的是"物理键 →
RetroPad 键"，玩家本来就只需要后者；两套开关同时存在时，选项一改描述符标签
跟着变，Controls 页的绑定看起来就"错位"了（还出现过重复 Menu 行）。

> 设备上可能残留旧版写进 `retroarch-core-options.cfg` 的 `mca_*` 行 ——
> 核心不再读取，无害；想清理可以直接删这几行。

### 4. 位掩码快速路径

```cpp
g_bitmask = g_env(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, nullptr);  // load_game 时问一次
// 每帧：一次回调拿全部 16 个键，省掉 7 次 input_state 调用
int16_t m = g_input_state(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK);
```

弱机上每帧 7 次跨模块回调不算小开销，支持就走位掩码。两条路径都保留
（`g_bitmask` 为假时退回逐键查询），并都有测试覆盖。

## 三、验证方法

`tools/make_probe_cartridge.py` 生成 `out/pxt64/probe.pxt64`：每个按键事件
把屏幕背景刷成固定颜色（A=红、B=黄、UP=绿、DOWN=橙、LEFT=蓝、RIGHT=淡蓝、
MENU=青、无输入=白）。`tools/test_buttons.py` 逐个按键跑最小前端，抓末帧
比对中心像素颜色 —— 颜色是离散的，能精确抓到"按 A 变成了按 B 的颜色"这类错位。

```
16/16 通过（v0.2.7；含两条"重置后再按键仍变色"的重启存活用例）
无输入white / A red / B yellow / UP green / DOWN orange / LEFT blue
RIGHT ltblue / START teal(Game Menu) / SELECT white(重启回启动白底)
X、Y、L 未映射 → 仍是白底（不会误触发）
位掩码路径:B→yellow   位掩码:SELECT→white
```

描述符本身另有静态自检（`libretro_host.py --dry` 的"输入配置自检"）：
必须恰好 1 条 Game Menu、1 条 Reset、同一个 RetroPad 键不得重复出现、
不得再发核心选项。

「SELECT → white」就是 reset 生效的证据：探针游戏被重启，回到启动白底。
连续重启用 `--press-at=60,110,160` 覆盖（3 次 HOST RESET 无崩溃，GC 堆回绕
正常）。

### 为什么不用 `console.log` 做探针

最初探针是 `console.log("PROBE:A:down")`，结果一条都抓不到：VM 里 JS 的
`console` 输出**不进** `codalLogStore`，而 `pxt_get_logs(0, …)` 只读那个环。
dmesg（`loading image` 之类）能读到，JS console 读不到。改用画面颜色做判据。

## 四、重置的实现（PATCH 5~9）

pxt 官方宿主是"单游戏单进程"，VM 没有 stop 接口 —— 但 `systemReset()`
本身就是一套完整的重启收尾（清 fiber / handler 绑定 / GC）。本项目把它
改造成宿主可触发的真重启（`runtime/pxtapp` PATCH 5）：

```
宿主线程                                VM 线程（mainRunLoop）
────────                                ──────────────────────
pxt_vm_request_reset()  ── 置标志 ──►   检测到标志 → hostRestartCleanup()
                                            gcFreeze → 清 reset 槽 → coreReset
                                            → 释放全部 fiber → gcReset
                                            → GC 游标绕回堆首
pxt_vm_poll_exit()==1  ◄─ mcaVmExited   线程正常 return（**不** pthread_exit）
用原始镜像的新拷贝重新
pxt_vm_start_buffer()  ──────────────►  spinThread: join 旧线程 → 起新线程
```

流程里还有一条 **PATCH 6（v0.2.5）** 补的收尾：`gcForgetAllBlocks()`。
重启时 GC 切块游标绕回堆首，如果 `firstBlock` 链还留着上一局的块，新块会落在
**同一批地址**上，`linkFreeBlock()` 的排序插入就把块插到自己后面（自环），
之后任何一次 `sweep()`/`gcReset()` 遍历 `firstBlock` 都是死循环。同时 `globals`
也必须丢掉 —— 否则新一局第一次 `gc()` 会拿上一局的指针去 mark 已被复用的内存，
`GC_CHECK` 失败 → `oops(41)` → `target_panic` → `systemReset()` → 再清理 → 再
`oops(41)` …… 这条递归在真机上的表现就是"重置后画面冻住，进程不崩也退不出去"。

PATCH 7（v0.2.6）修的是**真正的死锁**，也在 `screen---ext/screen.cpp`：
`WDisplay::update()` 里有一句**无超时**的 `pthread_cond_wait` —— 屏幕是
"游戏写帧、前端取帧"的生产者-消费者模型，前端不取它就永远等。而我们
v0.2.4 起 `retro_run` 在等待重置完成期间**跳过拉帧**，于是两边互相等待：
VM 线程停在取帧里读不到 `mcaResetRequest`，前端等 VM 线程退出。真机上
就是"按重置后画面冻住、不崩不退"。修法三条：

1. `WDisplay::update()` 的等待改成最多 50ms（超时丢帧），VM 线程不再可能
   被前端拖死；
2. `retro_run` **任何时刻都拉帧**（离线时拉取立刻返回黑屏+静音，代价可忽略）；
3. 顺带修掉两个重启间隙的前端线程炸弹：`pxt_screen_get_pixels` 里尺寸不符会
   `target_panic`，VM 版的 panic 走 `systemReset()` → 在**前端线程**上
   `pthread_exit`（等于把前端注销掉）；`instWDisplay` / `dac` / `screenBuf`
   都活在 VM 堆上，清理后前端再读就是 use-after-free。现在用 `pxt_vm_online()`
   标志 + `pxt_screen_shutdown()` / `pxt_audio_shutdown()`：清理第一步离线，
   新镜像装载完成再上线。

PATCH 8/9（v0.2.7）补的是**卸载与重启后的收尾**，都不在重置主流程里：

- **PATCH 8：卸载必须停 VM**。前端的"关闭游戏/换游戏"是
  `retro_unload_game() → retro_deinit() → dlclose(核心库)`。VM 跑在独立线程，
  以前这两个回调什么都不做 —— dlclose 之后线程还在执行已 unmap 的代码段，
  立刻 SIGSEGV，前端表现为"直接崩溃退出"。现在两个回调都调 `vm_stop()`：
  置重启标志 → 等线程自己走完清理 → `pthread_join` → 释放镜像；`load_game`
  遇到"已在运行"也先 `vm_stop()`（换游戏）。
- **PATCH 9：单例指针活过 GC 重置**。`WSynthesizer` / `WDisplay` 是
  `SINGLETON` 宏持有的 **static 指针**，对象 new 在 GC 堆上。GC 重置不会清
  static，于是重启后 `getWSynthesizer()` 返回上一局的野指针 —— 既不重新构造
  （`ExtDAC` 的构造器不跑，`dac` 一直 NULL），也不会报错，现象就是
  **"重置后彻底没声音"**。修法是清理时显式置 NULL。顺序上有个坑：必须
  **放在 reset 回调之后**，因为 `stopPlaying()` 自己会调 `getWSynthesizer()`，
  清早了它会当场 new 一个到即将被 `gcReset` 掉的旧堆里（实测 trace 里能看到
  cleanup 之后又冒出一行 `ExtDAC created`）。

关键坑（都实测踩过）：

1. **不能 pthread_exit**：官方 `systemReset()` 以 `pthread_exit` 结束。但 VM
   线程的栈被 fiber 调度器整块 memcpy 过（saveStack/restoreStack），Windows/
   mingw 下此时调 `pthread_exit` 直接 0xC0000005。改成清理后让线程正常
   return，`pthread_join` 照样干净返回。Linux 上原路径没问题，统一走新路径。
2. **GC 堆必须回绕，但只回绕不够**：64 位下 VM 堆固定 1MB
   （`PXT_VM_HEAP_ALLOC_BITS=20`），分配游标是函数内 static，`gcReset()` 不清
   它 —— 不回绕的话重开几次游戏就 `soft_panic(20)`。`resetRuntimeState()` 里把
   游标绕回 `gcBase`，同时清空 `resetFunctions[]`（槽满时重复注册会
   `target_panic`）。**游标回绕必须配套**：① `gcForgetAllBlocks()` 清块清单与
   `globals`（PATCH 6，见上）；② 释放 `img->stackBase`（原代码只 free
   `dataStart`，每次重启漏 8KB）。
3. **`mcaVmExited` 必须在线程真正退出时置位**：v0.2.4 之前它在清理中途就置了位，
   宿主看到后立刻 `pthread_join` 一个还在跑清理的线程 —— 时间窗口小，但真机上
   会顶住前端线程（RetroArch 整个卡住）。现在改由 `multiStart()` 在线程返回前
   设置。
4. **清理过程中再次 panic 要挡住重入**：`target_panic`/`soft_panic` 在 VM 版本
   里都走 `systemReset()`，一旦清理途中触发检查（`oops`），就会递归清理直到卡死。
   `mcaResetting` 标志挡住第二次进入，直接 `pthread_exit`。
5. **每次重启都写 trace**：`pxt_vm_trace()` / `mcaTrace()` 把重启的每一步写进
   `mca_trace.log`（依次尝试存档目录 → 当前目录 → `/tmp`，实际路径会打进
   RetroArch 日志的 `[MCA] core version ...` 横幅）。同时写 stderr，前端开了
   日志就能看到。十几行、每次 flush，对性能无影响。

入口有三处，全走同一条重启路径：

| 入口 | 触发 |
|---|---|
| 重置键（默认 SELECT，可在 Controls 改绑） | `retro_run` 里检测上升沿 → `vm_request_reset()` |
| 前端 Restart / Restart content | `retro_reset()` → `vm_request_reset()` |
| 同卡带重新载入 | `retro_load_game`（换卡带仍需前端重载 core） |

宿主重启时**必须用原始镜像的新拷贝**：VM 会就地打补丁（`vm_patch_image`），
且 `unloadVMImage()` 会 `free(img->dataStart)` —— 所以启动缓冲必须是
malloc 分配的（`vm_host.cpp` 的 `start_vm_from_orig()`），每次重启 malloc
新块、旧块由 VM 释放。

## 五、已知边界

1. **只有 1 个端口**（单人）。MakeCode 支持 P2（键值 +7），需要增加
   `port=1` 的描述符与 `retro_set_controller_port_device(1, …)`；真机 2P 怎么
   映射还没定，等有双人游戏需求再做。
2. **菜单键不是硬件 MENU**：MakeCode 真机的 menu 是独立小键，我们在
   RetroPad 上借 START 模拟。注意 RetroArch 自带的热键 **Menu Toggle**（呼出
   前端快速菜单）默认也可能落在 START/SELECT 上 —— 那是前端的键，与核心无关；
   想避免"按 START 同时弹出前端菜单"，到 Settings → Input → Hotkeys 里把
   Menu Toggle 改绑或设为 none。
3. **重置等待有 2 秒兜底**：`vm_poll_restart()` 若 120 帧内 VM 线程还没退出
   （极端情况：游戏卡在无暂停的死循环），放弃本次重置、写 trace 并在日志里
   报 TIMEOUT，前端线程不会卡死。
4. **重置排查看 `mca_trace.log`**：trace 每个进程启动时截断重建，看的是
   **本次运行**的记录。连续的 `cleanup 1/6 … 6/6` 重复出现而没有
   `cleanup done` ⟹ 清理途中 panic 递归；停在 `[host] join old vm thread` 没有
   `join done` ⟹ 旧线程没退出；停在 `[vm] start: unload image` ⟹ 新一局启动失败。
5. **没有核心选项**：v0.2.4 起全部输入设置走前端 Controls 页（见二.3）。
