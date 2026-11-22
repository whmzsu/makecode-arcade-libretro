# tool69 使用与目录规则（现场 README）

> 现场入口：`/opt/mca/README.md`
> 仓库源：`docs/TOOLHOST-README.md`
> 汇总自：`docs/TOOLHOST-LAYOUT.md` 架构树与 §八、`docs/TOOLHOST-NEW-ITEM-RULES.md`、
> `tools/tool69/TOOLHOST-README.txt`
> 更新日期：2026-11-22。如与项目仓库文档冲突，以仓库为准。

---

## 0. 一句话铁律

先 `source /opt/mca/env.sh`，之后只用 `$MCA_*` 变量；根目录文件只有 `env.sh` 和本 README，
目录只有 `src/ toolchain/ work/ archive/` 四层；旧三目录永久停用，无兼容软链。

---

## 1. 定位与纪律

- tool69（192.168.1.69）是 MakeCode Arcade libretro 项目的常驻 Linux 验证节点，**不是发版机**。
- Windows 本机只负责编辑与 git；交叉编译、arm64 行为回归、干净发行版构建都在这里跑。
- 共用桌面机（3 个会话、Samba/Ceph）：不改服务、不重启；编译一律 `nice` + `-j2`，
  不跑满 8 线程，不做性能基准（qemu 下的 fps 无参考价值，吞吐只认真机 cubexx / rgb30c）。

---

## 2. 目录架构

```text
/opt/mca/                        ← 唯一项目根
├── env.sh                       ← 全部路径的唯一定义点（脚本 source 后只认变量）
├── README.md                    ← 根目录唯一入口说明（本文件）
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
│   ├── build/                   ├─ 中间测试版本与活跃工作区
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
    ├── 20261122/                ├─ 本次硬切换归档
    └── <YYYYMMDD>/              └─ 后续按日期分卷
```

| 层 | 变更频率 | 内容 | 清空后果 |
|---|---|---|---|
| `src/` | 天级 | 唯一真相源，git 管理 | 重新同步即可 |
| `toolchain/` | 月级 | 编译器、SDK、chroot | 重新下载安装（代价高，故单独一层） |
| `work/` | 天级 | 中间构建、正式产物、报告、语料 | 全部可再生，语料需重新拷 |
| `archive/` | 只进不出 | 历史版本、旧产物、一次性探索 | 不可再生，故只移不删 |

---

## 3. env.sh 变量

`source /opt/mca/env.sh` 后可用：

| 变量 | 指向 | 变量 | 指向 |
|---|---|---|---|
| `MCA_ROOT` | `/opt/mca` | `MCA_BIN` | `/opt/mca/work/scripts` |
| `MCA_SRC` | `/opt/mca/src` | `MCA_REPORTS` | `/opt/mca/work/reports` |
| `MCA_TOOLCHAIN` | `/opt/mca/toolchain` | `MCA_FIXTURES` | `/opt/mca/work/fixtures/carts` |
| `MCA_WORK` | `/opt/mca/work` | `MCA_NDK` | `/opt/mca/toolchain/ndk` |
| `MCA_ARCHIVE` | `/opt/mca/archive` | `MCA_MUSL_ROOT` | `/opt/mca/toolchain/musl-cross` |
| `MCA_RELAY` | `/opt/mca/toolchain/relay` | `MCA_VM` | `/opt/mca/toolchain/vm` |
| `MCA_SYSROOT_ARM64` | `/opt/mca/toolchain/sysroot-arm64` | `MCA_SYSROOT_AMD64` | `/opt/mca/toolchain/sysroot-amd64` |
| `MCA_RELEASE` | `/opt/mca/work/release` | `MCA_BUILD` | `/opt/mca/work/build` |
| `MCA_RELEASE_LOCAL` | `/opt/mca/work/release/local` | `MCA_RELEASE_CI` | `/opt/mca/work/release/ci` |
| `MCA_BUILD_WORKSPACE` | `/opt/mca/work/build/workspace` | `MCA_BUILD_TEST` | `/opt/mca/work/build/test` |
| `MCA_KEEP_RELEASE` | `5` | `MCA_KEEP_TEST` | `3` |

新增路径必须先加进 `env.sh` 再写引用；脚本里禁止硬编码项目根路径前缀。

---

## 4. 新增文件/文件夹规则

任何新增之前先回答：**它属于哪一层？** 原则上只允许落在四层内；根目录文件只允许
`env.sh` 和本 README，不再增加其他根文件或同级项目目录。

| 要新增的东西 | 放到哪里 | 备注 |
|---|---|---|
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

