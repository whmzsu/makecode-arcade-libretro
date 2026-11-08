#!/usr/bin/env bash
# run_shellhost.sh —— 用 shell 宿主（不装 APK）在真机上跑卡带回归 / 长测
#
#   ./run_shellhost.sh --carts out/android_test/carts21 --baseline out/android_test/baseline21.txt
#   ./run_shellhost.sh --rounds 20 --frames 600          # 连续 20 轮，用来做长测
#   ./run_shellhost.sh --only ping.pxt64 --frames 3000   # 单张卡带跑 50 秒游戏时间
#
# 适用条件：设备的 `targetSdkVersion` 小于 30（Android 9 及以前，bionic 不做堆指针打标），
# 或前端把堆开在低地址。SDK >= 30 的设备上 bionic 会给 malloc 打 top-byte tag，
# pxt 的 NaN-boxing 必然 PANIC 906 —— 那种设备请用 run_on_device.sh 走 APK 路线。
#
# 每张卡带一个独立进程（跟 mca_host 的进程隔离语义一致），一张崩了不影响下一张。
#
# 退出码：0 全部一致 / 1 有差异、崩溃或超时 / 2 环境问题

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ="$(cd "$HERE/../.." && pwd)"

REMOTE=/data/local/tmp/mca
CARTS=""
CORE="$PROJ/out/android-manual-test/mca035b_libretro.so"
HOSTBIN="$PROJ/out/android/mca_host"
FRAMES=600
ROUNDS=1
SERIAL=""
ONLY=""
BASELINE=""
PUSH=1
DEFAULT_BASELINE="$PROJ/tools/fixtures/android21-baseline.txt"

while [ $# -gt 0 ]; do
  case "$1" in
    --serial)   SERIAL="$2"; shift 2 ;;
    --core)     CORE="$2"; shift 2 ;;
    --carts)    CARTS="$2"; shift 2 ;;
    --frames)   FRAMES="$2"; shift 2 ;;
    --rounds)   ROUNDS="$2"; shift 2 ;;
    --only)     ONLY="$2"; shift 2 ;;
    --baseline) BASELINE="$2"; shift 2 ;;
    --remote)   REMOTE="$2"; shift 2 ;;
    --no-push)  PUSH=0; shift ;;
    -h|--help)  sed -n '2,18p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) echo "未知参数: $1" >&2; exit 2 ;;
  esac
done

export MSYS_NO_PATHCONV=1
ADB="${ADB:-$LOCALAPPDATA/Android/Sdk/platform-tools/adb.exe}"
[ -x "$ADB" ] || ADB="$HOME/AppData/Local/Android/Sdk/platform-tools/adb.exe"
[ -x "$ADB" ] || ADB="$(command -v adb || true)"
[ -n "$ADB" ] && [ -x "$ADB" ] || { echo "找不到 adb（设置 ADB=...）" >&2; exit 2; }

# 关掉路径转换后，Windows 版 adb 读不了 /c/... 形式的本地路径，必须先转成 C:\...
if command -v cygpath >/dev/null 2>&1; then
  winpath() { cygpath -w "$1"; }
else
  winpath() { printf '%s' "$1"; }
fi

if [ -z "$SERIAL" ]; then
  SERIAL="$("$ADB" devices | awk 'NR>1 && $2=="device"{print $1; exit}')"
fi
[ -n "$SERIAL" ] || { echo "没有在线设备" >&2; exit 2; }
A=("$ADB" -s "$SERIAL")

# 没显式给 --baseline 就默认用仓库里那份 21 卡基线（out/ 会被清掉，基线不能放那儿）
if [ -z "$BASELINE" ] && [ -f "$DEFAULT_BASELINE" ]; then
  BASELINE="$DEFAULT_BASELINE"
  echo "使用默认基线: $BASELINE"
fi

SDK="$("${A[@]}" shell getprop ro.build.version.sdk | tr -d '\r')"
MODEL="$("${A[@]}" shell getprop ro.product.model | tr -d '\r')"
echo "设备: $SERIAL  $MODEL  SDK=$SDK"
if [ "${SDK:-0}" -ge 30 ]; then
  echo "警告: SDK=$SDK >= 30，非 app 进程会被打堆 tag，本脚本大概率 PANIC 906；请改用 run_on_device.sh" >&2
fi

