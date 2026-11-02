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
mkdir -p "$OUT"

# zip 用 python 的 zipfile 生成（保证跨平台确定性、无额外依赖）。
# Ubuntu / macOS 只有 python3，Windows 的 Git Bash 里 python 也能用。
PYTHON_BIN="$(command -v python3 || command -v python || true)"
if [ -z "$PYTHON_BIN" ]; then
  echo "需要 python 3（用来打 zip），但没找到 python3/python" >&2
  exit 2
fi

# 发版目标 = build_matrix.sh 的 MAIN_TARGETS
MAIN_TARGETS=(
  x86_64-windows-gnu
  aarch64-windows-gnu
  x86_64-linux-gnu.2.17
  aarch64-linux-gnu.2.17
  arm-linux-gnueabihf.2.17
  aarch64-linux-android21
  armv7a-linux-androideabi21
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
    arm-linux-gnueabihf.2.17)  echo "linux-armhf" ;;
    x86_64-linux-gnu)          echo "linux-x64" ;;
    aarch64-linux-gnu)         echo "linux-arm64" ;;
    arm-linux-gnueabihf)       echo "linux-armhf" ;;
    aarch64-linux-android21)   echo "android-arm64" ;;
    armv7a-linux-androideabi21) echo "android-armv7" ;;
    *)                         echo "${1//\//-}" ;;
  esac
}

# 版本号从源码单点读取，避免 .info 与二进制脱节（曾经硬编码 0.2.0，
# 重编到 0.2.1 后发行包仍显示 0.2.0，无法在 RetroArch 里区分新旧 core）
# 版本号统一来自 kCoreVersion 常量（retro_get_system_info 与运行时横幅都用它）
VERSION="$(sed -n 's/.*kCoreVersion = "\([^"]*\)".*/\1/p' \
           "$PROJ/core/src/libretro_core.cpp" | head -1)"
[ -z "$VERSION" ] && VERSION="0.0.0"

INFO_META="display_name = \"MakeCode Arcade (unofficial)\"
authors = \"whmzsu and contributors\"
supported_extensions = \"pxt64\"
corename = \"MakeCode Arcade\"
manufacturer = \"Microsoft\"
categories = \"Game engine\"
systemname = \"MakeCode Arcade\"
systemid = \"makecode_arcade\"
license = \"MIT\"
permissions = \"\"
display_version = \"$VERSION\"
supports_no_game = \"false\"
notes = \"Unofficial core: not affiliated with or endorsed by Microsoft. Plays ONLY .pxt64 bytecode images - the file browser lists .pxt64 files only. MakeCode Arcade .png cartridges contain source code only, NOT a playable game: convert them FIRST with cartconv (see Releases) or tools/png2pxt64.py. Buttons are remappable under Quick Menu > Controls (Start = in-game menu, Select = reset); the core exposes no options.\""

ok=0; skipped=0
for t in "${TARGETS[@]}"; do
  dir="$DIST/$t"
  core="$(compgen -G "$dir/makecode_arcade_libretro.*" | head -1)"
  if [ -z "$core" ]; then
    echo "跳过 $t（无产物）"
    skipped=$((skipped + 1))
    continue
  fi
  plat="$(platform_of "$t")"
  ext="${core##*.}"
  stag="$STAGE_ROOT/$plat"
  rm -rf "$stag"
  mkdir -p "$stag"
  cp "$core" "$stag/"
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

  选包：现代设备（arm64-v8a）用 android-arm64；
        老旧 32 位设备用 android-armv7。装错会表现为"核心加载失败"。

内容格式：
  .pxt64  转换后的 VM 字节码镜像，核心只认这个格式（加载内容里也只列出它）
  .png    MakeCode Arcade 保存的卡带，里面只有源码、不是游戏本身，
          核心不接受这个格式——必须先用 cartconv 转换成 .pxt64
          （cartconv 便携包见 Releases；或 tools/png2pxt64.py）

即时存档：
  RetroArch 的「保存状态」/「金手指→回退」都能用。v0.2.11 起存档可以跨会话：
  存一档 → 退出 RetroArch（甚至崩溃）→ 重开同一个游戏 → 加载 → 从存档那一刻
  接着玩，画面与原时间线逐帧一致。换游戏后加载旧档会被拒绝（日志 -10），
  游戏照常继续，不会崩前端。核心版本变了的旧档同样拒绝——升级核心后请重存。

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
          （cartconv 便携包见 Releases；或 tools/png2pxt64.py）

即时存档：
  RetroArch 的「保存状态」/「回退」都能用。v0.2.11 起存档可以跨会话：
  存一档 → 退出 RetroArch → 重开同一个游戏 → 加载 → 接着玩，画面与原时间线
  逐帧一致。换游戏后加载旧档会被拒绝（日志 -10），游戏照常继续。

排查：
  加载游戏后 RetroArch 日志里会打印
    [MCA] core version $VERSION, trace -> <路径>
  <路径> 就是 mca_trace.log，记录 VM 启动/重置/卸载的每一步。

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
  echo "  $(printf '%-32s' "$t") -> makecode_arcade_$plat.zip  ($((size / 1024)) KB)"
  ok=$((ok + 1))
done

rm -rf "$STAGE_ROOT"
echo
echo "打包完成：$ok 个发行包 -> $OUT"
[ "$ok" -gt 0 ]
