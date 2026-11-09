#!/usr/bin/env bash
# retroarch_smoke.sh — 真前端冒烟：用系统 RetroArch 无头加载核心 + 卡带
#
# 为什么需要：golden hash 用的是我们自己写的 ctypes 假前端（libretro_host.py），
# 它验证的是"我们理解的契约"。RetroArch 验证的是"真实前端怎么用它"：dlopen 路径、
# .info 解析、SET_PIXEL_FORMAT 协商、CONTROL 描述符、SAVE_DIRECTORY、截图、退出时
# 的 dlclose —— 这些假前端全测不到。
#
# 首次实跑（2026-11-09）就抓到"退出即 139"：pxt 运行时有 static 析构，RetroArch
# 先 dlclose 再走 atexit 链，跳进已解除映射的代码段。给核心加 DF_1_NODELETE 后
# 干净退出（EXIT=0），截图是真实 160x120 画面。
#
# 用法：
#   tools/retroarch_smoke.sh                       # 默认 x64 产物 + 3 张卡带
#   tools/retroarch_smoke.sh --core <so> --cart <pxt64> --cart <pxt64>
#   tools/retroarch_smoke.sh --frames 300 --keep   # 保留临时目录看日志
#
# 前提：Linux + retroarch（1.18+）+ 一个 X（无头用 xvfb-run）。没有 X 时用
#       xvfb-run -a 包一层：xvfb-run -a tools/retroarch_smoke.sh
# 退出码：0 全通过 / 1 有失败 / 2 环境问题

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ="$(cd "$HERE/.." && pwd)"

CORE="$PROJ/core/dist/x86_64-linux-gnu.2.17/makecode_arcade_libretro.so"
FRAMES=120
KEEP=0
CARTS=()

while [ $# -gt 0 ]; do
  case "$1" in
    --core)   CORE="$2"; shift 2 ;;
    --cart)   CARTS+=("$2"); shift 2 ;;
    --frames) FRAMES="$2"; shift 2 ;;
    --keep)   KEEP=1; shift ;;
    -h|--help) sed -n '2,22p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) echo "未知参数: $1" >&2; exit 2 ;;
  esac
done

command -v retroarch >/dev/null 2>&1 || { echo "缺 retroarch" >&2; exit 2; }
[ -f "$CORE" ] || { echo "找不到核心: $CORE（先 build_target.sh）" >&2; exit 2; }
if [ ${#CARTS[@]} -eq 0 ]; then
  for c in "$PROJ/tools/fixtures/ping.pxt64" "$PROJ/tools/fixtures/probe.pxt64"; do
    [ -f "$c" ] && CARTS+=("$c")
  done
fi
[ ${#CARTS[@]} -gt 0 ] || { echo "没有卡带可测" >&2; exit 2; }

TMP="$(mktemp -d /tmp/mca-ra-smoke.XXXXXX)"
cleanup() { [ "$KEEP" = 1 ] || rm -rf "$TMP"; }
trap cleanup EXIT
mkdir -p "$TMP"/{cores,shots,saves,states,system,home,cfg}

# .info 必须与 .so 同名，RetroArch 靠它认核心（没有也能 -L 直接加载，但信息页空白）
cp "$CORE" "$TMP/cores/makecode_arcade_libretro.so"
VER="$(sed -n 's/.*kCoreVersion *= *"\([^"]*\)".*/\1/p' "$PROJ/core/src/libretro_core.cpp" | head -1)"
sed "s/@VERSION@/${VER:-unknown}/" "$PROJ/tools/core.info.in" \
  > "$TMP/cores/makecode_arcade_libretro.info"

# video/audio/input 全 null：不需要窗口，也不需要声卡
cat > "$TMP/ra.cfg" <<EOF
video_driver = "null"
audio_driver = "null"
input_driver = "null"
input_joypad_driver = "null"
video_fullscreen = "false"
config_save_on_exit = "false"
quit_press_twice = "false"
log_verbosity = "true"
libretro_log_level = "2"
screenshot_directory = "$TMP/shots/"
savefile_directory = "$TMP/saves/"
savestate_directory = "$TMP/states/"
system_directory = "$TMP/system/"
EOF

pass=0
fail=0
printf "%-28s %-6s %-8s %s\n" 卡带 退出码 截图 结论
printf -- "----------------------------------------------------------------------\n"

for cart in "${CARTS[@]}"; do
  name="$(basename "$cart")"
  shot="$TMP/shots/$name.png"
  log="$TMP/$name.log"
  # 独立 HOME/XDG：别踩用户真实配置，也别让上一次的 content history 干扰
  HOME="$TMP/home" XDG_CONFIG_HOME="$TMP/cfg" timeout 120 retroarch \
      -c "$TMP/ra.cfg" -L "$TMP/cores/makecode_arcade_libretro.so" "$cart" \
      --max-frames="$FRAMES" --max-frames-ss --max-frames-ss-path="$shot" -v \
      > "$log" 2>&1
  rc=$?

  shot_ok="无截图"
  if [ -f "$shot" ]; then
    # 全黑/全白说明前端拿到的是空帧。probe.pxt64 本身就是纯白场景（压测用，
    # 不画东西），所以空白只提示不判失败——判失败的是"连截图都没产出"。
    if python3 -c 'import sys;from PIL import Image;lo,hi=Image.open(sys.argv[1]).convert("RGB").getextrema()[0];sys.exit(0 if hi-lo>16 else 1)' "$shot" 2>/dev/null; then
      shot_ok="有画面"
    else
      shot_ok="空白(提示)"
    fi
  fi

  ok=1
  grep -q "Validation OK" "$log" || { ok=0; reason="镜像没装载"; }
  grep -q "SET_PIXEL_FORMAT" "$log" || { ok=0; reason="${reason:-没协商像素格式}"; }
  grep -q "Geometry: 160x120" "$log" || { ok=0; reason="${reason:-几何信息不对}"; }
  [ "$rc" = 0 ] || { ok=0; reason="${reason:-退出码 $rc（139 = 退出段崩）}"; }
  [ "$shot_ok" = "无截图" ] && { ok=0; reason="${reason:-截图没产出}"; }

  if [ "$ok" = 1 ]; then
    pass=$((pass + 1)); printf "%-28s %-6s %-8s %s\n" "$name" "$rc" "$shot_ok" "PASS"
  else
    fail=$((fail + 1)); printf "%-28s %-6s %-8s %s\n" "$name" "$rc" "$shot_ok" "FAIL: $reason"
    echo "  日志：$log"
  fi
done

printf -- "----------------------------------------------------------------------\n"
echo "$pass 通过 / $fail 失败（帧数 $FRAMES，核心 $VER）"
[ "$KEEP" = 1 ] && echo "临时目录保留在：$TMP"
[ "$fail" = 0 ]