# ---- 部署 ----
if [ "$PUSH" = 1 ]; then
  [ -f "$CORE" ] || { echo "核心不存在: $CORE" >&2; exit 2; }
  [ -f "$HOSTBIN" ] || { echo "宿主二进制不存在: $HOSTBIN（见 README 的编译命令）" >&2; exit 2; }
  "${A[@]}" shell "mkdir -p $REMOTE/carts $REMOTE/data" >/dev/null
  "${A[@]}" push "$(winpath "$HOSTBIN")" "$REMOTE/mca_host" >/dev/null || exit 2
  "${A[@]}" push "$(winpath "$CORE")" "$REMOTE/core.so" >/dev/null || exit 2
  "${A[@]}" shell "chmod 755 $REMOTE/mca_host"
  if [ -n "$CARTS" ]; then
    [ -d "$CARTS" ] || { echo "卡带目录不存在: $CARTS" >&2; exit 2; }
    "${A[@]}" push "$(winpath "$CARTS")"/. "$REMOTE/carts/" >/dev/null || exit 2
  fi
fi

if [ -n "$ONLY" ]; then
  LIST="$ONLY"
else
  LIST="$("${A[@]}" shell "ls $REMOTE/carts/*.pxt64 2>/dev/null" | tr -d '\r' | xargs -n1 basename 2>/dev/null | sort)"
fi
[ -n "$LIST" ] || { echo "设备上没有卡带（用 --carts 指定目录）" >&2; exit 2; }

mkdir -p "$PROJ/out/android_test"
RESULT="$PROJ/out/android_test/last-run.txt"; : >"$RESULT"
echo "结果写到: $RESULT"

echo "帧数: $FRAMES  轮数: $ROUNDS  卡带: $(echo "$LIST" | wc -w) 张"
echo

r=0
while [ "$r" -lt "$ROUNDS" ]; do
  r=$((r + 1))
  [ "$ROUNDS" -gt 1 ] && echo "-------- 第 $r/$ROUNDS 轮 --------"
  for name in $LIST; do
    t0=$(date +%s)
    out="$("${A[@]}" shell "cd $REMOTE && ./mca_host ./core.so ./carts/$name $FRAMES --data-dir=$REMOTE/data 2>/dev/null; echo EXIT=\$?" | tr -d '\r')"
    t1=$(date +%s)
    h=$(echo "$out" | grep -oE "FINAL-FRAME-HASH [0-9a-f]{32}" | awk '{print $2}' | head -1)
    rc=$(echo "$out" | grep -oE "EXIT=[0-9]+" | cut -d= -f2 | head -1)
    st=$(echo "$out" | grep -oE "STATS .*" | head -1)
    echo "RESULT $name ${h:-FAIL} rc=${rc:-?} $((t1-t0))s  $st" | tee -a "$RESULT"
  done
  echo
done

echo "================ 与基线对比 ================"
if [ -n "$BASELINE" ] && [ -f "$BASELINE" ]; then
  awk 'NR==FNR{ if ($1=="RESULT") base[$2]=$3; next }
       $1=="RESULT"{
         n=$2; h=$3; rc=$4;
         if (!(n in base))       printf "%-40s 基线无此项  %s\n", n, h;
         else if (base[n]==h && rc=="rc=0") printf "%-40s 一致  %s\n", n, h;
         else if (base[n]==h)    printf "%-40s 哈希一致但 %s\n", n, rc;
         else                    printf "%-40s 差异  基线=%s 实测=%s %s\n", n, base[n], h, rc;
       }' "$BASELINE" "$RESULT"
else
  echo "（未给 --baseline，仅列表）"
  cat "$RESULT"
fi

bad=0
grep -qE "rc=[1-9]" "$RESULT" && bad=1
if [ -n "$BASELINE" ] && [ -f "$BASELINE" ]; then
  diffc=$(awk 'NR==FNR{ if ($1=="RESULT") base[$2]=$3; next }
               $1=="RESULT"{ n=$2; h=$3; if ((n in base) && base[n]!=h) c++ }
               END{ print c+0 }' "$BASELINE" "$RESULT")
  [ "${diffc:-0}" -gt 0 ] && bad=1
fi

echo
if [ "$bad" = 0 ]; then echo "结论: 全部一致"; exit 0; else echo "结论: 有差异或失败"; exit 1; fi
