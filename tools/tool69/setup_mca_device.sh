#!/usr/bin/env bash
# One-stop MakeCode Arcade setup for Debian13 + EmulationStation handhelds.
# Usage: setup_mca_device.sh <IP> [USER=ark]
#   Installs: core(.so+.info) + 64 carts + ES menu + theme icons + gamelist
#   Verifies: golden 3-case frame hashes (0556e78f/e82844f7/0eb7884c)
# Idempotent: re-running overwrites assets and does not duplicate ES menu block.
# Runs ON tool69; assets come from $MCA_WORK/deploy/mca-kit (see MEMORY-devices.md).
set -euo pipefail

IP=${1:?usage: setup_mca_device.sh <IP> [USER=ark]}
USER=${2:-ark}
REMOTE="$USER@$IP"
KIT="${MCA_KIT:-/opt/mca/work/deploy/mca-kit}"
SSH="ssh -o BatchMode=yes -o StrictHostKeyChecking=accept-new -o ConnectTimeout=10"
SCP="scp -o BatchMode=yes -o StrictHostKeyChecking=accept-new -o ConnectTimeout=10"
GOLDEN="0556e78f e82844f7 0eb7884c"

log() { printf '\n== %s ==\n' "$*"; }
die() { echo "ERROR: $*" >&2; exit 1; }

if [ "$(id -u)" -ne 0 ]; then
  [ -r /opt/mca/env.sh ] && source /opt/mca/env.sh || true
fi
for f in makecode_arcade_libretro.so makecode_arcade_libretro.info carts64.tgz theme.tgz gen_gamelist.py libretro_host.py ping.pxt64 probe.pxt64; do
  [ -f "$KIT/$f" ] || die "kit missing: $KIT/$f"
done

log "0. connectivity $REMOTE"
$SSH "$REMOTE" 'echo device-ok; uname -m; grep PRETTY /etc/os-release | cut -d\" -f2' || die "ssh failed"

log "1. transfer assets"
$SSH "$REMOTE" 'mkdir -p /tmp/mca-setup'
$SCP "$KIT"/makecode_arcade_libretro.so "$KIT"/makecode_arcade_libretro.info \
     "$KIT"/carts64.tgz "$KIT"/theme.tgz "$KIT"/gen_gamelist.py "$KIT"/libretro_host.py \
     "$KIT"/ping.pxt64 "$KIT"/probe.pxt64 \
     "$REMOTE:/tmp/mca-setup/"

log "2. install core + 64 carts"
$SSH "$REMOTE" 'set -e
  install -m 755 /tmp/mca-setup/makecode_arcade_libretro.so ~/.config/retroarch/cores/
  install -m 644 /tmp/mca-setup/makecode_arcade_libretro.info ~/.config/retroarch/cores/
  mkdir -p /roms/makecode
  tar xzf /tmp/mca-setup/carts64.tgz -C /roms/makecode/
  echo "carts=$(ls /roms/makecode/*.pxt64 | wc -l)"'

log "3. register ES menu (idempotent)"
$SSH "$REMOTE" 'set -e
  CFG=/etc/emulationstation/es_systems.cfg
  [ -f "$CFG" ] || { echo "no ES config at $CFG (not a Debian13+ES device?)" >&2; exit 1; }
  if grep -q "<name>makecode</name>" "$CFG"; then
    echo "ES menu already registered, skip"
  else
    [ -f "$CFG.bak-mca-setup" ] || cp -a "$CFG" "$CFG.bak-mca-setup"
    python3 - "$CFG" <<"PY"
import sys
p=sys.argv[1]
s=open(p,encoding="utf-8").read()
assert "<name>makecode</name>" not in s
block="""\t<system>
\t\t<name>makecode</name>
\t\t<fullname>MakeCode Arcade</fullname>
\t\t<path>/roms/makecode/</path>
\t\t<extension>.pxt64 .PXT64</extension>
\t\t<command>sudo perfmax %GOVERNOR% %ROM%; nice -n -19 /usr/local/bin/retroarch -L /home/ark/.config/retroarch/cores/makecode_arcade_libretro.so %ROM%; sudo perfnorm</command>
\t\t<platform>makecode</platform>
\t</system>
"""
assert s.count("</systemList>")==1, "unexpected es_systems.cfg structure"
open(p,"w",encoding="utf-8").write(s.replace("</systemList>",block+"</systemList>"))
PY
  fi
  python3 - "$CFG" <<"PYXML"
import sys, xml.etree.ElementTree as ET
print("XML OK, systems:", len(ET.parse(sys.argv[1]).getroot().findall("system")))
PYXML'

log "4. theme icons + gamelist"
$SSH "$REMOTE" 'set -e
  TH=/roms/themes/es-theme-nes-box
  [ -d "$TH" ] || { echo "theme not found: $TH" >&2; exit 1; }
  mkdir -p "$TH/makecode"
  [ -f "$TH/makecode/theme.xml" ] || cp "$TH/wasm4/theme.xml" "$TH/makecode/theme.xml"
  cd /tmp/mca-setup && tar xzf theme.tgz
  cp mca_icons/system.png mca_icons/background_icon.png "$TH/makecode/"
  chmod 777 "$TH/makecode" "$TH/makecode/"*
  python3 gen_gamelist.py /roms/makecode/
  python3 - <<"PYGL"
import xml.etree.ElementTree as ET
print("gamelist games:", len(ET.parse("/roms/makecode/gamelist.xml").getroot().findall("game")))
PYGL'

log "5. golden regression (3 cases)"
OUT=$($SSH "$REMOTE" 'set -e
  cd ~
  cp /tmp/mca-setup/ping.pxt64 /tmp/mca-setup/probe.pxt64 .
  CORE=~/.config/retroarch/cores/makecode_arcade_libretro.so
  export LD_LIBRARY_PATH=$HOME/.config/retroarch/cores:$LD_LIBRARY_PATH
  python3 libretro_host.py $CORE ping.pxt64 300 --data-dir=/tmp/mca-g1 2>&1 | grep FINAL-FRAME-HASH
  python3 libretro_host.py $CORE probe.pxt64 120 --data-dir=/tmp/mca-g2 2>&1 | grep FINAL-FRAME-HASH
  python3 libretro_host.py $CORE probe.pxt64 120 --press=A --release=100000 --data-dir=/tmp/mca-g3 2>&1 | grep FINAL-FRAME-HASH')
echo "$OUT"

FAIL=0
i=0
for want in $GOLDEN; do
  i=$((i+1))
  got=$(echo "$OUT" | sed -n "${i}p" | awk '{print $2}' | cut -c1-8)
  if [ "$got" = "$want" ]; then
    echo "golden $i: PASS ($got)"
  else
    echo "golden $i: FAIL (want $want, got ${got:-none})"
    FAIL=1
  fi
done
[ "$FAIL" -eq 0 ] || die "golden regression failed"

echo
echo "DONE — $REMOTE"
echo "ES restart to pick up icons: sudo systemctl restart emulationstation (or reboot)"
