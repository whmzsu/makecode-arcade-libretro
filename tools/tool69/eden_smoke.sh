#!/bin/bash
# Switch (NRO) 前端整包的模拟器冒烟：Eden（Yuzu 系）加载 NRO → 断言 RA 初始化。
#
# 目的：回答「构建出来的 libnx NRO 能不能真的被 Switch 模拟器加载、并让内嵌的
#       RetroArch 前端完成初始化」——这是工具机上唯一能做到的 Switch 运行态
#       验证（真机不可用时的兜底）。注意边界：
#   - 只证明 NRO 可被识别加载、RA 写好 retroarch.cfg、持续运行无崩溃；
#   - **不加载卡带、不出 frame hash**，content 加载仍需真机人工操作；
#   - 模拟器通过 ≠ 真机通过（fw21/22 的 mesa nv50_ir 上游问题是真机专属，见
#     docs/SWITCH-BUILD.md 的 #18905 边界）。
#
# 用法: eden_smoke.sh [core-v0.3.11]
#   tag 省略时取 release/local 下最新的完整版本。
# 前置: /opt/mca/env.sh；toolchain/emulators/eden/eden.AppImage（v0.2.1）；
#       toolchain/emulators/keys/{prod,title}.keys（21.2.0，用户自有 dump）。
# 无头要点（实测踩过的坑）：
#   - Eden 是 Yuzu 系，必须有 prod.keys，否则弹 "Derivation Components Missing"
#     对话框卡死；把 key 放进 <userdir>/keys/ 即过。
#   - Xvfb 下 llvmpipe：Vulkan 无 present queue（VK_ERROR_FEATURE_NOT_PRESENT），
#     必须切 OpenGL backend=0；llvmpipe 只报 GL 4.5，需 MESA_GL_VERSION_OVERRIDE=4.6
#     + MESA_GLSL_VERSION_OVERRIDE=460 才能通过 Eden 的 4.6 初始化检查。
#   - backend 有两处：qt-config.ini 的 backend= 值和 backend\default= 标志，
#     两处都要改，否则默认值覆盖显式值。首次运行先让 Eden 生成完整 ini 再改。
set -euo pipefail
source /opt/mca/env.sh

EMUDIR="$MCA_TOOLCHAIN/emulators/eden"
APPIMAGE="$EMUDIR/eden.AppImage"
KEYDIR="$MCA_TOOLCHAIN/emulators/keys"
RUNTIME=75   # 秒；timeout 杀掉是预期，rc=124 合法
WARMUP=15    # 秒；仅用于让 Eden 生成完整 qt-config.ini

STAMP=$(date +%Y%m%d-%H%M%S)
LOGD="$MCA_REPORTS/eden-smoke-$STAMP"
mkdir -p "$LOGD"
LOG="$LOGD/smoke.log"

die() { echo "FAIL: $*" | tee -a "$LOG"; exit 1; }
say() { echo "$*" | tee -a "$LOG"; }

# --- 0. 前置 ---
[ -x "$APPIMAGE" ] || die "缺 Eden AppImage: $APPIMAGE"
[ -s "$KEYDIR/prod.keys" ] || die "缺 prod.keys: $KEYDIR/prod.keys"
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
unzip -qo "$LOCAL/makecode_arcade_switch.zip" -d "$T/unzip" \
  || die "解压失败: $LOCAL/makecode_arcade_switch.zip"
NRO=$(find "$T/unzip" -name '*.nro' -print -quit)
[ -s "$NRO" ] || die "zip 内缺 NRO"

say "tag=$TAG version=${TAG#core-v}"
say "nro=$(basename "$NRO") $(stat -c %s "$NRO") B  sha256=$(sha256sum "$NRO" | cut -c1-16)"
say "prod.keys sha256=$(sha256sum "$KEYDIR/prod.keys" | cut -c1-16) (21.2.0)"

