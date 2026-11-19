#!/bin/sh
# bench_throughput.sh — 可复现的核心吞吐基线（低端性能造尺子第一步）
#
# 为什么需要它：
#   性能口径极易失真——"实际节拍"被 libretro_host.py 的 16ms sleep 封顶在
#   ~50fps，不是核心能力；"游戏真实帧率"受卡带逻辑限制。唯一可比的是
#   --turbo（不 sleep）下的墙钟吞吐 = frames / 墙钟秒。历史数据
#   docs/perf-2026-11-04/ 与 docs/DEVICE-TEST.md 都用这个口径。
#
# 用法（本机或真机，只要能跑 python3 + 核心 .so）：
#   tools/bench_throughput.sh <core.so> [frames] [repeats] [outdir]
# 例：
#   tools/bench_throughput.sh core/makecode_arcade_libretro.so 1200 3 out/bench
#
# 卡带：ping / Blocky_Boss_Battle / arcade-cube-land（轻/中/重）。
#   在 $CARTS_DIR 找，默认按顺序试 out/testcarts、tools/fixtures、$HOME/makecode。
#   可用 CARTS_DIR=... 覆盖；缺失的卡带跳过并提示。
# 输出：<outdir>/throughput.csv（name,run,fps,finalhash），屏幕打印每卡带中位数。
#   末帧哈希一并记录：吞吐再高，画面和 golden 不一致也没有意义。
set -u

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
CORE=${1:?用法: bench_throughput.sh <core.so> [frames=1200] [repeats=3] [outdir=out/bench]}
FRAMES=${2:-1200}
REPEATS=${3:-3}
OUT=${4:-out/bench}
HOST="$ROOT/tools/libretro_host.py"

# 找卡带目录
CARTS_DIR=""
for d in "${CARTS_DIR:-}" "$ROOT/out/testcarts" "$ROOT/tools/fixtures" "$HOME/makecode"; do
    [ -n "$d" ] || continue
    if [ -f "$d/arcade-cube-land.pxt64" ] || [ -f "$d/ping.pxt64" ]; then
        CARTS_DIR=$d; break
    fi
done
if [ -z "$CARTS_DIR" ]; then
    echo "找不到卡带目录（需含 ping 或 arcade-cube-land.pxt64），设 CARTS_DIR= 指定" >&2
    exit 2
fi

mkdir -p "$OUT"
CSV="$OUT/throughput.csv"
echo "name,run,fps,finalhash" > "$CSV"
echo "# core=$(basename "$CORE") frames=$FRAMES repeats=$REPEATS carts=$CARTS_DIR host=$(hostname)"

for cart in ping Blocky_Boss_Battle arcade-cube-land; do
    path="$CARTS_DIR/$cart.pxt64"
    if [ ! -f "$path" ]; then
        echo "# 跳过 $cart（无 $path）"
        continue
    fi
    for r in $(seq 1 "$REPEATS"); do
        log="$OUT/$cart.run$r.log"
        python3 "$HOST" "$CORE" "$path" "$FRAMES" --turbo --data-dir="$OUT/data" > "$log" 2>&1
        rc=$?
        if [ $rc -ne 0 ]; then
            echo "$cart,$r,NA,RC$rc" >> "$CSV"
            echo "  $cart run$r: FAIL rc=$rc"
            continue
        fi
        # fps 取宿主第 11 节 "N.N fps 实际节拍"；turbo 下这就是墙钟吞吐
        fps=$(grep -o '[0-9.]* fps 实际节拍' "$log" | tail -1 | grep -o '^[0-9.]*')
        hash=$(grep -o 'FINAL-FRAME-HASH [0-9a-f]*' "$log" | tail -1 | awk '{print $2}')
        [ -n "$fps" ] || fps=NA
        [ -n "$hash" ] || hash=NA
        echo "$cart,$r,$fps,$hash" >> "$CSV"
        echo "  $cart run$r: ${fps} fps  hash=$hash"
    done
done

echo "=== 中位数（相对 60fps 余量）==="
python3 - "$CSV" <<'PY'
import csv, statistics as st, sys
per = {}
for row in csv.DictReader(open(sys.argv[1], encoding='utf-8')):
    if row['fps'] not in ('NA', ''):
        per.setdefault(row['name'], []).append(float(row['fps']))
for name, vals in per.items():
    m = st.median(vals)
    hashes = set()
    for row in csv.DictReader(open(sys.argv[1], encoding='utf-8')):
        if row['name'] == name and row['finalhash'] not in ('NA', ''):
            hashes.add(row['finalhash'])
    tag = "一致" if len(hashes) == 1 else f"不一致({len(hashes)}种)"
    print(f"  {name:22s} {m:7.1f} fps  {m/60:.2f}×  hash {tag}")
PY
echo "CSV: $CSV"
