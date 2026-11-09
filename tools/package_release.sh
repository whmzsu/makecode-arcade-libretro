#!/usr/bin/env bash
# package_release.sh — 把 dist 里编好的 core 打包成可直接放进 RetroArch 的发行包
#
# 用法：
#   ./package_release.sh            # 只打包主流目标（发版）
#   ./package_release.sh --all      # 打包 dist 下所有目标（包名带完整三元组，不覆盖）
#   ./package_release.sh <triple>…  # 只打包指定目标
#
# 产物：core/dist/release/makecode_arcade_<platform>.zip
#   内含：makecode_arcade_libretro.{so,dll} + makecode_arcade_libretro.info + README.txt
#
# 说明：RetroArch 要求 .info 与 core 二进制同名，这样在「加载核心」列表里
#       才会显示成 "MakeCode Arcade" 而不是裸文件名。

set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
PROJ="$(cd "$HERE/.." && pwd)"
DIST="$PROJ/core/dist"
OUT="$DIST/release"
STAGE_ROOT="$OUT/.stage"
DBG_ROOT="$OUT/.debuginfo"
mkdir -p "$OUT"

# zip 用 python 的 zipfile 生成（保证跨平台确定性、无额外依赖）。
# Ubuntu / macOS 只有 python3，Windows 的 Git Bash 里 python 也能用。
PYTHON_BIN="$(command -v python3 || command -v python || true)"
if [ -z "$PYTHON_BIN" ]; then
  echo "需要 python 3（用来打 zip），但没找到 python3/python" >&2
  exit 2
fi

# 发版目标 = build_matrix.sh 的 MAIN_TARGETS（全部 64 位；32 位目标已整体移除）
MAIN_TARGETS=(
  x86_64-windows-gnu
  aarch64-windows-gnu
  x86_64-linux-gnu.2.17
  aarch64-linux-gnu.2.17
  aarch64-linux-android21
  aarch64-linux-musl
)

case "${1:-}" in
  --all)
    shift
    TARGETS=()
    for d in "$DIST"/*/; do
      t="$(basename "$d")"
      [ "$t" = "release" ] && continue
      compgen -G "$d/makecode_arcade_libretro.*" > /dev/null && TARGETS+=("$t")
    done
    ;;
  "")
    TARGETS=("${MAIN_TARGETS[@]}")
    ;;
  *)
    TARGETS=("$@")
    ;;
esac

# 平台短名：只给主流目标固定名字（用户看到的就是这个）；其余用完整三元组，
# 避免 x86_64-linux-gnu / .2.17 / gnux32 这类变体互相覆盖
#
# 同一平台有两套三元组名：
#   *.2.17                —— 本地 zig 矩阵（glibc 2.17 基线，正式发行包）
#   不带版本号             —— GitHub Actions 用发行版工具链编的（跑者自带基线）
# 只决定 zip 的内容和文件名，两者产出的包名相同（不同时存在，不会互相覆盖）
platform_of() {
  case "$1" in
    x86_64-windows-gnu)        echo "windows-x64" ;;
    aarch64-windows-gnu)       echo "windows-arm64" ;;
    x86_64-linux-gnu.2.17)     echo "linux-x64" ;;
    aarch64-linux-gnu.2.17)    echo "linux-arm64" ;;
    x86_64-linux-gnu)          echo "linux-x64" ;;
    aarch64-linux-gnu)         echo "linux-arm64" ;;
    aarch64-linux-android21)   echo "android-arm64" ;;
    aarch64-linux-musl)        echo "lakka-arm64" ;;
    *)                         echo "${1//\//-}" ;;
  esac
}

# 版本号从源码单点读取，避免 .info 与二进制脱节（曾经硬编码 0.2.0，
# 重编到 0.2.1 后发行包仍显示 0.2.0，无法在 RetroArch 里区分新旧 core）
# 版本号统一来自 kCoreVersion 常量（retro_get_system_info 与运行时横幅都用它）
VERSION="$(sed -n 's/.*kCoreVersion = "\([^"]*\)".*/\1/p' \
           "$PROJ/core/src/libretro_core.cpp" | head -1)"
