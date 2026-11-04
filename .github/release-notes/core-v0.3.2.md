# MakeCode Arcade core v0.3.2

## 游戏崩溃不再带崩前端（本版最重要的一条）

上一版存在一个真实且高频的缺陷：**游戏一旦 panic，核心会把整个 RetroArch
一起拖死**——玩家看到的是前端直接退出，而不是"游戏出错 → 回到菜单"。

本版把它修掉了，同时修掉同一根因下的第二个隐患（VM 的 `try/catch`）。

### 问题一：panic 后进程 0xC0000005

上游 pxt 的约定是 `soft_panic()` / `target_panic()` **不返回**（在 ESP32 上
直接 `abort()`），VM 里的 C++ 全部按这个约定书写：

```c
if (!isPointer(v)) failedCast(v);   // 不返回
return getVTable((RefObject *)v);   // 于是这里假定 v 一定可用
```

v0.2.14 起把 panic 改成"置一个标志就返回"，于是这些调用方拿着 `undefined`
继续解引用——以社区游戏 Strawberry Slam 为例，它在 `asRefCollection()` 上踩
`PANIC 980`（把 undefined 当数组用）后，进程当场访问违例，前端跟着退出。

**修法**：panic 之后不许 return。在 `mainRunLoop` 装 `setjmp` 回弹点，
VM 线程上的 `soft_panic` / `target_panic` 直接 `longjmp` 回去，由循环顶部的
`if (panicCode)` 分支执行原有的 cleanup 与正常线程退出——前端照旧自动重启
游戏，连续失败 3 次后放弃，**宿主进程始终存活**。

### 问题二：VM 的 try/catch 同样带崩前端

同一根子的另一半，只是在 Windows 上以另一种方式炸：`0xC0000028
STATUS_BAD_STACK`。

mingw-w64 的 `longjmp` 在 x86_64 上走 SEH（`RtlUnwindEx`），逐帧回退必须有
`.pdata`，而本工程此前按 `-fno-unwind-tables` 编译——`longjmp` 自己就崩，
还没轮到 panic 逻辑。游戏里只要用到 `try { … } catch { … }` 就能触发。

**修法**：Windows 目标打开 `-funwind-tables -fasynchronous-unwind-tables`
（DLL 体积 +22KB），panic 回弹点与 VM 异常回跳（`FiberContext::loopjmp`）
共用这套机制，恢复标准的 `setjmp` / `longjmp` 写法。

## 验证

- **卡带回归**：20 张语料全量跑完，19 ok + 1 panic，逐张数值与修复前一致
  （无行为变化）。
- **崩溃样本**：Strawberry Slam 6000 帧 → `PANIC 980` → 自动重启 3 次 →
  到上限停机，**进程退出码 0**，trace 完整走完 cleanup `0/6 → 6/6`。
- **异常样本**：新增 `tools/fixtures/trycatch-main.ts`（throw → catch →
  继续跑），600 帧正常出画面。
- **真机**：RG353M（RK3566）、R36S（RK3326）、RG353V（RK3566）三台都装上
  同一个 arm64 构建，功能与桌面一致——panic 后前端存活、异常样本正常、
  音频正常。性能数据见 `docs/DEVICE-TEST.md` 的 v0.3.2 一节。
- **CI**：64 位编译矩阵 + miniretro 冒烟 + makefile-compat 三平台全绿。

## 已知限制

- R36S（RK3326 / 918MB）跑最重的 `arcade-cube-land` 只有约 1.08 倍余量
  （65 fps，60fps 目标），是全系列下限；其余机型与组合余量均在 1.5 倍以上。
- 32 位前端仍不支持（原因见 v0.3.1 说明）。
- 即时存档 / 快进仍不提供（原因见 v0.3.0 说明）。

## 升级建议

直接覆盖旧的 `.so` / `.dll` / `.dylib` 即可，`.pxt64` 卡带与配置完全兼容。
**强烈建议升级**：本版解决的是"个别游戏把前端一起带死"的问题，
不升级的代价是玩家需要硬重启设备。

---

MakeCode Arcade core 是非官方的第三方项目，与 Microsoft 无关。
