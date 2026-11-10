# 真机验收记录

> **v0.3.0（2026-11-03）范围收敛**：核心移除即时存档/读档与快进/慢放，
> 只做官方模拟器同等的基本功能。下文 v0.2.10–v0.2.15 各节记录的存档类
> 问题（暂停握手、跨会话地址、固定基址、PANIC 840 等）随功能移除而**整体
> 作废**，仅作历史留档；真机验收只需覆盖运行/重置/卸载/按键/音频。
>
> **v0.3.1（2026-11-03）只支持 64 位前端**：32 位目标（armhf / armeabi-v7a）
> 已整体移除，不再构建也不发布。原因：卡带镜像是 PXT64，官方 pxt 编译器没有
> 32 位镜像输出，runtime 的 PXT32 镜像补丁是上游未实现的 TODO，32 位核心
> 装得上但跑不了任何卡带。详见 README「只支持 64 位前端」，实测见下文
> 2026-11-03 节。

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
- 架构确认：`uname -m` 必须为 `aarch64`。**本核心不提供 32 位版本**——armv7l
  等 32 位系统请升级到 64 位前端，或换设备。原因与实测见 README
  「只支持 64 位前端」。

### 待量化 / 待覆盖

1. ~~**帧率与占用**~~：已在 2026-11-03 用两台真机量化（见该节）。
2. ~~**RK3326**~~：已测（R36S / RK3326，2026-11-03 节，满速 cubeland 66fps）。
3. ~~**armhf（32 位）**~~：已上真机并得结论——**跑不了**。不是性能问题，
   是 PXT64 镜像与 32 位 VM 的格式不兼容（官方编译器无 32 位输出，上游
   PXT32 补丁是 TODO）。32 位已从发版矩阵移除，见 README。
4. **Android** 两个包未上机（其中 armv7 已随 32 位一并废弃）。

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
- ~~32 位 armv7 产物未上真机~~：已测（2026-11-03，结论=跑不了，见该节）。


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

## 2026-11-04（真机反馈 → 修复）：v0.2.15 加载存档"偶发成功 / cube-land 异常"

### 真机现象
- jumpy 偶尔几次存读档成功，cube-land 加载异常。
- 日志两处「取不到固定地址」：`[heap] 取不到固定地址 0x2000000000`、
  `[img] 取不到固定镜像地址（GC_BASE+1MB）` → 堆/镜像每进程随机地址 →
  下次打开必「地址不符」被 -10 拒。
- 同会话 load 成功后又 `PANIC 840 (errno=17 File exists)` →
  `vm thread gone unexpectedly -> auto restart (1/3)` 把加载状态吞掉。

### 根因
`core---vm/mca_fixed.cpp` 原 `mcaFixedMap` 只钉死 `GC_BASE=0x2000000000` 单地址。
RK3566/ArkOS（aarch64）上该地址拿不到（`MAP_FIXED_NOREPLACE` 被占或不被满足），
GC 堆 / 镜像 / 单例三块固定区全部退化成随机会址，跨会话快照（v2，依赖三者地址
可复现）彻底失效。PANIC 840 是「非固定地址布局」的副症状（本地 cube-land 同会话
load 跑 700/1600 帧均正常，无法本地复现），随固定基址稳定应一并消失。

### 修法
「钉死单地址」→「候选基址列表 + 整体探测」：
- 新增 `mcaTryMapRaw` / `mcaUnmapRaw` / `mcaMapAt` / `mcaChooseBase`。
- 候选顺序：先 0x2000000000（原地址，Windows/本机/支持的设备行为不变），失败再试
  0x20000000 / 0x40000000 / 0x80000000 / 0x100000000 / 0x200000000 / 0x400000000 /
  0x800000000 / 0x4000000000 / 0x8000000000。
- 同一设备/固件下第一个「整体能占住」的地址是确定的（MAP_FIXED 不受 ASLR 影响，
  低地址区通常空闲）→ 跨进程一致 → 跨会话恢复成立。
- 不动快照格式、不动 same-session 逻辑、不动模块重定位。
- 本地仿真开关 `MCA_SIM_FALLBACK=1`（env）：跳过候选 #0，强制走兜底地址，验证
  兜底路径下跨进程基址仍确定。

### 验证
- `test_savestate` S1–S10 全绿（S5 跨进程、S6 跨基址重定位）；
  `MCA_SIM_FALLBACK=1` 下 S5 仍过，trace 实测 `[fix] 固定基址选定 0000000020000000
  （候选 #1…）`，堆 0x20000000、镜像 0x20100000，符合设计。
- `test_modrange.py` 全过。
- `kCoreVersion` → 0.2.15，`.info` `display_version = "0.2.15"`（已验证）。

### 真机待验（v0.2.15）
1. **先重新存一次档**（≤v0.2.14 的跨会话档基址基于 0x2000000000，必被 -10 拒）。
2. jumpy / cube-land 各跑：存一档 → 退出 RetroArch → 重开 → 加载，确认能接着玩。
3. 日志首行看 `[fix] 固定基址选定 …`：出现即已抢到固定基址（跨会话成立）；
   若打「退回随机分配」说明全部候选被占，跨会话仍退化，需换更大候选表。
4. 若仍报 `PANIC 840`：抓完整 trace（看 `[snap]` 段 + 是否还有「取不到固定地址」），
   这是本修复的推断项，需新证据。

## 2026-11-03：两台真机 v0.3.0 全量回归 + 32 位结论

### 设备

| | RG353M | R36S（clone） |
|---|---|---|
| SoC | RK3566（4×A55 @1.8GHz） | RK3326（4×A35 @1.3GHz） |
| RAM | 2GB（1.9GB 可用） | 918MB |
| 系统 | ArkOS / aarch64 / Python 3.7.5 | Debian 13 / glibc 2.41 / aarch64 |
| 连接 | `ssh rg353m`（192.168.1.130） | `ssh r36s`（192.168.1.53） |

> R36S 的 `/home/ark` 原本是 0777，被 sshd `StrictModes` 拒掉公钥（密码能进、
> 免密静默失败）。已改成 755。

### v0.3.0 功能回归（两台一致通过）

| 项 | 结果 |
|---|---|
| ping 300 帧 | 正常，148 独立帧，无 panic |
| 重置（SELECT@120） | 清理 → 新 VM 线程 12ms 就绪 → 跑完；trace 有 `new vm thread created` |
| 换游戏 ping→jumpy | 第二张卡带正常出帧 |
| panic 1007 自愈 | 自动重启 1/3、2/3、3/3 后按设计放弃，无死锁 |
| 音频 | audio_probe 峰值 8160 |
| 真 RetroArch 前端 | 加载核心 → VM 启动 → 主循环 → SIGTERM 干净退出（临时配置 + null 驱动，不碰用户 cfg） |

### 性能（`--turbo` 满速，Python 宿主含自身开销）

| 游戏 | RG353M (RK3566) | R36S (RK3326) |
|---|---|---|
| ping | 361 fps | 158 fps |
| jumpy | 240 fps | 95 fps |
| cubeland | 166 fps | **66 fps** |

- 峰值 RSS：RG353M 11MB / R36S 13MB（含 Python 宿主）。
- 60fps 节拍下单核占用：RG353M 46%（含宿主），真实 RetroArch 更低。
- **R36S 是下限机型**：cubeland 只剩 1.1 倍余量，理论上单核会吃满；
  宿主节拍在 R36S 上本身只能到 ~40fps，真实 C 前端会好一些，需玩家实机手感确认。

### 32 位前端：结论是"跑不了"

两台都装了 `retroarch32` + 32 位核心（armhf zip 直接可用，glibc 2.17 基线向下兼容）：

- 前端能列出核心、能加载，日志也打 `VM started`；
- 但 **VM 线程 3ms 就退出**（trace `vm ready (online=0 live=0)`），画面全黑；
- 日志 `validation error 1064`（boxed number 的 vtable 校验失败）。

根因在 `runtime/pxtapp/core---vm/verify.cpp`：

```c
#ifdef PXT32
    if (sect.type == SectionType::NumberBoxes) {
        return (state->error = "TODO: NumberBoxes");   // 上游从未实现
    }
#endif
```

官方 pxt 编译器只产 PXT64 镜像，没有任何产出 32 位镜像的开关；它唯一的
32 位适配路径就是这个"把镜像就地补丁成 32 位原生布局"的函数，而上游把它
留成了 TODO，且调用方 `vmStartCore` 不检查返回值 → 镜像原样装载 → vtable 是
垃圾 → 1064。

处置（v0.3.1）：32 位**整体移除**——发版矩阵、GitHub Actions、buildbot 列表
里都不再有 32 位目标，`core/jni/Application.mk` 只列 `arm64-v8a`。核心侧也
不加任何运行时判断（既然不再产出 32 位构建，判断就是死代码）。除非上游把
`vm_patch_image` 的 `"TODO: NumberBoxes"` 补完，否则 32 位前端一律不支持。

## 2026-11-03（第三台）：RG353V 全新安装

| 项 | 值 |
|---|---|
| 硬件 | RG353V，RK3566（4×A55，上限 1.8GHz），963MB RAM |
| 系统 | Ubuntu 19.10（aarch64，kernel 4.19.172），Python 3.7.5 |
| SSH | `ssh rg353v`（192.168.1.77，ark，已装本机 ed25519 公钥） |
| 安装位置 | 核心 `~/.config/retroarch/cores/`（与 `libretro_directory` 一致），游戏 `~/makecode/` |
| core | `makecode_arcade_linux-arm64.zip` v0.3.1（`display_version=0.3.1`，aarch64） |
| 结果 | 全新部署成功，20 张 `.pxt64` 已推送 |

### 实测

- 满速吞吐（ping，2000 帧）：RG353V **347 fps** / RG353M **341 fps** —— 两台
  RK3566 同级。注意别拿宿主打印的"游戏真实帧率 ≈ 178 fps"和挂在总帧/耗时上
  的数（347）直接比，这是两个口径，我第一轮就比错了。
- Blocky_Boss_Battle：正常节拍 48.8 fps 实际出帧、音频峰值 1264（有声）。
- Strawberry_Slam：固定第 5133 tick `PANIC 980`，与 RG353M / R36S / 桌面一致。

### 顺带发现（已于 v0.3.2 修复，见下一节）

上面这个 PANIC 980 之后**进程直接崩了**（Windows 退出码 `0xC0000005`
访问违例；两台掌机上同样在 panic 输出后立刻中断，没有收尾统计）。也就是说
"panic 只是画个横幅然后自动重启"这条降级路径对 980 这条路径**没有生效**。

