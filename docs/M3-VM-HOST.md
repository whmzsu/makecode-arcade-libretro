# M3：pxt VM 宿主链路打通记录

> 2026-11-01。目标：`pxt64 字节码 → VM 执行 → 帧/音频/按键` 全链路，
> 不依赖 Docker、不依赖 RetroArch，全程 zig + node 本地完成。

## 链路总览

```
testproj(main.ts 来自卡带解码)
  │  pxt install --hwvariant vm        # 从 target.json bundledpkgs 拉 hw---vm
  │  pxt build --hwvariant vm          # fakebin/docker.cmd 空壳跳过原生编译
  ▼
built/binary.pxt64                    # VM 字节码镜像（= 镜像 section + SRCM 源码映射）
built/dockercross/pxtapp/**           # 官方生成的原生 VM app 源码树（含 Makefile.inc）
  │  build_vmapp.sh                   # zig 编译 33 个 .o（排除 sdlmain/main.cpp）
  ▼
vm_smoke.exe                          # 独立宿主（core/tests/vm_smoke.cpp）
  │  pxt_vm_set_data_directory()      # 存档目录（默认 /tmp，Windows 必须改）
  │  pxt_vm_start_buffer(img, len)    # 启动 VM（内部独立线程）
  │  pxt_screen_get_pixels()          # 每帧拉 160x120 BGRA32
  │  pxt_get_audio_samples()          # 44.1kHz 单声道 S16
  │  pxt_raise_event(2050/2051, key)  # 按键（2050=up 2051=down, key 1..7，+7=P2）
  ▼
out/ping_vm_*.bmp                     # ✅ ping 完整渲染（标题/比分/挡板/球）
```

## 宿主 ABI（与官方 sdlmain.cpp 一致，8 个 C 函数）

| 函数 | 说明 |
|---|---|
| `pxt_vm_set_data_directory(path)` | 存档/缓存目录，**启动前必须调用** |
| `pxt_vm_start_buffer(data, len)` | 启动 VM；data 会就地打补丁，需 8 字节对齐、可写 |
| `pxt_screen_get_pixels(w, h, u32*)` | 拉一帧 BGRA32 |
| `pxt_get_audio_samples(s16*, n)` | 拉音频（44100Hz mono） |
| `pxt_raise_event(src, val)` | src=2050/2051（up/down），val=Key 枚举或 0（any） |
| `pxt_get_logs(0, buf, size)` | 拉 VM 日志 |
| `pxt_get_panic_code()` | 0=正常；>=1000 软崩溃（可重启）；其他=致命 |

Key 枚举：1=LEFT 2=UP 3=RIGHT 4=DOWN 5=A 6=B 7=MENU；P2 在此基础上 +7（8..13）。

## .pxt64 镜像格式（实测）

- 文件 = 镜像 section 链（8 字节对齐，`{type:u8, flags:u8, aux:u16, size:u32}` 循环）
  + 尾部附加 SRCM 源码映射（**不参与校验，宿主须截掉**，否则 loadVMImage 报 1001）。
- 宿主应把长度截到 section 链末尾（vm_smoke.cpp 的 `image_section_end()`）。
- section type：1=InfoHeader 2=OpCodeMap 3=NumberLiterals 4=ConfigData
  5=IfaceMemberNames 6=NumberBoxes 0x20=Function 0x21=Literal 0x22=VTable。

## 本地构建踩过的坑（已固化到 build_vmapp.sh）

1. **codegen bug**：pxt 13.2.x 生成 `pointers.cpp` 时把带命名空间的参数类型
   `pxt::RefCollection*` 拼成 `aspxt::RefCollection` → 正则为 `pxt::asRefCollection`。
2. **64 位截断**：`core---vm/vm.cpp` 的 `find_src_map()` 用 `uint32_t` 截断指针 → `uintptr_t`。
3. **校验 1063**：NumberBoxes 尺寸校验按 32 位盒化数计算，PXT64 下
   `canBeTagged()` 恒 true 导致必然不匹配 → 该校验仅 `#ifdef PXT32` 时执行
   （64 位宿主根本不读 NumberBoxes，语义安全）。

## 构建命令速查

```bash
# 0) 一次性：tools/vm-build 下 npm i pxt-arcade@4.2.1（npmmirror）
# 1) 生成工程源码（从卡带解出 main.ts 等放到 node_modules/pxt-arcade/testproj/）
cd tools/vm-build/node_modules/pxt-arcade/testproj
PATH="$(cd ../../../../fakebin && pwd):$PATH" PXT_FORCE_LOCAL=1 \
  node ../../pxt-core/built/pxt.js install --hwvariant vm
PATH="$(cd ../../../../fakebin && pwd):$PATH" PXT_FORCE_LOCAL=1 \
  node ../../pxt-core/built/pxt.js build --hwvariant vm
# 2) 编译 VM 内核 + 引擎 + 冒烟宿主
cd ../../../../.. && ./build_vmapp.sh
# 3) 运行
built/dockercross/vm_smoke.exe <game.pxt64> <out_prefix> [frames] [--press=a]
```

