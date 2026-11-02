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
