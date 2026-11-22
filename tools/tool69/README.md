# tools/tool69 —— 工具机（tool69）运维脚本归档

tool69（192.168.1.69）是常驻 Linux 验证节点：交叉编译、arm64 行为回归、
干净发行版构建都在那里跑。机器上的主拷贝在 `/opt/mca/work/scripts/`，**本目录
是它的异地备份**——机器挂了或误删时从这里恢复。

> 完整使用与目录规则见项目 `docs/TOOLHOST-README.md`，现场副本在 `/opt/mca/README.md`；
> 脚本明细见 `TOOLHOST-README.txt`，目录设计见 `docs/TOOLHOST-LAYOUT.md`。

| 脚本 | 用途 |
|---|---|
| `regress_arm64.sh [帧数]` | 21 张语料 × 2 个核心，在 arm64 chroot 里逐张比对 FINAL-FRAME-HASH |
| `compare_regress.py` | 读取 regress 输出，产出 report.md |
| `ci_matrix.sh` | 多发行版/多构型干净构建矩阵 |
| `toolhost_daily.sh` | 每日体检（回归 + 报告落盘） |
| `measure_stack.sh` | 栈用量测量 |
| `vm_boot_check.sh` | arm64 虚拟机（TCG）启动自检 |
| `prune_versions.sh [--dry-run]` | 版本保留归档：release/local+ci 各 5、build/test 3 |

`env.sh` 也在此处留仓库备份；现场权威版是 `/opt/mca/env.sh`，修改后两边保持一致。
构建/发版目录分层：`build/workspace` 活跃工作区、`build/test` 中间版本、
`release/local` 与 `release/ci` 正式产物。

**注意**：
- 所有脚本先 `source /opt/mca/env.sh`，之后只用 `$MCA_*` 变量，禁止写死
  `/opt/mca-*` 旧路径。旧三目录（mca-tools/mca-work/mca-data）已于 2026-11-22
  硬切换删除，无兼容软链；目录布局见 `docs/TOOLHOST-LAYOUT.md`。
- 这些脚本里的路径是 tool69 机器专属，在别的机器上不能直接跑，只作恢复/参考用。
- 发版校验脚本不在这里——权威版本是 [`tools/verify_release.py`](../verify_release.py)
  和 [`tools/check_pe.py`](../check_pe.py)。部署到 tool69 时两者必须一起拷到
  `$MCA_BIN/`（verify_release.py 用 `HERE/check_pe.py`，缺了会导致 PE 体检 FAIL）。
- 修改这些脚本后：先在 tool69 上改 `/opt/mca/work/scripts/`，验证可用后再同步
  回本目录提交，保持两边一致。
