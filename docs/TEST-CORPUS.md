# 测试语料：跑哪些游戏，为什么是这些

改核心最怕的是"改完 ping 还是好的，玩家一装别的游戏就黑屏"。所以测试要有一批
**固定的真实游戏**，覆盖面按机制分，而不是按名气挑。清单在
`tools/fixtures/corpus.json`（含 share id，可随时重建）。

## 建议纳入的范围

分三层，按改动大小决定跑哪层：

| 层 | 什么时候跑 | 内容 | 耗时 |
|---|---|---|---|
| smoke | 每次改核心 | 8 张：最小基准 + 4 条不同代码路径 + 双人 + 静止画面 | ~1 分钟 |
| full | 发版前 | 20 张，补齐瓦片/物理/粒子/音乐/多关卡/大工程 | ~4 分钟 |
| known_bad | 每次改 VM 相关代码 | 1 张必定 panic 的卡带，确认崩溃仍然"优雅" | 几秒 |

挑选原则（这比"多跑几个游戏"重要）：

1. **每条取卡路径都要有样本**：`.png` 卡带走 cartconv 转换（ping / jumpy /
   falling duck / cube land），分享 id 走 fetch_shared（其余）。这两条路径的
   失败模式完全不同。
2. **代码形态要覆盖两端**：最短的 1.1KB（Save the Forest）和最大的 382KB
   （Mouse Adventure）都在里面。小样本编译快、定位问题快；大样本压 VM 堆和
   初始化路径。
3. **帧率不能都高**：TicTacTwo 是回合制，600 帧只出 5 个独立画面。它的作用是
   证明"画面不动 ≠ 卡死"——如果只留高帧率样本，反而会把自己骗过去。
4. **必须有双人样本**：GalgaMulti / Secure the river / TicTacTwo 走的是
   `arcade-mp` 扩展和多输入通道，和单人路径不是一套代码。
5. **留一个已知会崩的**：Strawberry Slam 固定在第 5133 tick 触发
   `PANIC_CAST_FROM_UNDEFINED`。它跑不过不是缺陷，价值在于「崩溃时核心是否干净
   退出、能否自愈」这条路径始终有人踩。

## 语料现状（2026-11-03）

20 张卡带全部来自官方社区清单 `docs/GameList.json` 与仓库里的 4 张真实 `.png`
卡带，`libretro_host.py` + 发布版 windows-x64 核心，每张 600 帧、第 60 帧按一次 A：

| 结果 | 张数 | 说明 |
|---|---|---|
| ok | 19 | 帧数跑满、有独立画面、非黑比例正常 |
| panic | 1 | Strawberry Slam，游戏自身问题（见下） |
| 编不出来 | 1 | Asphodel，依赖硬件扩展（已排除，见下） |

机制覆盖（按 `main.ts` 实际调用统计，不是靠名字猜）：

| 机制 | 覆盖 | 机制 | 覆盖 |
|---|---|---|---|
| 图像资源 | 16/18 | 瓦片地图 | 8/18 |
| 计时器 | 15/18 | 音乐/音效 | 7/18 |
| 物理 | 14/18 | 文字/气泡 | 6/18 |
| 粒子/特效 | 13/18 | 双人/多人 | 4/18 |
| 数组/循环 | 12/18 | 扩展精灵数据 | 4/18 |

帧率分布也很宽：27fps（GalgaMulti / Pigeon）到 1fps（Mouse Adventure 停在标题、
TicTacTwo 回合制）。**同一个".pxt64 能加载"不代表跑得对，只有分布铺开才能看出
个别卡带的问题。**

## 真机抽查（R36S / RK3326，918MB 下限机型）

抽 5 张（含最大镜像 382KB、以及那张已知会崩的）在设备上原生跑 600 帧：

| 卡带 | 结果 |
|---|---|
| Blocky_Boss_Battle | 正常，**音频峰值 1264（28 个有声采样帧）** |
| arcade-cube-land | 正常（基线对比） |
| Pigeon\_\_Deliverance | 正常 |
| Mouse_Adventure | 正常（最大镜像） |
| Strawberry_Slam | 同样崩在**第 5133 tick、PANIC 980** |

两点值得记：

1. **音频链路是通的**。桌面批量冒烟里几乎所有卡带音频峰值都是 0，一开始怀疑
   音频没出来；设备上射击类游戏（Blocky Boss Battle）一触发音效就有峰值 1264。
   所以桌面那些 0 只是"600 帧里没触发音效"，不是故障。
2. **panic 行为跨平台一致**（同样 tick、同样编号），说明这条崩溃路径不依赖
   平台，用桌面就能复现，不必上机。

## 本次发现

### 1. Strawberry Slam 在第 5133 tick 崩（游戏自身，不是核心）

```
failed type check for 0000000000000000 @0000000000000000
 PANIC 980            ← PANIC_CAST_FROM_UNDEFINED
```

