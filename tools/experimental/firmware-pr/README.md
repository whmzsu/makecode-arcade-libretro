# firmware-pr —— 固件收录 PR 工作线（2026-11-09/10，实验归档）

把 mca 核心提交进五个掌机固件上游收录的工作现场。**状态：PR 已发出，
收录与否看上游；这条线不在发版矩阵里，脚本按「重新生成」风格写，重跑前
需按当时版本号改脚本头的 COMMIT/VERSION 常量。**

## 五个目标

| 上游仓库 | 包形态 | 说明 |
|---|---|---|
| [knulli](https://github.com/knulli-cfw/distribution) (kn-*) | `.mk` + es_systems.yml 补丁 | Batocera 分支，RK3326 |
| [muOS](https://github.com/shawly/muOS) | 预编译 zip + `data/manifest` 条目 | `muos-*` |
| [ArkOS](https://github.com/christianhaitian/arkos) | `ArkOSUpdate.sh` 增量补丁 | `rg503` 分支 |
| [dArkOS](https://github.com/christianhaitian/dArkOS) | 同 ArkOS（PR #43：core_info_files `.info` + 4 个 es_systems.cfg MAKECODE 块） | 附带 [retroarch-cores](https://github.com/christianhaitian/retroarch-cores) 的 zip + 索引 PR |
| arkos4clone (a4c-*) | 8 个 es_systems 变体 | RK3566 克隆机固件 |

## scripts/ —— 工作流脚本

| 脚本 | 用途 |
|---|---|
| `gen_patches.py` | 读上游 pristine 副本（当时在 out/pr-patches/orig），产出可 `git apply` 的补丁（LF-only） |
| `gen_prs.py` | 生成五份固件收录 PR 的最终文件树（当时输出 out/pr-final/<n>/），内含 knulli LDFLAGS、VERSION 用 commit SHA 等踩坑修正 |
| `poster.py` | 用 GitHub Contents API 直接向 fork 提交文件并开 PR（不 clone 上 GB 的上游仓库），包目录下 meta.json 描述 upstream/fork/base/branch |
| `complete_darkos.py` | 补全 dArkOS PR #43（.info + es_systems 块）+ retroarch-cores 伴随 PR |
| `fix_darkos.py` / `post_darkos_list.py` / `poster.py` | dArkOS 核心清单的修补与发帖 |
| `muos-build.sh` / `muos-catalogue.sh` / `muos-check.sh` / `muos-commit.sh` | muOS 的核心构建器（上游 muOS RetroArch Core Builder 改造）与目录再生成 |
| `build_retroarch.sh` / `ArkOSUpdate.sh` / `dArkOS*_Update.sh` | ArkOS/dArkOS 侧的 RA 构建 + 固件增量更新脚本改造 |
| `verify_patches.sh` | 校验补丁可干净 apply |
| `arc-addcore` | 往 ArkOS es_systems.cfg 加 core 块的小工具 |

## data/ —— 脚本实际引用的数据

`muos-core.json`（muOS 核心元数据）、`kn-es_systems.yml`（knulli 系统表）、
`a4c-es_systems*.cfg`（arkos4clone 四个变体）、`es_rk3566.xml`、
`darkos_cores.txt` / `darkos-list-body.md`（dArkOS 清单）、
`arkos4clone_cores.json`。

> 上游仓库的完整树快照（a4c-tree.json / kn-tree.json / kn-es_features.yml /
> muos-tree.json 等，约 7 MB）脚本不引用，**没有收编**，留在
> `out/archive/pr-work/`，重跑 gen_patches.py 前需重新从上游拉 pristine 副本。
