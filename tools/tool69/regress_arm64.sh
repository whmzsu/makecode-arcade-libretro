#!/bin/bash
# arm64 全语料逐帧回归 —— 在 tool69 上运行，核心在 arm64 chroot 里由 qemu-user 执行
#
# 用法: /opt/mca-work/bin/regress_arm64.sh [帧数，默认 600]
# 产出: /opt/mca-work/reports/regress-<时间戳>/
#   baseline.txt   2 张基准卡带 × 2 个核心的 120 帧 FINAL-FRAME-HASH
#   sweep-*.log    全量批量冒烟原始输出
#   core-*.json    逐张卡带指标 + 末帧哈希
#   report.md      比对结论
#
# 为什么两个核心：core-stage033.so 是掌机上跑的真机 0.3.3（md5 9da97c82…），
# core-built.so 是工具机本地 zig 交叉编译产物。逐张哈希一致 = 换工具链不换语义。
set -u
R=/opt/mca-work/arm64-root
REPO=/opt/mca-work/repo
FRAMES="${1:-600}"
TS=$(date +%Y%m%d-%H%M%S)
OUT=/opt/mca-work/reports/regress-$TS
mkdir -p "$OUT" "$R/root/mca/reports" "$R/tmp/mca_data"
cp "$REPO/tools/libretro_host.py" "$REPO/tools/sweep_carts.py" "$R/root/mca/tools/"

# --- 基准组：与真机历史哈希对齐（120 帧、turbo、不按键） ---
: > "$OUT/baseline.txt"
for c in ping arcade-cube-land; do
  for core in core-stage033.so core-built.so; do
    h=$(chroot "$R" /usr/bin/python3 /root/mca/tools/libretro_host.py \
          "/root/mca/$core" "/root/mca/carts/$c.pxt64" 120 --turbo \
          --data-dir=/tmp/mca_data 2>&1 | grep -o 'FINAL-FRAME-HASH [0-9a-f]*' | head -1)
    printf '%-22s %-20s %s\n' "$c" "$core" "${h:-MISSING}" >> "$OUT/baseline.txt"
  done
done

# --- 全量组：21 张 × N 帧 × 2 核心 ---
for core in core-stage033.so core-built.so; do
  tag=$(basename "$core" .so)
  chroot "$R" /usr/bin/python3 /root/mca/tools/sweep_carts.py \
      --core "/root/mca/$core" /root/mca/carts -n "$FRAMES" --turbo \
      --press=A --press-at=60 --release=90 \
      --data-dir=/tmp/mca_data -o "/root/mca/reports/$tag.json" \
      > "$OUT/sweep-$tag.log" 2>&1 || true
  cp "$R/root/mca/reports/$tag.json" "$OUT/" 2>/dev/null || true
done

python3 /opt/mca-work/bin/compare_regress.py "$OUT" "$FRAMES" > "$OUT/report.md" 2>&1
cat "$OUT/report.md"
printf '\nREPORT_DIR=%s\n' "$OUT"
