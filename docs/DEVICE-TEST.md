# 真机验收记录

## 2026-11-01 · RK3566 + ArkOS（首台实机）

| 项 | 值 |
|---|---|
| 硬件 | RK3566（4×Cortex-A55 @1.8GHz），1GB RAM 级掌机 |
| 系统 | ArkOS（Linux aarch64） |
| 前端 | ArkOS 自带 RetroArch |
| core | `makecode_arcade_linux-arm64.zip` v0.2.1（`aarch64-linux-gnu.2.17` 目标） |
| 内容 | `out/pxt64/*.pxt64`（PNG 卡带经 `tools/png2pxt64.py` 离线转换） |
| 结果 | **运行正常，性能良好，完全无卡顿**（用户主观反馈） |

### 结论

路线 B 的立项前提得到实机确认：**去掉 JS 引擎层后，RK3566 这类低端
A55 掌机跑 MakeCode Arcade 游戏有充足余量**。同一台机器上，姊妹核心
mcarcade-core（QuickJS 解释官方 sim）约慢 3~4 倍、重游戏掉帧——本 core
在同一硬件上无卡顿，说明"pxt VM 字节码 + 原生 C++ 运行时"这条路的
性能假设成立。

### 这次真机暴露的两个问题（已修，记下来防复发）

1. **core 少实现 `retro_reset`** —— 上一版（0.2.0）在 ArkOS 上表现为
   "RetroArch 直接闪退回 ES，无任何提示"。原因：RetroArch 装载 core 时
   逐个 `dlsym` 25 个必需符号，缺一个就放弃加载，GUI/ES 下不显示错误。
   该缺陷与平台无关，Windows 与 Linux 一样会挂。
   → 已补实现 + 在 `libretro_core.cpp` 末尾加 25 符号编译期守卫表；
   `tools/inspect_binary.py` 的必需符号表也补全到 25（此前只有 14，
   一直误报 OK）。
2. **发行包 `.info` 版本号硬编码** —— `tools/package_release.sh` 里写死
   `display_version = "0.2.0"`，重编到 0.2.1 后包里仍显示 0.2.0，用户无法
   在 RetroArch 里区分新旧 core。
   → 改为从 `core/src/libretro_core.cpp` 的 `library_version` 单点读取。

### 另一处（Windows 专属，与 ARM 无关）

pxt 上游 `core---vm/platform.cpp` 的 `initRandomSeed()` 在
`open("/dev/urandom")` 失败后仍对 `fd = -1` 调 `read()`，Windows UCRT 判
"无效参数" → `__fastfail(0xC0000409)`，进程瞬间消失且不留任何日志/WER 记录。
→ PATCH 4：Windows 用系统时间+栈地址做种子，POSIX 侧加 `fd >= 0` 防护
（对 Linux 是纯保险，Linux 本就有 `/dev/urandom`）。

### 安装位置（ArkOS）

- 解压发行 zip，把 core 与 `.info` 放同一目录：ArkOS 上通常为 `/roms/cores/`
  （系统分区只读，core 放 roms 分区）。**不同平台/发行版的目录位置不一样**，
  以 RetroArch `设置 → 目录 → 核心 / 核心信息` 里显示的为准；ArkOS 的
  菜单入口可能在 `options → retroarch` 下。**`.info` 必须同放**，否则
  「加载核心」列表里不显示 "MakeCode Arcade"。
- 架构确认：`uname -m` 必须为 `aarch64`；若为 `armv7l` 需换
  `makecode_arcade_linux-armhf.zip`（armhf 尚未真机验证，见下）。

### 待量化 / 待覆盖

1. **帧率与占用**：目前只有主观"无卡顿"。RetroArch OSD 打开 FPS 显示，
   或 SSH 上 `top -H -p $(pidof retroarch)` 看单核占用，可给出确切数字。
2. **RK3326**（4×A35 @1.3GHz，比 A55 再弱一档）未测；这是路线 B 立项时
   点名的另一颗芯片。
