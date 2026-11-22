# tool69 目录规划（第一性原理设计）

> tool69 / 192.168.1.69 —— MakeCode Arcade libretro 项目的常驻 Linux 工具机。
> 本机上所有工具、所有磁盘空间，都只为这一个项目服务。
> 设计日期 2026-11-22 · **已于 2026-11-22 完成硬切换**；新增文件/目录速查见 §八。

---

## 一、设计出发点

抛开现有目录，从目的倒推三条原则：

1. **按生命周期分层，不按资源类型分层。** 通用服务器才按"工具/工作/数据"分类。
   单项目专用机唯一有意义的维度是**变更速度**：工具链几个月动一次、源码和产物天天变、
   历史版本永不回头。三者对备份、清理、权限的需求完全不同，必须物理分开。
2. **每个逻辑实体只有一个权威位置。** 现在源码有 4 份、musl 工具链有 2 份、
   产物散在 4 处。混乱的根源不是目录名字，是没有单一真相源。
3. **路径单点定义。** 现有 6 个脚本把路径写死，这才是"不敢重组"的真正障碍。
   把所有路径收进 `env.sh` 做成变量，脚本只认变量，目录就能随时调整而脚本不动。

---

## 二、目标架构

```
/opt/mca/                        ← 唯一项目根
├── env.sh                       ← 全部路径的唯一定义点（脚本 source 后只认变量）
├── README.md                    ← 根目录唯一入口说明（完整使用与规则）
├── src/                         ← 唯一源码树（git 主树，从 Windows 同步）
├── toolchain/                   ← 稳定层：装好后基本不动
│   ├── bin/                     ├─ zig
│   ├── ndk -> android-ndk-r27c  ├─ Android NDK + SDK
│   ├── android-sdk/             │
│   ├── musl-cross/              ├─ Lakka 全静态（唯一一份）
│   ├── venv/                    ├─ Python 3.12
│   ├── relay/                   ├─ ctr/vita/nx 工具链 + RA 树 + fetch.sh
│   ├── sysroot-arm64/           ├─ arm64 chroot
│   ├── sysroot-amd64/           ├─ amd64 干净 rootfs
│   └── vm/                      └─ arm64 虚拟机镜像
├── work/                        ← 活跃层：全部可再生，可整体清空
│   ├── build/                   ├─ 中间测试版本与活跃工作区（见 §四）
│   │   ├── workspace/           ├─ ctr / vita / vm-measure
│   │   └── test/                └─ 中间测试版本，按版本分目录
│   ├── release/                 ├─ 正式发版产物（与 build 并排）
│   │   ├── local/               ├─ 本地构建正式产物，按版本分目录
│   │   └── ci/                  └─ GitHub CI 发版产物下载，按版本分目录
│   ├── reports/                 ├─ 回归/矩阵/体检/每日报告
│   ├── fixtures/carts/          ├─ 21 张 .pxt64 语料（第三方，勿入仓库）
│   ├── scripts/                 ├─ 运维脚本
│   └── tmp/                     └─ 一次性分析脚本
└── archive/                     ← 冷层：只进不出，带 MANIFEST.md
    ├── 20261122/                ├─ 本次归档
    └── <YYYYMMDD>/              └─ 后续按日期分卷
```

四层的职责边界：

| 层 | 变更频率 | 内容 | 清空后果 |
|---|---|---|---|
| `src/` | 天级 | 唯一真相源，git 管理 | 重新同步即可 |
| `toolchain/` | 月级 | 编译器、SDK、chroot | 重新下载安装（代价高，故单独一层） |
| `work/` | 天级 | 中间构建、正式产物、报告、语料 | 全部可再生，语料需重新拷 |
| `archive/` | 只进不出 | 历史版本、旧产物、一次性探索 | 不可再生，故只移不删 |

---

## 三、现状 → 目标映射

### 3.1 工具链（进 `toolchain/`）