已排除的：

- 不是 `drawPanic` 的锅——它是空实现（`runtime/pxtapp/core---vm/platform.cpp`
  里的 `// TODO`）。
- 不是 `dmesg_flush`——`LOG_TO_FILE` 未定义时它是空函数；日志里那行
  `errno=2 No such file or directory` 只是之前遗留在 errno 里的值。
- 不是设备相关——桌面、RG353M、RG353V 崩在同一个位置。

怀疑方向：这个 panic 来自 `verify.cpp` 的 `failed type check`，也就是
**VM 镜像校验阶段**，不在 fiber 里；而 v0.2.14 的"panic 后从 mainRunLoop
收尾退出"补丁是按 fiber 内 panic 设计的。非 fiber 路径走 `target_panic` →
`systemReset()` 时，VM 状态正处于半执行态，收尾可能用到已释放/未初始化的结构。

## 2026-11-03（v0.3.2）：panic 带崩前端 —— 已修 + 三机统一实测

### 修复

上一节记的"未修"问题定位并修掉了，同时发现并修掉一个同类隐患：

1. **panic 后进程崩溃**（0xC0000005）。根因不是 verify 阶段（上一节的怀疑
   方向错了）：panic 发生在 fiber 内的 `asRefCollection()`——游戏把 undefined
   当数组用。上游 pxt 的约定是 `soft_panic()` **不返回**（ESP32 上直接 abort），
   VM 里的 C++ 全按这个约定写：`if (!isPointer(v)) failedCast(v);` 之后紧跟
   `getVTable((RefObject*)v)`。v0.2.14 把 panic 改成"置标志就返回"之后，这些
   调用方拿着 NULL/undefined 继续解引用，进程当场挂掉，RetroArch 跟着死。
   修法：panic 之后**不许 return**——在 mainRunLoop 装 `setjmp` 回弹点，
   `soft_panic`/`target_panic` 在 VM 线程上直接 `longjmp` 过去，由循环顶的
   `if (panicCode)` 分支做原有的 cleanup + 正常线程退出（前端自动重启）。
2. **VM 的 try/catch 同样带崩前端**（0xC0000028 STATUS_BAD_STACK）。同一根
   子的另一半：mingw-w64 的 `longjmp` 在 x86_64 上走 SEH（RtlUnwindEx），
   逐帧回退必须有 .pdata，而本工程按 `-fno-unwind-tables` 编译——`longjmp`
   自己就崩。修法：Windows 目标打开 `-funwind-tables
   -fasynchronous-unwind-tables`（体积 +22KB），panic 回弹点与 VM 异常回跳
   （FiberContext::loopjmp）共用这套机制，恢复标准 `setjmp/longjmp` 写法。

验证（桌面 win64 + 三台真机）：

- Strawberry_Slam 6000 帧：panic 980 → 自动重启 ×3 → 到上限后放弃，**进程
  退出码 0**，前端继续出帧直到 unload。trace 完整走完 cleanup 0/6→6/6。
- 新增异常样本（`tools/fixtures/trycatch-main.ts` 编译的 trythrow.pxt64）：
  throw → catch → 继续跑，600 帧正常出画面。
- 20 张语料全量回归：19 ok + 1 panic（数值与修复前逐张一致，无行为变化）。

### 三台真机对照（同一构建 aarch64 v0.3.2，2026-11-03 晚）

| 项 | RG353M | R36S | RG353V |
|---|---|---|---|
| SoC | RK3566 4×A55 @2.0GHz | RK3326 4×A35 @1.3GHz | RK3566 4×A55 @1.8GHz |
| RAM | 2GB | 918MB | 963MB |
| 系统 | Ubuntu 19.10 / kernel 4.19.172 / glibc 2.30 | Debian 13 / kernel 4.4.189 / glibc 2.41 | Ubuntu 19.10 / kernel 4.19.172 / glibc 2.30 |
| Python（跑宿主脚本用） | 3.7.5 | 3.13.5 | 3.7.5 |
| SSH 别名 | `ssh rg353m`（192.168.1.130） | `ssh r36s`（192.168.1.53） | `ssh rg353v`（192.168.1.77） |
| 核心目录 | `~/.config/retroarch/cores/` | 同左 | 同左 |
| 游戏目录 | `~/makecode/`（20 张 .pxt64） | 同左 | 同左 |
| 旧核心备份 | `*.bak-0.2.15`、`*.bak-0.3.1` | `*.bak-0.3.1` | `*.bak-0.3.1` |

性能（`--turbo` 满速 1200 帧、墙钟计时；同口径同卡带，可直接横向比）：

| 卡带 | RG353M | R36S | RG353V |
|---|---|---|---|
| ping（最轻） | 284 fps（4.7×） | 150 fps（2.5×） | 300 fps（5.0×） |
| Blocky_Boss_Battle（中） | 203 fps（3.4×） | 92 fps（1.5×） | 211 fps（3.5×） |
| arcade-cube-land（最重） | 146 fps（2.4×） | **65 fps（1.08×）** | 150 fps（2.5×） |

（括号 = 相对 60fps 的余量。R36S 跑 cubeland 依然只有 1.08 倍余量，是全系列
下限；其余组合余量 1.5× 以上。）

60fps 节拍下（宿主脚本 16ms 节拍，宿主本身有 Python 开销，节拍略低于 60）：

| 指标 | RG353M | R36S | RG353V |
|---|---|---|---|
| ping 实际节拍 / 单核占用 | 49.5 fps / ~26% | 38.9 fps / ~42% | 48.6 fps / ~27% |
| Blocky 实际节拍 / 单核占用 | 50.1 fps / ~35% | 39.2 fps / ~54% | 49.4 fps / ~37% |
| 峰值 RSS（turbo 采样） | 11.7–12.9 MB | 14.3–18.6 MB | 11.5–12.9 MB |

功能回归（三台一致）：

| 项 | 结果 |
|---|---|
| ping 600 帧 | 295 独立画面，正常 |
| Strawberry_Slam 6000 帧 | panic 980 后自动重启 ×3，宿主存活（exit 0） |
| trythrow（异常样本）600 帧 | 39 独立画面，throw/catch 路径正常 |
| 音频 | Blocky_Boss_Battle 有声（此前 RG353V 实测峰值 1264） |

### 口径备注（避免再踩）

- "游戏真实帧率 ≈ N fps"是 VM 出帧率（受游戏自身逻辑限制），**不是**设备
  性能；横向比较一律用 `--turbo` 的墙钟吞吐。
- "实际节拍"受 Python 宿主的 16ms sleep 粒度影响（≈50 fps 封顶），不等于
  RetroArch 实机帧率；只用来确认"没掉到远低于节拍"。
- R36S 的 RSS 偏高是 kernel 4.4 的统计口径差异，不代表多吃 6MB。

## 2026-11-04：30 分钟稳定性长跑（三台并行）

### 设计

- 核心：v0.3.2（aarch64，5,152,368 字节），三台同一份产物。
- 每台连续 4 段，段间不重启宿主：`arcade-cube-land` 20000 帧 →
  `Blocky_Boss_Battle` 20000 帧 → `ping` 20000 帧 → `Strawberry_Slam`
  6000 帧（panic 场景收尾）。60fps 节拍（非 `--turbo`），即真实游玩节奏。
- 每 20 秒采样一次：`VmRSS` / `VmHWM` / `utime+stime`（jiffies）/ SoC 温度 /
  load1 / 日志里的帧进度。进度不增即判卡顿。
- 脚本与原始采样：`docs/stability-2026-11-04/run.sh`、`rg353m.csv` /
  `r36s.csv` / `rg353v.csv`。

### 结果

| 设备 | 段 | rc | 时长 | 预热后 RSS | 平台期斜率 | 温度 | 停滞 |
|---|---|---|---|---|---|---|---|
| RG353M | cubeland | 0 | 6m27s | 11.4→11.4 MB | +0.00 MB/min | 41–45℃ | 0 |
| RG353M | blocky | 0 | 6m50s | 11.2→11.2 MB | +0.00 | 41–43℃ | 0 |
| RG353M | ping | 0 | 6m42s | 11.5→11.6 MB | +0.03 | 41–43℃ | 0 |
| RG353M | panic | 0 | 1m59s | — | — | 41–43℃ | 0 |
| R36S | cubeland | 0 | 8m57s | 14.0→14.0 MB | +0.00 | 46–50℃ | 0 |
| R36S | blocky | 0 | 8m39s | 14.0→14.0 MB | +0.00 | 46–49℃ | 0 |
| R36S | ping | 0 | 8m39s | 14.1→14.5 MB | +0.06 | 46–50℃ | 0 |
| R36S | panic | 0 | 2m30s | — | — | 46–48℃ | 0 |
| RG353V | cubeland | 0 | 6m31s | 11.4→11.4 MB | +0.00 | 41–45℃ | 0 |
| RG353V | blocky | 0 | 6m54s | 11.2→11.2 MB | +0.00 | 43–44℃ | 0 |
| RG353V | ping | 0 | 6m45s | 11.5→11.7 MB | +0.03 | 43–44℃ | 0 |
| RG353V | panic | 0 | 2m00s | — | — | 42–43℃ | 0 |

合计 12 段、198,000 帧、约 73 分钟机时（单台 22–29 分钟）。

节拍（各段统计行）：RG353M 48.9–51.6 fps、RG353V 48.4–51.2 fps、
R36S 37.2–40.1 fps——与 v0.3.2 功能测试一致，长跑后无衰减。

### 结论

1. **零崩溃**：12 段全部 `rc=0`，宿主进程从未异常退出，段间切换（上一段
   结束、下一段新进程）也没有残留状态问题。
2. **零泄漏**：去掉启动预热（约 90 秒，VM 堆与卡带装载）后，RSS 完全持平；
   平台期斜率 0.00–0.06 MB/min，最长的 8 分钟段累计漂移 < 0.5 MB，属采样噪声。
   段间基线差异（cubeland 11.4 / blocky 11.2 / ping 11.5）反映的是各游戏自身
   堆占用，不是累积。
3. **零卡顿**：所有相邻采样点之间帧进度都在增长，停滞计数全为 0——没有出现
   "进程活着但 VM 卡住"的中间态。
4. **panic 自愈在长时运行后依然有效**：Strawberry 段 6000 帧内 `PANIC 980`
   出现 4 次（tick 5133 / 10266 / 15400 / 20533），即首次 panic 后自动重启
   3 次、到上限停止重试，**此后仍持续出帧直到 6000 帧跑满**，退出码 0。
   这正是 v0.3.2 想达到的行为，且比单次 6000 帧测试更严格（此时已连续跑过
   一小时负载）。
