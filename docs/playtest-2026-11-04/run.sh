#!/bin/sh
# playtest.sh —— 全卡带 + 随机按键"边玩边测"
# 每张卡带 7000 帧（约 2.3~3 分钟游戏时间），--random-input 每 18 帧换向，
# 每张存末帧 BMP。清单 /tmp/cartlist.txt（三台统一 21 张）。
# 产物：/tmp/play-<host>.csv 每张结果；/tmp/play-<host>.log 摘要；
#       /tmp/play-<host>/<name>.bmp 末帧；/tmp/sample-<host>.csv 资源采样。
set -u

HOST=$(hostname | tr -d ' \n')
CORE="$HOME/.config/retroarch/cores/makecode_arcade_libretro.so"
DIR="/tmp/play-$HOST"
OUT="/tmp/play-$HOST.csv"
LOG="/tmp/play-$HOST.log"

rm -rf "$DIR"; mkdir -p "$DIR"
: > "$OUT"; : > "$LOG"; : > "/tmp/sample-$HOST.csv"
echo "name,rc,distinct,beat_fps,panics,end_ts" >> "$OUT"
echo "ts,rss_kb,hwm_kb,temp_c,load1,cart" >> "/tmp/sample-$HOST.csv"
echo "start $(date) host=$HOST carts=$(wc -l < /tmp/cartlist.txt) core=$(ls -l "$CORE" | awk '{print $5}')" >> "$LOG"

# 后台资源采样：每 30 秒记一次 RSS/温度，并标注当前卡带
(
  while [ ! -f "$DIR/.done" ]; do
    rss=$(awk '/VmRSS/{print $2}' /proc/$(pgrep -f "libretro_host.py" | head -1)/status 2>/dev/null)
    hwm=$(awk '/VmHWM/{print $2}' /proc/$(pgrep -f "libretro_host.py" | head -1)/status 2>/dev/null)
    t=$(cat /sys/class/thermal/thermal_zone0/temp 2>/dev/null); t=$(( ${t:-0} / 1000 ))
    echo "$(date +%H:%M:%S),${rss:-0},${hwm:-0},$t,$(cut -d' ' -f1 /proc/loadavg),$(cat "$DIR/.cur" 2>/dev/null)" >> "/tmp/sample-$HOST.csv"
    sleep 30
  done
) &
SAMPLER=$!

while read -r name; do
  [ -z "$name" ] && continue
  echo "$name" > "$DIR/.cur"
  log="$DIR/$name.log"
  python3 /tmp/libretro_host.py "$CORE" "$HOME/makecode/$name.pxt64" 7000 \
    --data-dir="$DIR/d_$name" --random-input=1234:18 --bmp="$DIR/$name.bmp" > "$log" 2>&1
  rc=$?
  eval "$(python3 -c "
import re
t = open('$log', encoding='utf-8', errors='replace').read()
d = re.search(r'独立画面 (\d+) 帧', t)
b = re.search(r'([\d.]+) fps 实际节拍', t)
print('distinct=%s beat=%s' % (d.group(1) if d else 0, b.group(1) if b else 0))
")"
  panics=$(grep -c "PANIC" "$log" 2>/dev/null)
  echo "$name,$rc,${distinct:-0},${beat:-0},${panics:-0},$(date +%H:%M:%S)" >> "$OUT"
  echo "=== $name rc=$rc distinct=${distinct:-0} beat=${beat:-0} panic=${panics:-0} @ $(date +%H:%M:%S)" >> "$LOG"
  grep -E "Traceback|core dumped|FAIL" "$log" | head -2 >> "$LOG"
  rm -rf "$DIR/d_$name"
done < /tmp/cartlist.txt

echo done > "$DIR/.done"
kill "$SAMPLER" 2>/dev/null
echo "ALL_DONE $(date +%H:%M:%S)" >> "$LOG"
