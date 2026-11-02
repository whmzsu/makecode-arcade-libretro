# runtime/VENDORING — pxt C++ 运行时 vendor 计划

路线 B 的核心资产来自微软 pxt 生态（均 MIT 许可）。**注意：M2 第一件事
是代码考古确认下列每个组件的准确落点**，本清单记录的是当前认知，以
实际仓库为准修订本文件。

## 待 vendor / 移植组件

| 组件 | 来源 | 用途 | 许可 | 状态 |
|---|---|---|---|---|
| pxt VM（字节码解释器） | github.com/microsoft/pxt | 执行游戏字节码；与 libSDL 原生版同源 | MIT | M3 |
| 游戏引擎 C++（screen/sprite/physics/sound） | pxt-arcade 的硬件运行时（codal-arcade 一系）或 pxt-arcade 内对应 C++ 目录 | 引擎实现，剥离 codal 硬件层后接 HAL | MIT | M2 考古 |
| 目标 TS 库（API 定义） | github.com/microsoft/pxt-arcade `libs/` | API 面 / 类型定义，转换器侧使用 | MIT | M5 |
| PNG blob 编解码权威参考 | pxt `pxtlib/util.ts decodeBlobAsync` | 与我们的 C++ 实现逐行比对 | MIT | M1 |
| libretro.h | github.com/libretro/libretro-common | 已放入 core/libretro.h | MIT | 完成 |
| cart.c / cart.h | whmzsu/mcarcade-core `src/` | 卡带解析（PNG+隐写+LZMA+source 提取），已在两个生产 core 验证 | MIT | **已 vendor 至 core/src/** |
| decompress.c/.h | whmzsu/mcarcade-core `src/` | LZMA-alone 解压 | MIT | **已 vendor 至 core/src/** |
| miniz | whmzsu/mcarcade-core `third_party/miniz/` | PNG inflate（仅编 miniz.c 合并单元） | MIT | **已 vendor 至 core/third_party/** |
| LzmaDec | whmzsu/mcarcade-core `third_party/lzma/` | LZMA 解码内核 | public domain | **已 vendor 至 core/third_party/** |
| cache.c/.h | whmzsu/mcarcade-libretro `src/` | 编译产物落盘缓存（MCC1 容器） | MIT | 备用，M5 接转换器时引入 |
| compiler.c + runtime/*.js | whmzsu/mcarcade-libretro | 内嵌 pxtc 的完整工程方案（内存 VFS/Buffer 替身/microtask 抽干） | MIT | M5 转换器参考 |

## 已确认的参考实现（不 vendor 进 core，仅对照）

- jan4984/makecode-arcade-emulator（Rust+V8）：其 PNG 隐写解码是可工作的
  移植，本项目的 C++ 版以其为蓝本并以 pxt 原始实现为准。
- makecode.games 的 PNG→native 转换服务：证明 PNG→字节码→原生链路
  可行，转换器（tools/pxt-convert）可参考其产物形态。

## vendor 原则

1. 以 git subtree / 明确版本 tag 引入，禁止裸拷贝散文件；
2. 每个组件在 `runtime/<name>/VERSION` 记录来源 repo + tag + 日期；
3. 对上游的修改（如 fiber 帧步进改造）尽量做成独立 patch 文件，
   放 `runtime/patches/`，便于跟随上游更新；
4. 构建集成走 core/Makefile 的变量注入，不修改上游构建文件。

## 代码考古 checklist（M2 开工项）

**2026-11-01 考古完成，详细结论见 docs/M2-RUNTIME-NOTES.md：**

- [x] pxt VM 落点：`microsoft/pxt-common-packages/libs/core---vm/`
      （vm.cpp/vmload/vmcache/scheduler/target/config/control/keys 等，
      -fno-rtti -fno-exceptions，无第三方依赖）
- [x] 官方宿主 ABI：hw---vm/sdlmain.cpp 的 7 个 pxt_* 函数，
      已对齐到 core/src/platform.h
- [x] 引擎分层：引擎 TS 编进 VM 字节码；C++ 侧只有 screen---ext /
      mixer---ext 设备原语
- [x] screen---ext / mixer---ext C++ 源完整性与 codal 依赖确认（已 vendor
      至 runtime/pxtapp/，剥离 codal 硬件层后独立构建通过）
- [x] Makefile.inc（PXT_OBJS）已生成并接入 core/Makefile
- [x] scheduler.cpp 帧步进改造完成（3 个游戏无关 patch，见 README）
- [x] Sound 合成器位置：mixer---ext，44.1kHz 单声道输出