[ -z "$VERSION" ] && VERSION="0.0.0"

# .info 模板：display_version 用 @VERSION@ 占位，CI 的 NX 打包（.github/
# workflows/nx.yml）也读同一份，避免版本号在两处各写一遍而漂移。
INFO_TMPL="$PROJ/tools/core.info.in"
[ -f "$INFO_TMPL" ] || { echo "缺少 $INFO_TMPL" >&2; exit 1; }
INFO_META="$(sed "s|@VERSION@|$VERSION|" "$INFO_TMPL")"

# 只在"可加载的核心二进制"里挑，绝不靠 glob 顺序。
# 血泪教训（v0.3.0/v0.3.1 的 Windows 包）：`compgen -G ... | head -1` 在 CI 的
# bash 上不保证排序，Windows 产物目录里同时有 .dll 和 .pdb 时挑中了 .pdb，
# 发出去的包里只有调试符号、没有 DLL，README 也照着写了 .pdb 的安装步骤。
# 显式按扩展名优先级找，找不到就是打包失败，不能静默降级。
pick_core() { # $1=目录  → 打印核心文件路径（找不到则空）
  local d="$1" e
  for e in dll so dylib; do
    if [ -f "$d/makecode_arcade_libretro.$e" ]; then
      printf '%s\n' "$d/makecode_arcade_libretro.$e"
      return 0
    fi
  done
  return 1
}

# ---------------------------------------------------------------- strip
# 为什么发行包要 strip：未 strip 的 linux/arm64 核心 5.0 MB，其中 4.3 MB 是
# .debug_*；strip 后 732 KB（zip 从 1.38 MB 降到 287 KB），而末帧哈希逐位
# 不变（0.3.7 三构型实测）。所以玩家拿到的是 strip 版，符号另发 debuginfo。
# Android 由 NDK 打包时已 strip，Windows 产物本来就没有符号表，两者跳过。
#
# strip 工具按目标架构挑：GNU 交叉 binutils 优先，其次 llvm-strip（全架构），
# 最后才试宿主 strip —— 宿主 strip 若是单目标 BFD 会直接报格式错误，所以
# 每个候选都要实际执行并用 readelf 验一下产物，不能只看命令存在。
do_strip() { # $1=源 $2=目标 $3=triple  → 成功返回 0
  local src="$1" dst="$2" t="$3" c
  local -a cands
  case "$t" in
    x86_64-*)  cands=(x86_64-linux-gnu-strip llvm-strip strip) ;;
    aarch64-*) cands=(aarch64-linux-gnu-strip llvm-strip strip) ;;
    *)         cands=(llvm-strip strip) ;;
  esac
  for c in "${cands[@]}"; do
    command -v "$c" >/dev/null 2>&1 || continue
    cp "$src" "$dst" || return 1
    if "$c" --strip-unneeded "$dst" 2>/dev/null && [ -s "$dst" ] \
       && readelf -h "$dst" >/dev/null 2>&1; then
      echo "  strip: $c ($(basename "$src") $(stat -c%s "$src") -> $(stat -c%s "$dst") B)"
      return 0
    fi
    rm -f "$dst"
  done
  echo "  找不到能处理 $t 的 strip 工具（试过 ${cands[*]}）" >&2
  return 1
}

# 从 ELF 的 .note.gnu.build-id 取出 build-id；没有就输出空字符串。
build_id_of() {
  readelf -n "$1" 2>/dev/null | sed -n 's/.*Build ID: *\([0-9a-fA-F]*\).*/\1/p' | head -1
}