3. **armhf（32 位）** 编译通过但未真机验证。注意 `base/gc.cpp` 的
   `gcAllocBlock()` 在非 PXT64 分支走 `mmap(GC_BASE=0x2000000000)`，
   32 位下该常量会截断；实际行为未验证（不带 `MAP_FIXED` 时 addr 仅作
   提示，可能不影响，但需实测）。
4. **Android** 两个包未上机。

## 2026-11-01（第二晚）：重置挂起（v0.2.5 修复）

现象：真机上按重置键、或走 RetroArch 的「重启游戏」，画面立即冻住；进程不崩、
仍能关掉游戏。

定位思路与结论：

- 复位走的是 PATCH 5 的"VM 线程自清理 + 宿主重起线程"。这条路上有三个问题叠在
  一起，且**只在重置用到"新堆块"时暴露**，所以此前用小卡带测不出来：
  1. `gcAllocBlock()` 的切块游标被绕回堆首（PATCH 5 为了重开不 OOM 加的），但
     `firstBlock` 块清单没清 → 新块落在同一地址 → `linkFreeBlock()` 自环 →
     `sweep()` 遍历 `firstBlock` 死循环；
  2. 就算清了块清单，`globals` 还指着上一局的堆内存 → 新一局第一次 `gc()` 用
     悬垂指针 mark → `GC_CHECK` 失败 `oops(41)` → `target_panic` →
     `systemReset()` → 再清理 → 再 `oops(41)` … 无限递归（"画面冻住"的真身）；
  3. `mcaVmExited` 在清理中途就置位，宿主可能立刻 `pthread_join` 一个还在清理的
     线程，把前端线程顶住。
- 修法（PATCH 6）：`gcForgetAllBlocks()`（清 firstBlock/firstFree/globals）+
  `mcaResetting` 挡重入 + 退出标志挪到 `multiStart()` 真正返回前 +
  `unloadVMImage()` 释放 `stackBase`（原来每次重启漏 8KB）。
- 现场证据：存档目录下 `mca_trace.log` 会逐步记录 `cleanup 1/6 … 6/6`、
  `thread exit`、`join done`、新线程启动。卡住时最后一行即可定位。

回归：三个样例卡带各连按 3 次、cubeland 连按 12 次重置，均无 PANIC、帧率不变；
按键用例 14/14。

## 2026-11-01（第三轮）：重置挂起未解决 → v0.2.6 找到真死锁

v0.2.5 装机后重置/前端「重启游戏」仍挂起且没有日志。复盘发现两件事：

1. **真死锁（PATCH 7 修复）**：pxt 屏幕是生产者-消费者模型 ——
   `WDisplay::update()`（VM 线程）写帧前会用**无超时**的 `pthread_cond_wait`
   等前端取走上一帧；而 v0.2.4 起 `retro_run` 在重置等待期间**跳过拉帧**。
   于是 VM 线程卡在取帧里永远读不到 `mcaResetRequest`，前端等 VM 线程退出
   → 画面冻住、不崩不退。v0.2.3~v0.2.5 的所有重置问题里，这是最主要的一个
   （此前的按键回归用「回白底」判据，分不清"重启成功"和"冻住"，所以一直
   没测出来）。
2. **日志没看到的原因**：trace 只写存档目录一个位置，fopen 失败就退到
   stderr（真机上看不见）。v0.2.6 改为存档目录 → 当前目录 → `/tmp` 三级
   回退、同时写 stderr，实际路径打进 RetroArch 日志的
   `[MCA] core version 0.2.6, trace -> ...` 横幅 —— 先看这行确认版本和路径。

修复内容（PATCH 7）：
- `WDisplay::update()` 取帧等待加 50ms 超时（超时丢帧），VM 线程不可能再被
  前端拖死；
- `retro_run` 任何时刻都拉帧（重启等待期间也拉，离线时立即返回黑屏+静音）；
- 新增 `pxt_vm_online()` 全局在线标志 + `pxt_screen_shutdown()` /
  `pxt_audio_shutdown()`：清理第一步离线（丢 `instWDisplay`/`dac`/取帧计数），
  新镜像装载完成再上线；重启间隙前端的拉取变成纯本地黑屏/静音，不再碰
  已释放的 VM 堆与单例；