5. **温度**：RG353M 41–45℃、RG353V 42–45℃、R36S 46–50℃。R36S 高 5℃
   与其无散热设计相符，距 SoC 降频阈值（~80℃）仍有很大余量。

### 口径备注

- 两台 RK3566 的 RSS 基线 11.2–11.7 MB，R36S 14.0–14.9 MB，与上一节一致；
  除已记的内核统计差异外，另一种可能是 R36S 上的 Python 3.13 宿主本身基线
  更大（另两台是 3.7）——两者尚未分离，比较核心内存时以同机型前后对比为准。
- `r36s` / `rg353v` 上运行的是采样脚本第一版（无 `VmHWM`、CPU 用 `ps` 平均
  值），`rg353m` 是第二版（读 `/proc/<pid>/stat` 差分）。RSS、进度、温度
  三列口径相同，结论不受影响；后续统一用第二版。

### 长跑附带发现：ping 卡带自身有确定性 panic

ping 段 20000 帧跑过约 11 分钟后，三台真机都在 **tick 41683** 报
`PANIC 980`（cast-from-undefined），之后每 ≈41683 tick 一次（83400 /
125083 / 166800）——三台真机与桌面 win64 **同 tick、同错误码、同末帧
hash**（`032c69da…`，抓帧确认就是 PANIC 红色横幅画面）。

结论：这是 ping 游戏自身的确定性 bug（600 帧冒烟根本跑不到那个 tick），
与核心、与 ARM 平台都无关；反过来它恰好证明 panic 处理路径在长时运行后
依然严格一致：横幅 → 自动重启 ×3 → 到上限停机（画面定格在 PANIC 红屏），
宿主全程存活、rc=0。已把该行为记录进 `tools/fixtures/corpus.json` 的
ping 条目，长跑语料判读时 panic 属预期。

## 2026-11-04：全卡带随机按键"边玩边测"（三台并行 × 21 张 × 7000 帧）

### 设计

在上午 30 分钟空跑长跑的基础上，这轮给宿主新增了 `--random-input=1234:18`：
每 18 帧重抽一次按键——方向盘 55% 概率按住（其中 1/4 是斜向双键）、
ABXY/LR 45% 概率（其中 1/4 双键同按），模拟玩家乱按。种子固定，三台输入
序列完全一致、可复现。

- 每台依次跑同一份 21 张卡带清单，每张 7000 帧（≈2.3–3 分钟游戏时间），
  60fps 节拍；单台 50 分钟（RK3566）/ 61 分钟（R36S），合计约 147,000 帧。
- 每张记录：退出码、独立画面数、节拍、PANIC 次数、末帧 BMP。
- 产物：`docs/playtest-2026-11-04/`（三台逐卡带 CSV + 资源采样 CSV +
  21 张末帧拼图 `wall.png` + 脚本 `run.sh`）。

### 结果

- **63 段（21×3）全部 rc=0**，无一崩溃、无花屏、无黑屏死机。
- **三台独立画面数逐张一致**：19/21 张三台完全相同，ping 差 2 帧、
  Strawberry 差 3 帧（来自各机节拍差对 panic 时机的影响）——确定性种子下
  VM 行为跨架构逐帧可复现。
- PANIC 只出现在两个已知有游戏自身 bug 的卡带上：ping ×2（tick 41683
  周期，见上节）、Strawberry_Slam ×4（tick 5133 周期），三台次数一致；
  panic 后自动重启 ×3 → 停机 → 宿主存活，全部符合预期。
- 末帧拼图（`wall.png`）人工核验：正常游戏画面 + 一批 GAME OVER（乱按
  乱玩死了，恰好证明输入响应与游戏逻辑正常），没有异常帧。
- 节拍：RG353M 48.9–53.0 fps、RG353V 48.2–52.9 fps、R36S 37.3–46.1 fps，
  与短测/长跑一致；资源采样无泄漏迹象，温度区间与上午长跑相同。

### 结论

「能玩 + 乱按」这条更接近真实游玩的路径同样干净：输入处理、游戏逻辑、
panic 降级、音频、资源占用全部稳定。两个 panic 卡带的行为与单测完全一致，
没有因为长时运行或大量按键而出现新的失效模式。

## 2026-11-04（晚）：8 台跨机型性能对照 + DDR 调频发现

### 背景

下午扫局域网时新接入 6 台 ArkOS 掌机（详见 `docs/perf-2026-11-04/nodes.txt`），
连同原有的 rg353m / rg353v 共 8 台，全部刷成**同一份 v0.3.2 发布版**
（arm64 核心 md5 `5a295ebf…`，5,169,256 字节；`.info` `display_version = "0.3.2"`），
卡带统一 21 张。这给了一个难得的机会：用同一二进制、同一语料、同一宿主脚本，
看同 SoC 不同机型 / 不同单元的差异到底有多大。

### 方法

- CPU 调频统一 `performance`（原状 ondemand/interactive 混用，会污染结果），
  实测负载下均稳定在各自最高频（RK3566 1992MHz / RK3326 1512MHz），无降频。
- DDR 保持系统默认 `dmc_ondemand`（见下面第 4 条，这本身就是个发现）。
- 3 张卡带（ping / Blocky_Boss_Battle / arcade-cube-land）× 1200 帧 × 3 次，
  取中位数；全部 `--turbo`（不限速），指标是**墙钟吞吐** fps，不是"游戏真实帧率"。
- 宿主脚本统一到同一版本（rg353m/rg353v 原先的旧副本已更新，否则两套脚本
  口径不同，rg353m 第一轮被压低约 10%，已重测覆盖）。
- 脚本与原始数据：`docs/perf-2026-11-04/`（`bench.sh`、`bench_results.csv`、
  `analyze.py`、`membw.c`）。

### 结果（吞吐 fps，中位数）

| 设备 | SoC | ping | Blocky | cubeland | cubeland 余量 |
|---|---|---|---|---|---|
| RG353M | RK3566@2.0 | 364 | 249 | 164 | 2.73× |
| RG353V | RK3566@1.8 | 374 | 256 | 166 | 2.76× |
| RK2023 | RK3566@2.0 | 372 | 260 | 177 | 2.95× |
| RGB30 #1 | RK3566@2.0 | 447 | 310 | 205 | 3.42× |
| RGB30 #2 | RK3566@2.0 | 426 | 287 | 189 | 3.15× |
| RGB30 #3 | RK3566@2.0 | 388 | 265 | 179 | 2.98× |
| RG351MP #1 | RK3326@1.5 | 193 | 136 | 84 | 1.41× |
| RG351MP #2 | RK3326@1.5 | 196 | 138 | 85 | 1.42× |

- RK3566@2.0 组 cubeland 中位 **179 fps**，极差 164–205（25% 离散）。
- RK3326@1.5 两台 **84 / 85 fps，差 1%**。
- RK3566@2.0 / RK3326@1.5 = **2.11×**，与两代微架构 + 频率差相符。

### 结论

1. **八台全部远高于 60fps**。最慢的 RG351MP（RK3326@1.5，也就是给 R36S 那档
   SoC 留余量的机型）在最重的 cubeland 上仍有 **1.41× 余量**；对 60fps 节拍而言，
   这批硬件没有一台是瓶颈。
2. **RK3326 两台几乎完全一致（84/85）**，说明测试方法本身可重复；因此 RK3566 组
   那 25% 的离散是**设备差异**，不是测量噪声。
3. **离散与 DRAM 随机访存延迟负相关**（满频实测）：

   | 设备 | 随机访存延迟 | cubeland |
   |---|---|---|
   | RGB30 #1 | 38.7 ns | 205 |
   | RGB30 #2 | 40.7 ns | 189 |
   | RK2023 | 41.0 ns | 177 |
   | RG353M | 43.3 ns | 164 |

   解释器是"取指-取字节码-访存"的强延迟敏感负载，三四纳秒的 DDR 延迟差就能
   放大成 20%+ 的吞吐差。这属于颗粒/训练/走线的单元差异，**核心侧没有可优化项**
   （同一二进制在最快的 RGB30 上就是最快，说明代码没有机型相关分支）。
4. **DDR 调频是免费的 10%**：RK3566 默认 `dmc_ondemand` 在本负载下**根本不升频**，
   实测 DDR 峰值一直停在 324MHz（`dmc_ondemand` 只看 GPU/NPU 负载信号）。
   手动 `echo performance > /sys/class/devfreq/dmc/governor` 把 DDR 拉到 1056MHz：

   | 设备 | cubeland | Blocky |
   |---|---|---|
   | RG353M | 164 → 176（+7.5%） | 249 → 282（+13%） |
   | RGB30 #1 | 205 → 227（+10.7%） | 310 → 352（+13.7%） |
   | RG351MP #1 | 84.5 → 87（+3%） | 136 → 139（+2%） |

   RK3326 的 DDR 上限本来就 666MHz 且常驻高频，所以几乎无增益；RK3566 则普遍
   +7~14%。这条对最终用户有实用价值（不需要改核心，只是系统调频），
   但要写进文档得先确认各家固件是否持久化该设置。
5. **口径提醒**：今天更早记录的 rg353m 146 fps（cubeland）是"ondemand + 旧宿主
   脚本"的组合，与本次 164 不可直接比较；横向比较一律以本表为准。
6. 实测 `memcpy` 带宽（12.4 / 11.0 / 12.8 GB/s）与吞吐**不成正比**，说明瓶颈是
   **延迟**而非**带宽**——这也是为什么 DDR 升频带来的增益（+10%）没有延迟差异
   带来的离散（25%）那么大。

---

## Lakka (L4T Linux) @ Nintendo Switch OLED — 2026-11-07

设备：`ssh root@192.168.1.61`（Lakka / L4T，内核 4.9.140 aarch64，Switch OLED
Mariko，6 核 max 2.295GHz，3.9GB 可用内存）。SD 卡 vfat 以 **ro** 挂载在
`/flash` 与 `/storage` 同一分区，核心在 `/storage/cores/`，卡带在
`/flash/roms/pxt64_collections/`（20 张）。测试宿主放 `/tmp`（tmpfs，可执行）。

**Windows 上密码登录 Lakka 的办法**：Lakka 只开密码认证（root/root）。Git Bash
的 ssh 需要原生 askpass exe（cmd 脚本会静默失败）：

