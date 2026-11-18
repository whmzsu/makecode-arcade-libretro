#!/usr/bin/env bash
# 在 tool69 上复现「发版矩阵 + GitHub Actions 发行版工具链构型」
#
# A. zig 发版矩阵（5 目标，build_matrix.sh）
# B. 发行版工具链 CI 复现：linux-x64 / linux-arm64 gcc，windows-x64 MinGW，
#    android-arm64 ndk-build —— 与 .github/workflows/build.yml 同构型
# B2. Lakka musl 全静态（musl.cc aarch64-linux-musl-cross，Docker Hub 不通的替代）
# C. package_release.sh 打 zip + verify_release.py 逐包体检（6 主包 + 3 调试包）
#
# 低负载：全程 nice，编译 -j2，不在共用桌面机上占满 8 线程。
set -uo pipefail
REPO=/opt/mca-work/repo
NDK=/opt/mca-tools/ndk
OUT=/opt/mca-work/reports/matrix-$(date +%Y%m%d-%H%M%S)
mkdir -p "$OUT/artifacts"
export ZIG=zig ANDROID_NDK_HOME="$NDK"
cd "$REPO"
S="$OUT/summary.txt"
: > "$S"

log() { printf '%s\n' "$*" | tee -a "$S"; }

# ---------------------------------------------------------------- A. zig 矩阵
log "### A. zig 发版矩阵（5 目标）"
nice -n 5 ./tools/build_matrix.sh > "$OUT/matrix-zig.log" 2>&1
rc=$?
log "A_RC=$rc"
tail -14 "$OUT/matrix-zig.log" | tee -a "$S"
cp core/dist/MATRIX.md "$OUT/" 2>/dev/null || true

# ------------------------------------------------- B. 发行版工具链（CI 构型）
log ""
log "### B. 发行版工具链 CI 复现"
make_one() {
  local name="$1" plat="$2" cc="$3" cxx="$4"
  local d="$OUT/artifacts/$name"
  mkdir -p "$d"
  rm -f core/makecode_arcade_libretro.so core/makecode_arcade_libretro.dll
  ( cd "$REPO" && CC="$cc" CXX="$cxx" nice -n 5 make -f core/Makefile \
      platform="$plat" -j2 ) > "$OUT/build-$name.log" 2>&1
  local r=$?
  local bin=""
  [ -f core/makecode_arcade_libretro.so ]  && bin=core/makecode_arcade_libretro.so
  [ -f core/makecode_arcade_libretro.dll ] && bin=core/makecode_arcade_libretro.dll
  if [ "$r" = 0 ] && [ -n "$bin" ]; then
    cp "$bin" "$d/"
    log "$name  OK  $(stat -c%s "$bin") bytes  $(file -b "$bin" | cut -c1-70)"
    { readelf -d "$bin" 2>/dev/null | grep NEEDED
      readelf -V "$bin" 2>/dev/null | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1
      printf 'retro_exports=%s\n' "$(nm -D "$bin" 2>/dev/null | grep -c ' retro_')"
    } >> "$OUT/build-$name.log"
  else
    log "$name  FAIL rc=$r（见 build-$name.log）"
    tail -6 "$OUT/build-$name.log" >> "$S"
  fi
}
make_one linux-x64      unix           gcc                       g++
make_one linux-arm64    unix-aarch64  aarch64-linux-gnu-gcc     aarch64-linux-gnu-g++
make_one windows-x64    win64         x86_64-w64-mingw32-gcc    x86_64-w64-mingw32-g++

# Android：ndk-build 走 core/jni（与 CI 完全同参）
d="$OUT/artifacts/android-arm64"; mkdir -p "$d"
"$NDK/ndk-build" NDK_PROJECT_PATH=. APP_BUILD_SCRIPT=core/jni/Android.mk \
    NDK_APPLICATION_MK=core/jni/Application.mk APP_ABI=arm64-v8a \
    NDK_OUT=out/ndk/obj NDK_LIBS_OUT=out/ndk/libs -j2 \
    > "$OUT/build-android-arm64.log" 2>&1
rc=$?
if [ "$rc" = 0 ] && [ -f out/ndk/libs/arm64-v8a/libretro.so ]; then
  cp out/ndk/libs/arm64-v8a/libretro.so "$d/makecode_arcade_libretro.so"
  log "android-arm64  OK  $(stat -c%s out/ndk/libs/arm64-v8a/libretro.so) bytes  $(file -b out/ndk/libs/arm64-v8a/libretro.so | cut -c1-70)"
  readelf -lW "$d/makecode_arcade_libretro.so" | awk '$1=="LOAD"{print "LOAD_ALIGN=" $NF}' | sort -u >> "$OUT/build-android-arm64.log"
else
  log "android-arm64  FAIL rc=$rc（见 build-android-arm64.log）"
  tail -8 "$OUT/build-android-arm64.log" >> "$S"
fi

# ------------------------------------- B2. Lakka（musl 全静态，musl.cc 工具链）
# CI 用 dockcross 镜像（Docker Hub 在本机不通）；本机装的是 musl.cc 官方
# aarch64-linux-musl-cross（GCC 11.2.1），build_musl.sh 两条都支持。
log ""
log "### B2. Lakka musl 全静态（musl.cc 工具链）"
MUSL_TC=/opt/mca-data/tools/aarch64-linux-musl-cross/bin
if [ -x "$MUSL_TC/aarch64-linux-musl-g++" ]; then
  ( cd "$REPO" && PATH="$MUSL_TC:$PATH" nice -n 5 ./tools/build_musl.sh ) \
      > "$OUT/build-musl.log" 2>&1
  rc=$?
  so="$REPO/core/dist/aarch64-linux-musl/makecode_arcade_libretro.so"
  if [ "$rc" = 0 ] && grep -q "自检通过" "$OUT/build-musl.log"; then
    log "musl-arm64  OK  $(stat -c%s "$so") bytes  NEEDED=$(readelf -d "$so" | grep -c NEEDED)"
  else
    log "musl-arm64  FAIL rc=$rc（见 build-musl.log）"
    tail -8 "$OUT/build-musl.log" >> "$S"
  fi
else
  log "musl-arm64  SKIP（缺 $MUSL_TC 工具链）"
fi

# ------------------------------------------------------------- C. 打包 + 体检
log ""
log "### C. 发行包打包与体检"
MCA_REQUIRE_STRIP=1 nice -n 5 ./tools/package_release.sh > "$OUT/package.log" 2>&1
log "PACKAGE_RC=$?"
tail -12 "$OUT/package.log" | tee -a "$S"
python3 /opt/mca-work/bin/verify_release.py "$REPO/core/dist/release" \
        "$REPO/core/src/libretro_core.cpp" > "$OUT/release-verify.md" 2>&1
log "VERIFY_RC=$?"
cat "$OUT/release-verify.md" | tee -a "$S"

log ""
log "REPORT_DIR=$OUT"