ok=0; skipped=0
for t in "${TARGETS[@]}"; do
  dir="$DIST/$t"
  core="$(pick_core "$dir" || true)"
  if [ -z "$core" ]; then
    echo "跳过 $t（无核心二进制 .dll/.so/.dylib）"
    skipped=$((skipped + 1))
    continue
  fi
  plat="$(platform_of "$t")"
  ext="${core##*.}"
  case "$ext" in
    dll|so|dylib) ;;
    *) echo "打包失败：$t 选中的不是核心二进制（$core）" >&2; exit 1 ;;
  esac
  stag="$STAGE_ROOT/$plat"
  rm -rf "$stag"
  mkdir -p "$stag"

  # 谁需要 strip：只有 Linux/ELF 共享库。Android 的 .so 由 NDK 打包时已经
  # strip 过，Windows 的 PE 产物本来就不带符号表（llvm-nm 数为 0），
  # 对它们再 strip 没有收益，跳过。
  want_strip=0
  case "$t" in
    *android*|*windows*) want_strip=0 ;;
    *) [ "$ext" = "so" ] && want_strip=1 ;;
  esac

  dbg_dir=""
  if [ "$want_strip" = 1 ]; then
    if do_strip "$core" "$stag/$(basename "$core")" "$t"; then
      # 原件留一份，稍后单独打成 -debuginfo.zip
      dbg_dir="$DBG_ROOT/$plat"
      rm -rf "$dbg_dir"
      mkdir -p "$dbg_dir"
      cp "$core" "$dbg_dir/$(basename "$core")"
    elif [ "${MCA_REQUIRE_STRIP:-0}" = 1 ]; then
      # 发版环境（CI）里 strip 失败必须红：否则又会静默发出 5 MB 的包，
      # 与固件仓库要求的 strip 产物不一致，事后只能重发。
      echo "::error file=$core::$t 找不到可用的 strip 工具，拒绝打包" >&2
      exit 1
    else
      echo "打包警告：$t 未能 strip，发行包将带上调试信息（体积约 5 倍）" >&2
      cp "$core" "$stag/"
    fi
  else
    cp "$core" "$stag/"
  fi
  printf '%s\n' "$INFO_META" > "$stag/makecode_arcade_libretro.info"
  case "$plat" in
    android-*)
      cat > "$stag/README.txt" <<EOF
MakeCode Arcade — libretro core ($plat)

安装（RetroArch Android，两个目录要拆开）：
  背景：Android 10 起 /storage/emulated/0 是 noexec，core 的 .so 从那里
        无法 dlopen（表现为"能识别核心但加载失败"）。而 .info 只是文本，
        放哪都能读。RetroArch 正好把这两件事分成两个独立设置。

  1) 设置 → 用户界面 → 打开「显示高级设置」
  2) 设置 → 目录 → 核心（Cores）      保持默认（App 私有），不要改到外部存储
  3) 设置 → 目录 → 核心信息（Core Info）→ 指向可写的外部目录，例如
                                   内部存储/RetroArch/info
  4) 把本 zip 里的 makecode_arcade_libretro.info 放进第 3 步的目录
  5) 先解压本 zip（解到 RetroArch 能访问的目录，如 内部存储/Download），
     得到 makecode_arcade_libretro.so；然后
     加载核心 → 「安装或恢复核心」→ 选中这个 .so，它会被拷进核心目录。
     注意：该文件浏览器按平台过滤扩展名，Android 上只列 .so，
     zip 不会出现在列表里，必须先解压；
     而且 Install 只拷 .so、.info 会被丢弃，所以第 3~4 步必须手动做。
  6) 重启 RetroArch → 加载核心列表里出现 "MakeCode Arcade"
  7) 加载内容 → 选 .pxt64

  如果菜单里找不到「核心信息」项：用配置文件给 libretro_info_path 赋值即可
  （设置 → 配置 → 保存新配置 → 编辑该行 → 加载配置）。
  只装 .so 不装 .info 是不行的：实测选中游戏后「选择核心」列表里选不到本核心。

  选包：Android 只有 64 位版（arm64-v8a）。本核心不支持 32 位设备——
        卡带镜像是 PXT64，32 位 VM 跑不了任何游戏，详见 README。

内容格式：
  .pxt64  转换后的 VM 字节码镜像，核心只认这个格式（加载内容里也只列出它）
  .png    MakeCode Arcade 保存的卡带，里面只有源码、不是游戏本身，
          核心不接受这个格式——必须先用 cartconv 转换成 .pxt64
          （cartconv 便携包见 makecode-arcade-cartconv 仓库的 Releases）