| 现状 | 目标 | 动作 |
|---|---|---|
| `/opt/mca-tools/bin/zig` | `toolchain/bin/zig` | mv |
| `/opt/mca-tools/android-ndk-r27c` | `toolchain/android-ndk-r27c` | mv |
| `/opt/mca-tools/ndk`（软链） | `toolchain/ndk` | 重建软链 |
| `/opt/mca-tools/android-sdk` | `toolchain/android-sdk` | mv |
| `/opt/mca-tools/venv` | `toolchain/venv` | mv |
| `/opt/mca-data/tools/aarch64-linux-musl-cross` | `toolchain/musl-cross` | mv（唯一一份，删 scratch 里那份重复） |
| `/opt/mca-data/tools/relay` | `toolchain/relay` | mv（含 extract 工具链、RA 树、fetch.sh） |
| `/opt/mca-work/arm64-root` | `toolchain/sysroot-arm64` | mv |
| `/opt/mca-work/amd64-root` | `toolchain/sysroot-amd64` | mv |
| `/opt/mca-work/vm` | `toolchain/vm` | mv |
| `/opt/mca-tools/env.sh` | `env.sh` | 重写（见 §四） |

### 3.2 源码（进 `src/`，见 §四的合并策略）

| 现状 | 目标 | 动作 |
|---|---|---|
| `/opt/mca-work/repo` | `src/` | mv（作为唯一真相源） |
| `/opt/mca-work/src` | 并入 `src/` 或归档 | 视构建脚本是否支持离树构建，见 §四 |
| `/opt/mca-data/scratch/v0310` `repo-p2` | `archive/` | mv（旧快照） |
| `/opt/mca-data/scratch/ctr-build` `vita-build` | `work/build/workspace/ctr` `work/build/workspace/vita` | mv（活跃构建工作区） |
| `/opt/mca-data/scratch/vm-measure` | `work/build/vm-measure` | mv（32 位栈测量） |

### 3.3 工作产物（进 `work/`）

| 现状 | 目标 | 动作 |
|---|---|---|
| `/opt/mca-work/bin/*.sh` `*.py` | `work/scripts/` | mv |
| `/opt/mca-work/reports` | `work/reports/` | mv |
| `/opt/mca-work/mca/carts` | `work/fixtures/carts/` | mv |
| `/opt/mca-work/mca/libretro_host.py` | `work/scripts/` | mv |
| `/opt/mca-work/tmp` | `work/tmp/` | mv |
| `/opt/mca-work/cores` `buildcheck` `releases/*` | `archive/` | mv（旧产物） |
| `/opt/mca-data/scratch/relbuild-*` `rel038` `ra-core-pack` `artifacts` | `archive/` | mv（旧构建树） |
| `/opt/mca-tools/packages` | `archive/` | mv（1.7G 安装包存档） |
| `/opt/mca-work/` 顶层散件 | `archive/` | mv |
| `/opt/mca-data/scratch/aarch64-linux-musl-cross*` | `archive/` | mv（323M 重复 + 104M tgz） |

---

## 四、两个必须解决的设计问题

### 4.1 源码为何有两份，怎么合一

`repo/` 是同步自 Windows 的 git 主树，`src/` 是发版构建树——分开的原因是
`build_target.sh` 会在源码目录里生成 `dist/`、`build/`，污染主树。
**这不是冗余，是构建方式导致的必然。**

第一性原理要求唯一真相源，解法有两条，需先验证再选：

- **方案 A（推荐）**：`src/` 为唯一源码；构建前 `rsync` 到 `work/build/workspace/<target>/` 再编译，
  产物永不落源码树。前提是 `build_target.sh` / `build_musl.sh` 不依赖脚本自身相对路径
  （脚本在 `tools/` 下，离树后要用绝对路径调用）。
- **方案 B（保守）**：保留两份但明确分工——`src/` 唯一真相源，`work/build/workspace/` 是它的
  构建 staging 副本；`repo/` 归档。若脚本无法离树，用 `cp -a src work/build/workspace/<target>` 后编译。

**这一步风险最高，放最后做，且先在一处验证脚本可离树。** 未验证前不删任何一份源码。

### 4.2 硬切换规则（不保留旧路径）

**决定：不留 symlink、不留旧变量别名。** 切换完成后
`/opt/mca-tools` / `/opt/mca-work` / `/opt/mca-data` 三个路径必须**不存在**，
后续所有使用（脚本、cron、文档、会话）一律按新路径走。

规则如下：

