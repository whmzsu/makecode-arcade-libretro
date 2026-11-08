#!/usr/bin/env bash
# run_on_device.sh —— 在真机上一键跑完一批卡带，拿回末帧哈希
#
#   ./run_on_device.sh --carts out/deploy/stage --frames 600
#   ./run_on_device.sh --carts out/testcarts --only arcade-jumpy-platformer.pxt64
#   ./run_on_device.sh --carts out/deploy/stage --baseline out/android_test/baseline.txt
#
# 做的事：
#   1) 装 APK（--install 时）并 push 卡带到 /sdcard/Android/data/org.mca.coretest/files/carts/
#   2) 写 frames.txt / only.txt，force-stop 后 am start —— 每张卡带一个全新进程，
#      一张崩了不影响下一张（等价于 mca_host 的进程隔离语义）
#   3) 从 logcat 抓 "RESULT <名字> <hash> ..." 行，汇总成表；给了 --baseline 就逐行 diff
#
# 退出码：0 全部一致 / 1 有差异或失败 / 2 环境问题

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ="$(cd "$HERE/../.." && pwd)"

PKG=org.mca.coretest
ACT=android.app.NativeActivity
APK="$PROJ/out/android_test/mca-coretest-arm64-v8a.apk"
CARTS=""
FRAMES=600
SERIAL=""
ONLY=""
BASELINE=""
INSTALL=0
WAIT=90          # 单张卡带的等待上限（秒）

while [ $# -gt 0 ]; do
  case "$1" in
    --serial)   SERIAL="$2"; shift 2 ;;
    --apk)      APK="$2"; shift 2 ;;
    --carts)    CARTS="$2"; shift 2 ;;
    --frames)   FRAMES="$2"; shift 2 ;;
    --only)     ONLY="$2"; shift 2 ;;
    --baseline) BASELINE="$2"; shift 2 ;;
    --wait)     WAIT="$2"; shift 2 ;;
    --install)  INSTALL=1; shift ;;
    -h|--help)  sed -n '2,16p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) echo "未知参数: $1" >&2; exit 2 ;;
  esac
done

export MSYS_NO_PATHCONV=1
ADB="${ADB:-$LOCALAPPDATA/Android/Sdk/platform-tools/adb.exe}"
[ -x "$ADB" ] || ADB="$HOME/AppData/Local/Android/Sdk/platform-tools/adb.exe"
[ -x "$ADB" ] || ADB="$(command -v adb || true)"
[ -n "$ADB" ] && [ -x "$ADB" ] || { echo "找不到 adb（设置 ADB=... 或装 platform-tools）" >&2; exit 2; }

if [ -z "$SERIAL" ]; then
  SERIAL="$("$ADB" devices | awk 'NR>1 && $2=="device"{print $1; exit}')"
fi
[ -n "$SERIAL" ] || { echo "没有在线设备" >&2; exit 2; }
A=("$ADB" -s "$SERIAL")
echo "设备: $SERIAL"

EXT=/sdcard/Android/data/$PKG/files

if [ "$INSTALL" = 1 ] || ! "${A[@]}" shell pm list packages 2>/dev/null | grep -q "$PKG"; then
  echo "安装 $APK"
  "${A[@]}" install -r "$APK" >/dev/null || { echo "安装失败" >&2; exit 2; }
fi

"${A[@]}" shell "mkdir -p $EXT/carts" || exit 2

if [ -n "$CARTS" ]; then
  [ -d "$CARTS" ] || { echo "卡带目录不存在: $CARTS" >&2; exit 2; }
  echo "推送卡带: $CARTS/*.pxt64"
  for f in "$CARTS"/*.pxt64; do
    [ -f "$f" ] && "${A[@]}" push "$f" "$EXT/carts/" >/dev/null
  done
fi

"${A[@]}" shell "echo $FRAMES > $EXT/frames.txt"

# 要跑哪些：--only 优先，否则扫设备上 carts/ 目录
if [ -n "$ONLY" ]; then
  LIST="$ONLY"
else
  LIST="$("${A[@]}" shell "ls $EXT/carts/*.pxt64 2>/dev/null" | tr -d '\r' | xargs -n1 basename 2>/dev/null | sort)"
fi
[ -n "$LIST" ] || { echo "设备上没有卡带（先 --carts 指定目录推送）" >&2; exit 2; }

TMP="$(mktemp -d)"
RESULT="$TMP/results.txt"
: >"$RESULT"

count=0
for name in $LIST; do
  count=$((count + 1))
  printf '\r[%d] %-40s' "$count" "$name"
  "${A[@]}" shell "echo '$name' > $EXT/only.txt"
  "${A[@]}" shell "am force-stop $PKG"
  "${A[@]}" logcat -c 2>/dev/null
  "${A[@]}" shell "am start -n $PKG/$ACT" >/dev/null 2>&1

  hit=""
  for _ in $(seq 1 "$WAIT"); do
    sleep 1
    hit="$("${A[@]}" logcat -d -s MCA-TEST 2>/dev/null | tr -d '\r' | grep -E '^(.*MCA-TEST.*)?(RESULT|FAIL|FATAL)' | head -3)"
    echo "$hit" | grep -q "RESULT" && break
  done

  line="$(echo "$hit" | grep "RESULT" | head -1 | sed 's/.*RESULT/RESULT/')"
  if [ -n "$line" ]; then
    echo "$line" >>"$RESULT"
    printf '\r[%d] %-40s %s\n' "$count" "$name" "$(echo "$line" | awk '{print $3}')"
  else
    echo "RESULT $name TIMEOUT-OR-CRASH" >>"$RESULT"
    printf '\r[%d] %-40s %s\n' "$count" "$name" "TIMEOUT/CRASH"
  fi
done

"${A[@]}" shell "rm -f $EXT/only.txt"

echo
echo "================ 结果 ================"
cat "$RESULT"

if [ -n "$BASELINE" ] && [ -f "$BASELINE" ]; then
  echo
  echo "================ 与基线对比 ================"
  awk 'NR==FNR{ if ($1=="RESULT") base[$2]=$3; next }
       $1=="RESULT"{
         n=$2; h=$3;
         if (!(n in base))            printf "%-40s 新增（基线无）\n", n;
         else if (base[n]==h)         printf "%-40s 一致  %s\n", n, h;
         else                         printf "%-40s 差异  基线=%s 实测=%s\n", n, base[n], h;
       }' "$BASELINE" "$RESULT" >"$TMP/diff.txt"
  cat "$TMP/diff.txt"
  if grep -q "差异\|新增" "$TMP/diff.txt" || grep -q "TIMEOUT" "$RESULT"; then
    echo; echo "结论: 有差异"; exit 1
  fi
  echo; echo "结论: 全部一致"
fi

rm -rf "$TMP"
exit 0