- `pxt_screen_get_pixels` 不再在前端线程上 `target_panic`（VM 版 panic 会走
  `systemReset()` → 前端线程 `pthread_exit`），尺寸不符改为蓝屏；
- trace 三级路径 + stderr 双写。

回归：探针 16/16（新增两条「重置后再按键仍变色」的存活用例 —— 旧判据
"回白底"无法区分重启成功和冻住，这正是此前几轮都没测出真死锁的原因）；
ping/jumpy/cubeland 各连按 6 次重置全部走完 `cleanup done → thread exit →
join → 新线程`，帧率不变。

## 2026-11-01（第四轮）：关闭游戏/换游戏崩溃、重置后没声音 → v0.2.7

现象：重置已经正常了，但（a）关闭游戏、（b）中途换另一张卡带，RetroArch 直接
崩溃退出；（c）每次重置之后声音消失，画面正常。

### (a)(b) 卸载时没停 VM 线程 —— PATCH 8

前端的生命周期是 `retro_unload_game() → retro_deinit() → dlclose(核心库)`。
v0.2.6 之前这两个回调都是空实现，VM 线程继续跑；`dlclose` 把核心库 unmap 掉
之后，线程还在执行已经不属于自己的代码段 → SIGSEGV，整个前端进程退出。
"换游戏"更糟：新卡带 `load_game` 时老线程还在跑，两套 VM 共用同一批全局状态
（gcBase / globals / 单例），装载新镜像就是在接管正在被使用的堆。

修法：`retro_unload_game` / `retro_deinit` 调 `vm_stop()` ——
置重启标志 → VM 线程自己走完 `cleanup` 序列并 `return` → 宿主
`pthread_join` → 释放镜像（`dataStart` + 运算栈）；`vm_start_from_image()`
遇到"VM 已在跑"也先 `vm_stop()`（换游戏）。等待上限 3 秒，超时会写
`[host] stop TIMEOUT`，不会把前端顶死。

### (c) 单例指针活过 GC 重置 —— PATCH 9

`WSynthesizer` / `WDisplay` 是 `SINGLETON` 宏持有的 **static 指针**，对象本身
`new` 在 GC 堆上。GC 重置只作废堆，不动 static —— 重启后
`getWSynthesizer()` 认为"已经创建过"，既不重新构造（`ExtDAC` 构造器不跑，
`dac` 一直 NULL），也不会报错，结果就是静音。地址被新一局复用时更危险：
旧指针会被当成 `WSynthesizer` 用，类型错乱写坏内存。

修法：`pxt_music_shutdown()` / `pxt_audio_shutdown()` / `pxt_screen_shutdown()`
在清理时置 NULL。顺序有坑：必须放在 **reset 回调之后** —— 回调里的
`stopPlaying()` 自己会调 `getWSynthesizer()`，清早了它会当场 new 一个新的到
即将被 `gcReset` 掉的旧堆里（trace 里能看到 cleanup 之后又冒出一行
`ExtDAC created`）。

### 新增的回归手段

- `tools/libretro_host.py --switch=a,b,c`：连续换多张卡带；
  `--dlclose`：结束前真正 `dlclose` 核心库（等价于前端关闭游戏/卸载核心）；
  `--dry` 之外的音频统计新增「采样峰值」。
- `tools/make_probe_cartridge.py --audio`：一直播 440Hz 的发声探针卡带。
- `tools/test_lifecycle.py`：三连换游戏 + 关闭游戏后 dlclose + 重置后音频
  峰值必须重新 > 0（并要求 `ExtDAC` 恰好重建 N 次）。

回归结果（Windows 本机）：换游戏 3 连各 30 帧、dlclose 正常退出、两次重置后
音频峰值始终 8160（`ExtDAC` 重建 3 次 = 启动 + 2 次重置）、按键 16/16。

---

## 2026-11-01（第五轮）：Android aarch64 验收通过

平台：Android arm64 设备 + RetroArch（`aarch64-linux-android21` 产物，
v0.2.7）。**结论：功能表现与 Linux aarch64 一致**，PATCH 6~9 跨平台成立。

### 部署：两个目录必须拆开（Android 特有）

