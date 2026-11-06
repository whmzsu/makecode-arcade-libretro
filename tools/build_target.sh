#!/usr/bin/env bash
# build_target.sh — 按目标三元组交叉编译 libretro core
#
# 用法：
#   ./build_target.sh aarch64-linux-gnu.2.17     # Linux（zig 自带 sysroot）
#   ./build_target.sh x86_64-windows-gnu         # Windows（zig）
#   ./build_target.sh aarch64-linux-android21    # Android（NDK clang，见下）
#   ./build_target.sh native                     # 本机
#
# 编译器选择：
#   * 非 Android：zig cc / zig c++（自带 libc 头，无需交叉工具链）
#   * Android   ：NDK 自带 clang（官方路径；zig 自带 libc++ 与 bionic 头版本不匹配）
#                 NDK 位置：$ANDROID_NDK_HOME，或默认 ../common tools/android-ndk-r27c
#                 API 级别写在三元组末尾（21/24/...）
#
# 产物：core/dist/<triple>/makecode_arcade_libretro.{so|dll|dylib}
#
# 说明：
#   * 全部源码（VM 内核 + 引擎 + core）都是纯 C/C++，无汇编/无平台专有 API，
#     因此同一套 flag 可覆盖所有 zig 支持的 libc 目标。
#   * 每个 triple 独立对象目录，可并行；已编过的对象会跳过。

set -euo pipefail

TRIPLE="${1:?usage: build_target.sh <triple|native>}"

HERE="$(cd "$(dirname "$0")" && pwd)"
PROJ="$(cd "$HERE/.." && pwd)"
CORE="$PROJ/core"
ZIG="${ZIG:-$PROJ/../common tools/zig/zig.exe}"
PXTAPP="$PROJ/runtime/pxtapp"

# 编译产物按 32/64 位与目标切分；这里只关心目标平台
case "$TRIPLE" in
  native)
    TAG="native"
    TARGET_FLAGS=()
    CXX="$ZIG"; CXXMODE="c++"
    CCC="$ZIG"; CCMODE="cc"
    ;;
  *android*)
    TAG="$TRIPLE"
    TARGET_FLAGS=("--target=$TRIPLE")
    CXXMODE=""; CCMODE=""
    ;;
  *)
    TAG="$TRIPLE"
    TARGET_FLAGS=(-target "$TRIPLE")
    CXX="$ZIG"; CXXMODE="c++"
    CCC="$ZIG"; CCMODE="cc"
    ;;
esac

WORK="$CORE/build/$TAG"
OBJV="$WORK/vm_objs"
OBJC="$WORK/core_objs"
DIST="$CORE/dist/$TAG"
mkdir -p "$OBJV" "$OBJC" "$DIST"

# Android：定位 NDK clang
if [ -z "${CXX:-}" ]; then
  NDK="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-$PROJ/../common tools/android-ndk-r27c}}"
  if [ ! -d "$NDK" ]; then
    echo "找不到 Android NDK：$NDK" >&2
    echo "请设置 ANDROID_NDK_HOME 或把 NDK 放到 common tools/ 下" >&2
    exit 2
  fi
  # 取 prebuilt/<host> 目录（windows-x86_64 / linux-x86_64 / darwin-x86_64）
  PREBUILT="$(ls -d "$NDK"/toolchains/llvm/prebuilt/* 2>/dev/null | head -1)"
  CXX="$PREBUILT/bin/clang++.exe"
  [ -x "$CXX" ] || CXX="$PREBUILT/bin/clang++"
  CCC="$PREBUILT/bin/clang.exe"
  [ -x "$CCC" ] || CCC="$PREBUILT/bin/clang"
  if [ ! -x "$CXX" ]; then
    echo "NDK clang 不可用：$CXX" >&2
    exit 2
  fi
  NDK_VERSION="$(sed -n 's/^Pkg.Revision *= *//p' "$NDK/source.properties" 2>/dev/null | head -1)"
fi

# 目标形态（WIN_BASELINE 只对 Windows 目标非空）
WIN_BASELINE=()
case "$TAG" in
  *windows*) EXT="dll"; LINK_EXTRA=(-shared -Wl,--export-all-symbols)
             # mingw-gcc 默认按 XP 裁 windows.h，platform.cpp 用了
             # GetTickCount64（Vista+），不抬到 Win7 编不过；用 #ifndef 兜底，
             # zig 自己的 0x0a00 不会被覆盖（见 core/src/win32_baseline.h）
             WIN_BASELINE=(-include "$CORE/src/win32_baseline.h") ;;
  *macos*)   EXT="dylib"; LINK_EXTRA=(-dynamiclib) ;;
  *musl*)    # musl（Lakka）不在本地矩阵：zig 0.17 的 musl 只能产动态 libc.so
             # （Lakka rootfs 没有 libc.so，加载不了）或非 PIC 静态 libc.a
             # （数据符号 ABS64 重定位，进 DSO 必炸 R_AARCH64_ABS64 错）。
             # 走 tools/build_musl.sh（musl.cc 交叉工具链 + GNU ld -shared -static），
             # 见 .github/workflows/build.yml 的 lakka-musl job。
             echo "musl 目标不支持本地 zig 构建，请用 tools/build_musl.sh（CI 环境）" >&2
             exit 2 ;;
  *android*) # -static-libstdc++：把 libc++ 静态链进去，产物只依赖 libc/libm/libdl，
             # RetroArch 加载插件时无需 libc++_shared.so（不同 App 的 STL 版本差异会炸）
             # max-page-size=16384：Android 15 起有 16KB 页设备，4KB 对齐的 .so 无法 dlopen
             EXT="so"
             LINK_EXTRA=(-shared -static-libstdc++ -Wl,-z,max-page-size=16384
                         -Wl,-soname,libmakecode_arcade_libretro.so) ;;
  *)         # 不加 --no-undefined：libc++ 里未用到的代码可能引用目标 glibc 没有的
             # 符号（如 ppc64le 的 __vsnprintfieee128），动态链接下无害；
             # 缺对象的情况由 build_matrix.sh 的产物清理 + 自检兜底。
             EXT="so"; LINK_EXTRA=(-shared -Wl,-soname,makecode_arcade_libretro.so) ;;