## libretro 接入要点（下一步）

- VM 在独立线程运行 → `retro_run` 只需拉帧 + 拉音频 + flush 日志 + 查 panic，
  与官方 SDL 宿主的循环同构；音频按 735 样本/帧（44100/60）喂 `audio_batch`。
- 存档目录映射到 RetroArch `system`/`save` 目录下的 per-game 子目录。
- RetroPad → Key 映射直接复用 sdlmain 的表；多人 (P2) 用 +7 偏移。
- 镜像来源：M5 转换器（PNG→pxt64）已经打通——把卡带源码放进 testproj 编一次即可，
  可脚本化为 `tools/vm-build/cart2pxt64.py`。

## libretro 接入完成（M3 验收记录）

实现：`core/src/vm_host.cpp`（宿主封装）+ `core/src/libretro_core.cpp`（接口层），
构建脚本 `tools/build_core.sh` 一键产出 DLL + 三个测试程序。
源文件清单唯一真源是 `core/sources.mk`（Makefile / Android.mk / 三个 build_*.sh 共用）。

- **载入格式**：**只认 `.pxt64`**。`.png` 卡带一律在通用镜像校验处失败——核心
  不解码 PNG，也不为 PNG 做专门提示，玩家先用 `tools/png2pxt64.py` 转换
  （v0.3.5 删掉了 PNG 解码链路与"同名 .pxt64 旁载"分支）。
- **音频**：VM 输出 44.1kHz 单声道 S16 → 复制为立体声喂 `audio_batch`（735 样本/帧）。
- **视频**：VM 输出 BGRA32 → RGB565 → `video_cb`。
- **按键**：RetroPad 状态沿检测 → `pxt_raise_event(DOWN/UP, key)` + any-key 补发；
  START → KEY_MENU。
- **存档**：`pxt_vm_set_data_directory(<save目录>/makecode_arcade)`，启动前调用。
- **NX 诊断**：`core/src/nx_diag.{h,cpp}`（非 Switch 平台编成空 TU），
  调用点只写 `MCA_NX_BOOT(...)`，平台条件不在 `libretro_core.cpp` 里出现。

### 关键架构结论（实测）

**pxtapp C++ 源码树与游戏无关**：ping 与 jumpy 生成的 pxtapp（含 70KB 的
pointers.cpp、functions.json、pxtconfig.h）逐字节一致（仅我们的 3 个补丁差异）。
即同一份预编译 core 可运行任意卡带的 .pxt64——"拖进来就玩"的模型成立。

### 已知限制

1. **VM 无停止接口**（官方宿主 = 单游戏单进程）：~~同进程只能启动一次 VM~~
   **v0.2.3 已解决重开问题（PATCH 5）**：利用 VM 自带 `systemReset()` 的清理
   序列实现真重启（清 fiber/handler/GC → 线程正常退出 → 宿主用原始镜像重新
   start）。reset 键与前端 Restart 都走这条路，详见 docs/INPUT.md「重置的实现」。
   **v0.2.5（PATCH 6）修掉重开路上的三个坑**：GC 块清单自环（堆块地址复用）、
   重启后 `globals` 悬垂导致的 `oops(41)` 递归、清理中 panic 的重入；并把退出
   标志改到线程真正返回时置位。
   **v0.2.6（PATCH 7）修掉真正的死锁**：`WDisplay::update()` 无超时等前端取帧
   × 前端在重置等待期间不拉帧 = 互相等待；另加 `pxt_vm_online()` 在线标志挡住
   重启间隙前端对已释放单例的访问。详见 docs/INPUT.md 四章、docs/DEVICE-TEST.md。
   **v0.2.7（PATCH 8/9）把"卸载"和"重启后音频"补齐**：`retro_unload_game` /
   `retro_deinit` 里真正停 VM 线程（等线程清理完再 `pthread_join`，否则前端
   `dlclose` 后线程还在跑已 unmap 的代码 → 前端崩溃；关闭游戏、换游戏都走这条
   路）；`WSynthesizer`/`WDisplay` 这些 **static 单例指针**在清理时显式置 NULL
   （GC 重置不会清 static，不置 NULL 则重启后音频通道拿到野指针 → 没声音）。
   排查用 `mca_trace.log` 的 `cleanup …` / `stop requested` / `thread exit` 行。
   换**另一张卡带**：同一进程内 unload → load 已支持（`load_game` 见 VM 在跑
   就先 `vm_stop`），`tools/test_lifecycle.py` 有三连换游戏与 dlclose 用例。