功能边界：
  本核心不做即时存档 / 回退（Savestate / Rewind）——RetroArch 的
  「保存状态」「金手指→回退」对本核心不可用（v0.3.0 起明确移除，
  只做官方模拟器同等的基本功能）。「重启游戏」（SELECT 键或前端重启）
  可用来重新开始当前游戏。

排查：
  加载游戏后 RetroArch 日志里会打印
    [MCA] core version $VERSION, trace -> <路径>
  <路径> 就是 mca_trace.log（默认在 RetroArch 的 saves 目录），
  记录 VM 启动/重置/卸载的每一步，卡住时把最后几行发出来即可。
  日志开关：设置 → 日志 → 日志级别 = 信息，日志到文件 = 开。

目标平台：$t
core 版本：$VERSION
构建时间：$(date -u +"%Y-%m-%d %H:%M UTC")
EOF
      ;;
    *)
      cat > "$stag/README.txt" <<EOF
MakeCode Arcade — libretro core ($plat)

安装：
  先解压本 zip。「安装或恢复核心」的文件浏览器按平台过滤扩展名，只列当前
  平台的核心文件（Windows .dll / Linux .so / macOS .dylib），zip 不在列表里，
  所以解压这一步绕不开。

  方式 A：手动拷贝（推荐，最直接）
    1) 把 makecode_arcade_libretro.$ext 放进 RetroArch 的「核心」目录，
       把 makecode_arcade_libretro.info 放进「核心信息」目录
       （两者也可以是同一个目录，取决于 RetroArch 的配置）。
       目录的实际位置因平台/发行版而异，请打开
       RetroArch 设置 → 目录 → 核心 / 核心信息 查看你设备上的真实路径，
       不要照搬网上教程里写死的路径。
    2) 有的发行版「核心信息」目录默认与核心目录不同，且菜单里看不到
       该项——先开 设置 → 用户界面 → 显示高级设置，或用配置文件给
       libretro_info_path 赋值。.info 没放对目录时，「加载核心」列表里
       不会出现本核心。

  方式 B：用 RetroArch 安装
    1) 加载核心 → 「安装或恢复核心」→ 选中解压出来的
       makecode_arcade_libretro.$ext，它会被拷进已配置的核心目录
    2) Install 只拷二进制，.info 不会被带过去——装完确认「核心信息」目录里
       有 makecode_arcade_libretro.info，没有就按方式 A 的第 2 步补上

  3) 加载核心 → 选择 .pxt64 文件

内容格式：
  .pxt64  转换后的 VM 字节码镜像，核心只认这个格式（加载内容里也只列出它）
  .png    MakeCode Arcade 保存的卡带，里面只有源码、不是游戏本身，
          核心不接受这个格式——必须先用 cartconv 转换成 .pxt64
          （cartconv 便携包见 makecode-arcade-cartconv 仓库的 Releases）

功能边界：
  本核心不做即时存档 / 回退（Savestate / Rewind）——RetroArch 的
  「保存状态」「金手指→回退」对本核心不可用（v0.3.0 起明确移除，
  只做官方模拟器同等的基本功能）。「重启游戏」（SELECT 键或前端重启）
  可用来重新开始当前游戏。

排查：
  加载游戏后 RetroArch 日志里会打印
    [MCA] core version $VERSION, trace -> <路径>
  <路径> 就是 mca_trace.log，记录 VM 启动/重置/卸载的每一步。

  本包里的核心已 strip（去掉调试信息，体积约为原件的 1/5）。如果要把崩溃
  地址翻译回源码行，请下载同版本的 makecode_arcade_$plat-debuginfo.zip，
  按包内 BUILD.txt 用 build-id 对齐后再 addr2line。

目标平台：$t
core 版本：$VERSION
构建时间：$(date -u +"%Y-%m-%d %H:%M UTC")
EOF
      ;;
  esac
  zip="$OUT/makecode_arcade_$plat.zip"
  rm -f "$zip"
  "$PYTHON_BIN" - "$stag" "$zip" <<'PY'
import os, sys, zipfile
src, dst = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(dst, "w", zipfile.ZIP_DEFLATED) as z:
    for name in sorted(os.listdir(src)):
        z.write(os.path.join(src, name), name)
