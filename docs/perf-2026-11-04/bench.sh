#!/bin/sh
CORE=$HOME/.config/retroarch/cores/makecode_arcade_libretro.so
for cart in ping Blocky_Boss_Battle arcade-cube-land; do
  for r in 1 2 3; do
    line=$(python3 /tmp/libretro_host.py "$CORE" "$HOME/makecode/$cart.pxt64" 1200 \
             --data-dir=/tmp/bench --turbo 2>&1 | grep "实际节拍")
    fps=$(echo "$line" | grep -o '[0-9.]* fps 实际节拍' | grep -o '^[0-9.]*')
    echo "RESULT,$(hostname),$cart,$r,${fps:-NA}"
  done
done