Android 上装第三方 core 有个绕不开的矛盾：

- **核心目录**（`libretro_directory`）里的 `.so` 要被 `dlopen`，必须能
  `mmap(PROT_EXEC)`。Android 10 起 `/storage/emulated/0` 挂载带 `noexec`，
  且不在 App 的 linker namespace 白名单内 —— 放这里**能扫到、能读 `.info`，
  但一加载就报错**（`is not accessible for the namespace` /
  `failed to map segment`）。
- **核心信息目录**（`libretro_info_path`）里的 `.info` 只是文本，随便哪都能读。

而 `Install or Restore a Core` 的文件浏览器**按平台过滤扩展名**——Android 上
只列 `.so`，`zip` 根本不出现在列表里，必须先手动解压；选中 `.so` 后它只把
这一个文件拷进核心目录，`.info` 直接丢弃。所以正确做法是把两个目录分开：

1. 设置 → 目录 → **核心（Cores）**：保持**默认**（App 私有，
   `/data/user/0/com.retroarch.*/cores`），解压出的 `.so` 用 Install 装进去
2. 设置 → 目录 → **核心信息（Core Info）**：指向可写的外部目录，
   例如 `/storage/emulated/0/RetroArch/info`，把 `.info` 放这儿
   （该项默认隐藏，需先开 设置 → 用户界面 → 显示高级设置）

若菜单里没有「核心信息」项，用配置文件改 `libretro_info_path` 同样有效。

### 反例：没有 `.info` 就用不了（已实测）

试过「只装 core、关掉文件浏览器扩展名过滤、直接选游戏」这条路，**不通**：
选中 `.pxt64` 后弹出的「选择核心」对话框里**选不到本 core**。也就是说
`.info` 不是可选项而是必需品，不能指望前端绕过。

### 踩坑顺序（供下次装 Android 时避开）

1. 想直接选 zip 走 Install → 文件浏览器只列 `.so`，**zip 压根不在列表里**；
   解压后用 Install 能装进 `.so`，但 `.info` 照样被丢弃
2. 为了补 `.info` 把**核心目录**改到外部存储 → 能识别、加载必失败（noexec）
3. 正确解：核心目录留默认，只把**核心信息目录**指出去

### 未覆盖

- Android 上的量化数据（fps / 单核占用）还没测。
- 32 位 armv7 产物未上真机（设备是 64 位）。


## 2026-11-02（桌面端）：v0.2.10 确定性 + 即时存档落地

### 结论

- **核心完全确定**：同一卡带 + 同一输入序列，两次独立进程跑 300 帧的
  末帧哈希逐字节一致（5/5）。快进/慢放成为前端取帧频率的天然结果。
- **retro_serialize/unserialize 落地**：同会话恢复后画面与原时间线逐帧
  重合（`tools/test_savestate.py` S1–S4 全绿）。

### 本轮修掉的非确定性来源（按发现顺序）

1. **取帧等待用 `pthread_cond_timedwait`**：zig-mingw 的 winpthreads 对它
   直接返回 `EINVAL(22)`，300 帧里 288 次"立即超时"——前端实际没等，读到
   的是上一帧。与 v0.2.9 的 mca_tick 是同一个坑，新增 `mca_pub_wait/signal`
   （Windows=自动重置事件，POSIX=cond+待取票）替换。
2. **启动异步**：`retro_load_game` 立即返回，装载期间前端白拉的黑屏帧不计
   圈数，「前端第 N 帧 ↔ VM 第 N 圈」错开一个由墙钟决定的偏移。现在
   `spinThread` 同步等到主循环停在取帧闸门上（`mcaMainLoopLive`）。
3. **取帧请求条件化**：离线帧不请求 → 补圈数随墙钟漂移。改为无条件请求，
   请求序号与发布序号严格配对（第 N 次取帧 = 第 N 圈 = 第 N 次发布）。
