#!/bin/bash
# 3DS 前端整包的模拟器冒烟：Azahar（Citra 后继）安装 CIA → 启动 → 断言 RA 初始化。
#
# 目的：回答「构建出来的 CIA 能不能真的被 3DS 模拟器安装、以正确 title 启动、
#       并让内嵌的 RetroArch 前端完成初始化」——这是工具机上唯一能做到的
#       3DS 运行态验证（真机不可用时的兜底）。注意边界：
#   - 只证明 CIA 可安装可启动、RA 写好 retroarch.cfg、持续运行无崩溃；
#   - **不加载卡带、不出 frame hash**，content 加载仍需真机人工操作；
#   - 模拟器通过 ≠ 真机通过（old3ds 算力、4:3 比例都是真机专属）。
#
# 用法: azahar_smoke.sh [core-v0.3.11]
#   tag 省略时取 release/local 下最新的完整版本。
# 前置: /opt/mca/env.sh；toolchain/emulators/azahar/azahar.AppImage（2126.2）。
# 注意: AppImage 只带 xcb/wayland Qt 插件（无 offscreen），必须走 xvfb-run；
#       软件渲染用 llvmpipe，避免依赖工具机没有显示的 GPU。
set -euo pipefail
source /opt/mca/env.sh

EMUDIR="$MCA_TOOLCHAIN/emulators/azahar"
APPIMAGE="$EMUDIR/azahar.AppImage"
# 我们 CIA 的 title id（core/assets/ctr 的 UniqueId 0xF1CA1）
TID=000400000F1CA100
RUNTIME=75   # 秒；timeout 杀掉是预期，rc=124 合法

STAMP=$(date +%Y%m%d-%H%M%S)
LOGD="$MCA_REPORTS/azahar-smoke-$STAMP"
mkdir -p "$LOGD"
LOG="$LOGD/smoke.log"

die() { echo "FAIL: $*" | tee -a "$LOG"; exit 1; }
say() { echo "$*" | tee -a "$LOG"; }

# --- 0. 前置 ---
[ -x "$APPIMAGE" ] || die "缺 Azahar AppImage: $APPIMAGE"
command -v xvfb-run >/dev/null 2>&1 || die "缺 xvfb-run"

if [ $# -ge 1 ]; then
  TAG="$1"
else
  TAG=$(find "$MCA_RELEASE_LOCAL" -mindepth 1 -maxdepth 1 -type d \
        -name 'core-v*' -printf '%f\n' 2>/dev/null | sort -V | tail -n 1)
fi
[ -n "$TAG" ] || die "找不到 release/local 版本"
LOCAL="$MCA_RELEASE_LOCAL/$TAG"
T="$LOGD/work"
mkdir -p "$T"/{unzip,home,data,config,cache}
unzip -qo "$LOCAL/makecode_arcade_ctr.zip" -d "$T/unzip" \
  || die "解压失败: $LOCAL/makecode_arcade_ctr.zip"
CIA="$T/unzip/makecode_arcade_libretro.cia"
[ -s "$CIA" ] || die "zip 内缺 CIA"

say "tag=$TAG version=${TAG#core-v}"
say "cia=$(stat -c %s "$CIA") B  sha256=$(sha256sum "$CIA" | cut -c1-16)"

# 隔离用户目录（XDG_DATA_HOME/azahar-emu），别踩工具机真实配置
export HOME="$T/home"
export XDG_DATA_HOME="$T/data"
export XDG_CONFIG_HOME="$T/config"
export XDG_CACHE_HOME="$T/cache"
export LIBGL_ALWAYS_SOFTWARE=1
export MESA_GL_VERSION_OVERRIDE=4.5

UDIR="$XDG_DATA_HOME/azahar-emu"
ELOG="$UDIR/log/azahar_log.txt"

# --- 1. 安装 CIA（Qt 前端的 -i/--install，装完 exit 0）---
set +e
timeout 180 xvfb-run -a "$APPIMAGE" -i "$CIA" > "$LOGD/install.log" 2>&1
RC=$?
set -e
[ "$RC" = 0 ] || die "CIA 安装退出码 $RC（见 $LOGD/install.log）"
grep -q "Installed CIA successfully." "$LOGD/install.log" \
  || die "未见 Installed CIA successfully.（见 $LOGD/install.log）"
if [ -f "$ELOG" ]; then
  cp "$ELOG" "$LOGD/install-emulator.log"
fi
say "CIA 安装完成"

# --- 2. 定位已安装的 app ---
APP=$(find "$UDIR/sdmc" -type f -path "*title/00040000/0f1ca100/content/*.app" \
      | head -n 1)
[ -n "$APP" ] && [ -s "$APP" ] || die "安装后找不到 title $TID 的 .app"
say "title=$TID app=$(basename "$APP") ($(stat -c %s "$APP") B)"

# --- 3. 启动已安装 app ---
set +e
timeout "$RUNTIME" xvfb-run -a "$APPIMAGE" "$APP" > "$LOGD/run-stdout.log" 2>&1
RC=$?
set -e
case "$RC" in
  0|124) ;;
  *) die "Azahar 退出码 $RC（预期 0 或 124，见 $LOGD/run-stdout.log）" ;;
esac
[ -f "$ELOG" ] || die "缺模拟器日志 $ELOG"
cp "$ELOG" "$LOGD/run-emulator.log"

# --- 4. 断言 ---
grep -q "Azahar starting..." "$LOGD/run-emulator.log" \
  || die "app 未进入启动流程"
grep -q "Program ID: $TID" "$LOGD/run-emulator.log" \
  || die "模拟器未按 title $TID 加载"
# RetroArch 真初始化会写 sdmc/retroarch/retroarch.cfg（不是停在 loader）
[ -s "$UDIR/sdmc/retroarch/retroarch.cfg" ] \
  || die "RA 未初始化（sdmc/retroarch/retroarch.cfg 缺失）"
if grep -qE "Undefined Instruction|SIGSEGV|exception_handler.cpp:LogException" \
      "$LOGD/run-emulator.log"; then
  die "运行期出现崩溃信号"
fi
# 模拟器自身的 stub / llvmpipe 告警只记录，不判失败
NSTUB=$(grep -c "ReportUnimplementedFunction" "$LOGD/run-emulator.log" || true)
NGPU=$(grep -c "Texture size (1x1) is not multiple of 4" "$LOGD/run-emulator.log" || true)
say "NOTE: 模拟器 stub 告警 $NSTUB 条、llvmpipe 纹理告警 $NGPU 条（已知噪声）"

say ""
say "PASS: $TAG 的 CIA 在 Azahar 中安装并以 $TID 启动 ${RUNTIME}s，RA 已初始化且无崩溃"
say "日志: $LOGD/run-emulator.log"
say "边界: 未加载卡带、未出 frame hash；真机行为仍以设备验收为准"
