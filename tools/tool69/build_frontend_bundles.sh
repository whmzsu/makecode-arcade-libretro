#!/usr/bin/env bash
# 本地构建三套前端整包（3DS CIA / Vita SELF / Switch NRO），与 package_release.sh 的
# 12+5 标准 core 包一起构成完整 release/local/<tag>/。
#
# 用法: build_frontend_bundles.sh [core-v0.3.11]
# 前置: tool69 的 relay 工具链与 RetroArch 树已就绪；仅在本机运行。
# 并行度: 取 env.sh 的 MCA_BUILD_JOBS（默认 6，留 2 核给系统）；可临时覆盖：
#   MCA_BUILD_JOBS=4 "$MCA_BIN/build_frontend_bundles.sh" core-v0.3.11
set -euo pipefail
source /opt/mca/env.sh
JOBS="${MCA_BUILD_JOBS:-6}"

VERSION="$(sed -n 's/.*kCoreVersion = "\([^"]*\)".*/\1/p' \
  "$MCA_SRC/core/src/libretro_core.cpp" | head -1)"
[ -n "$VERSION" ] || { echo "读不到 kCoreVersion" >&2; exit 1; }
TAG="${1:-core-v$VERSION}"
D="$MCA_RELEASE_LOCAL/$TAG"
W="$MCA_BUILD_WORKSPACE"
RA="$MCA_TOOLCHAIN/relay/retroarch"
RA_NX="$MCA_TOOLCHAIN/relay/retroarch-nx-v1.22.2"
LOGD="$MCA_REPORTS/frontend-build-$(date +%Y%m%d-%H%M%S)"

mkdir -p "$D" "$LOGD"
exec > >(tee "$LOGD/build.log") 2>&1
echo "version=$VERSION tag=$TAG log=$LOGD/build.log jobs=$JOBS"
T0=$(date +%s)
stage() { printf '\n>>> %s  (+%ss)\n' "$1" "$(( $(date +%s) - T0 ))"; }

sync_src() { # $1=workspace dir
  mkdir -p "$1"
  rsync -a --delete --exclude='frontends/' --exclude='*.tgz' "$MCA_SRC/" "$1/"
}

info_file() { # $1=output path
  sed "s|@VERSION@|$VERSION|" "$MCA_SRC/tools/core.info.in" > "$1"
  grep -E 'display_(name|version)' "$1"
}

# --- 3DS: core 静态库 -> RetroArch 3DS CIA ---
stage "CTR start"
sync_src "$W/ctr"
export DEVKITPRO="$MCA_TOOLCHAIN/relay/extract/ctr/devkitpro"
export DEVKITARM="$DEVKITPRO/devkitARM"
export PATH="$DEVKITARM/bin:$DEVKITPRO/tools/bin:$PATH"
nice -n 5 make -C "$W/ctr/core" platform=ctr -j"$JOBS"
cp "$W/ctr/core/makecode_arcade_libretro_ctr.a" "$RA/libretro_ctr.a"
cd "$RA"
nice -n 5 make -f Makefile.ctr clean >/dev/null 2>&1 || true
nice -n 5 make -f Makefile.ctr -j"$JOBS" HAVE_RETRONFS=0 HAVE_CHEEVOS_RVZ=0
rm -f retroarch_3ds.bnr retroarch_3ds.icn retroarch_3ds.smdh retroarch_3ds.cia
nice -n 5 make -f Makefile.ctr retroarch_3ds.cia \
  APP_TITLE="MakeCode Arcade" \
  APP_DESCRIPTION="MakeCode Arcade (RetroArch core)" \
  APP_AUTHOR="MCA (unofficial)" \
  APP_PRODUCT_CODE="CTR-P-MCA2" \
  APP_UNIQUE_ID=0xF1CA1 \
  APP_ICON="$MCA_SRC/core/assets/ctr/icon48.png" \
  APP_BANNER="$MCA_SRC/core/assets/ctr/banner256.png" \
  APP_AUDIO="$MCA_SRC/core/assets/ctr/banner.wav" \
  HAVE_RETRONFS=0 HAVE_CHEEVOS_RVZ=0
mkdir -p "$LOGD/stage/ctr"
cp retroarch_3ds.cia "$LOGD/stage/ctr/makecode_arcade_libretro.cia"
info_file "$LOGD/stage/ctr/makecode_arcade_libretro.info"
( cd "$LOGD/stage/ctr" && zip -q -r "$D/makecode_arcade_ctr.zip" . )
echo "ctr: $(stat -c %s "$D/makecode_arcade_ctr.zip") B"

# --- Vita: core 静态库 -> RetroArch SELF ---
stage "Vita start"
sync_src "$W/vita"
export VITASDK="$MCA_TOOLCHAIN/relay/extract/vita/vitasdk"
export PATH="$VITASDK/bin:$PATH"
unset DEVKITPRO DEVKITARM || true
nice -n 5 make -C "$W/vita/core" platform=vita -j"$JOBS"
cp "$W/vita/core/makecode_arcade_libretro_vita.a" "$RA/libretro_vita.a"
cd "$RA"
nice -n 5 make -f Makefile.vita clean >/dev/null 2>&1 || true
nice -n 5 make -f Makefile.vita retroarch_vita.self -j"$JOBS"
head -c 4 retroarch_vita.self | cmp -s - <(printf 'SCE\000') \
  || { echo "Vita SELF 头错误" >&2; exit 1; }
mkdir -p "$LOGD/stage/vita"
cp retroarch_vita.self "$LOGD/stage/vita/makecode_arcade_libretro.self"
info_file "$LOGD/stage/vita/makecode_arcade_libretro.info"
( cd "$LOGD/stage/vita" && zip -q -r "$D/makecode_arcade_vita.zip" . )
echo "vita: $(stat -c %s "$D/makecode_arcade_vita.zip") B"

# --- Switch: core 静态库 -> RetroArch NRO ---
stage "Switch start"
sync_src "$W/nx"
export DEVKITPRO="$MCA_TOOLCHAIN/relay/extract/nx/devkitpro"
export PATH="$DEVKITPRO/devkitA64/bin:$DEVKITPRO/tools/bin:$PATH"
unset VITASDK || true
nice -n 5 make -C "$W/nx/core" platform=libnx -j"$JOBS"
cp "$W/nx/core/makecode_arcade_libretro_libnx.a" "$RA_NX/libretro_libnx.a"
cd "$RA_NX"
nice -n 5 make -f Makefile.libnx clean >/dev/null 2>&1 || true
nice -n 5 make -f Makefile.libnx -j"$JOBS"
if [ -f retroarch_switch.elf ]; then
  n=$(aarch64-none-elf-nm retroarch_switch.elf \
      | grep -cE ' T (retro_load_game|pxt_vm_start_buffer|mca_tick_init)$' || true)
  [ "$n" -eq 3 ] || { echo "核心符号未全进 NX ELF ($n/3)" >&2; exit 1; }
fi
[ "$(stat -c %s retroarch_switch.nro)" -gt 1000000 ] || { echo "NRO 太小" >&2; exit 1; }
mkdir -p "$LOGD/stage/switch/core"
cp retroarch_switch.nro "$LOGD/stage/switch/core/makecode_arcade_libretro_libnx.nro"
info_file "$LOGD/stage/switch/makecode_arcade_libretro.info"
cp "$MCA_SRC/tools/nx-install-notes.txt" "$LOGD/stage/switch/安装说明.txt"
( cd "$LOGD/stage/switch" && zip -q -r "$D/makecode_arcade_switch.zip" core makecode_arcade_libretro.info 安装说明.txt )
echo "switch: $(stat -c %s "$D/makecode_arcade_switch.zip") B"

printf 'version=%s\nsource=%s\nfrontend_bundles=ctr/vita/switch\nlog=%s\n' \
  "$VERSION" "$MCA_SRC" "$LOGD/build.log" > "$LOGD/FRONTEND-INFO.txt"
stage "DONE"
echo "DONE: $D now has $(find "$D" -maxdepth 1 -name '*.zip' | wc -l) release zips (elapsed $(( $(date +%s) - T0 ))s, jobs=$JOBS)"
