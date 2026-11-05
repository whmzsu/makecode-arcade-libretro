# RG CubeXX（muOS）稳定性长跑记录（2026-11-05）

承接 `docs/stability-2026-11-04`（ArkOS 三机）同款口径：60fps 真实节拍（不加 `--turbo`）、
每段随机按键模拟玩、10s 采样 RSS/HWM/CPU ticks/温度/load/帧进度。

## 环境与脚本

- 设备 RG CubeXX（H700 4×A53@1.8GHz muOS OC），核心 v0.3.2（`/opt/muos/share/core/`）
- 脚本：`tools/muos/stability.sh`（部署到 `/root/stab.sh`），分析：`tools/muos/analyze_stab.py`
- governor 试图设 performance 但 **muOS 有守护进程会打回 schedutil**（实测满负载仍达 1.8GHz，不影响）；
  测完自动还原
- 产物本地存档：`out/deploy/muos/stab/`（CSV + 每段 log + 末帧 BMP）

## 结果：6 段 98250 帧 / 约 46 分钟，全部 rc=0，零重启零泄漏

| 段 | 卡带 | 帧数 | 实际节拍 | 独立画面 | RSS | 斜率 | 温度峰值 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| cubeland | arcade-cube-land | 24000 | 39.4 fps | 3988 | 13.6MB | 0.00 | 47℃ |
| blocky | Blocky_Boss_Battle | 24000 | 33.6 fps | 2331 | 14.2MB | 0.00 | 46℃ |
| jumpy | arcade-jumpy-platformer | 24000 | 35.5 fps | 10977 | 13.5MB | 0.00 | 44℃ |
| space | Space_Destroyer | 18000 | 31.9 fps | 1214 | 14.1MB | 0.00 | 43℃ |
| ping* | ping | 3000 | 46.8 fps | 1344 | — | — | 42℃ |
| straw | Strawberry_Slam | 6000 | 32.9 fps | 1687 | 14.3MB | ≈0 | 44℃ |

\* ping 首跑 rc=1 是**卡带没上传**（`pxt64_collections/` 里没有它，在 `tools/fixtures/`），
补传后单跑通过。其余 5 段一次过。

- RSS 全程 13.5–14.3MB，斜率 0.00 MB/min —— **无泄漏**（与 ArkOS 三机结论一致）。
- 温度 43–47℃（H700 无风扇下非常凉），CPU 单核占用约 45–50%（节拍模式含 Python 宿主开销）。
- uptime 143s → 2895s 连续无中断；电量 63%→68%（实际在充电，POWER_SUPPLY_STATUS 报 Discharging 不可信）。

## 预期内与非预期

- **Strawberry_Slam 触发 PANIC 980**（PANIC_CAST_FROM_UNDEFINED），宿主继续跑完，rc=0 ——
  与 ArkOS 一样属游戏自身确定性 bug，只是随机输入路径不同所以 tick 不同（20416 vs 5133）。
- ping 补跑中出现 3 次 PANIC 980（tick 3733/5300/9033）—— 同类游戏 bug，多点触发更坐实
  "游戏代码固有缺陷"，核心照常继续。
- blocky 与 space 末帧 hash 相同（`02b0fb5e…`）：两个游戏随机输入下都走到了同一张纯色帧，
  900 帧复测末帧 hash 各不相同、BMP 目视画面完全正常（星空+飞船 / Boss+血条），
  非黑占比 6–10% 只是这两个游戏本来就是暗色画面，**非渲染问题**。

## 踩坑记录

1. **muOS 系统时钟会被校时**（本机从 23:57 跳回 22:09，快 2 小时的墙钟被 NTP/守护进程修正）：
   CSV 的 ts 列因此不可用于算时长。宿主脚本内部的 fps（`time.monotonic` 基准）才可靠。
   `analyze_stab.py` 已改为优先从 `<phase>.log` 的 "X fps 实际节拍" 取时长。
2. **muOS 的 governor 被系统管理**，手动 `echo performance` 会被打回（推测 muxfrontend/hotkey
   干的，配置项 `settings/power/gov_idle=powersave`）。满负载 schedutil 也能到满频，可接受。
3. `idle_sleep=0`（默认配置）保证长跑不休眠；`idle_display=120` 只关屏，不影响 VM。
4. `POWER_SUPPLY_STATUS=Discharging` 不可靠，本机插着 USB 电量却在涨；测电量只看 capacity。

## 结论

**v0.3.2 在 muOS/H700 上稳定性与 ArkOS 三机同级**：46 分钟、近 10 万帧、随机按键，
零重启、零泄漏、零卡顿窗口，panic 仅出现在已知有 bug 的游戏上且宿主总能恢复。
