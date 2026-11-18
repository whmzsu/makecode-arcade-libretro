#!/bin/bash
# VM 栈尺寸 × GC 堆高水位测量（3DS/Vita 定容，PXT32 = x86-32 构建 + 32 位 C 宿主）
# 用法: ZIG=zig bash measure_stack.sh   （在 tool69 上跑）
# 产物: /opt/mca-data/scratch/vm-measure/results/sweep.csv
#       每行: tag,cart,rc,final_hash,hwm_kb
set -u
BASE=/opt/mca-data/scratch/vm-measure
SRC=$BASE/src
CARTS=$SRC/out/carts63
OUT=$BASE/results
HOST32=$BASE/mca_host32
mkdir -p "$OUT"
cd "$SRC" || exit 1
ZIG="${ZIG:-zig}"

echo "== build 32-bit C host..."
"$ZIG" cc -target x86-linux-gnu.2.17 -O2 -Itools -Icore -o "$HOST32" tools/mca_host_android.c || exit 1
ls -la "$HOST32"

run_sweep() {
  local tag="$1" stack="$2" extra=""
  [ "$stack" != "default" ] && extra="-DMCA_VM_STACK_SIZE=$stack"
  echo "== [$tag] stack=$stack build..."
  # 关键：build_target.sh 的 needs_build 按 mtime 跳过未改动源文件，
  # 换宏不触发重编译 → 必须清掉对象/产物强制全量重编，否则六组跑的是同一个 .so。
  rm -rf core/build core/dist
  if ! ZIG="$ZIG" EXTRA_CXXFLAGS="$extra -DMCA_GC_HWM_LOG=1" \
       bash tools/build_target.sh x86-linux-gnu.2.17 > "$OUT/build-$tag.log" 2>&1; then
    echo "$tag,BUILD-FAIL,," >> "$OUT/sweep.csv"
    tail -3 "$OUT/build-$tag.log"
    return 1
  fi
  local SO
  SO=$(find core/dist/x86-linux-gnu.2.17 -name "*.so" 2>/dev/null | head -1)
  [ -z "$SO" ] && { echo "$tag,SO-NOT-FOUND,," >> "$OUT/sweep.csv"; return 1; }
  echo "   so=$SO size=$(stat -c %s "$SO")"
  # 宏生效自检：.so 尺寸必须随配置变化（不同栈常量会进 spinThread 代码段）
  echo "$tag so_size=$(stat -c %s "$SO")" >> "$OUT/so_sizes.txt"
  local c name d rc hash hwm tr
  for c in "$CARTS"/*.pxt64; do
    name=$(basename "$c")
    d=$(mktemp -d)
    ( cd "$d" && "$HOST32" "$SRC/$SO" "$c" 600 --data-dir="$d/dd" > out.txt 2>&1 )
    rc=$?
    hash=$(grep -o "FINAL-FRAME-HASH [0-9a-f]*" "$d/out.txt" 2>/dev/null | awk '{print $2}' | head -1)
    tr=$(find "$d" -name "mca_trace.log" 2>/dev/null | tail -1)
    hwm=$(grep -o "gc heap hwm=[0-9]*" "$tr" 2>/dev/null | tail -1 | grep -o "[0-9]*")
    echo "$tag,$name,$rc,${hash:-none},${hwm:-0}" >> "$OUT/sweep.csv"
    rm -rf "$d"
  done
  echo "== [$tag] done ($(wc -l < "$OUT/sweep.csv") rows total)"
}

: > "$OUT/sweep.csv"
for cfg in "default:default" "s2m:2097152" "s512k:524288" "s256k:262144" "s128k:131072" "s64k:65536"; do
  tag=${cfg%%:*}; sz=${cfg##*:}
  run_sweep "$tag" "$sz"
done
echo "ALL-DONE"
