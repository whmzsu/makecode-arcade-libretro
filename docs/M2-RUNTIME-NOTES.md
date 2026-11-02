# M2 考古笔记 — pxt 官方 VM 运行时定位（2026-11-01）

结论先行：**路线 B 的全部 C++ 资产都已定位，且官方宿主 ABI 与 libretro
回调几乎一一对应**，M3 的集成面比预估小得多。

## 1. 官方 VM 宿主 ABI（7 个 C 函数）

来源：`microsoft/pxt-arcade` → `libs/hw---vm/sdlmain.cpp`（已存
refs/pxt-arcade/hw---vm/）。SDL 宿主通过 dlsym 从 libpxt.{so,dll,dylib}
取这些符号；我们的 libretro core 等价于把 SDL 换成 libretro 回调：

| 官方 ABI | 作用 | libretro 对应 |
|---|---|---|
| `pxt_vm_start_buffer(uint8_t*, unsigned)` | 从内存镜像启动 VM（.pxt64 字节码） | `retro_load_game` 内调用 |
| `pxt_vm_start(const char* fn)` | 从文件启动（同上） | 不需要 |
| `pxt_raise_event(int src, int val)` | 输入：src=2050抬起/2051按下，val=按键1..7，补发 val=0 any | input_state 按键沿 |
| `pxt_screen_get_pixels(int w, int h, uint32_t*)` | 拉帧，BGRA32 | video_cb（转 RGB565） |
| `pxt_get_audio_samples(int16_t*, unsigned)` | 拉音频，44100Hz 单声道 S16 | audio_batch（每帧 735 采样） |
| `pxt_get_logs(int, char*, int)` | VM 日志 | log 回调 |
| `pxt_get_panic_code()` | 0 正常 / -1 重启中 / >=1000 软崩溃 / 其他致命 | 决定重载或报错 |

按键枚举：LEFT=1 UP=2 RIGHT=3 DOWN=4 A=5 B=6 MENU=7（RESET=100 走 TS 层）。
已按此对齐 `core/src/platform.h`。

## 2. VM C++ 运行时的准确位置

**`microsoft/pxt-common-packages` → `libs/core---vm/`**（不是 pxt 主仓）：

- `vm.cpp`(27KB) / `vm.h` — VM 内核
- `vmload.cpp` — 镜像装载；`vmcache.cpp` — 代码缓存
- `scheduler.cpp`(14.9KB) — codal 风格 fiber 调度器（帧步进改造对象）
- `target.cpp` / `config.cpp` / `control.cpp` / `keys.cpp` / `codalemu.cpp`
  / `dmesg.cpp` / `platform.cpp` — 设备层模拟
- `verify.cpp` — 镜像校验
- `additionalFilePath: ../core---linux` — 与 linux 变体共享部分文件
- Makefile：`-std=c++11 -fno-rtti -fno-exceptions -fno-unwind-tables
  -fwrapv -fno-threadsafe-statics`，产物 `libpxt.so / pxt.dll` +
  `pxt-vm-cli`，无第三方依赖（仅 -lm -lpthread）

## 3. 引擎分层（关键认知）

pxt 的做法是：**引擎 TS（libs/game、libs/screen 的 sprite/physics 逻辑）
本身也被编译进 VM 字节码**，C++ 侧只提供设备原语：

- `screen---ext` / `mixer---ext`：C++ 扩展（屏幕缓冲、音频合成）
- `hw---vm` 的 pxt.json：`compileServiceVariant: "vm"`，依赖
  core---vm + mixer---ext + screen---ext + settings---files + game
- 即：VM 镜像 = 引擎 TS + 游戏 TS，全部字节码化

**重要推论**：这套"VM+字节码引擎"在 120MHz Cortex-M4 真机上跑满
60fps（hw---vm 官方列在硬件支持页），性能余量论证闭环。

## 4. M3 集成方案（确定稿）

1. vendor `core---vm` 的 .cpp/.h + `screen---ext` + `mixer---ext` 到
   `core/runtime/`（含 `additionalFilePath` 指向的 core---linux 共享文件）；
2. 用官方 7 函数 ABI 编出 VM 对象，替换 sdlmain 的角色：
   `retro_load_game` → `pxt_vm_start_buffer`；`retro_run` →
   screen/audio 拉取 + 输入沿下发；panic code → 重载/报错；
3. 编译注意：`-fno-rtti -fno-exceptions -fwrapv`，C++11。

## 5. 遗留问题（M3/M5 解决）

- [ ] **字节码镜像怎么产出**：pxt CLI 对 `compileServiceVariant: vm`
      的构建命令与 .pxt64 镜像格式（M5 转换器的核心问题；可参考
      makecode.games 的 PNG→libpxt 服务与 pxt-vm-cli）
- [ ] `core---vm` 的 Makefile.inc 由 pxt 构建生成（PXT_OBJS 清单），
      需要跑一次 pxt 构建拿到完整文件列表
- [ ] screen---ext / mixer---ext 的 C++ 源是否完整可编（可能引用
      codal 头），vendor 时逐一确认
- [ ] scheduler.cpp 的帧步进改造评估（官方 SDL 版跑的是实时循环，
      libretro 需按 16.639ms 步进——或先用实时时钟对齐方案）