2. **启动竞速**：VM 线程初始化（GC/显示）需几毫秒；宿主必须按 ~60fps 节拍
   拉帧（真前端天然满足）。假前端 core_test 已加 16ms/帧 sleep——空转会导致
   整个测试在 VM 起跑前跑完，画面全黑（0xff000000 = screen.cpp 的 no-display 分支）。
3. VM 日志每条重复两遍（官方宿主同样现象，pxt_get_logs 行为，不影响功能）。

### 对 pxt 上游的 9 个补丁（PATCH 1/2/3/5~10；PATCH 4 已被 10 取代）

> **校验**：`python tools/check_pxt_patches.py`（CI 的 `pxt-patches` job 每次 push 都跑）。
> 补丁没有独立 patch 文件、构建时也不重放，全靠这棵树本身带着——脚本是唯一能
> 拦住"重新生成 pxtapp 时补丁全丢"的东西。加新补丁要同步登记进那张表，
> `--strict` 会把树上未登记的编号抓出来。

> PATCH 1/2/3 是 64 位移植与编译修复（`build_vmapp.sh` 里有重放脚本）；
> PATCH 5 起是 libretro 前端才需要的行为：重开、重开后音频、卸载、确定性。

> PATCH 7~9（v0.2.6/v0.2.7）同样只落在 `runtime/pxtapp`：`screen---ext/screen.cpp`
> （取帧等待超时 / shutdown）、`mixer---ext/sound.cpp`、`mixer---ext/melody.cpp`
> （单例置 NULL）、`core---vm/{scheduler,vmload}.cpp`、`platform.h`。
> **从上游重生成 pxtapp 后必须手工重放**，否则重置/卸载/音频会一起退化。

> PATCH 6（v0.2.5）只落在 `runtime/pxtapp`（仓库内 vendored 树，构建直接用这棵），
> 涉及 `base/gc.cpp`（新增 `gcForgetAllBlocks`）、`core---vm/scheduler.cpp`
> （trace / 重入保护 / 块清单清理）、`core---vm/vmload.cpp`（退出标志与 trace）、
> `core---vm/verify.cpp`（释放 stackBase）、`core---vm/vmcache.cpp`（数据目录
> 访问器）、`platform.h` + `pxtbase.h`（声明）。**从上游重新生成 pxtapp 后要按
> docs/INPUT.md 四章的说明手工重放**，否则 reset 会退化回"重置后画面冻住"。

| # | 文件 | 问题 | 后果 |
|---|---|---|---|
| 1 | core---vm/vm.cpp | `find_src_map()` 用 `uint32_t` 截断 64 位指针 | 镜像装载错误 |
| 2 | pointers.cpp | codegen 把 `pxt::RefCollection*` 拼成 `aspxt::` | 编不过 |
| 3 | core---vm/vmload.cpp | VM 线程 `pthread_create(attr=NULL)`：栈取**宿主 exe** 的预留（RetroArch 仅 2MB） | 前端里栈溢出无声崩溃 |
| 4 | core---vm/platform.cpp | `initRandomSeed()` 对 fd=-1 调 `read()`：Windows 无 /dev/urandom，UCRT 无效参数 → `__fastfail(0xC0000409)` | **进程立刻消失**，无日志、无栈回溯、无 WER 记录 |

补丁 4 是"RetroArch 直接崩溃退出，啥信息都没"的根因（2026-11-01 定位）。
**它现在不在册**：整个 `initRandomSeed()` 在 PATCH 10 里换成"种子 = 镜像 hexHash"，
fd 路径一并删除，崩溃点不复存在。表格保留这一行是因为旧日志里还有它。
诊断手段：`tools/libretro_host.py`（ctypes 最小前端，复现 dlopen 装载路径 +
25 符号解析 + 帧循环，`--bmp=` 可存末帧），core 内置 Windows 未处理异常
过滤器会把异常码/地址打到 stderr。

### 回归结果（2026-11-01）

| 游戏 | pxt64 | vm_smoke | libretro 假前端 | libretro_host.py（动态加载） | RetroArch 1.16 实测 |
|---|---|---|---|---|---|
| ping | 148,832B 镜像 | ✅ 渲染 | ✅ 渲染（标题/挡板/球） | ✅ 150 帧无 panic | ✅ 存活+渲染 |
| jumpy | 190,232B | ✅ 渲染 | ✅ 渲染（tilemap/角色/金币/文字） | ✅ 150 帧无 panic | ✅ |
| cube-land | 181,608B | ✅ 渲染 | ✅ 渲染（文字场景） | ✅ 150 帧无 panic | ✅ |

三款游戏 panic=0，卡带解码冒烟同步通过。
