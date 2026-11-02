#!/usr/bin/env bash
# make_local.sh — 用 zig 驱动 core/Makefile（本地没有目标平台工具链时）
#
# 用法：
#   ./make_local.sh               # 本机平台（Makefile 自动探测）
#   ./make_local.sh win64         # Windows x64（MinGW ABI）
#   ./make_local.sh linux-x64     # Linux x86_64，glibc 2.17 基线
#   ./make_local.sh clean [目标]  # 清理该目标的产物与对象
#
# 为什么要这层封装：make 的 CC/CXX 变量是按空格切分后再拼进 shell 命令的，
# zig 若装在带空格的路径下（本机是 "common tools/zig"），直接
# `make CC="/path/to/zig cc"` 会在空格处断掉。这里生成两个两行的包装脚本，
# 把带空格的路径关在脚本里，再让 make 用它们。
#
# 产物位置与 CI 完全一致：core/makecode_arcade_libretro.{dll,so,dylib}

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
PROJ="$(cd "$HERE/.." && pwd)"
ZIG="${ZIG:-$PROJ/../common tools/zig/zig.exe}"
MAKE="${MAKE:-make}"

if [ ! -x "$ZIG" ]; then
  echo "找不到 zig：$ZIG（用 ZIG 环境变量指定 zig 可执行文件）" >&2
  exit 2
fi

MODE="${1:-auto}"
if [ "$MODE" = "clean" ]; then
  MODE="${2:-auto}"
  DO_CLEAN=1
else
  DO_CLEAN=0
fi

case "$MODE" in
  auto|native) PLATFORM=""; TARGET_FLAG="" ;;
  win64)       PLATFORM="win64"; TARGET_FLAG="-target x86_64-windows-gnu" ;;
  linux-x64)   PLATFORM="unix";  TARGET_FLAG="-target x86_64-linux-gnu.2.17" ;;
  *)
    echo "用法: $0 [auto|win64|linux-x64|clean]" >&2
    exit 2
    ;;
esac

WRAP="$PROJ/out/toolwrap"
mkdir -p "$WRAP"
for L in cc c++; do
  cat > "$WRAP/zig-$L.sh" <<EOF
#!/bin/sh
exec "$ZIG" $L $TARGET_FLAG "\$@"
EOF
  chmod +x "$WRAP/zig-$L.sh"
done

ARGS=(-f "$PROJ/core/Makefile")
[ -n "$PLATFORM" ] && ARGS+=(platform="$PLATFORM")
ARGS+=(CC="$WRAP/zig-cc.sh" CXX="$WRAP/zig-c++.sh")

if [ "$DO_CLEAN" = "1" ]; then
  exec "$MAKE" "${ARGS[@]}" clean
fi
exec "$MAKE" "${ARGS[@]}" -j"$(nproc 2>/dev/null || echo 4)"
