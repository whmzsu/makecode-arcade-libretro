#!/bin/bash
# Vita 前端整包的模拟器冒烟：官方 RetroArch VPK + 导入我们的 core self。
#
# 目的：回答「构建出来的 self 能不能真的被 RA 加载并进入主循环」——这是工具机上
# 唯一能做到的 Vita 运行态验证（真机不可用时的兜底）。注意边界：
#   - 只证明 self 能被 RA 识别、进入 Running 且不崩；**不跑卡带、不出 frame hash**。
#   - offscreen 模式无键盘输入通道，无法用 CLI 把 .pxt64 塞进 RA 的 argv[1]，
#     所以 content 加载仍需真机人工操作。
#   - 模拟器通过 ≠ 真机通过（4:3、C2-12828-1 都是真机专属）。
#
# 用法: vita3k_smoke.sh [core-v0.3.11]
#   tag 省略时取 release/local 下最新的完整版本。
# 前置: /opt/mca/env.sh；toolchain/emulators/vita3k 下有 AppImage 与 ra-vpk/。
set -euo pipefail
source /opt/mca/env.sh

EMUDIR="$MCA_TOOLCHAIN/emulators/vita3k"
APPIMAGE="$EMUDIR/Vita3K-x86_64.AppImage"
RA_VPK="$EMUDIR/ra-vpk/RetroArch.vpk"
CFG=/root/.config/Vita3K/config.yml
VITAFS=/root/.local/share/Vita3K/Vita3K
UX="$VITAFS/ux0"
APP="$UX/app/RETROVITA"

# 无头显示：offscreen + EGL surfaceless 走软件 Vulkan，dummy 音频避开 ALSA。
export SDL_VIDEODRIVER=offscreen
export SDL_AUDIODRIVER=dummy
export EGL_PLATFORM=surfaceless

STAMP=$(date +%Y%m%d-%H%M%S)
LOGD="$MCA_REPORTS/vita3k-smoke-$STAMP"
mkdir -p "$LOGD"
LOG="$LOGD/smoke.log"

die() { echo "FAIL: $*" | tee -a "$LOG"; exit 1; }
say() { echo "$*" | tee -a "$LOG"; }

# --- 0. 前置 ---
[ -x "$APPIMAGE" ] || die "缺 Vita3K AppImage: $APPIMAGE"
[ -f "$RA_VPK" ]    || die "缺官方 RA VPK: $RA_VPK"
[ -f "$CFG" ]       || die "缺 Vita3K 配置: $CFG（先手动跑一次生成）"

# tag
if [ $# -ge 1 ]; then
  TAG="$1"
else
  TAG=$(find "$MCA_RELEASE_LOCAL" -mindepth 1 -maxdepth 1 -type d \
        -name 'core-v*' -printf '%f\n' 2>/dev/null | sort -V | tail -n 1)
fi
[ -n "$TAG" ] || die "找不到 release/local 版本"
LOCAL="$MCA_RELEASE_LOCAL/$TAG"
T="$LOGD/unzip"
mkdir -p "$T"
unzip -qo "$LOCAL/makecode_arcade_vita.zip" -d "$T" \
  || die "解压失败: $LOCAL/makecode_arcade_vita.zip"
SELF_SRC="$T/makecode_arcade_libretro.self"
INFO_SRC="$T/makecode_arcade_libretro.info"
[ -s "$SELF_SRC" ] || die "zip 内缺 self"
[ -s "$INFO_SRC" ] || die "zip 内缺 info"
# fSELF 头校验（与构建期断言一致）
[ "$(head -c 4 "$SELF_SRC" | od -An -tx1 | tr -d ' \n')" = "53434500" ] \
  || die "self 头不是 fSELF (SCE\\0)"

say "tag=$TAG version=${TAG#core-v}"
say "appimage=$($APPIMAGE --version 2>/dev/null | tail -n 1)"
say "self=$(stat -c %s "$SELF_SRC") B  sha256=$(sha256sum "$SELF_SRC" | cut -c1-16)"

# --- 1. 安装官方 RA VPK（幂等）---
if [ -f "$APP/eboot.bin" ]; then
  say "RA VPK 已安装，跳过（$APP）"
else
  say "安装官方 RA VPK..."
  timeout 300 "$APPIMAGE" -c "$CFG" --frontend sdl -B Vulkan "$RA_VPK" \
    > "$LOGD/install.log" 2>&1 || die "RA VPK 安装失败（见 $LOGD/install.log）"
  grep -q "installed successfully" "$LOGD/install.log" \
    || die "未看到 installed successfully（见 $LOGD/install.log）"
  say "RA VPK 安装完成"
fi

# --- 2. 导入 self + info（真实 RA 目录结构）---
cp "$SELF_SRC" "$APP/makecode_arcade_libretro.self"
mkdir -p "$APP/info"
cp "$INFO_SRC" "$APP/info/makecode_arcade_libretro.info"
say "已导入 self + info 到 $APP"

# --- 3. 启动 self（-S 用 Vita 虚拟路径 app0:/，主机绝对路径会拼错）---
# 只跑 45s：足够 RA 扫描 core 并进主循环，超时是预期（timeout 杀掉）。
set +e
timeout 45 "$APPIMAGE" -c "$CFG" --frontend sdl -B Vulkan \
  -r RETROVITA -S "app0:/makecode_arcade_libretro.self" \
  > "$LOG" 2>&1
RC=$?
set -e
# 124=timeout(预期)，其它非 0 视为异常
case "$RC" in
  0|124) ;;
  *) die "Vita3K 退出码 $RC（预期 0 或 124）" ;;
esac

# --- 4. 断言 ---
grep -q "Main executable retroarch_vita.elf" "$LOG" \
  || die "self 未被识别为 RA 主程序"
grep -q "set_phase.*Launching -> Running" "$LOG" \
  || die "RA 未进入 Running 阶段"
# core_info.cache 生成后 RA 不再逐行扫 info，改用两个稳健证据之一：
#   首次运行会有 "Opening file: app0:/info/makecode_arcade_libretro.info"；
#   之后走缓存会出现 "remaps/MakeCode Arcade ..." 的 core 专属配置读取。
if ! grep -q "info/makecode_arcade_libretro.info" "$LOG" \
   && ! grep -q "remaps/MakeCode Arcade" "$LOG"; then
  die "RA 未识别到我们的 core（既无 info 扫描也无 MakeCode 配置读取）"
fi
if grep -qiE "SIGSEGV|FATAL|unhandled exception" "$LOG"; then
  die "运行期出现崩溃信号"
fi
# 真机历史坑（PATCH 16）在模拟器里未必复现，仅记录不判失败
if grep -q "C2-12828-1" "$LOG"; then
  say "NOTE: 日志出现 C2-12828-1（模拟器环境，仅供参考）"
fi

say ""
say "PASS: $TAG 的 Vita self 在 Vita3K 中被 RA 识别并运行 45s 无崩溃"
say "日志: $LOG"
say "边界: 未加载卡带、未出 frame hash；真机行为仍以设备验收为准"