4. **随机种子混入"运行次数"**：pxt settings 模块初始化时
   `seedAddRandom(runNumber())`，runNumber 持久化在存档目录、每次 +1 →
   两次运行第 2 帧起画面分叉（球的 x 差 2 像素）。`seedAddRandom` 改为
   忽略熵源，随机序列唯一来源 = 卡带镜像哈希（`initRandomSeed`，并补上
   之前从未被调用的镜像播种路径——现在在 vmStartCore 里显式调用）。
5. **屏幕缓冲未清零**：`WDisplay` 的 screenBuf/pubBuf 是 `new uint8_t[]`，
   头几帧画面取决于堆里残留内容。构造时清零。

### 已知现象（非本 core 缺陷）

- Pong 模板长按 **UP** 会触发游戏自身的 `target_panic(1980)`（精灵越界），
  蓝屏 + 日志，属游戏层行为；left/right 对玩家桨无效（桨上下移动）。
- pxt 的 settings（高分等）仍持久化在存档目录，但**不再影响随机序列**。

### 真机待验

- v0.2.10 尚未在 RK3566 / Android 上验收；改动集中在帧节拍与取帧路径，
  理论上真机表现只会更稳（启动同步化会稍微拉长 load 时间，约几十 ms）。
- Rewind（RetroArch 逐帧回带）依赖本快照接口，真机内存占用待测
  （单份快照 ≈ 1.6MB，rewind 环形缓冲默认几十份）。


## 2026-11-02（桌面端）：v0.2.11 存档跨会话

### 结论

v0.2.10 的存档只在自己那次会话里有效——真机报告的两条现象（`unserialize
failed: -10`、界面说"加载成功"但游戏照旧）都源于此。v0.2.11 起：

- **存一档 → 退出 RetroArch（甚至崩溃）→ 重开同一个游戏 → 加载 → 继续玩**，
  画面与原时间线**逐帧一致**（`test_savestate.py` S5：42..240 共 199 帧）。
- 动态加载器把 core 换到别的基址时照样成立（S6：重定位 286 个模块指针）。
- 拿别的游戏的档恢复会被拒绝（`-10` + 一行日志），游戏照常继续，不会崩前端
  （S3）。垃圾数据同理（S4）。

### 做法：让地址可复现

1. 三类不可预测的分配钉到固定地址（`runtime/pxtapp/mca_fixed.h`）：

   | 对象 | 地址 | 容量 |
   |---|---|---|
   | GC 堆 | `GC_BASE` | 1MB |
   | 卡带镜像 | `GC_BASE + 1MB` | 预映射 4MB |
   | VM 单例（WDisplay / WSynthesizer / ExtDAC） | `GC_BASE + 5MB` | 512KB bump 区 |

   于是堆内的绝对指针、镜像内的代码地址在新进程里依然成立。
2. 剩下三类仍不可复现，恢复时逐个处理：fiber（重建，`sp`/`stackCopy`
   按字节偏移还原）、event（重建并重串链，`HandlerBinding::pending` 跟着改
   指向）、**指向 core 自身代码的指针**（`RefAction` 的内核回调、fiber 的
   `wakeFn`；快照头记下保存时的模块区间，恢复时按差值整体平移）。
3. 卡带身份：堆和镜像都钉死后"地址不符"不再能区分换卡带，快照头额外记
   镜像字节数 + FNV-1a 指纹（ping 148832/`491f3759`，jumpy 190232/`ac861ae8`）。

### 本轮修掉的两个真问题

1. **暂停握手抢跑**（存量，v0.2.10 就有）：`pxt_state_pause()` 只看
   `mcaPaused`，而这个标志的清除在 VM 侧晚于置位，连续存/取时宿主会立刻
   看到上一次的残留 1 → 搬的是 VM 正在改的半截状态。真机上表现为"读档后
   立刻 panic（`failed type check`）/ 几十帧后画面走岔"。改为等"一个**新的**
   停稳序号"（`mcaPauseAck`）。
2. **恢复时往已释放的内存里填活对象**（本版引入后自查发现）：同进程恢复
   图省事，把 fiber `xfree` 之后**原地写回**，那块内存已经回到分配器手里，
   游戏下一次 `setupThread` 新建 fiber 就会盖住它——表现为"读档后几十帧
   画面才分叉"（S2 约 1/8 概率，压测更明显）。现在一律 `xmalloc` 重建，
   旧地址只作映射键；`stackCopy` 同样重建（原来直接沿用已释放的旧指针）。
   回归新增 **S7 rewind 热路径**（同一存档每 20 帧读一次，压这套内存管理）。