1. **路径唯一定义点**：`env.sh` 定义
   `MCA_ROOT` / `MCA_SRC` / `MCA_TOOLCHAIN` / `MCA_WORK` / `MCA_ARCHIVE`。
   脚本只 source env.sh 并用变量，禁止出现 `/opt/mca-*` 字面量；
   旧变量名 `MCA_TOOLS` / `MCA_MUSL` 等一并停用，不做别名。
2. **切换前清点引用**：grep 出全部引用点并逐个改完——cron、脚本
   （`ci_matrix.sh` / `toolhost_daily.sh` / `relay/fetch.sh` 等）、env.sh、
   本地文档、项目 MEMORY。唯一例外是 `archive/MANIFEST.md` 里为还原记录的原路径。
3. **切换窗口**：暂停 cron → 改脚本/cron 引用 → 一次性 `mv` 三个旧目录 →
   恢复 cron → 立即验收。窗口内不跑构建，接受中间态不可运行。
   **没有 symlink 兜底，所以"改引用"和"搬迁"必须在同一窗口内一次做完。**
4. **验收硬条件（全部满足才算切完）**：
   - `test ! -e /opt/mca-tools && test ! -e /opt/mca-work && test ! -e /opt/mca-data`
   - `grep -rE '/opt/mca-(tools|work|data)'` 扫新树脚本与文档，零命中（MANIFEST 除外）
   - `source env.sh` 后五个变量均指向新目录且可访问
   - cron 次日报告正常产出；21 卡末帧哈希与切换前一致
5. **回退**：失败时按 `archive/MANIFEST.md` 反向 `mv`，并用切换前备份的
   脚本/cron 还原引用。因此切换前必须先留一份脚本与 crontab 快照。
6. **后续新增**：新脚本、新文档、新会话一律以 `/opt/mca/` 为准；
   新增工具先改 env.sh 再写引用；临时分析放 `work/tmp/`，
   不得在 `/opt` 根下再建 `mca-*` 目录。

---

## 五、迁移顺序（每步可独立回滚）

| 阶段 | 内容 | 风险 | 验证 |
|---|---|---|---|
| 0 | 写新 `env.sh`（仅新变量名），建 `/opt/mca/` 骨架，**清点全部旧路径引用** | 无 | 引用清单完整、脚本/crontab 已备份 |
| 1 | 归档死内容（§3.3 中进 archive 的项） | 低 | 引用检查仍指向现状 |
| 2 | **切换窗口**：暂停 cron → 改脚本/cron 引用 → 一次性 `mv` 三个旧目录（含工具链/work/源码）→ 恢复 cron | 高 | §4.2 验收硬条件全过、全矩阵 + 21 卡哈希一致 |
| 3 | 源码合一（§4.1，最后做） | 高 | 21 卡末帧哈希全一致 |

任何阶段失败，按 `archive/MANIFEST.md` 反向 `mv` + 脚本/crontab 备份回退。
阶段 2 因无 symlink 兜底，**必须在同一维护窗口内一次做完并当场验收**。

---

## 六、命名与约定

- **一律小写 + 短横线**，目录名表达"是什么"不表达"谁建的"。
- **`work/` 下只放可再生内容**，任何"丢了要重下几十分钟"的东西都必须进 `toolchain/`。
- **`archive/` 按日期分卷**，卷内保留原目录结构，`MANIFEST.md` 记录
  `原路径 → 归档路径 → 日期 → 还原命令`，只 `mv` 不 `rm`。
- **路径只在 env.sh 定义一次**，脚本里禁止再出现 `/opt/mca-*` 字面量。
- **旧路径永久停用**：`/opt/mca-tools` `/opt/mca-work` `/opt/mca-data` 及旧变量名
  一律不再使用，仅允许出现在 `archive/MANIFEST.md` 的还原记录里。
  每次迁移或新增脚本后用 §4.2 的 grep 做一次路径审计，零命中才算干净。
- 语料 `.pxt64` 是第三方内容，永远只在 `work/fixtures/`，不进 git 仓库。

---

## 七、不变的运行约束

- `/opt` 是 `LABEL=workbuddy` 整块分区（153G，剩约 128G）；根分区仅剩 39G，
  新增大文件一律进 `/opt`。同分区 `mv` 是原子改名，不复制数据。
