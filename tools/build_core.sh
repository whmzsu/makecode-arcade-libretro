#!/usr/bin/env bash
# build_core.sh — 一键构建 MakeCode Arcade libretro core（路线 B 全量）
#
# 组件：
#   runtime/pxtapp/            pxt 官方 VM 内核 + 引擎原语（vendored，已打 3 个补丁）
#   core/src/vm_host.cpp       VM 宿主封装（镜像装载/按键沿/拉帧拉音频）
#   core/src/libretro_core.cpp libretro 接口
#
# 产物（core/ 下）：
#   makecode_arcade_libretro.dll   libretro core
#   core_test.exe                  假前端（跑任意 .pxt64 出 BMP）
#   contract_test.exe              假前端契约自检（不需要卡带）
#   build/vm_smoke.exe             独立 VM 宿主（官方 ABI 直调）
#
# 环境变量：ZIG（默认 <项目父目录>/common tools/zig/zig.exe）

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
PROJ="$(cd "$HERE/.." && pwd)"
CORE="$PROJ/core"
ZIG="${ZIG:-$PROJ/../common tools/zig/zig.exe}"
PXTAPP="$PROJ/runtime/pxtapp"
BUILD="$CORE/build"
OBJV="$BUILD/vm_objs"

CXXFLAGS=(
  -std=c++11
  -fno-rtti -fno-exceptions -fno-unwind-tables
  -fwrapv -fno-threadsafe-statics -fno-strict-aliasing
  -ffunction-sections -O2 -g
  -I"$PXTAPP" -I"$CORE/src"
  -Wno-unused-parameter -Wno-unused-private-field -Wno-vla-cxx-extension
)

mkdir -p "$OBJV" "$BUILD"

echo "== 1. pxtapp（VM 内核 + 引擎原语）=="
count=0
for f in "$PXTAPP"/base/*.cpp "$PXTAPP"/core---vm/*.cpp "$PXTAPP"/game/*.cpp \
         "$PXTAPP"/mixer---ext/*.cpp "$PXTAPP"/screen---ext/*.cpp \
         "$PXTAPP"/settings---files/*.cpp "$PXTAPP"/pointers.cpp; do
  rel="${f#$PXTAPP/}"
  out="$OBJV/${rel//\//_}.o"
  if [ ! -f "$out" ] || [ "$f" -nt "$out" ]; then
    "$ZIG" c++ "${CXXFLAGS[@]}" -c "$f" -o "$out"
  fi
  count=$((count + 1))
done
echo "  $count objects -> $OBJV"

echo "== 2. core 源码 =="
CINC=(-I"$CORE/src")
for f in "$CORE"/src/libretro_core.cpp "$CORE"/src/vm_host.cpp; do
  out="$BUILD/$(basename "$f" .cpp).o"
  if [ ! -f "$out" ] || [ "$f" -nt "$out" ]; then
    "$ZIG" c++ -std=c++14 -O2 "${CINC[@]}" -I"$PXTAPP" -c "$f" -o "$out"
  fi
done
echo "  core objects -> $BUILD"

echo "== 3. 链接 =="
VM_OBJS=$(ls "$OBJV"/*.o)
# 显式列表：老构建目录里可能还有已删源码的 .o，glob 会静默链进产物
CORE_OBJS="$BUILD/libretro_core.o $BUILD/vm_host.o"

"$ZIG" c++ -shared -o "$CORE/makecode_arcade_libretro.dll" $CORE_OBJS $VM_OBJS
echo "  -> $CORE/makecode_arcade_libretro.dll"

"$ZIG" c++ -std=c++14 -O2 -I"$CORE/src" -I"$CORE" "$CORE/tests/core_test.cpp" \
  $CORE_OBJS $VM_OBJS -o "$CORE/core_test.exe" -lpthread
echo "  -> $CORE/core_test.exe"

"$ZIG" c++ -std=c++14 -O2 -I"$CORE/src" -I"$PXTAPP" "$CORE/tests/contract_test.cpp" \
  $CORE_OBJS $VM_OBJS -o "$CORE/contract_test.exe" -lpthread
echo "  -> $CORE/contract_test.exe"

"$ZIG" c++ -std=c++11 -O2 -I"$PXTAPP" "$CORE/tests/vm_smoke.cpp" $VM_OBJS \
  -o "$BUILD/vm_smoke.exe" -lpthread
echo "  -> $BUILD/vm_smoke.exe"

echo "== 完成 =="