esac

# -fPIC：非 Windows 的共享库必须；MSVC 风格 PE 不需要（也不接受）
PIC=(-fPIC)
case "$TAG" in *windows*) PIC=() ;; esac

# 栈展开信息（.pdata/.xdata）：
#   * mingw-w64 的 setjmp/longjmp 在 x86_64 上走 SEH（RtlUnwindEx），
#     逐帧回退必须有 .pdata，否则 longjmp 自己就崩（0xC0000005 / STATUS_BAD_STACK）。
#     VM 的 try/catch（FiberContext::loopjmp）和 panic 回弹点都依赖它，
#     所以 Windows 目标必须开 -funwind-tables。
#   * 其它平台用不到，保持 -fno-unwind-tables 省体积。
UNWIND=(-fno-unwind-tables)
case "$TAG" in *windows*) UNWIND=(-funwind-tables -fasynchronous-unwind-tables) ;; esac

CXXFLAGS=(
  "${TARGET_FLAGS[@]}" "${PIC[@]}" "${WIN_BASELINE[@]}"
  -std=c++11
  -fno-rtti -fno-exceptions "${UNWIND[@]}"
  -fwrapv -fno-threadsafe-statics -fno-strict-aliasing
  -ffunction-sections -O2
  -I"$PXTAPP"
  -Wno-unused-parameter -Wno-unused-private-field -Wno-vla-cxx-extension
)
CFLAGS=("${TARGET_FLAGS[@]}" "${PIC[@]}" "${WIN_BASELINE[@]}" -O2 "${UNWIND[@]}")
INC=(-I"$CORE/src" -I"$CORE/third_party/miniz" -I"$CORE/third_party/lzma")

needs_build() { [ ! -f "$2" ] || [ "$1" -nt "$2" ]; }

echo "[$TAG] 编译器: $( [ -n "$CXXMODE" ] && echo zig || echo "NDK clang ${NDK_VERSION:-}" )"
echo "[$TAG] VM 内核 + 引擎"
n=0; skipped=0
for f in "$PXTAPP"/base/*.cpp "$PXTAPP"/core---vm/*.cpp "$PXTAPP"/game/*.cpp \
         "$PXTAPP"/mixer---ext/*.cpp "$PXTAPP"/screen---ext/*.cpp \
         "$PXTAPP"/settings---files/*.cpp "$PXTAPP"/pointers.cpp; do
  rel="${f#$PXTAPP/}"
  out="$OBJV/${rel//\//_}.o"
  if needs_build "$f" "$out"; then
    "$CXX" $CXXMODE "${CXXFLAGS[@]}" -c "$f" -o "$out"
    n=$((n + 1))
  else
    skipped=$((skipped + 1))
  fi
done
echo "  编译 $n 个 / 跳过 $skipped 个"

echo "[$TAG] core 源码"
for f in "$CORE"/src/cart.c "$CORE"/src/decompress.c \
         "$CORE"/third_party/miniz/miniz_tinfl.c "$CORE"/third_party/lzma/LzmaDec.c; do
  out="$OBJC/$(basename "$f" .c).o"
  if needs_build "$f" "$out"; then
    "$CCC" $CCMODE "${CFLAGS[@]}" "${INC[@]}" -c "$f" -o "$out"
  fi
done
for f in "$CORE"/src/libretro_core.cpp "$CORE"/src/png_loader.cpp "$CORE"/src/vm_host.cpp; do
  out="$OBJC/$(basename "$f" .cpp).o"
  if needs_build "$f" "$out"; then
    "$CXX" $CXXMODE "${CXXFLAGS[@]}" -std=c++14 "${INC[@]}" -c "$f" -o "$out"
  fi
done

echo "[$TAG] 链接 -> $DIST/makecode_arcade_libretro.$EXT"
rm -f "$DIST/makecode_arcade_libretro.$EXT"   # 避免上次失败残留被误判为成功
"$CXX" $CXXMODE "${TARGET_FLAGS[@]}" "${PIC[@]}" "${LINK_EXTRA[@]}" \
  -o "$DIST/makecode_arcade_libretro.$EXT" "$OBJC"/*.o "$OBJV"/*.o
ls -l "$DIST/makecode_arcade_libretro.$EXT" | awk '{print "  " $5 " bytes"}'