版本保留策略：`release/local` 与 `release/ci` 各保留最新 **5** 个完整版本，
`build/test` 保留最新 **3** 个。完整版本须带标记文件：local/test 为 `BUILD-INFO.txt`，
ci 为 `SOURCE.txt` + `SHA256SUMS`；不完整目录只告警不自动归档。
超出版本由 `prune_versions.sh` 移到 `archive/<YYYYMMDD>/...` 并写 `MANIFEST.md`；
先用 `prune_versions.sh --dry-run` 预览。

命名与引用纪律：

1. 一律小写 + 短横线；名字表达“是什么”，不表达“谁建的”。
   例外：根目录入口 `env.sh` 与本 README 沿用大写文件名，以便一眼识别。
2. 路径只在 `env.sh` 定义一次；脚本里禁止硬编码项目根路径前缀。
3. 旧三目录与旧变量名永久停用，只允许出现在归档还原清单里。
4. 新增大文件一律进 `/opt` 分区；根分区空间有限。
5. 每次新增后按项目设计文档的路径审计做一次检查；除归档还原清单外应零命中。

新增顺序：**定层级 → 如需新路径先改 `env.sh` → 建目录 → 写引用 → 路径审计 → 提交/同步**。

---

## 5. 运维脚本（均在 `$MCA_BIN`）

| 脚本 | 用途 |
|---|---|
| `regress_arm64.sh [帧数]` | 21 张语料 × 2 个核心，在 arm64 chroot 里逐张比对 `FINAL-FRAME-HASH` |
| `compare_regress.py` | 读取 regress 输出，产出 `report.md` |
| `ci_matrix.sh` | 多发行版/多构型干净构建矩阵 + 发行包体检 |
| `verify_release.py` | 发行包“装一遍再发”：机器类型、版本、NEEDED、glibc、导出符号、对齐、无 `.pdb` |
| `toolhost_daily.sh` | cron 每日 03:20：arm64 全量回归 + 在线掌机只读巡检 + 报告 |
| `measure_stack.sh` | 栈用量测量 |
| `vm_boot_check.sh` | arm64 虚拟机（TCG）启动自检 |
| `prune_versions.sh [--dry-run]` | 按版本保留策略归档正式产物/测试版本的旧目录 |
| `relay/fetch.sh` | 在 `$MCA_RELAY` 下逐 commit 增量拉取工具链 relay |

`verify_release.py` 与 `check_pe.py` 必须成对部署，并与 Windows 项目的 `tools/` 保持同步；
前者用同目录的 `check_pe.py`，缺失会导致 PE 体检 FAIL。EXPECT 表以 Windows 版为准。
`src/tools/*.sh` 从 git archive 解包后可能丢可执行位，构建前先 `chmod +x`。

---

## 6. 常用命令

```bash
ssh tool69
source /opt/mca/env.sh

cd "$MCA_SRC" && ZIG=zig ANDROID_NDK_HOME="$MCA_NDK" \
    ./tools/build_target.sh aarch64-linux-gnu.2.17

chroot "$MCA_SYSROOT_ARM64" /usr/bin/python3 /root/mca/tools/libretro_host.py \
    /root/mca/core-stage033.so /root/mca/carts/ping.pxt64 120 --turbo \
    --data-dir=/tmp/mca_data

"$MCA_BIN/regress_arm64.sh" 600
"$MCA_BIN/ci_matrix.sh"
```

Windows 侧同步源码：

```bash
git ls-files > list && tar czf src.tgz -T list && scp src.tgz tool69:/opt/mca/
# 到 tool69 后解到 /opt/mca/src/。必须包含 core/libretro.h。
```

---

## 7. 历史结论与约束

- 21 张语料 × 600 帧：真机 0.3.3 与工具机 zig 产物逐张末帧哈希全一致；
  ping `f93e4597…` / cubeland `23210fb3…` 与历史基准对齐；
  唯一异常是已知的 Strawberry_Slam panic（游戏自身 bug）。
- x86_64（gcc 干净版 / zig 版）与 arm64 三路哈希同为 `f93e4597…`，跨架构语义等价。
- 干净 Debian bookworm（g++ 12）编出的 `.so` 依赖 `GLIBC_2.34`；发版 zig 包钉在 2.17。
  两套 CI 分工不可合并。
- Lakka / musl 全静态：musl.cc GCC 11.2.1，`build_musl.sh` 自动识别 triplet；
  功能等价已验证，但**仍未在真实 Lakka 设备上跑过**，这是唯一缺口。
- cron：`20 3 * * *` 跑 `$MCA_BIN/toolhost_daily.sh`，报告落 `$MCA_REPORTS/daily-<时间>/`。
- 网络：github.com / raw / jsdelivr / Docker Hub 不通；api.github.com、清华 PyPI/apt、
  npmmirror、dl.google.com、cloud.debian.org 通；musl.cc 需分片并发下载。
- `/opt` 是独立大分区；根分区空间有限，新增大文件一律进 `/opt`。

完整设计依据见项目仓库 `docs/TOOLHOST-LAYOUT.md`。
