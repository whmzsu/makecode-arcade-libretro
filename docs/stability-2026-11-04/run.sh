#!/bin/sh
# stability.sh —— 真机稳定性长跑（3 段 + panic 收尾）
# 部署路径：/tmp/stability.sh   依赖：/tmp/libretro_host.py
# 产物：/tmp/stab-<host>.csv（采样曲线）/tmp/stab-<host>.log（阶段摘要）/tmp/stab-<host>/<phase>.log
# 采样列：ts,phase,event,rss_kb,hwm_kb,cputicks,temp_c,load1,progress
#   cputicks = utime+stime（jiffies，CLK_TCK=100）→ 平均单核占用 = Δticks/(Δ秒×100)
set -u

HOST=$(hostname | tr -d ' \n')
CORE="$HOME/.config/retroarch/cores/makecode_arcade_libretro.so"
DIR="/tmp/stab-$HOST"
OUT="/tmp/stab-$HOST.csv"
LOG="/tmp/stab-$HOST.log"

rm -rf "$DIR"; mkdir -p "$DIR"
: > "$OUT"; : > "$LOG"
echo "ts,phase,event,rss_kb,hwm_kb,cputicks,temp_c,load1,progress" >> "$OUT"
echo "start $(date) host=$HOST core=$(ls -l "$CORE" | awk '{print $5}')" >> "$LOG"

read_temp() {
  v=$(cat /sys/class/thermal/thermal_zone0/temp 2>/dev/null)
  if [ -n "${v:-}" ]; then echo $((v / 1000)); else echo 0; fi
}

mem_of() {
  awk -v k="$2" '$1==k":"{print $2}' "/proc/$1/status" 2>/dev/null
}

sample() {
  ph=$1; pid=$2
  while kill -0 "$pid" 2>/dev/null; do
    ticks=$(awk '{print $14+$15}' "/proc/$pid/stat" 2>/dev/null)
    prog=$(grep -o 'frame [0-9]*:' "$DIR/$ph.log" 2>/dev/null | tail -1 | tr -dc '0-9')
    echo "$(date +%H:%M:%S),$ph,sample,$(mem_of "$pid" VmRSS),$(mem_of "$pid" VmHWM),${ticks:-0},$(read_temp),$(cut -d' ' -f1 /proc/loadavg),${prog:-0}" >> "$OUT"
    sleep 20
  done
}

run_phase() {
  ph=$1; cart=$2; frames=$3
  echo "$(date +%H:%M:%S),$ph,start,,,,,," >> "$OUT"
  echo "=== PHASE $ph cart=$cart frames=$frames @ $(date +%H:%M:%S) ===" >> "$LOG"
  python3 /tmp/libretro_host.py "$CORE" "$HOME/makecode/$cart" "$frames" --data-dir="$DIR" > "$DIR/$ph.log" 2>&1 &
  pid=$!
  sample "$ph" "$pid" &
  wait "$pid"; rc=$?
  echo "$(date +%H:%M:%S),$ph,end_rc_$rc,,,,,," >> "$OUT"
  echo "=== END $ph rc=$rc @ $(date +%H:%M:%S) ===" >> "$LOG"
  grep -E "统计|FINAL-FRAME-HASH|Traceback|core dumped|PANIC" "$DIR/$ph.log" | tail -4 >> "$LOG"
}

run_phase cubeland arcade-cube-land.pxt64 20000
run_phase blocky Blocky_Boss_Battle.pxt64 20000
run_phase ping ping.pxt64 20000
run_phase panic Strawberry_Slam.pxt64 6000

echo "ALL_DONE $(date +%H:%M:%S)" >> "$LOG"
