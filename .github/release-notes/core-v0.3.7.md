# MakeCode Arcade core v0.3.7

## 一句话：两个「每张卡带都会踩」的越界，加一个「退出就崩」

这一版的行为改动只有三处，全部来自新装的三类检查工具（覆盖率 / fuzz / 真前端冒烟），
没有新功能。三张基准卡带的末帧哈希与 0.3.6 逐位相同 —— 修的是**装载期和退出期的
内存安全**，不是画面。

**建议升级**：其中一条是「退出 RetroArch 时 SIGSEGV」，如果你在用 Linux/Android 掌机，
大概率已经遇到过。

## 1. 装载期越界写：名字表少分配 7 字节（PATCH 11）

`core---vm/verify.cpp` 里上游的宏少了一对括号：

```c
#define ALLOC_ARRAY(tp, sz) (tp *)xmalloc(sz == 0 ? 1 : sizeof(tp) * sz)
```

唯一的传表达式调用点是 `loadIfaceNames()` 的 `ALLOC_ARRAY(uintptr_t, len + 1)`，
展开后是 `sizeof(uintptr_t) * len + 1` —— **先乘后加**，比需要少分配 7 字节，末尾两个
`dst[i]` 写出了边界。

每张卡带装载都会走到这里（562 个接口名对应分配 4497 字节，应为 4504）。生产路径上
之所以一直没崩，是 glibc 的 malloc 对 16 字节对齐请求有额外余量；Android / musl 的
分配器不保证这一点。ASAN 用精确分配立刻就报 `heap-buffer-overflow` @ `verify.cpp:361`。

修法：宏给 `sz` 补上括号。

## 2. 装载期越界读：段表遍历先算后判（PATCH 12）

```c
// 改之前
for (sect = (VMImageSection *)img->dataStart;
     (next = vmNextSection(sect), (uint64_t *)sect < img->dataEnd); sect = next)
```

逗号表达式从左往右算：走到 `dataEnd` 那一轮**先**读了 `sect->size`，**再**判边界，
等于越界读 4 字节。`countSections()` 里剩余不足 8 字节时同样照读段头。

生产路径上被 `vmStartFile()` 多分配的 16 字节兜住，fuzz 里精确分配即报
`heap-buffer-overflow`（READ of size 4）。修法：先判边界，再读段头。

## 3. 退出即 139：static 析构 + dlclose

用真 RetroArch 无头加载核心，第一次实跑就复现了用户报的那个现象：游戏能玩，**退出
RetroArch 时进程 139**。

原因：pxt 运行时带 static 析构，`retro_unload_game()` 之后前端 `dlclose()` 卸掉核心，
但 `atexit` 链上的析构函数仍然指向那片已解除映射的代码段，进程退出时跳进去就段错误。

修法：链接期加 `-Wl,-z,nodelete`（`core/Makefile` 的 unix 目标 + `tools/build_target.sh`
的 linux/android 分支），产物多一个 `DF_1_NODELETE` 标志，ld.so 退出前不回收。代价是
换核心时不释放内存，单次会话可忽略。

> 已验证范围：Linux x86_64 / aarch64、Android（ELF）。Windows 是 PE，没有对应标志，
> 修复方式取决于前端是否 `FreeLibrary`，本版未改也未实测。

## 4. 验证工具：三个探照灯都进了仓库

这轮的价值不在三个修复，而在「以后能自己发现」。三件工具都带脚本，可重复跑：

| 工具 | 命令 | 首轮结果 |
|---|---|---|
| 覆盖率 | `tools/coverage.sh` | 7294 行可执行，**46%** 覆盖；最大盲区 `screen---ext/image.cpp` 仅 18% |
| fuzz | `tools/fuzz_pxt.sh` | 首次变异第 3 轮即崩（就是上面 PATCH 12）；修后 208,744 runs / 301 秒无崩溃 |
| 真前端 | `tools/retroarch_smoke.sh` | 3 张卡带 3/3 PASS，截图是真实 160×120 画面（就是上面第 3 条） |

顺带修了 `SANITIZE=address` 与 `undefined` 共用对象目录的老坑（切换构型会链到旧对象，
报 `undefined symbol: __ubsan_handle_*`）。现在目录名带 sanitizer 名，`COVERAGE=1` 也是
独立目录，互不覆盖。

## 5. CI

新增 `retroarch-smoke` job（ubuntu-24.04 + 发行版 RetroArch + Xvfb，2 张夹具卡带
各 120 帧）。它守的是 golden hash 守不到的那一层：dlopen 路径、`.info` 解析、
`SET_PIXEL_FORMAT` 协商、退出时的 `dlclose`。

`pxt-patches` 现在校验 **11 条**补丁（PATCH 1/2/3/5~12，PATCH 4 记 RETIRED）。

fuzz 没进 CI：单轮 5 分钟起步、且需要 `-fsanitize=fuzzer` 的全套运行时，留在本地按需跑。

## 验证

- **末帧哈希**：三个基准用例与 0.3.6 逐位相同

  ```text
  ping 300 帧     0556e78f7ce0e828f2dc922d210dc047
  probe 120 帧    e82844f7160449d1767aa69afe788800
  probe 120 帧按A 0eb7884cb40cef0cb549946af3a942ab
  ```

- **契约自检**：`contract_test` 27 项断言，常规与 `--reject-rgb565` 两种构型全过；
  UBSan 构型同样全过。
- **ASAN**：修复前 `arcade-cube-land` 5 帧即报两处越界；修复后 60 帧零报告。
- **fuzz**：208,744 runs / 301 秒，零崩溃。
- **真前端**：`ping` / `probe` / `Jumpy Platformer` 各 120 帧，3/3 PASS，退出码 0。

## 已知限制（未变）

- 32 位前端不支持（PXT64 镜像在 32 位 VM 下无法执行）。
- 即时存档 / rewind / 快进 / 金手指 / Netplay 不提供（v0.3.0 起的范围收敛）。
- Strawberry_Slam 在固定 tick 触发 `PANIC 980`，游戏自身 bug，核心侧不修。
- Switch NX 是独立发布线（`nx-v*`），本版不发 NX 包。

## 升级建议

**建议升级**。画面与 0.3.6 完全一致（末帧哈希逐位相同），但装卡带时少两次越界、
退出前端时不再 139。用 musl 的工具链（Lakka / Knulli / muOS 一类）尤其应该升：那类
分配器不保证 glibc 的对齐余量，第 1 条本来就可能真的踩坏堆。

---

MakeCode Arcade core 是非官方的第三方项目，与 Microsoft、Nintendo 均无关。
