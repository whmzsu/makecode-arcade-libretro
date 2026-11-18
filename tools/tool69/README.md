# tools/tool69 —— 工具机（tool69）运维脚本归档

tool69（192.168.1.69）是常驻 Linux 验证节点：交叉编译、arm64 行为回归、
干净发行版构建都在那里跑。机器上的主拷贝在 `/opt/mca-work/bin/`，**本目录
是它的异地备份**——机器挂了或误删时从这里恢复。

> 详细的环境说明（目录布局、chroot、资源纪律）见 `TOOLHOST-README.txt`，
> 它是 tool69 上 `/opt/mca-work/bin/` 里 README 的归档副本。

| 脚本 | 用途 |
|---|---|
| `regress_arm64.sh [帧数]` | 21 张语料 × 2 个核心，在 arm64 chroot 里逐张比对 FINAL-FRAME-HASH |
| `compare_regress.py` | 读取 regress 输出，产出 report.md |
| `ci_matrix.sh` | 多发行版/多构型干净构建矩阵 |
| `toolhost_daily.sh` | 每日体检（回归 + 矩阵 + 报告落盘） |
| `measure_stack.sh` | 栈用量测量 |
| `vm_boot_check.sh` | arm64 虚拟机（TCG）启动自检 |

**注意**：
- 这些脚本里的路径是 tool69 机器专属（`/opt/mca-work/...`），在别的机器上
  不能直接跑，只作恢复/参考用。
- 发版校验脚本不在这里——权威版本是 [`tools/verify_release.py`](../verify_release.py)，
  out/toolhost 里曾有一份旧快照（缺 32 位平台断言），**不要再用**。
- 修改这些脚本后：先在 tool69 上改 `/opt/mca-work/bin/`，验证可用后再同步
  回本目录提交，保持两边一致。