### 测试

`tools/test_savestate.py` 全绿（S1–S7），并发三份压测亦全绿；
`MCA_TEST_OUT=<dir>` 可分开输出目录并行跑。
`python tools/test_buttons.py` / `test_lifecycle.py` 仍全绿。

### 真机待验

- v0.2.11 未在 RK3566 / Android 上验收。请重点看：退出 RetroArch 后重进
  游戏加载存档能否续上、Rewind 长时间按住是否稳定。
- 单份快照 ≈ 1.6MB（含 19240 字节画面双缓冲）；Rewind 环形缓冲默认几十份，
  掌机内存占用待实测。


## 2026-11-02（真机反馈 → 修复）：v0.2.12 读档 `pause failed -2`

### 真机现象

v0.2.11 上保存正常，加载失败，日志：

```
[libretro ERROR] [MCA] unserialize: pause failed -2
[ERROR] [State]: 加载状态失败于 ".../cubeland.state1"
```

（v0.2.11 之前是 `unserialize failed: -10`，两个问题不重叠：-10 是卡带身份/
地址不符，-2 是"VM 停不下来，放弃快照"。）

### 根因

暂停请求只在 `mainRunLoop` 顶部被检查，而 fiber 里一段不调 `pause` 的循环会让
`exec_loop` 一直跑到它自然让出为止。桌面几十毫秒，真机 ARM 慢一个数量级 →
超过宿主 5 秒预算 → 放弃。

**桌面复现**（`tools/make_probe_cartridge.py --busy` 生成 10 亿次自增探针）：

```
[host] frame 20: 按下 A
[core] [MCA] unserialize: pause failed -2      <-- 与真机同一行
trace: [snap] pause timeout 5s: spot=2 loops=24 req=151 fibers=3 live=1
       online=1 panic=0 reset=0
```

`spot=2` = VM 卡在解释器里跑 fiber；`loops=24` = 帧号自第 24 圈起不再推进，
说明它压根没回到循环顶，不是没看到请求。

### 修法

`exec_loop` 每 2048 条指令查一次 `mca_pause_pending()`，命中则把下一条指令存进
`resumePC`、结束本次 exec_loop，调度器回主循环顶后立即停稳。

**踩坑记录**：第一版在检查点里直接调 `sleep_ms(1)` 让出 —— 段错误。原因是
`sleep_ms`/`schedule()` 作用于 `currentFiber`，而执行到该点的 `ctx` 未必等于
当前 fiber（fiber 创建路径会改 `currentFiber`），结果把**别的** fiber 的 pc
清空了。改成直接写 `ctx` 才安全。

另外暂停超时预算 2s → 5s，超时写一行诊断到 `mca_trace.log`（spot / loops /
req / fibers / live / online / panic / reset）。

### 验证

- 桌面 S8（忙等中读档）：v0.2.11 代码 **必现 -2**；v0.2.12 通过
- `test_savestate` S1–S8 全绿；`test_buttons` 16/16；`test_lifecycle` 全过
- 发版矩阵待跑

### 真机待验（重点）

1. **游戏启动后立刻从菜单加载存档** —— 这是最容易撞上"VM 正在跑启动代码"的时机
2. 游戏里忙的那一刻（画面元素多、刚开局）加载存档
3. 长按 Rewind 是否稳定
4. 若仍失败，取 `mca_trace.log` 里 `[snap] pause timeout` 那行 —— 里面的
   `spot` 直接指出 VM 卡在哪（1=等帧 2=跑 fiber 3=卡在暂停里 0=主循环顶）


## 2026-11-02（真机反馈 → 修复）：v0.2.13 读档仍有 `-2` 与"卡带不同"

### 真机现象（v0.2.12）

保存正常，加载交替出现两种失败：