- 共用桌面机纪律：不改服务、不重启、编译一律带 `nice`、不做性能基准。并行度分两类：
  CI 模拟 `ci_matrix.sh` 固定 `-j2`；前端整包 `build_frontend_bundles.sh` 取 `MCA_BUILD_JOBS`
  （默认 6，留 2 核给系统，可临时覆盖）。
- cron `20 3 * * *` 跑 `toolhost_daily.sh`，报告落 `work/reports/daily-<时间>/`。
- 网络：github.com / raw / jsdelivr / Docker Hub 不通；api.github.com、清华 PyPI/apt、
  npmmirror、dl.google.com、cloud.debian.org 通。musl.cc 需分片并发下载。
- 干净 Debian（g++ 12）依赖 GLIBC_2.34，发版 zig 包钉 2.17，两套 CI 分工不可合并。

---

## 八、新增文件/文件夹规则（速查）

任何新增之前先回答一个问题：**它属于哪一层？** 原则上只允许落在 `/opt/mca/`
四层内，不得在 `/opt` 根下再建 `mca-*` 同级目录。唯一例外是根目录的入口说明
`README.md`：它和 `env.sh` 一样是一眼可见的入口，仅此一份，
后续不再增加其他根文件。

| 要新增的东西 | 放到哪里 | 备注 |
|---|---|---|
| 根目录入口说明（仅此一份） | `/opt/mca/README.md` | 完整使用与规则；不得再添其他根文件 |
| 编译器、SDK、venv、chroot、VM 镜像、长期工具 | `toolchain/` | 装好后基本不动；新增路径必须先写进 `env.sh` |
| 源码、从 Windows 同步的文件 | `src/` | 唯一真相源；构建产物不落这里 |
| 活跃构建工作区、staging 副本 | `work/build/workspace/<name>/` | 全部可再生；用完即弃 |
| 中间测试版本 | `work/build/test/<version>/` | 非正式产物；默认保留 3 个 |
| 本地正式构建产物 | `work/release/local/<version>/` | 按版本；默认保留 5 个 |
| GitHub CI 发版产物下载 | `work/release/ci/<version>/` | 按版本；默认保留 5 个，视为只读原件 |
| 回归/矩阵/体检/每日报告 | `work/reports/` | 报告产出物，不放入口说明 |
| 运维脚本 | `work/scripts/` | 脚本只 source `env.sh`，只用 `$MCA_*` 变量 |
| 一次性分析脚本、临时输出 | `work/tmp/` | 用完清理；不得长期驻留 |
| 第三方 `.pxt64` 语料 | `work/fixtures/carts/` | 永远不进 git 仓库 |
| 旧产物、旧快照、退役脚本 | `archive/<YYYYMMDD>/` | 只 `mv` 不 `rm`，并更新 `MANIFEST.md` |

版本保留策略：`release/local` 与 `release/ci` 各保留最新 **5** 个完整版本，
`build/test` 保留最新 **3** 个。完整版本须带标记文件：
local/test 为 `BUILD-INFO.txt`，ci 为 `SOURCE.txt` + `SHA256SUMS`；
不完整目录只告警不自动归档。超出的版本由 `$MCA_BIN/prune_versions.sh`
移到 `archive/<YYYYMMDD>/{release/local,release/ci,build/test}/`，并写 `MANIFEST.md`。

命名与引用纪律：

1. 一律小写 + 短横线；名字表达“是什么”，不表达“谁建的”。唯一例外是根目录入口说明
   `README.md`（沿用大写文件名以便一眼识别）。
2. 路径只在 `env.sh` 定义一次；脚本里禁止出现 `/opt/mca-*` 字面量。
3. 旧路径 `/opt/mca-tools` `/opt/mca-work` `/opt/mca-data` 和旧变量名永久停用，
   只允许出现在 `archive/MANIFEST.md` 的还原记录里。
4. 新增大文件一律进 `/opt` 分区，根分区空间有限。
5. 每次新增后按 §4.2 的 grep 做一次路径审计，零命中（MANIFEST 除外）才算干净。

新增顺序：**定层级 → 如需新路径先改 `env.sh` → 建目录 → 写引用 → 路径审计 → 提交/同步**。