PY
  size=$(stat -c%s "$zip" 2>/dev/null || echo 0)
  # 硬断言：zip 里必须真的躺着核心二进制（.dll/.so），.info 版本还得对得上。
  # 只数包数量是不够的——v0.3.1 就是"5 个包都在、但 Windows 包里没有 DLL"。
  "$PYTHON_BIN" - "$zip" "makecode_arcade_libretro.$ext" "$VERSION" <<'PY'
import re, sys, zipfile
path, core, version = sys.argv[1], sys.argv[2], sys.argv[3]
names = zipfile.ZipFile(path).namelist()
if core not in names:
    sys.exit(f"打包失败：{path} 里没有 {core}，实际内容 {names}")
info = zipfile.ZipFile(path).read("makecode_arcade_libretro.info").decode()
got = re.search(r'display_version = "([^"]+)"', info)
if not got or got.group(1) != version:
    sys.exit(f"打包失败：{path} 的 .info 版本 "
             f"{got.group(1) if got else '缺失'} != {version}")
PY
  echo "  $(printf '%-32s' "$t") -> makecode_arcade_$plat.zip  ($((size / 1024)) KB, 含 $ext)"

  # 调试符号单独成包：玩家下载主包（strip，小 5 倍），
  # 真出崩溃时再取 debuginfo 包对地址。两个包必须来自同一份构建，
  # build-id 就是这条绑定关系（这也是 0.3.8 起加 -Wl,--build-id=sha1 的原因）。
  if [ -n "$dbg_dir" ]; then
    bid="$(build_id_of "$stag/makecode_arcade_libretro.$ext")"
    hash="$("$PYTHON_BIN" - "$stag/makecode_arcade_libretro.$ext" <<'PY'
import hashlib, sys
print(hashlib.md5(open(sys.argv[1], "rb").read()).hexdigest())
PY
)"
    cat > "$dbg_dir/BUILD.txt" <<EOF
MakeCode Arcade — 调试符号（$plat）
================================================
本包里的 makecode_arcade_libretro.$ext 是「未 strip」的原件，
只用于把崩溃地址翻译回源码行，不能当核心加载。

core 版本：$VERSION
目标三元组：$t
build-id：$( [ -n "$bid" ] && printf '%s' "$bid" || printf '(未生成)' )
strip 后主件 md5：$hash

用法：
  1) 从 makecode_arcade_$plat.zip 解出 strip 过的核心，确认两边同一份构建：
       readelf -n makecode_arcade_libretro.$ext | grep 'Build ID'
     应当等于上面的 build-id。
  2) 把崩溃日志里的地址翻译成源码位置：
       addr2line -e <本包里的 makecode_arcade_libretro.$ext> -f -C 0x<地址>
     （aarch64 用 aarch64-linux-gnu-addr2line，x86_64 用 addr2line）
  3) build-id 对不上说明主包与本包不是同一次构建，先换同一版本重试。

构建时间：$(date -u +"%Y-%m-%d %H:%M UTC")
EOF
    dbgzip="$OUT/makecode_arcade_$plat-debuginfo.zip"
    rm -f "$dbgzip"
    "$PYTHON_BIN" - "$dbg_dir" "$dbgzip" <<'PY'
import os, sys, zipfile
src, dst = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(dst, "w", zipfile.ZIP_DEFLATED) as z:
    for name in sorted(os.listdir(src)):
        z.write(os.path.join(src, name), name)
PY
    dsize=$(stat -c%s "$dbgzip" 2>/dev/null || echo 0)
    echo "  $(printf '%-32s' "$t") -> makecode_arcade_$plat-debuginfo.zip  ($((dsize / 1024)) KB, build-id ${bid:0:12}…)"
  fi
  ok=$((ok + 1))
done

rm -rf "$STAGE_ROOT" "$DBG_ROOT"
echo
dbgn=$(ls "$OUT"/*-debuginfo.zip 2>/dev/null | wc -l)
echo "打包完成：$ok 个发行包（另 $dbgn 个 debuginfo 包）-> $OUT"
[ "$ok" -gt 0 ]