```
[libretro ERROR] [MCA] unserialize: pause failed -2
[ERROR] [State]: 加载状态失败于 ".../cubeland.state5"
[snap] 拒绝恢复：卡带不同（快照 181608 字节/1064f705，当前 181608 字节/e2483410）
```

### 关键证据：`spot=3`

用户贴的 `mca_trace.log`：

```
[snap] saved 96704 bytes (heap 65536, fibers 4, events 0)
[snap] pause timeout 5s: spot=3 loops=742 req=742 fibers=4 live=1 online=1 panic=0 reset=0
```

`spot=3` = **VM 已经停在安全点上**。所以不是 v0.2.12 修的那个"VM 跑不停"，
而是宿主在干等一个永远不会到来的回执 —— 握手协议本身有竞态。

### 根因 1：暂停握手（"停稳次数 +1"）

RetroArch 读档前会先 `retro_serialize` 存一个撤销点，两次 `pause` 紧挨着。
旧协议的 resume 只是个电平标志（request=0）：VM 还没观察到它，请求又变回 1，
VM 就一直在内层暂停循环里，不再产生第二个回执，宿主等的序号永远不到 → 5 秒放弃。

桌面复现（`--save-state-at=N` 与 `--load-state-at=N` 同一帧，三次）：
v0.2.12 的代码 **3 次里 2 次** 报 `-2`，trace 同样是 `spot=3`。

**修法**：want/done 双序号。宿主每次 pause 换新序号写 `mcaPauseWant`，VM 停稳后把
该序号写 `mcaPauseDone`；VM 内层循环条件是"want 仍等于我这次的序号"，换序号必然
退出 —— 不再要求"中间那次 resume 必须被观察到"。回归 **S9**。

### 根因 2：卡带指纹算在 `vm_patch_image()` 之后

patch 会把 vtable / 内核函数指针（指向 core 自身模块）**就地写回镜像**。指纹原本
拿 patch 之后的 `dataStart..dataEnd` 现算 —— Linux/Android 的 `.so` 走 ASLR，换进程
就变，于是**自存的档**重开后被判成"换了卡带"。Windows DLL 基址固定，桌面没暴露。

**修法**：装载时、patch **之前**对原始字节算一次（`vmload.cpp` 的 `mca_image_hash()`），
存档与校验都用它；启动横幅打印 `cart fingerprint`。回归 **S10**（两次进程一致
且等于 Python 现算的原始镜像 FNV-1a；把计算挪回 patch 之后即变红）。

> 旧档（≤v0.2.12）在 ARM 上过不了校验，需用 v0.2.13 重新存。

### 顺带

`mcaTrace` 开文件移进互斥区：两个线程同时首次写 trace 会各自 `fopen(...,"w")`，
后开的截掉先写的 —— 桌面 `[vm] ...` 那几行经常整个消失，真机排障也可能丢关键行。

### 验证

- `test_savestate` **S1–S10 全绿**，发行包 DLL 亦全绿；S9/S10 都用对照构建验过会红
- `test_buttons` 16/16、`test_lifecycle` 全过；发版矩阵 7/7，7 个 zip 的 `.info`
  均为 0.2.13
- cubeland 桌面跨进程读档 `-> OK`；模拟菜单空转 1.5 秒 + 撤销点 + 读档也 OK

### 真机待验（重点）

1. **先用 v0.2.13 重新存一次档**（旧档指纹是补丁后的镜像，必被拒）
2. 存完退出 RetroArch → 重开 → 加载
3. 菜单里连按几次 Load State（撤销点 + 读档连续发生，正是 S9 的场景）
4. 若仍失败，看日志里的 `cart fingerprint` 两次是否相同、trace 里
   `pause timeout` 那行的 `spot`/`want`/`done`

---

## 2026-11-02（真机反馈 → 修复）：v0.2.14 加载存档卡死 / 取不到模块区间

### 真机给的两组日志

```
[snap] loaded (heap 65536, fibers 3, events 0)
[vm] cleanup 0/7 output offline
[vm] cleanup 1/6 gcFreeze
[vm] cleanup 2/6 reset callbacks        ← 之后什么都没有：画面冻住，进程不崩不退
```

