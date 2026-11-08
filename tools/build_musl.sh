#!/usr/bin/env bash
# build_musl.sh — 交叉编译 Lakka（musl）全静态 libretro core
#
# 背景：
#   * Lakka / LibreELEC 系是 musl libc，glibc 动态 .so 加载不了；
#   * zig 0.17 的 musl 目标给不出可用的静态产物（动态 libc.so 进不了
#     Lakka rootfs；静态 libc.a 是 exe 专用非 PIC 版本，数据符号 ABS64
#     重定位进 DSO 必报 R_AARCH64_ABS64 错，见 core/src/musl_missing.c 头注释）；
#   * libretro 官方 buildbot 的 Lakka core 用的就是 musl.cc 的
#     aarch64-linux-musl-cross 交叉工具链 + GNU ld `-shared -static`
#     （GNU ld 对该组合产出零 NEEDED 的自包含 DSO，与 zig 不同）。
#
# 用法（工具链解压后）：
#   PATH=$TC/bin:$PATH tools/build_musl.sh
# 产物：core/dist/aarch64-linux-musl/makecode_arcade_libretro.so
#
# 本脚本不依赖 musl_missing.c：musl.cc 是完整官方 musl，无 zig 裁剪问题。

set -euo pipefail

CC="${CC:-}"
CXX="${CXX:-}"
if [ -z "$CC" ] || [ -z "$CXX" ]; then
  # 自动探测两种常见 triplet：musl.cc（aarch64-linux-musl-*）与
  # dockcross（aarch64-unknown-linux-musl-*）
  for p in aarch64-linux-musl aarch64-unknown-linux-musl; do
    if [ -z "$CC" ] && command -v "$p-gcc" >/dev/null 2>&1; then CC="$p-gcc"; fi
    if [ -z "$CXX" ] && command -v "$p-g++" >/dev/null 2>&1; then CXX="$p-g++"; fi
  done
fi
CC="${CC:-aarch64-linux-musl-gcc}"
CXX="${CXX:-aarch64-linux-musl-g++}"

HERE="$(cd "$(dirname "$0")" && pwd)"
PROJ="$(cd "$HERE/.." && pwd)"
CORE="$PROJ/core"
PXTAPP="$PROJ/runtime/pxtapp"

# 源文件清单的唯一真源：core/sources.mk（与 Makefile / Android.mk / 另两个
# build_*.sh 共用）
MCA_SOURCES_MK="$CORE/sources.mk"
# shellcheck source=lib/sources.sh
source "$HERE/lib/sources.sh"
TAG="aarch64-linux-musl"
WORK="$CORE/build/$TAG"
OBJV="$WORK/vm_objs"
OBJC="$WORK/core_objs"
DIST="$CORE/dist/$TAG"
mkdir -p "$OBJV" "$OBJC" "$DIST"

for tool in "$CC" "$CXX"; do
  command -v "$tool" >/dev/null 2>&1 || {
    echo "找不到 $tool：需要 musl.cc aarch64-linux-musl-cross 工具链在 PATH" >&2
    exit 2
  }
done

echo "[$TAG] 编译器: $($CXX --version | head -1)"

# flags 与 build_target.sh 的 zig 分支一致（musl-gcc/g++ 无需 WIN_BASELINE）
COMMON=(-O2 -fPIC -fwrapv -fno-strict-aliasing -ffunction-sections
        -fno-rtti -fno-exceptions -fno-unwind-tables -fno-threadsafe-statics
        -Wno-unused-parameter -Wno-unused-private-field -Wno-vla-cxx-extension)
CXXFLAGS=("${COMMON[@]}" -std=c++11)
CORECXXFLAGS=("${COMMON[@]}" -std=c++14)
CFLAGS=("${COMMON[@]}" -std=c99)
INC=(-I"$CORE/src" -I"$PXTAPP")

needs_build() { [ ! -f "$2" ] || [ "$1" -nt "$2" ]; }

echo "[$TAG] VM 内核 + 引擎"
n=0; skipped=0
VM_OBJS=()
while IFS= read -r f; do
  rel="${f#$PXTAPP/}"
  out="$OBJV/${rel//\//_}.o"
  VM_OBJS+=("$out")
  if needs_build "$f" "$out"; then
    "$CXX" "${CXXFLAGS[@]}" "${INC[@]}" -c "$f" -o "$out"
    n=$((n + 1))
  else
    skipped=$((skipped + 1))
  fi
done < <(mca_vm_sources)
echo "  编译 $n 个 / 跳过 $skipped 个"

echo "[$TAG] core 源码"
# musl.cc 是完整官方 musl，不需要 musl_missing.c（zig musl 裁剪补丁）
CORE_OBJS=()
while IFS= read -r f; do
  out="$OBJC/$(basename "$f" .cpp).o"
  CORE_OBJS+=("$out")
  needs_build "$f" "$out" && "$CXX" "${CORECXXFLAGS[@]}" "${INC[@]}" -c "$f" -o "$out"
done < <(mca_core_sources)

echo "[$TAG] 链接（全静态 DSO）"
# GNU ld 的 -shared -static：musl libc.a/libm 全部静态链入，产物零 NEEDED。
# C++ 运行库也要静态（-static 已覆盖 libc/libstdc++/libgcc，musl gcc 的
# libstdc++.a 本身就是 musl 版）。
# 对象列表由清单推出，不用 "$OBJV"/*.o：老构建目录里可能还有已删源码的 .o
# （cart/png_loader/LzmaDec），glob 会把它们静默链进产物。
"$CXX" -shared -static -Wl,--build-id=none \
  -o "$DIST/makecode_arcade_libretro.so" \
  "${CORE_OBJS[@]}" "${VM_OBJS[@]}"
ls -l "$DIST/makecode_arcade_libretro.so" | awk '{print "  " $5 " bytes"}'

# 自检：必须是零 NEEDED 的 AArch64 ELF DSO
if command -v readelf >/dev/null 2>&1; then
  so="$DIST/makecode_arcade_libretro.so"
  needed=$(readelf -d "$so" 2>/dev/null | grep -c NEEDED || true)
  [ "$needed" = "0" ] || { echo "错误：产物仍有 $needed 个 NEEDED 依赖" >&2; exit 1; }
  n_retro=$(readelf --dyn-syms -W "$so" | grep -c " retro_" || true)
  echo "  自检通过：NEEDED=0，导出 retro_* $n_retro 个"
  [ "$n_retro" -ge 20 ] || { echo "错误：retro_* 导出不足" >&2; exit 1; }
fi