```bash
# 用 zig 编一个 20 行 C 程序：打印 $ASKPASS_PW
ASKPASS_PW=root SSH_ASKPASS="C:/tmp/askpass/askpass.exe" SSH_ASKPASS_REQUIRE=force \
  ssh -o PreferredAuthentications=password -o NumberOfPasswordPrompts=1 root@192.168.1.61 '...' < /dev/null
```

（本机没有 sshpass/plink，pip 装 paramiko 也不通；zig cc -target
x86_64-windows-gnu 编 askpass 是最省事的路。）

### 结果：20/20 卡带全绿

每张 300 帧（`tools/libretro_host.py` + `--data-dir=/tmp/sw_N`）：

| 判定 | 数量 |
|---|---|
| OK（rc=0、无 PANIC、有统计行） | **20** |
| CRASH / PANIC / NO-STAT | 0 |

样例数据（300 帧，60fps 节拍）：

| 卡带 | 独立帧 | 画面非黑峰值 | 音频峰值 |
|---|---|---|---|
| Blocky_Boss_Battle | 128 | 9% | **1264**（与真机一致） |
| Bunny_Hop_ | 46 | 100% | 4080 |
| Jewel_Raider | 5 | 98% | 16376 |
| Cave_Explorer | 150 | 100% | 0 |
| Space_Destroyer | 150 | 6% | 0 |

独立帧低的（GalgaMulti 8、Mouse_Adventure 2、cube-land 8）都是**停在标题画面
等输入**，非性能问题：整个循环始终稳定 58~60 fps 节拍。

### 生命周期

- **换卡带**（`--switch`，Space_Destroyer → Cave_Explorer）：第二张正常跑 150 帧；
- **dlclose**（卸载核心库）：`dlclose OK`，无崩溃；
- **重置**（`--press=SELECT --press-at=120`）：`mca_trace` 显示
  `HOST RESET begin/end` + `initRuntime return (restart path)`，重置后继续出帧；
- **60 秒长测**（Cave_Explorer 3600 帧）：59.6 fps 节拍、1077 独立帧、
  `unload + deinit` 干净退出，无泄漏迹象。

**结论：v0.3.3 的 `makecode_arcade_lakka-arm64.zip` 在 Switch Lakka 上功能完整**
（20 张卡带全部可跑；图形、音频、重置、换卡、卸载全通过）——与八台 ARM 掌机
的结论一致，Lakka 这条线闭环。

---

## 三台 RGB30 换装 Debian 13 (trixie) — 2026-11-07

设备：`rgb30a` 192.168.1.26 / `rgb30b` 192.168.1.85 / `rgb30c` 192.168.1.101
（原 ArkOS 已刷成 Debian 13 trixie + EmulationStation-fcamod，三台同一镜像，
host key 相同）。RK3566 4 核 1.992GHz，966MB 内存，Python 3.13.5。

### 部署（三台相同）

- v0.3.3 `makecode_arcade_linux-arm64.zip`（glibc）核心 → `~/.config/retroarch/cores/`
- 20 张卡带 → `/roms/makecode/`
- ES 注册：`/etc/emulationstation/es_systems.cfg` 插入 makecode 系统（130 系统，
  字母序 macintosh 前，命令同模板 `sudo perfmax %GOVERNOR% %ROM%; nice -n -19
  /usr/local/bin/retroarch -L .../makecode_arcade_libretro.so %ROM%; sudo perfnorm`），
  重启 emulationstation.service 生效。已重启，XML 校验通过。
- SSH：公钥已装（三台），别名 rgb30a/b/c 恢复可用。
- **前端验证（用户实机确认）**：ES 系统列表出现 "MakeCode Arcade" 分类，
  点进去可正常启动游戏 —— 全链路（ES → perfmax → retroarch -L → 核心）通。

### 全量冒烟（每台 20 张 × 300 帧，60fps 节拍）

**20/20 全绿 × 3 台**，rc=0、无 PANIC。关键指标三台逐位一致，且与
Lakka@Switch / 八台掌机历史数据一致（Blocky 音频峰值 1264、Jewel 16376、
Bunny 4080、Secure_the_river 16128）。

### 吞吐（turbo，governor=performance + DDR performance 1056MHz，2000 帧）

| 卡带 | rgb30b | rgb30c | rgb30a（热限频） |
|---|---|---|---|
| arcade-cube-land | **186.7** | **186.6** | 164.0 |
| arcade-jumpy-platformer | 283.3 | 285.3 | 243.8 |
| Blocky_Boss_Battle | 302.1 | 305.3 | 252.6 |
| Space_Destroyer | 390.1 | 395.4 | 332.9 |

与旧 ArkOS RGB30（cubeland 205/189/179）同档，无退化。

### 生命周期（三台全过）

换卡带（--switch）+ dlclose、重置（SELECT@120，trace 显示
`HOST RESET begin/end` + `initRuntime return (restart path)`）、
60 秒长测（3600 帧 56.5fps 节拍 1077 独立帧干净退出）。

### rgb30a 散热异常（待处理）

空载 soc 85°C（b/c 65~67°C），CPU max 被热限到 1.4~1.8GHz → 吞吐低 12%。
被动散热机型不该空载 85°C，怀疑导热垫接触/壳体问题，或刷机后温控策略变化。
建议关机凉透后复测温度；若仍异常查散热装配。

### SSH 开机自启（本镜像的坑，2026-11-07 修）

- `/opt/system/Enable Remote Services.sh` 里 `sudo systemctl enable ssh` 那一行是
  **被注释掉的**，脚本只执行 `sudo systemctl start ssh.service`；`Disable Remote
  Services.sh` 同理只 stop 不 disable。→ 重启后 SSH 静默失联（rgb30c 当天早上
  22 端口 refused 就是这个原因，与硬件无关）。
- 修法：`sudo systemctl enable ssh.service`。三台已做并验证：冷启动后日志
  `Starting ssh.service → Server listening on 0.0.0.0 port 22 → Started`，
  PID 921/925，`/run/sshd` 由 unit 的 `RuntimeDirectory=sshd` 在开机时重建。
- **锁死状态（会踩第二次）**：sshd 运行中被 `systemctl stop ssh`（该 unit
  `KillMode=process`，子进程不跟随退出）会留下陈旧状态 —— 22 端口仍能应答
  `SSH-2.0-OpenSSH_*` banner，但 KEX 阶段被 RST；此时 `systemctl start` 因端口
  占用以 255 退出（`sshd -t` 却通过），点多少次“启用远程服务”都无效。
  只能整机重启恢复，重启后 RuntimeDirectory 重建即正常。
- **采样污染（重要）**：该镜像 `/etc/xdg/systemd/user/default.target.wants/
  fluidsynth.service` 是全局启用的，任何登录（含 SSH）都会拉起 `fluidsynth -is
  -r 48000 -z 512 default-GM.sf3`，实测吃 50~57% 单核 + 172MB 内存，能把 SoC
  从 62°C 抬到 77°C。远程采样前必须先
  `systemctl --user stop fluidsynth mpris-proxy`，否则数据全废。

### 待机温度复测（2026-11-07 上午，屏保黑屏 + 无登录会话）

| 设备 | SoC 空载 | 备注 |
|---|---|---|
| rgb30b | 48.9~51.2°C | 正常 |
| rgb30c | 55.0°C | 正常 |
| rgb30a | 61.7~67.5°C（冷启动 1 分钟即 67.5） | **异常**，比同负载 b 高 10~12°C |
| rg353m（ArkOS） | 51.9~53.8°C | 正常 |
| rk2023（ArkOS） | 53.8°C | 正常 |
| clone-r36s（RK3326 对照） | 50.8°C | 说明 50°C 级是这类机器的地板 |

CPU 86~94% idle、DDR 324MHz、无异常进程 → 不是软件跑飞；停掉 ES 只降 3°C。
`thermal_zone0` 降频触发点**按系统不同**（hyst 均为 2°C，critical 均 115°C）：
ArkOS / R36S Clone = passive **70°C**，RGB30 的 Debian 13 = passive **75°C**。
rgb30a 待机就贴着 70~75 这条线，这正是它吞吐低 12% 的直接原因。

补测（清理登录态服务后、开机 5 分钟稳态）：rgb30a 51.2°C vs b/c 46.7°C，差距收窄到
4.5°C。说明之前 +10~13°C 里有一部分是「刚重启 + ES 冷启动 + 探针叠加」的残留热，
但上一轮 perf 测试里 rgb30a 空载 85°C 仍无法用软件解释，重活下（游戏 60s）仍需复看。

### 关掉登录态常驻服务（2026-11-07）

三台 RGB30 有全局 user 级自启：`/etc/xdg/systemd/user/default.target.wants/
{fluidsynth,mpris-proxy}.service` —— 任何用户登录（含 SSH/TTY）都会拉起
`fluidsynth -is -r 48000 -z 512 default-GM.sf3`（实测 50~57% 单核 + 172MB）
和 mpris-proxy。已 `sudo systemctl --global disable fluidsynth.service
mpris-proxy.service`（三台）。

验证（全新 SSH 登录后）：`pgrep -x fluidsynth` / `mpris-proxy` 均为空，
`/etc/xdg/systemd/user/default.target.wants/` 与 `/etc/systemd/user/default.target.wants/`
均空，未做 mask（保留手动启动能力）。

清理后六台空载（10s 采样，ES 屏保黑屏）：

| 设备 | busy | SoC |
|---|---|---|
| rgb30a | 3.3% | 51.2°C |
| rgb30b | 3.3% | 46.7°C |
| rgb30c | 3.3% | 46.7°C |
| rg353m | 9.7% | 54.4°C |
| rg353v | 5.4% | 42.5°C |
| rk2023 | 4.9% | 50.6°C |

samba 六台均 `disabled/inactive`、无进程（ES 菜单「Enable Remote Services」
仍会临时 `start` 起来，重启即失效，属预期）。

## R36S Clone 两台（原 RG351MP 硬件）：喇叭无声 → 换 DTB 修好 — 2026-11-07

### 设备

| 节点 | 硬件 | 系统 | 正确的 DTB |
|---|---|---|---|
| `r36s-clone-a`（192.168.1.31，旧名 rg351mpa） | RG351MP | R36S-Clone 镜像（Debian 13 + **内核 4.4.189**） | `rk3326-r36s-type2-panel1-with-amp-linux.dtb` |
| `r36s-clone-b`（192.168.1.137，旧名 rg351mpb） | RG351MP | 同上 | 同上 |

### 现象

