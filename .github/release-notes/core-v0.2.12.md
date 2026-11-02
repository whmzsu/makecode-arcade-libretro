**MakeCode Arcade (unofficial) — libretro core v0.2.12**

> 非官方核心，与 Microsoft 无关联。只播放 `.pxt64` 字节码镜像；
> MakeCode Arcade 的 `.png` 卡带里只有源码、不是可玩游戏，需先用
> [cartconv](https://github.com/whmzsu/makecode-arcade-libretro/releases) 或 `tools/png2pxt64.py` 转换。

## 修掉真机读档失败：`[MCA] unserialize: pause failed -2`

v0.2.11 让存档可以跨会话，但真机（RK3566 / ArkOS）上实测**保存正常、加载失败**：

```
[libretro ERROR] [MCA] unserialize: pause failed -2
[ERROR] [State]: 加载状态失败于 ".../cubeland.state1"
```

`-2` 是"请求 VM 停稳，等了 5 秒它没停"。

### 根因

拍快照前核心要先把 VM 停到一个安全点，而 VM 只在**主循环顶部**检查这个请求。
fiber 里只要有一段不调 `pause` 的循环（游戏初始化、紧凑的 CPU 循环），解释器
就会一直跑到它自然让出为止 —— 主循环顶那个检查点根本轮不到。桌面上这段通常
只有几十毫秒（所以自动化测试全绿），真机 ARM 慢一个数量级，很容易超过 5 秒，
读档就被放弃了。

桌面用"10 亿次自增、不调 pause"的探针卡带稳定复现了同一条日志：
`pause timeout 5s: spot=2 loops=24 req=151 fibers=3 panic=0`（`spot=2` = 卡在
解释器里，`loops=24` = 帧号完全不再推进）。

### 修法

解释器自己每 2048 条指令查一次暂停请求（一次 volatile 读，热路径开销 <1%），
命中就把"下一条指令"存进 `resumePC` 并结束本次 `exec_loop` —— 调度器回到主循环
顶后立刻看到请求并停稳。无论 VM 在跑什么，几毫秒内都能暂停。

顺带把等待预算从 2 秒提到 5 秒，并在超时时往 `mca_trace.log` 写一行诊断
（VM 当时在干什么 / 帧号 / fiber 数 / panic 状态），以后有类似问题一眼定位。

> 注意：不能在解释器里直接调 `sleep_ms()` / `pause()` —— 它们作用于
> `currentFiber`，而执行到那里的 `ctx` 未必就是当前 fiber（fiber 创建路径会改
> 它），那样会把别的 fiber 的 pc 清掉，实测直接段错误。

## 回归

`tools/test_savestate.py` **S1–S8 全绿**（新增 **S8 忙等**：游戏卡在不调
`pause` 的长循环里读档必须成功；用 v0.2.11 的代码跑这一条必现 `-2`）。
`test_buttons` 16/16、`test_lifecycle` 全过。

S8 需要"忙等探针"卡带，缺失时自动生成，也可以手动：

```
python tools/make_probe_cartridge.py --busy     # -> out/pxt64/busy.pxt64
python tools/test_savestate.py
```

## 升级提示

- 存档格式未变（v3），v0.2.11 存的档可以直接用。
- 本版没有已知的行为变化；如果之前遇到过"读档没反应/加载成功但游戏照旧"，
  这次应当消失。若仍有问题，把 `mca_trace.log` 里 `[snap]` 开头的行发出来
  （`pause timeout` 那行会写清 VM 卡在哪）。