```
[snap] 拒绝恢复：本平台取不到模块区间，跨会话恢复不安全
[snap] module relocate: old=0 size=0 cur=0 hits=0+0
```

### 根因 1：panic 之后 `pthread_exit` 绕过线程收尾

`systemReset()` 末端是 `pthread_exit()`。它**不走** `multiStart()` 的 return 路径，
于是 `mcaVmExited` 永远不置位 —— 宿主用 `pxt_vm_running()` 判断"VM 还活着"，
前端永不重启：画面停在最后一帧，进程不崩也不退。（桌面 Mingw 上则是从 fiber 里
`pthread_exit` 直接 0xC0000005，`test_lifecycle` 里那个 panic 探针一跑就崩。）

**修法**：panic 只置 `panicCode`（横幅照常画）。解释器 `if (panicCode) break;`
结束当前 fiber，控制权回主循环，走 `hostRestartCleanup()` 后**正常 return**，
线程从 `multiStart` 收尾退出（`mcaVmExited=1`）。宿主侧新增"线程猝死 → 自动重启"
（跳变触发，单次装载最多 3 次，防止坏卡带变成每帧重启）。

### 根因 2：前端线程上的 panic 会跑清理序列

`retro_run` 里拉音频会 `target_disable_irq()`；若那里 panic，`systemReset()`
在**前端线程**上跑清理，`stopPlaying()` 又去锁同一把 `irqMutex` → 自死锁；
就算不死锁，`pthread_exit` 也会把 RetroArch 的 render 线程注销掉。

**修法**：前端线程上的 panic 只登记码值 + `pxt_vm_request_reset()`；`irqMutex`
改成显式初始化的递归锁（同线程重入不再自锁）。

### 根因 3：maps 解析器"就地改写"，只能遍历一次

算模块区间天生要两遍：先找出"本 core 的路径"，再按路径把所有映射段聚合成 `[lo,hi)`。
旧实现把每行 `\n` 就地改成 0 来切行 —— 第一遍走完缓冲区只剩第一行，第二遍读到的是
报废数据，`g_modBase` 恒 0 → 快照 `modBase/modSize` 全 0 → 跨会话恢复被拒（`-13`），
重定位永远 `hits=0+0`。桌面走 Windows 的 `VirtualQuery` 分支，这段代码从没被执行过。

**修法**：只读解析（逐行指针推进，路径拷进调用方缓冲）。新增
`mcaModuleMapSelfTest()` 内嵌样本自测，关键断言"同一份文本连续解析两遍结果一致"
（就地改写型实现必挂）；`tools/test_modrange.py` 用 ctypes 调它，并断言真实分支
取到非零区间 —— 在 Linux CI 上就等价于覆盖了真机那条路径。

### 诊断增强（trace）

`[mod] module range base=… (ok/UNAVAILABLE)`、`[vm] PANIC n on vm/frontend thread`、
`[vm] cleanup begin on … thread`、逐个 reset 回调 `enter/done`、
`[snap] vm was paused N ms`、`[snap] vm resumed after load`、
`[host] vm thread gone unexpectedly -> auto restart (n/3)`。

### 验证

- 新增 `make_probe_cartridge.py --panic`（`control.panic(7)`）与 `test_lifecycle`
  第 4 段：退出码 0、panic 走主循环收尾、自动重启 3 次后达上限停止。修复前该项在
  桌面上直接 0xC0000005。
- 新增 `test_modrange.py`（maps 解析自测 + 真实区间非零）。
- `test_savestate` S1–S10 全绿；`test_buttons` 16/16；`test_lifecycle` 5/5。
- 矩阵 7/7，7 个 zip 的 `.info` 均为 0.2.14。

### 真机待验（v0.2.14）

1. **先重新存一次档**（≤v0.2.13 的跨会话档 `modBase=0`，必被 `-13` 拒）
2. 存档 → 退出 RetroArch → 重开 → 加载，确认能接着玩
3. 故意让游戏崩一下（或按 Reset），确认画面能自己回来，而不是冻住
4. 日志自查：启动横幅 `core version 0.2.14`、trace 里
   `[mod] module range base=… (ok)`、读档后 `[snap] vm resumed after load`