刷 R36S-Clone 镜像后**喇叭全无声**，耳机正常。ALSA 一切显示正常：
`aplay` 送出无报错、`Playback Path=SPK_HP`、音量 100%、PCM 跑到 RUNNING。

### 根因：镜像默认 DTB 没声明外部功放

镜像默认用 `rk3326-r36s-type1-panel5-linux.dtb`，其 `rk817 codec` 节点缺
外部功放声明的三个属性（与老系统能出声的 DTB 逐属性比对得出）：

| 属性 | 老系统（有声） | `type1-panel5`（无声） |
|---|---|---|
| `spk-ctl-gpios` | `<&gpio3 7 GPIO_ACTIVE_HIGH>` | **缺失** |
| `use-ext-amplifier` | 有（布尔） | **缺失** |
| `spk-mute-delay-ms` | `500` | **缺失** |
| `spk-volume` | `11` | `3` |

RG351MP 的喇叭挂在**外部功放**上，使能脚是 GPIO3_A7（gpio-103），要靠 codec
驱动按 `spk-ctl-gpios` 拉高。DTB 不声明 → 驱动只开 RK817 片内功放 → 外部功放
始终不使能 → 喇叭无声。耳机走 codec 内部 HP 通路，完全绕开外部功放，所以有声。

正确 DTB 文件名里的 **`with-amp`** 就是这个意思。

### 走过的弯路（别重踩）

1. **`hp-det-gpio` 极性**：克隆 DTB 是 `ACTIVE_LOW`、老系统 `ACTIVE_HIGH`，导致
   系统永远认为「耳机已插入」，喇叭被静音。我按老系统改成 `ACTIVE_HIGH`（单字节
   补丁，`hp-det` 标志位 1→0），重启后 `Headphone Jack=off` —— **但这只是必要条件，
   不是充分条件**，功放没使能照样无声。改极性后不要误判为「已修好」。
2. 停 ES 独占、`aplay -D hw:0,0` 直连、循环放音验证耳机通路，都只能证明
   **codec/I2S/DMA 正常**，不能证明喇叭通路正常。
3. 判断喇叭通路是否正常，最快的判据是看 DTB 里有没有 `use-ext-amplifier` +
   `spk-ctl-gpios`，而不是反复调 ALSA。

### 结论

`/boot` 里选 `rk3326-r36s-type2-panel1-with-amp-linux.dtb`，重启后喇叭正常。
以后重刷该镜像，**一律选带 `with-amp` 的那份 DTB**。

### 验证与现状（2026-11-07 11:40）

两台已切到 with-amp DTB 并重启，`/boot/boot.ini` 的 `load mmc X:1 ${dtb_loadaddr}`
直接指向该文件（a 用 `mmc 1:1`、b 用 `mmc 0:1`，各自 TF/eMMC 编号不同，正常）。
运行中 `/proc/device-tree/i2c@ff180000/pmic@20/codec` 已含
`spk-ctl-gpios = <0x68 0x07>`、`use-ext-amplifier`、`spk-mute-delay-ms`；
`/sys/kernel/debug/gpio` 出现 `gpio-103 | spk-ctl  out hi`（放音时拉高、空闲拉低）。
用户实机确认两台喇叭有声。

注意：`amixer -c 0 cget name="Headphone Jack"` 在**没插耳机**时仍报 `on`
（该 DTB 的 `hp-det-gpio` 是 `ACTIVE_LOW`，gpio-86 空闲读 lo），但外部功放照常
使能、喇叭有声 —— 所以这个 mixer 元素在本机型上不可作为「喇叭是否可用」的判据。

设备已改名（2026-11-07）：`r36s-clone-a`（192.168.1.31）、`r36s-clone-b`
（192.168.1.137），SSH 别名 `r36sclonea` / `r36scloneb`（旧别名 rg351mpa/b 保留兼容）。
设备内 `/etc/r36s-clone-info` 记录了镜像/DTB 信息。顺手修了恢复镜像后
`/home/ark` 又变回 777 导致 StrictModes 拒公钥的问题（改 755 + 重装公钥）。

### 2026-11-07 12:00 重装回填 + 全量冒烟（a 机）

用户重刷后镜像自带内容全清（核心/卡带/ES 条目/公钥全无，`/roms` 只剩空系统目录），
已回填：v0.3.3 linux-arm64 核心 + 21 张卡带 → `/roms/makecode/` +
ES 条目（`systems=136 makecode=1`）。用户实机确认「分类在，能玩游戏」。

a 机 21 张全量冒烟（600 帧/张，`--turbo`）：

| 判定 | 数量 | 说明 |
|---|---|---|
| ok | 19 | 独立帧 12~300，非黑 3~100% |
| static | 1 | `Mouse_Adventure` 独立帧 2 —— 没模拟按键停在标题画面，非缺陷 |
| panic | 1 | `Strawberry_Slam` 固定第 5133 tick `PANIC 980`，宿主存活 —— 游戏自身 bug，与 11-04 桌面判读一致 |

吞吐参考（a/b 两台几乎逐位相同，未跑 perfmax）：cubeland 66.5/66.4、ping 158/159、
Blocky 98.8/99.6（音频峰值 1264）、Space_Destroyer 156.6/161.7 fps。
cubeland 低于 11-06 的 77.6 是因为 governor 默认档 + DDR 未调 performance。

### 2026-11-07 12:12 b 机 21 张全量冒烟（与 a 对照）

| 判定 | a | b |
|---|---|---|
| ok | 19 | 19 |
| static | 1（Mouse_Adventure） | 1（同） |
| panic | 1（Strawberry_Slam 5133 tick PANIC 980） | 1（同） |

**关键：两台 21/21 张卡带的「独立帧数」与「非黑百分比」完全一致**
（277/53/31/220/33/15/15/12/59/2/277/35/282/300/300/117/15/15/33/15/295），
即同一卡带在两台机器上产生完全相同的帧序列 —— 核心的确定性在 ARM 真机可复现，
不只是桌面端。

吞吐差异（b 略高，同负载同 governor 默认档）：

| 卡带 | a | b |
|---|---|---|
| Pigeon's Deliverance | 240.5 | 248.0 |
| Secure the river | 225.7 | 226.3 |
| Stack the Goats | 220.7 | 219.8 |
| Blocky Boss Battle | 104.3 | 101.2 |
| arcade-cube-land | 66.6 | 67.5 |

两位数字落在 ±3% 内，属正常机间离散（11-04 的 RK3566 离散可达 25%，RK3326 同型号仅 1%）。

### 全机温度快照（2026-11-07 12:20，屏保/待机态）

横切 8 台可连设备（Switch/Lakka 关机、原 r36s .81 送修）+ 12:25 补测 cubexx：

| 设备 | 系统 | SoC | GPU | 空载 busy | 当前频率 | 降频线 | 电池 |
|---|---|---|---|---|---|---|---|
| rg353m | ArkOS (U19.10) | 54.4°C | 51.2°C | 23.4% | 1.992GHz (100%) | 70°C | 74% 放电 470mA |
| rg353v | ArkOS | 42.5°C | 40.6°C | 5.0% | 1.608GHz (89%) | 70°C | 100% **充电中** +411mA |
| rk2023 | ArkOS | 48.3°C | 47.2°C | 4.0% | 1.8GHz (90%) | 70°C | **37% 放电 985mA** |
| rgb30a | Debian 13 | 52.5°C | 46.1°C | 3.2% | 1.104GHz (55%) | 75°C | 61% |
| rgb30b | Debian 13 | 47.8°C | 45.6°C | 3.2% | 1.104GHz (55%) | 75°C | 69% |
| rgb30c | Debian 13 | 46.1°C | 41.9°C | 3.4% | 1.104GHz (55%) | 75°C | 59% |
| r36s-clone-a | Debian 13 | 53.8°C | 50.4°C | 15.6% | 1.2GHz (93%) | 70°C | 42% |
| r36s-clone-b | Debian 13 | 51.2°C | 51.7°C | 2.4% | 1.008GHz (78%) | 70°C | 51% |
| cubexx | muOS 2606.0 | 46.9~47.7°C | 46.6~47.1°C | ~5% | 1.51~1.8GHz | 75°C | 84% **充电中**，电池 30.0°C |

cubexx 另读三个独立感应点：`ve_thermal_zone` 45.9°C、`ddr_thermal_zone` 46.8°C，
`axp2202-battery` **30.0°C 是真实读数**（rk817 那批恒 18.8 是死值）—— 这说明 H700
这代的电池温度传感是好的，可作为以后判断充电发热的可靠指标。

读数口径：`thermal_zone0`(soc-thermal) / `thermal_zone1`(gpu-thermal)；busy 为 10s
`/proc/stat` 均值；`battery=18.8°C` 是 rk817 死值，全体一致，不可用。

结论：

- **全部在安全区**，距各自的 passive 触发点还有 17~29°C 余量，没有一台在降频。
- **cubexx 边充电边跑前端只有 47°C，电池 30.0°C** —— 是全场唯一"插着电还最凉"的机器，
  直接印证了「插电≠发烫」：真正升温来自低电量大电流充电，满电涓流几乎无感。
- **rg353m 最高 54.4°C**，同时 busy 也是最高（ES 主题动画，14h 累计 65 分钟 CPU，
  均摊 7.8% 单核，瞬时可达 38%），属"开机久了 + ES 前台在跑"的正常结果。
- **rgb30a 比 b/c 高 4.7~6.4°C**，负载与频率三者完全相同（3.2% / 1.104GHz），
  与上午的结论一致 —— 这台是散热装配差异（导热垫），软件侧无解释项。
- **r36s-clone-a 53.8°C** 是 21 分钟前刚刷完机，ES 首次建库 + 卡带写入的余热，
  b（50 分钟）已回落到 51.2°C，属正常收敛过程。
- rk2023 电量 37% 且放电 985mA（约 3.7W），是这屋里功耗最高的一台，注意别让它跑到
  低电自动关机。

## 在线设备软件清单（2026-11-07 12:35）

9 台在线（Switch/Lakka 关机、原 r36s .81 送修）。采集脚本 `out/therm/softinfo.sh`。

> **历史快照**：本节两张表是 12:35 的读数。13:00~13:20 期间 rg353m / rg353v 已换成
> Debian 13，表中这两行的 ArkOS 数据不再代表当前状态，现值见下方「换系统」章节；
> 其余 7 行未变。

### 系统层

