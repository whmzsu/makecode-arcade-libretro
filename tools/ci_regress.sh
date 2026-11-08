#!/usr/bin/env bash
# ci_regress.sh — 末帧哈希回归（golden hash）
#
# 为什么需要：core/tests/contract_test 只断言 API 契约（符号、哨兵、协商时机、
# load 失败路径）。它断言不了"跑出来的画面还是同一张画面"——VM 语义、渲染路径、
# 输入映射任何一处漂移，契约照样全绿。这是 CI 上唯一能抓住"游戏画错了"的断言。
#
# 用法：
#   tools/ci_regress.sh <core>                  # 与基线比对，不一致 exit 1
#   tools/ci_regress.sh <core> --record         # 用当前结果重写基线
#   tools/ci_regress.sh <core> --only ping      # 只跑名字含 ping 的用例
#   tools/ci_regress.sh <core> --baseline FILE  # 换基线文件
#   tools/ci_regress.sh <core> --bmp-dir DIR    # 顺手把末帧落成 BMP（排查用）
#
# 基线格式（tools/fixtures/golden-frames.txt）：
#   <相对仓库根的卡带路径>|<帧数>|<额外宿主参数，无则 ->|<末帧 MD5>
#
# 口径是 FINAL-FRAME-HASH，不是文件大小/md5。**改核心、改帧数、改输入时序、
# 换编译器后都必须 --record 重生成**，否则基线本身就成了误报源。
#
# 退出码：0 全一致 / 1 有差异或缺画面 / 2 环境问题

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ="$(cd "$HERE/.." && pwd)"

BASELINE="$PROJ/tools/fixtures/golden-frames.txt"
CORE=""
RECORD=0
ONLY=""
BMPDIR=""

while [ $# -gt 0 ]; do
  case "$1" in
    --record)        RECORD=1; shift ;;
    --baseline)      BASELINE="$2"; shift 2 ;;
    --only)          ONLY="$2"; shift 2 ;;
    --bmp-dir)       BMPDIR="$2"; shift 2 ;;
    -h|--help)       sed -n '2,20p' "${BASH_SOURCE[0]}"; exit 0 ;;
    -*)              echo "未知参数: $1" >&2; exit 2 ;;
    *)               CORE="$1"; shift ;;
  esac
done

[ -n "$CORE" ] || { echo "用法: tools/ci_regress.sh <core> [--record]" >&2; exit 2; }
[ -f "$CORE" ] || { echo "找不到核心: $CORE" >&2; exit 2; }
[ -f "$BASELINE" ] || [ "$RECORD" = 1 ] || { echo "找不到基线: $BASELINE" >&2; exit 2; }

# CI 上有 python3；本地 Git Bash 往往只有 python
PY=""
for c in python3 python; do
  if command -v "$c" >/dev/null 2>&1; then PY="$c"; break; fi
done
[ -n "$PY" ] || { echo "找不到 python" >&2; exit 2; }

TMP="$PROJ/out/ci_regress"
rm -rf "$TMP"; mkdir -p "$TMP"

# 每个用例一个独立存档目录：core 会往 GET_SAVE_DIRECTORY 写 settings / trace，
# 共用目录会让用例之间互相影响（也见过 trace 文件被并发写坏）
run_case() {
  local cart="$1" frames="$2" extra="$3" tag="$4" bmp="$5"
  local -a args=( "$PROJ/tools/libretro_host.py" "$CORE" "$PROJ/$cart" "$frames"
                  --turbo --data-dir="$TMP/$tag" )
  [ -n "$bmp" ] && args+=( "--bmp=$bmp" )
  # extra 是"参数串"，必须按空格拆成多个实参（不能整体加引号）
  local -a xtra=()
  if [ "$extra" != "-" ] && [ -n "$extra" ]; then
    read -r -a xtra <<< "$extra"
    args+=( "${xtra[@]}" )
  fi
  "$PY" "${args[@]}" 2>&1
}

printf '%-34s %-6s %-10s %s\n' 用例 帧数 结果 末帧哈希
printf '%s\n' "----------------------------------------------------------------------------"

fail=0
total=0
ok=0
new_lines=""

while IFS='|' read -r cart frames extra want; do
  case "$cart" in ''|\#*) continue ;; esac
  # 去掉 CRLF（基线文件在 Windows 上被 checkout 过的话）
  cart="${cart%$'\r'}"; frames="${frames%$'\r'}"
  extra="${extra%$'\r'}"; want="${want%$'\r'}"
  name="$(basename "$cart")"
  [ -n "$ONLY" ] && case "$name" in *"$ONLY"*) ;; *) continue ;; esac

  total=$((total + 1))
  bmp=""
  if [ -n "$BMPDIR" ]; then
    mkdir -p "$BMPDIR"
    bmp="$BMPDIR/${name%.pxt64}.bmp"
  fi

  out="$(run_case "$cart" "$frames" "$extra" "$name" "$bmp")"
  rc=$?
  got="$(printf '%s\n' "$out" | sed -n 's/^FINAL-FRAME-HASH \([0-9a-f]\{32\}\).*/\1/p' | tail -1)"

  if [ $rc -ne 0 ] || [ -z "$got" ]; then
    printf '%-34s %-6s %-10s %s\n' "$name" "$frames" "崩溃/无画面" "rc=$rc"
    # 失败原因平时藏在折叠的日志里，转成注解让公开仓库也能匿名看到
    printf '%s\n' "$out" | grep -E '^\[FAIL\]|^   \[FAIL\]|load_game|PANIC|Traceback' \
      | head -6 | sed 's/^/::error::  /'
    echo "::error file=tools/fixtures/golden-frames.txt::${name} 跑不出末帧（rc=$rc）"
    fail=$((fail + 1))
    new_lines+="$cart|$frames|$extra|$got"$'\n'
    continue
  fi

  if [ "$got" = "$want" ]; then
    printf '%-34s %-6s %-10s %s\n' "$name" "$frames" "一致" "$got"
    ok=$((ok + 1))
  elif [ "$RECORD" = 1 ]; then
    printf '%-34s %-6s %-10s %s (原 %s)\n' "$name" "$frames" "已记录" "$got" "${want:0:8}"
    ok=$((ok + 1))
  else
    printf '%-34s %-6s %-10s %s\n' "$name" "$frames" "不一致" "$got"
    printf '%s\n' "  期望 $want"
    echo "::error file=tools/fixtures/golden-frames.txt::${name} 末帧哈希漂移：期望 $want，实际 $got"
    fail=$((fail + 1))
  fi
  new_lines+="$cart|$frames|$extra|$got"$'\n'
done < "$BASELINE"

printf '%s\n' "----------------------------------------------------------------------------"
printf '%d 个用例：一致 %d，失败 %d\n' "$total" "$ok" "$fail"

if [ "$RECORD" = 1 ] && [ "$total" -gt 0 ]; then
  # 保留原有注释与顺序：注释头原样搬过去，正文整体替换
  {
    sed -n '/^#/p' "$BASELINE"
    printf '%s' "$new_lines" | sed '/^$/d'
  } > "$BASELINE.tmp"
  mv "$BASELINE.tmp" "$BASELINE"
  echo "基线已更新：$BASELINE（$(printf '%s' "$new_lines" | grep -c . ) 行）"
  echo "提交前务必跑一遍不带 --record 的比对，确认是稳定值（连跑两次结果相同）。"
  exit 0
fi

[ "$fail" -eq 0 ] && [ "$total" -gt 0 ] || exit 1
exit 0
