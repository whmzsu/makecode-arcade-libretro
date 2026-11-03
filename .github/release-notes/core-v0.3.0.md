# MakeCode Arcade core v0.3.0

## 范围收敛：移除即时存档与快进/慢放

从本版本起，核心**只做官方模拟器同等的基本功能**：能开机、能玩、能重置、
能退出。不再做即时存档 / 读档（Savestate / Rewind），也不做快进 / 慢放。

- `retro_serialize_size` 恒为 0，RetroArch 会自动禁用「保存状态」「回退」
  与 run-ahead；核心内部与存档相关的全部机制（固定地址映射、快照序列化、
  暂停握手、卡带指纹校验、模块区间重定位）已整体移除，代码量净减约 1500 行。
- 快进 / 慢放本就不需要核心参与（虚拟时钟是帧驱动的，前端取帧越快游戏越快），
  此次把残留的兼容接口（`pxt_set_time_scale`）与文档一并清理。
- 移除的回归基建：`tools/test_savestate.py`（S1–S10）、`tools/test_modrange.py`、
  CI 的 behavior-linux-arm64 / behavior-windows-arm64 / behavior-linux-armhf
  快照 job。保留 miniretro 独立前端 900 帧冒烟（真 aarch64 runner，转正）。

动机：存档/加载跨架构（64/32 位、ASLR、地址可复现）的维护成本远超收益，
多轮真机问题（`pause failed -2`、跨会话 `-10/-13`、PANIC 840）都源于此。
砍掉之后核心回到"一个确定、简单、快的字节码解释器"本体。

## 注意

- **旧版本（≤0.2.15）生成的 `.state` 存档文件全部失效**——本版本起不再有
  存档功能，请直接删除。
- 重置游戏仍可用：SELECT 键（默认绑定，可在 Controls 改绑）或前端
  「重启游戏」。

## 修复（随移除一并解决）

- 32 位平台（armhf/armv7）候选基址按 64 位写死导致的堆分配 bug
  （`0x2000000000` 截断为 0 与 NULL 相等误判成功）——随固定地址机制删除。
- armhf 上 `retro_serialize` 返回 -1（`pause failed`）——该路径已不存在。

## 验证

- 本地（Windows x64，zig 全量重编）：ping 冒烟 300 帧、按键回归 16/16、
  生命周期回归全过（换游戏三连 / dlclose / 重置音频 / panic 自愈）。
- CI：全平台编译矩阵 + miniretro 冒烟。

---

MakeCode Arcade core 是非官方的第三方项目，与 Microsoft 无关。