| 设备 | IP | DT model | 系统 | 内核 | libc | 内存 | CPU 上限 | 根分区 |
|---|---|---|---|---|---|---|---|---|
| rg353m | .130 | Anbernic RG353M v1 | ArkOS (Ubuntu 19.10) | 4.19.172 | glibc 2.30 | 1970MB | 1.99GHz | 8.7G / 79% |
| rg353v | .77 | RK3566 EVB2 LP4X V10 | ArkOS (Ubuntu 19.10) | 4.19.172 | glibc 2.30 | 963MB | 1.80GHz | 8.5G / 74% |
| rk2023 | .17 | Powkiddy RK2023 | ArkOS (Ubuntu 19.10) | 4.19.172 | glibc 2.30 | 964MB | 1.99GHz | 11G / 72% |
| rgb30a | .26 | Powkiddy RGB30 | Debian 13 trixie | 5.10.226 | glibc 2.41 | 967MB | 1.99GHz | 11G / 51% |
| rgb30b | .85 | Powkiddy RGB30 | Debian 13 trixie | 5.10.226 | glibc 2.41 | 967MB | 1.99GHz | 11G / 51% |
| rgb30c | .101 | Powkiddy RGB30 | Debian 13 trixie | 5.10.226 | glibc 2.41 | 967MB | 1.99GHz | 11G / 51% |
| r36s-clone-a | .31 | R36S Clone Type2（RG351MP 硬件） | Debian 13 trixie | **4.4.189** | glibc 2.41 | 918MB | 1.30GHz | 6.1G / 79% |
| r36s-clone-b | .137 | 同上 | Debian 13 trixie | 4.4.189 | glibc 2.41 | 918MB | 1.30GHz | 6.1G / 79% |
| cubexx | .149 | sun50iw9（全志 H700） | MustardOS 2606.0 ANDROMEDA | 4.9.170 | **glibc 2.44 (Buildroot)** | 973MB | 1.80GHz | 7.8G / 55% |

四个不同的内核分支：**4.4.189（R36S Clone 镜像）/ 4.9.170（muOS）/ 4.19.172（ArkOS）/
5.10.226（RGB30 Debian）**。同一颗 RK3566 上跑着 3 种内核，这也解释了 DTB 与
`hp-det`/`spk-ctl` 行为在不同机器上不一致。

### 模拟器 / 应用层

| 设备 | 前端 | RetroArch | MakeCode 核心 | 卡带 | 核心目录 | 启动 DTB |
|---|---|---|---|---|---|---|
| rg353m | emulationstation | 1.19.1 | 0.3.2 | **25**（21+4 张诊断卡） | `~/.config/retroarch/cores` | `rk3566-OC.dtb` |
| rg353v | emulationstation | **1.15.0** | 0.3.2 | 21 | 同上 | `rk3566-OC.dtb` |
| rk2023 | emulationstation | 1.19.1 | 0.3.2 | 21 | 同上 | `rk3566-OC.dtb` |
| rgb30a/b/c | emulationstation | 1.22.2 | **0.3.3** | **20**（缺 `ping`） | 同上 | `rk3566-rgb30.dtb` |
| r36s-clone-a/b | emulationstation | 1.22.2 | **0.3.3** | 21 | 同上 | `rk3326-r36s-type2-panel1-with-amp-linux.dtb` |
| cubexx | **muxfrontend** | 1.22.2 | **0.3.2** | 21 | `/opt/muos/share/core` | muOS 内置（无 boot.ini） |

Python：ArkOS 三台 3.7.5 / RGB30 与 Clone 3.13.5 / cubexx 3.11.8。
SSH：8 台 openssh 均 `enabled+active`；cubexx 用 **dropbearmulti**（22 端口 root，
另 2022 端口 sftp jail），由 muOS 前端拉起，无 systemd。

### 三个待统一项（13:20 更新）

1. **核心版本不一致**：rk2023 + cubexx 仍是 **0.3.2**；rg353m / rg353v 换系统时已升到
   **0.3.3**，与 RGB30 三台 + Clone 两台一致 → 目前 9 台里 7 台是 0.3.3，待升级仅 2 台。
2. **RetroArch 版本跨度大**：换系统后 rg353m / rg353v 均为 1.22.2，最旧的 1.15.0 已消失；
   现存 1.19.1（rk2023）与 1.22.2（其余 8 台）两代。
3. **RGB30 缺 `ping.pxt64`**（20/21），是 11-07 部署时漏传；其余 20 张齐全。
   （rg353m 原有的 4 张诊断卡随换系统被清掉，现为干净 21 张。）

## rg353m / rg353v 换系统（2026-11-07 13:00~13:20）

两台从 ArkOS(U19.10, kernel 4.19.172) 换成 **Debian 13 trixie + kernel 5.10.226** ——
与 RGB30 三台同一镜像家族（`/opt/system` 脚本、`/roms` 布局、RA 1.22.2、perfmax/perfnorm
全部一致）。IP 不变（.130 / .77），但主机密钥更换需 `ssh-keygen -R`。

两台新镜像的共同状态：131 个核心、ES active、`ssh.service` **disabled**、
`/etc/xdg/systemd/user/default.target.wants/` 带 **fluidsynth + mpris-proxy 全局自启**、
家目录 700（sshd StrictModes 可接受）、sudo 免密。

### rg353v（.77）—— 已完成全套

- 公钥安装 + `systemctl enable ssh`（enabled/active）
- `--global disable fluidsynth mpris-proxy`（新登录 procs/wants 均空）
- 核心 v0.3.3 linux-arm64 → `~/.config/retroarch/cores/`
- 21 张卡带 → `/roms/makecode/`（此镜像的卡带目录不再是 ~/makecode）
- ES 注册：`/etc/emulationstation/es_systems.cfg`（systems=130, makecode=1），ES 已重启
- 冒烟（900 帧 --turbo，governor 默认）：**cubeland 180.1 / ping 390.4 / Blocky 264.6 /
  Space 384.3 fps**，Blocky 音频峰值 1264 与历史一致

### rg353m（.130）—— 13:15 重启后补齐，已完成全套

换系统后首次配置时撞上已记录过的 **sshd 锁死坑**：手动启动过的 sshd 占着 22 端口但
`/run/sshd` 缺失 → KEX 阶段 RST（公钥/密码都连不上），`systemctl start ssh` 因端口占用
退 255。与 RGB30 上的表现完全一致——**此坑无需手工杀进程，整机断电重启即自愈**
（因为公钥已写入、服务已 enable，systemd 开机拉起的是干净 sshd）。

13:15 用户重启后复核：`ssh=enabled/active`、BatchMode 公钥登录正常、
`procs=[]`（fluidsynth/mpris-proxy 均未复活）、`wants=[]`、家目录 700。
随后按 `deploy353.sh` 补齐：核心 0.3.3（5169256 B）、21 张卡带（含 `ping`）、
ES 注册 `/etc/emulationstation/es_systems.cfg`（systems=130, makecode=1），ES active。

冒烟（900 帧 --turbo，governor=ondemand）：

| 卡带 | rg353m | rg353v | 差 |
|---|---|---|---|
| arcade-cube-land | 179.3 | 180.1 | −0.4% |
| ping | 386.4 | 390.4 | −1.0% |
| Blocky_Boss_Battle | 263.8 | 264.6 | −0.3%（音频峰值同为 1264） |
| Space_Destroyer | 379.5 | 384.3 | −1.2% |

**两台换到同一镜像后吞吐差 ≤1.2%，方法可重复**；此前 ArkOS 时代 rg353m 因 ES 主题
动画空载 busy 高一个数量级而显得偏热/偏慢，现在软件环境完全对齐。
注意 rg353m 是 1970MB 内存、rg353v 963MB，但本核心是延迟敏感型、RSS 仅 11MB，
内存容量不体现在吞吐上。

## 最新设备清单（2026-11-07 13:30，补齐后）

9 台在线设备一次性同源采集，脚本 `out/therm/softinfo2.sh`，原始输出
`out/therm/softinfo-latest.txt`。Switch/Lakka 关机、原 r36s .81 送修，均不计入。

**全场已统一**：MakeCode 核心 **0.3.3**（9 台 md5 全部 = `9da97c82…a676fe93bb4d61634f7dbf`）、
卡带 **21 张**（含 `ping.pxt64`）、前端注册齐全（8 台 ES `makecode=1` + 1 台 muOS catalogue）。

### 系统层

| 设备 | IP | DT model | 系统 | 内核 | libc | 内存 | CPU 上限 | 根分区 | governor |
|---|---|---|---|---|---|---|---|---|---|
| rg353m | .130 | Anbernic RG353M | Debian 13 trixie | 5.10.226 | glibc 2.41 | 1971MB | 1.99GHz | 11G/53% | ondemand |
| rg353v | .77 | Anbernic RG353V/VS | Debian 13 trixie | 5.10.226 | glibc 2.41 | 967MB | 1.99GHz | 11G/52% | ondemand |
| rk2023 | .17 | Powkiddy RK2023 | **Ubuntu 19.10 (ArkOS)** | **4.19.172** | **glibc 2.30** | 964MB | 1.99GHz | 11G/72% | ondemand |
| rgb30a | .26 | Powkiddy RGB30 | Debian 13 trixie | 5.10.226 | glibc 2.41 | 967MB | 1.99GHz | 11G/51% | ondemand |
| rgb30b | .85 | Powkiddy RGB30 | Debian 13 trixie | 5.10.226 | glibc 2.41 | 967MB | 1.99GHz | 11G/51% | ondemand |
| rgb30c | .101 | Powkiddy RGB30 | Debian 13 trixie | 5.10.226 | glibc 2.41 | 967MB | 1.99GHz | 11G/51% | ondemand |
| r36s-clone-a | .31 | R36S Clone Type2 | Debian 13 trixie | **4.4.189** | glibc 2.41 | 918MB | **1.30GHz** | 6.1G/79% | ondemand |
| r36s-clone-b | .137 | R36S Clone Type2 | Debian 13 trixie | 4.4.189 | glibc 2.41 | 918MB | 1.30GHz | 6.1G/79% | ondemand |
| cubexx | .149 | sun50iw9 (H700) | **MustardOS 2606.0** | **4.9.170** | **glibc 2.44 (Buildroot)** | 973MB | 1.80GHz | 7.8G/55% | **powersave** |

系统分布：Debian 13 ×7 / ArkOS ×1 / muOS ×1；内核 4 个分支
（5.10.226 ×5、4.4.189 ×2、4.19.172 ×1、4.9.170 ×1）。

### 应用层

