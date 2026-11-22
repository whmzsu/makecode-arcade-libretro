# tool69 新增文件/文件夹规则（速查）

> 现场完整版：`/opt/mca/README.md`（本文件已并入，仓库内保留规则速查）
> 权威设计：项目仓库 `docs/TOOLHOST-LAYOUT.md` §八
> 更新日期：2026-11-22

## 一句话规则

新增原则上只允许落在 `/opt/mca/` 四层内；唯一例外是本文件所述的根目录入口
`README.md`——根目录只再允许 `env.sh` 和 `README.md`，不再增加其他根文件或同级
`mca-*` 目录。路径先写进 `env.sh`，脚本只用 `$MCA_*` 变量；旧的三套路径已永久停用，
没有兼容软链。

## 归属判定

| 要新增的东西 | 放到哪里 | 备注 |
|---|---|---|
| 根目录入口说明（仅此一份） | `/opt/mca/README.md` | 完整使用与规则；不再增加其他根文件 |
| 编译器、SDK、venv、chroot、VM 镜像、长期工具 | `toolchain/` | 装好后基本不动；新路径必须先写进 `env.sh` |
| 源码、从 Windows 同步的文件 | `src/` | 唯一真相源；构建产物不落这里 |
| 活跃构建工作区、staging 副本 | `work/build/workspace/<name>/` | 全部可再生，用完即弃 |
| 中间测试版本 | `work/build/test/<version>/` | 非正式产物；默认保留 3 个 |
| 本地正式构建产物 | `work/release/local/<version>/` | 按版本；默认保留 5 个 |
| GitHub CI 发版产物下载 | `work/release/ci/<version>/` | 按版本；默认保留 5 个，视为只读原件 |
| 回归/矩阵/体检/每日报告 | `work/reports/` | 只放报告产出物 |
| 运维脚本 | `work/scripts/` | 只 source `env.sh`，只用 `$MCA_*` 变量 |
| 一次性分析脚本、临时输出 | `work/tmp/` | 用完清理，不得长期驻留 |
| 第三方 `.pxt64` 语料 | `work/fixtures/carts/` | 永远不进 git 仓库 |
| 旧产物、旧快照、退役脚本 | `archive/<YYYYMMDD>/` | 只 `mv` 不 `rm`，并更新 `MANIFEST.md` |

版本保留：正式产物 `release/local` 与 `release/ci` 各留最新 **5** 个完整版本，
中间测试 `build/test` 留最新 **3** 个；完整标记 local/test=`BUILD-INFO.txt`、
ci=`SOURCE.txt` + `SHA256SUMS`。超出由 `prune_versions.sh --dry-run` 预览后归档。

## 命名与引用纪律

1. 一律小写 + 短横线；名字表达“是什么”，不表达“谁建的”。唯一例外是根目录入口 `README.md` 的大写文件名。
2. 路径只在 `env.sh` 定义一次；脚本里禁止硬编码 `/opt/mca-*` 前缀字面量。
3. 旧三目录与旧变量名一律不再使用，只允许出现在归档还原清单里。
4. 新增大文件一律进 `/opt` 分区；根分区空间有限。
5. 每次新增后按设计文档 §4.2 做一次路径审计；除归档还原清单外应零命中。

## 新增顺序

定层级 → 如需新路径先改 `env.sh` → 建目录 → 写引用 → 路径审计 → 提交/同步。

完整背景与映射表见项目仓库 `docs/TOOLHOST-LAYOUT.md`。
