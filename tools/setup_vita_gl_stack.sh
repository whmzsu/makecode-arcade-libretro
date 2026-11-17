#!/bin/sh
# 安装 RetroArch Vita 完整前端（Makefile.vita 默认 vitaGL 路径）的 5 个
# 链接依赖，全部装进 $VITASDK/arm-vita-eabi/{include,lib}。
#
# 依赖顺序（每个是下一个的前置）：
#   taiHEN v0.11（头 + 预编 stub）
#     -> SceShaccCgExt（cmake 工程，真用 taiHEN API）
#       -> vitashark（shader 编译器 hook）
#         -> math-neon（NEON 数学库）
#           -> vitaGL
#
# 踩坑记录（2026-11-17 tool69 实测，照此配方在 CI 复现）：
#   * vitasdk snapshot 容器/包都不带这 5 个库，vitaGL 官方无 release 资产，
#     vitashark release 无资产，只能源码装；
#   * taiHEN 必须用 v0.11 release tarball（含预编 stub）——从源码编内核模块
#     会撞 vitasdk 头版本漂移，且 exports.yml 是老式 syscall stub 不能直接编；
#   * taiHEN v0.11 的 taihen.h 是旧式无原型函数指针，GCC 15 默认 C23 把 ()
#     当 (void)，SceShaccCgExt 编译会炸 —— 用 -std=gnu17 绕过，不改源码；
#   * SceShaccCgExt 的头和库要手动拷进 vitasdk，vitashark 才编得过。
#
# 环境要求：$VITASDK 已设、PATH 里有 arm-vita-eabi-*、git/cmake/curl。
set -e

: "${VITASDK:?need VITASDK env}"
PREFIX="$VITASDK/arm-vita-eabi"
WORK="$(mktemp -d)"
echo "工作目录: $WORK"
trap 'rm -rf "$WORK"' EXIT

command -v git   >/dev/null || { echo "缺 git";   exit 1; }
command -v cmake >/dev/null || { echo "缺 cmake"; exit 1; }
command -v curl  >/dev/null || { echo "缺 curl";  exit 1; }
NJ=$(nproc 2>/dev/null || echo 4)

cd "$WORK"

# ---- 1. taiHEN v0.11（官方 release tarball，sha256 见仓库 CI 首跑日志） ----
echo "== taiHEN v0.11"
curl -fsSL -o taihen.tar.gz \
  https://github.com/yifanlu/taiHEN/releases/download/v0.11/taihen.tar.gz
tar xzf taihen.tar.gz
cp -r include/* "$PREFIX/include/"
cp -r lib/*     "$PREFIX/lib/"
[ -f "$PREFIX/include/taihen.h" ] || { echo "taihen.h 没装上"; exit 1; }

# ---- 2. SceShaccCgExt ----
echo "== SceShaccCgExt"
git clone --depth 1 https://github.com/bythos14/SceShaccCgExt.git
cmake -B SceShaccCgExt/build -S SceShaccCgExt \
  -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_FLAGS="-std=gnu17"
cmake --build SceShaccCgExt/build -j "$NJ"
cp SceShaccCgExt/include/*.h "$PREFIX/include/"
cp SceShaccCgExt/build/libSceShaccCgExt.a "$PREFIX/lib/"

# ---- 3. vitashark ----
echo "== vitashark"
git clone --depth 1 https://github.com/Rinnegatamante/vitashark.git
make -C vitashark -j "$NJ"
make -C vitashark install

# ---- 4. math-neon ----
echo "== math-neon"
git clone --depth 1 https://github.com/Rinnegatamante/math-neon.git
make -C math-neon -j "$NJ"
make -C math-neon install

# ---- 5. vitaGL ----
echo "== vitaGL"
git clone --depth 1 https://github.com/Rinnegatamante/vitaGL.git
make -C vitaGL -j "$NJ"
make -C vitaGL install

# ---- 验收 ----
echo "== 验收"
for f in libvitaGL.a libvitashark.a libmath_neon.a libSceShaccCgExt.a libtaihen_stub.a; do
  ls -l "$PREFIX/lib/$f" || exit 1
done
echo "vitaGL 依赖链安装完成"