| 设备 | 前端 | RetroArch | 核心 | 核心目录（总数） | 卡带目录 | 卡带 | ES 注册 | Python | 启动 DTB | SSH |
|---|---|---|---|---|---|---|---|---|---|---|
| rg353m | emulationstation | 1.22.2 | 0.3.3 | `~/.config/retroarch/cores` (132) | `/roms/makecode` | 21 | 130+1 | 3.13.5 | `rk3566-353m.dtb` | enabled/active |
| rg353v | emulationstation | 1.22.2 | 0.3.3 | 同上 (132) | `/roms/makecode` | 21 | 130+1 | 3.13.5 | `rk3566-353v.dtb` | enabled/active |
| rk2023 | emulationstation | **1.19.1** | 0.3.3 | 同上 (117) | **`~/makecode`** | 21 | **127+1** | **3.7.5** | `rk3566-OC.dtb` | enabled/active |
| rgb30a/b/c | emulationstation | 1.22.2 | 0.3.3 | 同上 (132) | `/roms/makecode` | 21 | 130+1 | 3.13.5 | `rk3566-rgb30.dtb` | enabled/active |
| r36s-clone-a/b | emulationstation | 1.22.2 | 0.3.3 | 同上 (146) | `/roms/makecode` | 21 | 136+1 | 3.13.5 | `rk3326-…-with-amp-linux.dtb` | enabled/active |
| cubexx | **muxfrontend** | 1.22.2 | 0.3.3 | **`/opt/muos/share/core`** (195) | **`/mnt/mmc/ROMS/MakeCode Arcade`** | 21 | muOS catalogue | 3.11.8 | 无 boot.ini | **dropbearmulti** |

注：cubexx 无 `.info`（muOS 不走 libretro info 目录），版本只能 `strings` 读；
`VINFO` 空是正常的，不代表缺文件。ES 注册列「N+1」= 系统总数 + makecode 条目数。

### 本轮补齐动作（13:19~13:30）

1. **RGB30 三台补 `ping.pxt64`**：20 → 21 张，三台 md5 与本地一致
   （`b6efe3f6e60f…`）。
2. **rk2023 核心 0.3.2 → 0.3.3**：备份到 `~/core_backup_032/`，换新 `.so` + `.info`，
   冒烟 900 帧 turbo 全过（cubeland 170.0 / ping 394.5 / Blocky 258.9（音频 1264）/
   Space 398.6 fps）。
3. **cubexx 核心 0.3.2 → 0.3.3**：备份到 `/root/core_backup_032/`，直接换
   `/opt/muos/share/core/makecode_arcade_libretro.so`；四张冒烟均正常出帧。
4. **rk2023 补 ES 注册**（此前从未注册，前端看不到分类）：
   `/etc/emulationstation/es_systems.cfg` 126 → 127 系统、`makecode=1`，ES 已重启。
   用 `out/therm/reg_es_arkos.py`（ArkOS 卡带路径是 `~/makecode`，与其他机不同）。

### 三个新踩到的坑

- **0.3.2 与 0.3.3 的 `.so` 大小完全相同**（都是 5169256 B），判断是否旧版必须用
  md5 或 `strings`，看大小会漏判。实测 rk2023 / cubexx 的 0.3.2 是两份不同二进制。
- **ArkOS 的 `es_systems.cfg` 不是合法 XML**（第 2063 行 `<command>` 里有未转义的
  `2>&1`），ES 用宽松解析器。插入条目时不能用 `xml.etree` 严格校验，否则会误判为
  「XML_INVALID」而拒绝写入；改用 `<system>` / `</systemList>` 计数校验。
- **muOS 待机时 governor 是 `powersave`，CPU 锁 288MHz**：无头冒烟会读到
  cubeland 24.2 / ping 62.4 fps，比真实性能低约 6 倍，容易误判成核心坏了。
  临时 `echo performance > .../scaling_governor` 后是 1.8GHz、
  cubeland 158.7 / ping 422.3 fps，与历史 156 fps 一致；测完已恢复 powersave。
  换核心后备份文件要移出核心目录（`.so.bak032` 后缀仍可能被前端 glob 扫到）。

### 前端验收（用户实测，2026-11-07 13:36）

rk2023 新补的 ES 条目经用户实机确认：**MakeCode Arcade 分类可见、游戏可正常进入**。
至此 9 台在线设备全部完成「核心 + 卡带 + 前端注册 + 冒烟」闭环，其中前端可见性
由用户实机确认（无头脚本只能验证核心加载与出帧，无法替代前端这一层）。

## 工具机 192.168.1.69（2026-11-07 21:50 配置）

用户提供一台常驻 Linux 主机做构建/验证环境（用户选「完整构建环境」）。`root/ally` 可登录；
已装本机 ed25519 公钥（追加，原有 key 保留），本机 `~/.ssh/config` 加别名 **`tool69`**
（兼 `mcatool`）。sshd 配置、密码登录方式均未改动。

### 规格与已装工具

| 项 | 值 |
|---|---|
| 机型 | MoreFine S500+ 迷你主机，无电池，`HandleLidSwitch=ignore`（可常驻） |
| 系统 | Ubuntu 24.04.3 LTS / kernel 6.8.0-90 / x86_64 |
| CPU / 内存 / 磁盘 | Ryzen 3 5300U 4C8T（AMD-V、`/dev/kvm` 可用）/ 7.1G（6.4G 可用）/ 60G（39G 可用） |
| 容器 | Docker 28.2.2（overlay2, systemd cgroup） |
| 工具链 | zig 0.17.0（PyPI wheel）、clang/lld 18、cmake 3.28、ninja 1.11、gcc/g++ 13、Python 3.12 + pip、node 18.19 / npm 9.2、qemu-user-static 8.2.2 + binfmt、ccache、patchelf、debootstrap、rsync、strace/gdb |

路径：`/opt/mca-tools`（venv、`env.sh`、`bin/zig` → `/usr/local/bin/zig`）；
`/opt/mca-work`（`src/` 源码、`arm64-root` chroot、`mca/` 冒烟素材、`buildcheck/`）。
使用 `source /opt/mca-tools/env.sh` 拿 PATH。

### 两条已跑通的链路

**① 真实构建 aarch64 核心**（源码 = `core/src` + `core/third_party` + `core/libretro.h` +
`runtime/pxtapp` + `tools/build_target.sh`，225KB tar）：

```
cd /opt/mca-work/src && ZIG=zig ./tools/build_target.sh aarch64-linux-gnu.2.17
→ core/dist/aarch64-linux-gnu.2.17/makecode_arcade_libretro.so
  5197360 B, ELF64 AArch64 DYN, strings 版本 0.3.3
```
第一次同步漏了 `core/libretro.h`，报 `../libretro.h file not found`；补上后
「编译 0 个 / 跳过 34 个」+ 链接成功，**以后同步源码务必带上这个头文件**。
`build_target.sh` 默认找 Windows 的 `zig.exe`，Linux 侧用 `ZIG=zig` 覆盖即可。

**② arm64 无头冒烟**（x86 宿主 + qemu-user + chroot）：

```
# rootfs: Debian bookworm arm64 / Python 3.11.2
debootstrap --arch=arm64 --foreign --include=python3,ca-certificates --no-check-gpg \
  bookworm /opt/mca-work/arm64-root https://mirrors.tuna.tsinghua.edu.cn/debian
cp /usr/bin/qemu-aarch64-static <rootfs>/usr/bin/
chroot <rootfs> /debootstrap/debootstrap --second-stage

chroot /opt/mca-work/arm64-root /usr/bin/python3 /root/mca/libretro_host.py \
  /root/mca/core-built.so /root/mca/carts/ping.pxt64 120 --turbo --data-dir=/tmp/mca_data
```
`qemu-aarch64` binfmt 由 qemu-user-static 自动注册（`flags: POF`），静态 arm64 ELF
可以不写 qemu 前缀直接执行（已用 `zig cc -target aarch64-linux-musl -static` 验证）。

**逐帧等价对照**（120 帧 turbo，`FINAL-FRAME-HASH`）：

| 卡带 | 工具机新编核心 | 真机在用的 0.3.3 | 一致 |
|---|---|---|---|
| ping | `f93e4597e3503d50ea1374e2b87fba35` | 同 | ✅ |
| arcade-cube-land | `23210fb3b56189cd2946ea37bf44b204` | 同 | ✅ |

两份二进制 md5/大小都不同（5197360 / `f7f76e45…` vs 5169256 / `9da97c82…`），
但画面逐字节一致 → **不同宿主 zig 产物语义等价；判等价要看画面哈希，不是大小或 md5**。

### 网络与踩到的坑

- `api.github.com` / pypi / npm / npmmirror / apt archive / 清华镜像 **通**；
  `github.com`、`raw`、`jsdelivr`、**Docker Hub**（registry-1.docker.io / auth.docker.io）**不通** ——
  所以拉不到 arm64 容器镜像，改用 debootstrap 建 rootfs。
- `ziglang.org` 与 `files.pythonhosted.org` 从这台只有约 **20KB/s**（100MB 要一小时以上）；
  **zig 走清华 PyPI 镜像装 `ziglang==0.17.0` wheel，12 秒完成**（101MB）。
  该版本在 PyPI 被标记 yanked，但 `zig version` / `zig cc` / 实际构建全部正常
  （备选 `0.17.0.post0` / `0.17.1`）。Debian 的清华镜像 1.9MB/s，debootstrap
  second-stage 在 qemu 下约 8.5 分钟，不是卡死。
- apt 安装触发 needrestart 重启了 ssh/smbd/nmbd 等一批服务，复查均 active；
  `systemctl degraded` 只是 ModemManager + networkd-wait-online 两个无害失败。
- 这是台共用桌面机（3 个 root 会话、Samba、X11 转发、Ceph 相关服务），
  后续只在 `/opt/mca-work` 下作业，不改服务、不重启、不占满 CPU。

### 能力边界

- **能替代**：Linux 侧 ELF/Makefile/脚本检查、aarch64 交叉编译、arm64 核心的功能/出帧/崩溃验证
  （不必再等 GitHub 免费 ARM runner，也不必为一次功能测试去开掌机）。
- **不能替代**：性能量化。qemu 模拟下 cubeland 120 帧只有 4 个独立画面、游戏真实帧率 ≈3 fps，
  吞吐数字没有参考价值；性能基准仍只能用 rgb30c / cubexx 真机。
- **不解决**：github.com / raw / jsdelivr 的墙依旧，拉 release 资产仍走
  `api.github.com/.../releases/assets/<id>`。

## core 0.3.4 在线两台部署与实测（2026-11-07 23:40~23:47）

