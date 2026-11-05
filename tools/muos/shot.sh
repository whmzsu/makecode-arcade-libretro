#!/bin/bash
# 抓一张 muOS 设备屏幕（无头验证前端 UI 用）
#
# 用法: tools/muos/shot.sh [ssh目标] [输出png]
#   默认 ssh目标 = cubexx，输出 = out/deploy/muos/shot.png
#
# 原理: muOS 帧缓冲是 32bpp、720x1440 双缓冲（stride 2880），
#       前 720 行即当前可见帧，取出来按 BGRA 转 PNG。
# 前提: 已配好 ssh 免密（公钥），否则会卡在密码提示。
set -e

TARGET="${1:-cubexx}"
OUT="${2:-out/deploy/muos/shot.png}"
HERE="$(cd "$(dirname "$0")" && pwd)"
RAW="$(dirname "$OUT")/s.raw"

mkdir -p "$(dirname "$OUT")"
ssh -o ConnectTimeout=8 "$TARGET" 'dd if=/dev/fb0 of=/tmp/s.raw bs=2880 count=720 2>/dev/null'
scp -q -o ConnectTimeout=8 "$TARGET:/tmp/s.raw" "$RAW"
python3 "$HERE/fb2png.py" "$RAW" bgra
cp "$(dirname "$RAW")/fb0_bgra.png" "$OUT"
echo "saved: $OUT"