- 与按键无关：按 A 和不按，崩在同一 tick；
- 依赖都已锁版本（`arcade-sprite-data#v0.1.0` 等），不是版本漂移；
- `assets.json` 是空的，不是"资源没打进镜像"；
- 源码里有 `sprites.readDataSprite(tempSprite, "hitbox")`，取不到精灵后当对象用，
  是最典型的 undefined 强转。

结论：这是游戏的逻辑 bug，核心的处置是对的（panic 而不是踩内存）。这类卡带
留在语料里的意义是盯住"崩溃路径"本身。

v0.3.2 起这条路径的判定升级为：panic 之后**宿主进程必须活着**（前端不被带
崩），自动重启最多 3 次然后停机；`sweep_carts.py` 把它单列为 `panic`，
与真崩（`crash`，宿主非零退出）区分。

### 2. Asphodel follows directions 编不出来（合理排除）

```
thermometer/temperature.cpp(6): cannot determine value of Hot = SENSOR_THRESHOLD_HIGH
lightsensor/lightsensor.cpp(27): cannot determine value of Dark = SENSOR_THRESHOLD_LOW
```

它依赖温度 / 光照传感器扩展。这类扩展在 `hw---vm` 变体下没有对应硬件常量，
编不出来是正常的——掌机上本来也没有这两个传感器，玩家拿这张卡带也跑不了。
**不要为了让它编过而去给 VM 变体造假常量**，那会把问题挪到运行时更难查。

## 怎么复现

```bash
# 1) 取源码 + 编译成 .pxt64（在 makecode-arcade-cartconv 仓库）
python tools/fetch_shared.py --manifest <corpus.json 里的 games> -o out/testcarts

# 2) .png 卡带那条路径
node cartconv.js png_examples/*.png -o out/testcarts

# 3) 批量冒烟（在本仓库）
python tools/sweep_carts.py out/testcarts -n 600 --press A --press-at 60 --release 90
```

`sweep_carts.py` 默认用发行包里那个核心（"发出去的那个"），结果写
`out/sweep.json`；判定分 ok / static / blank / short / crash 五类，异常会单独列出。

`.pxt64` 不要提交进仓库：那是第三方游戏内容，清单里有 id，谁都能重建。

## png_examples 里的卡带

- `arcade-*.png`（4 张）：官方分享页下载的真实卡带，格式基准。
- `community-*.png`（17 张）：由 cartconv 的 `tools/make_cart_png.py` 从分享 id
  源码生成的卡带（payload 结构与官方一致，编码算法照抄 pxt `encodeBlobAsync`，
  每张都用参考解码器回读校验过；封面图是核心实跑截的游戏画面）。

注意两点：

1. **核心不能直接运行 .png**——`retro_load_game` 只认 .pxt64 字节码，PNG 文件
   送到核心会在通用镜像校验处失败（日志：`not a valid .pxt64 image`），核心
   不解码 PNG、也不做额外提示。.png 的用途是官方模拟器拖拽导入；跑核心前先
   `python tools/png2pxt64.py` 转换。
2. **bpp=3 是上游 pxt 的坑**：官方编码器与解码器在 bpp=3 下互不一致（实测第 0
   字节就错），真实卡带也只有 bpp 1/2/4。`make_cart_png.py` 已跳过 3。

## 还缺什么（下一步）

- **音效**：桌面 600 帧样本大多没触发音效（真机抽跑已确认链路正常）。要成规模
  验证，得加长跑 + 多按键，或者直接上机听。
- **长跑**：没有跑过 5 分钟以上，帧循环里的缓慢泄漏看不出来。
- **堆上限**：最大的镜像才 382KB，离 1MB VM 堆还远。缺一个故意吃满堆的样本，
  用来验证 `soft_panic` 横幅 + 自动重启这条降级路径。
- **异常路径样本已补**：`tools/fixtures/trycatch-main.ts`（throw→catch），
  覆盖 VM 的 op_try / longjmp 回跳链——v0.3.2 之前它在 mingw 上会把宿主
  带崩（STATUS_BAD_STACK），修复后必须保持不崩。
- **真机批量**：只抽了 5 张，剩下的 15 张还没在 RK3566 / RK3326 上跑过；两台
  都还没做"跑满 20 张 + 记录帧率"的完整表。
- **CI 化**：`sweep_carts.py` 目前是本地脚本（工具机每日 03:20 定时跑全量）。
  v0.3.5 起 `contract_test`（契约自检 + UBSan 构型）已进 GitHub Actions 的
  `makefile-compat` job；语料级回归仍没进 CI——若把 5 张 smoke 卡带放进去
  （需要解决第三方内容不入库的问题——可以 CI 里现场按 id 拉），能挡住"某次改动
  只崩个别游戏"这类回归。