发版 `core-v0.3.4`（CI run 42386567562，15/15 job 绿；6 个包体检 6/6 PASS），
随后只给当时在线的 2 台换核心。**其余 7 台关机、仍是 0.3.3（md5 `9da97c82…`）**。

### 部署与备份

| 设备 | 核心路径 | .info 路径 | 备份位置 | 新 md5 |
|---|---|---|---|---|
| cubexx (.149, muOS) | `/opt/muos/share/core/makecode_arcade_libretro.so` | `/opt/muos/share/emulator/retroarch/info/` | `/root/mca-backups/core-0.3.3/` | `7bd2bfe0…` |
| rgb30c (.101, Debian13) | `~/.config/retroarch/cores/makecode_arcade_libretro.so` | 同核心目录 | `~/mca-backups/core-0.3.3/` | `7bd2bfe0…` |

用的是发行包 `makecode_arcade_linux-arm64.zip` 里的 `.so` 与 `.info`（5170520 B），
备份放在**家目录**而不是核心目录（避免被前端 glob 扫成重复条目），核心目录最终只剩
目标文件，没有 `.bak` / `.old`。两台的 `libretro_host.py` 也换成修复后的版本。

**部署前的真实不一致**：cubexx 的 `.info` 还写着 **0.3.2**（11-05 那次把核心升到 0.3.3
时，muOS 的 `.info` 在系统目录里，漏改了）；rgb30c 是 0.3.3。本次两台 `.info` 一并
对齐到 0.3.4。备份 `.info` 的 md5 分别是 `21b18de5…` / `c5346275…`。

### 无头冒烟：逐帧哈希一致、吞吐无退化

| 设备 | governor | ping 末帧哈希 / 吞吐 | cubeland 末帧哈希 / 吞吐 |
|---|---|---|---|
| cubexx | powersave（待机 288MHz） | `f93e4597…` / 63.1 fps | — |
| cubexx | 临时 performance | `f93e4597…` / 424.1 fps | `23210fb3…` / 152.9 fps |
| rgb30c | ondemand | `f93e4597…` / 372.1 fps | `23210fb3…` / 167.3 fps |

吞吐取宿主的「实际节拍」（总帧/耗时），不是宿主打印的「游戏真实帧率」。
cubexx 待机 powersave 与 performance 差 6.7 倍，再次印证跑性能前必须临时切
performance（测完已恢复 powersave）。cubeland 152.9 fps 与 11-05 记录的 158 fps
同档，**0.3.4 相对 0.3.3 没有性能退化**；两张卡带的哈希与真机 0.3.3 基准、
工具机全量回归、发行包三路冒烟全部一致。

### 真 RetroArch 前端层（1.22.2，/tmp 配置 + null 驱动）

两台日志都出现 `[MCA] core version 0.3.4`、`Loading content file`、跑到 16 万+ tick、
已知 panic 后核心自动重载（`HOST RESET`）。

- rgb30c：`timeout 20` 正常结束，RC=124。
- cubexx：RC=139（SIGSEGV），但日志显示崩在 `[Core] Unloading core symbols...`
  之后，即**退出卸载阶段**。**用备份的 0.3.3 核心在同样配置下重跑同样是 139**，
  0.3.4 再跑一次也是 139 → 与核心版本无关，是 muOS 定制 RA 在无头 null 驱动下
  退出路径的既有现象；游戏运行段正常，13:36 用户实机前端验收也正常。

### 遗留

- 其余 7 台开机后按同流程替换即可（备份移出核心目录），届时全场才重新一致。
- Switch NX 是独立 `nx-v*` 发布线，本次未跟随，现有 NX 产物仍 0.3.3。
- 若 muOS 前端仍显示旧版本号，重启一次 RetroArch 让它重建 `core_info.cache`。

---

## 2026-11-09 · v0.3.7 两台真机验收（退出 139 的真正根因）

### 部署

| 设备 | 核心路径 | `.info` 路径 | 备份 |
|---|---|---|---|
| cubexx (.149, muOS / RG CubeXX) | `/opt/muos/share/core/` | `/opt/muos/share/emulator/retroarch/info/` | `/root/mca-backups/core-0.3.5/` |
| rgb30c (.101, Debian 13) | `~/.config/retroarch/cores/` | 同核心目录 | `~/mca-backups/core-0.3.5/` |

两台都是 0.3.5 → 0.3.7，发行包 `makecode_arcade_linux-arm64.zip` 里的 `.so`（5007984 B，
md5 `a3e8b64d…`）与 `.info`。备份留在**家目录**，核心目录只剩目标文件。cubexx 是
BusyBox 环境：没有 `tar -z` / `stat` / `install`，用 `gzip -dc | tar x` + `cp` + `chmod`。

### golden 三例：逐位一致

| 用例 | cubexx | rgb30c | 桌面基线 |
|---|---|---|---|
| ping 300 帧 | `0556e78f…` | `0556e78f…` | `0556e78f…` |
| probe 120 帧 | `e82844f7…` | `e82844f7…` | `e82844f7…` |
| probe 120 帧按 A | `0eb7884c…` | `0eb7884c…` | `0eb7884c…` |

真游戏卡带：cubexx `Blocky_Boss_Battle` 300 帧 `18a21077…`、rgb30c `Jumpy Platformer`
300 帧 `e2d84eb5…`，均无 PANIC。

### 真 RetroArch 退出：0.3.5 = 139 / 0.3.7 = 0

`/tmp` 配置 + video/audio/input 全 null + 独立存档目录，`--max-frames=120`，
同一张 `ping.pxt64`，只换核心：

| 设备 | 0.3.5 退出码 | 0.3.7 退出码 |
|---|---|---|
| cubexx（muOS RA 1.22.2） | **139** | **0** |
| rgb30c（Debian RA） | **139** | **0** |

两台的 0.3.5 日志都停在 `[Core] Unloading core symbols...` 之后（即 `dlclose` 之后、
`atexit` 链上），0.3.7 走完同样四行后干净退出。

**更正本文件上面的旧结论。** 2026-11-05 那轮写的是「0.3.3 / 0.3.4 都是 139 → 与核心
版本无关，是 muOS 定制 RA 的既有现象」。当时两台都返回 139，是因为**那个 bug 在核心侧
一直存在**（pxt 运行时的 static 析构函数在 `dlclose` 后仍挂在 `atexit` 链上，进程退出
时跳进已解除映射的代码段），换哪个版本都一样；而 Debian 那台当时判成「正常结束」
是因为 `timeout 20` 的 RC=124 掩盖了真实退出码，没有像这次一样拿 0.3.5/0.3.7 做对照。

修法在链接期：`-Wl,-z,nodelete`（`DF_1_NODELETE`，`readelf -d` 可见 `FLAGS_1: NOW
NODELETE`），ld.so 不回收映射，析构函数跳进去仍在。代价是换核心时不释放内存，单次
会话可忽略。**只在 ELF 上生效**：Windows 是 PE，没有对应标志，修复方式取决于前端是否
`FreeLibrary`，未实测。

## core 0.3.8 真机部署与实测（2026-11-10 08:25~08:40）

发版 `core-v0.3.8`（tag run `42654310708` 全绿，6 主包 + 3 调试包体检全 PASS）。
0.3.8 **零行为改动**，只改了出厂形态（strip / debuginfo / build-id）与两处漏配的
`DF_1_NODELETE`（Android、Lakka）。因此真机这一轮的目标不是"找新 bug"，而是
**确认等价 + 确认退出仍干净**。

### 部署

| 设备 | 核心路径 | `.info` | 备份 | md5 | 结果 |
|---|---|---|---|---|---|
| rgb30c (.101, Debian 13) | `~/.config/retroarch/cores/` | 同目录 | `~/mca-backups/core-0.3.7/` | `eb4eb217…` / 732 496 B | ✅ 已升 |
| cubexx (.149, muOS) | `/opt/muos/share/core/` | `/opt/muos/share/emulator/retroarch/info/` | — | — | ⏸ **设备不在线**（无 ARP、22 端口不通），待开机补 |
| Mi Max 2 (`b2aec2db`, Android 7.1.1) | shell 宿主直跑，不动 RA | — | — | `ed2a6a56…` / 455 136 B | ✅ 21 卡回归 |

rgb30c 换完 `display_version = "0.3.8"`，核心目录只留目标文件（`.bak` 不留），
0.3.7 的 5 007 984 B 原件与 `.info` 在 `~/mca-backups/core-0.3.7/`。

### golden 三例（rgb30c）

| 用例 | 实测 | 桌面基线 | |
|---|---|---|---|
| ping 300 帧 | `0556e78f7ce0e828f2dc922d210dc047` | 同 | ✅ |
| probe 120 帧 | `e82844f7160449d1767aa69afe788800` | 同 | ✅ |
| probe 120 帧按 A | `0eb7884cb40cef0cb549946af3a942ab` | 同 | ✅ |

真游戏 `arcade-jumpy-platformer` 300 帧 → `e2d84eb5325d9436a69fc5acee35358b`
（与 0.3.7 实测值一致），无 PANIC。

### 真 RetroArch 退出：0.3.8 仍是 0

`/tmp` 配置 + video/audio/input 全 null + 独立存档目录，`ping.pxt64` 120 帧：

| 设备 | 0.3.5 | 0.3.7 | 0.3.8 |
|---|---|---|---|
| rgb30c（Debian RA 1.22.2） | 139 | 0 | **0** |

日志里 `[Core] Unloading core symbols...` 之后干净退出，`SET_PIXEL_FORMAT: RGB565`、
`Geometry: 160x120` 均正常。strip 掉 `.debug_info` 没有影响任何行为。

### Android（Mi Max 2）21 卡 ×600 帧

新核心 md5 `ed2a6a56…`（同 455 136 B，但补了 `DF_1_NODELETE`，故与 0.3.7 不同）：

```
ADB=.../adb.exe bash tools/android_test/run_shellhost.sh \
  --serial b2aec2db --core out/rel038-android/makecode_arcade_libretro.so \
  --carts out/android_test/carts21 --baseline tools/fixtures/android21-baseline.txt
→ 21/21 与桌面 x86_64 基线逐位一致，结论「全部一致」
```

### 未完成

- **cubexx 没升**：08:24 起 `.149` 无 ARP、22 端口超时（同网段扫描只看到 .1/.69/.84/.101/.196），
  判定设备关机或掉网。开机后按上表路径补一次（BusyBox 环境：`gzip -dc | tar x` + `cp` + `chmod`）。
