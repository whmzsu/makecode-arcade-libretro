#!/usr/bin/env bash
# tool69 常驻每日回归（cron 03:20）—— 全程 nice + flock，不与共用桌面机抢资源
#
# 做三件事：
#   1) arm64 chroot 里跑 21 张语料 × 600 帧逐帧回归，与真机 0.3.3 基准比对；
#   2) 在线的两台掌机（cubexx .149 / rgb30c .101）轻量巡检：温度、核心 md5、
#      卡带数、governor（读不到就标 SKIP，不重试、不唤醒设备）；
#   3) 汇总成一份报告，落在 $MCA_REPORTS/daily-<日期>/。
#
# 明确不做：不重新同步源码、不重装依赖、不跑性能基准（qemu 下的 fps 无意义）、
# 不改任何服务、不重启设备。源码更新由 Windows 侧 scp 推送后手工触发全量矩阵。
set -uo pipefail
source /opt/mca/env.sh
BIN=$MCA_BIN
REPORTS=$MCA_REPORTS
OUT="$REPORTS/daily-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$OUT"
exec 9>/tmp/mca-daily.lock
if ! flock -n 9; then
    echo "已有实例在运行，退出"
    exit 0
fi

DEV_CMD='
printf "temp: ";
for z in /sys/class/thermal/thermal_zone0/temp /sys/class/thermal/thermal_zone1/temp; do
  [ -r "$z" ] && printf "%s=%s " "$(basename $(dirname $z))" "$(cat $z)"
done; echo
printf "governor: "; cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo "?"
printf "core md5: ";
find "$HOME/.config/retroarch/cores" /opt/muos/share/core \
     -maxdepth 1 -name "makecode_arcade_libretro.so" -exec md5sum {} \; 2>/dev/null | head -2
printf "carts: ";
find /roms/makecode "/mnt/mmc/ROMS/MakeCode Arcade" "$HOME/makecode" \
     -name "*.pxt64" 2>/dev/null | wc -l
'

{
    echo "# 每日回归 $(date '+%F %T %Z')"
    echo
    echo "## 1. arm64 全语料逐帧回归（21 张 × 600 帧）"
    nice -n 10 "$BIN/regress_arm64.sh" 600 > "$OUT/regress.log" 2>&1
    echo "退出码：$?"
    grep -E "^## 结论|判定分布|非预期异常|REPORT_DIR" "$OUT/regress.log" | head -6
    echo
    echo "## 2. 在线设备巡检（只读）"
    for spec in "cubexx|root@192.168.1.149" "rgb30c|ark@192.168.1.101"; do
        name="${spec%%|*}"; dest="${spec##*|}"
        echo "### $name ($dest)"
        if out=$(ssh -i /root/.ssh/id_ed25519_mca -o BatchMode=yes -o ConnectTimeout=5 \
                     -o StrictHostKeyChecking=accept-new \
                     "$dest" "$DEV_CMD" 2>/dev/null); then
            printf '%s\n' "$out"
        else
            echo "SKIP：工具机尚未配置到该设备的免密登录（或设备关机）"
        fi
    done
    echo
    echo "## 3. 磁盘与负载"
    df -h /opt | tail -1
    uptime
} > "$OUT/summary.txt" 2>&1

cat "$OUT/summary.txt"
# 只保留最近 30 份每日报告
find "$REPORTS" -maxdepth 1 -type d -name "daily-*" -printf '%T@ %p\n' \
    | sort -rn | tail -n +31 | cut -d' ' -f2- | xargs -r rm -rf
printf '\nDAILY_DIR=%s\n' "$OUT"