# 隔离用户目录（XDG_DATA_HOME/eden），别踩工具机真实配置
export HOME="$T/home"
export XDG_DATA_HOME="$T/data"
export XDG_CONFIG_HOME="$T/config"
export XDG_CACHE_HOME="$T/cache"
export QT_QPA_PLATFORM=xcb
mkdir -p "$XDG_DATA_HOME/eden/keys"
cp "$KEYDIR/prod.keys" "$KEYDIR/title.keys" "$XDG_DATA_HOME/eden/keys/"

UDIR="$XDG_DATA_HOME/eden"
ELOG="$UDIR/log/eden_log.txt"
CFGFILE="$XDG_CONFIG_HOME/eden/qt-config.ini"

# --- 1. warmup：让 Eden 生成完整 qt-config.ini（Vulkan 失败也不影响生成）---
set +e
timeout "$WARMUP" xvfb-run -a "$APPIMAGE" "$NRO" > "$LOGD/warmup.log" 2>&1
set -e
[ -s "$CFGFILE" ] || die "warmup 未生成配置 $CFGFILE"

# --- 2. 切 OpenGL backend（两处都要改）---
python3 - "$CFGFILE" <<'PY'
import sys
p = sys.argv[1]
out = []
for line in open(p, encoding="utf-8").read().splitlines():
    if line == "backend\\default=true":
        line = "backend\\default=false"
    elif line == "backend=1":
        line = "backend=0"
    out.append(line)
open(p, "w", encoding="utf-8").write("\n".join(out) + "\n")
PY
grep -q '^backend=0$' "$CFGFILE" || die "backend 未切成 OpenGL"

# --- 3. 正式运行 ---
export MESA_GL_VERSION_OVERRIDE=4.6
export MESA_GLSL_VERSION_OVERRIDE=460
export EGL_PLATFORM=surfaceless
rm -f "$ELOG"
set +e
timeout "$RUNTIME" xvfb-run -a "$APPIMAGE" "$NRO" > "$LOGD/run-stdout.log" 2>&1
RC=$?
set -e
case "$RC" in
  0|124) ;;
  *) die "Eden 退出码 $RC（预期 0 或 124，见 $LOGD/run-stdout.log）" ;;
esac
[ -f "$ELOG" ] || die "缺模拟器日志 $ELOG"
cp "$ELOG" "$LOGD/run-emulator.log"

# --- 4. 断言 ---
grep -q "Loading RetroArch" "$LOGD/run-emulator.log" \
  || die "Eden 未识别/加载 NRO（无 Loading RetroArch）"
grep -m1 "Renderer.backend: OpenGL" "$LOGD/run-emulator.log" \
  || die "backend 未生效为 OpenGL"
grep -q "CreateGraphicsPipeline" "$LOGD/run-emulator.log" \
  || die "未见 shader 编译（NRO 未真正进入渲染）"
# RetroArch 真初始化会写 sdmc/retroarch/retroarch.cfg（不是停在 loader）
[ -s "$UDIR/sdmc/retroarch/retroarch.cfg" ] \
  || die "RA 未初始化（sdmc/retroarch/retroarch.cfg 缺失）"
if grep -qE "SIGSEGV|exception_handler.cpp:LogException" "$LOGD/run-emulator.log"; then
  die "运行期出现崩溃信号"
fi
# llvmpipe 的 shader 指令 / 未映射写告警是已知噪声，只记录不判失败
NINSN=$(grep -c "assert Invalid insn" "$LOGD/run-emulator.log" || true)
NWRITE=$(grep -c "Unmapped Write8" "$LOGD/run-emulator.log" || true)
say "NOTE: llvmpipe shader 指令告警 $NINSN 条、未映射写告警 $NWRITE 条（已知噪声）"

say ""
say "PASS: $TAG 的 NRO 在 Eden 中加载并启动 ${RUNTIME}s，RA 已初始化且无崩溃"
say "日志: $LOGD/run-emulator.log"
say "边界: 未加载卡带、未出 frame hash；真机行为仍以设备验收为准"
