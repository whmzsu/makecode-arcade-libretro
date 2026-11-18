#!/bin/sh
set -e
ROOT=$(pwd)
P=$ROOT/out/pr-patches
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

check () { # $1=name  $2=patch
  echo "=== $1"
  ( cd "$T/$1" && git init -q . && git add -A && git -c user.email=a@b -c user.name=a commit -qm base && \
    git apply --check --verbose "$2" && echo "  apply --check OK" && git apply "$2" && echo "  已应用" ) || echo "  !!! 失败"
}

# muOS
mkdir -p "$T/muos/data" "$T/muos/manifest/core/Game Engine" "$T/muos/system"
cp out/pr-work/muos-core.json "$T/muos/data/core.json"
check muos "$P/01-muos-extra/0001-core-makecode-arcade.patch"
echo "--- 结果文件数: $(find "$T/muos" -type f -not -path '*/.git/*' | wc -l)"
echo "--- core.json 条目可解析:"
python -c "import json;d=json.load(open(r'$T/muos/data/core.json'));print(json.dumps(d['makecode-arcade'],ensure_ascii=False))" 2>&1 | head -3

# Knulli
mkdir -p "$T/knulli/package/system/knulli-system" "$T/knulli/package/emulationstation/knulli-es-system"
cp out/pr-work/kn-Config.in "$T/knulli/Config.in"
cp out/pr-work/kn-knulli-system-Config.in "$T/knulli/package/system/knulli-system/Config.in"
cp out/pr-work/kn-es_systems.yml "$T/knulli/package/emulationstation/knulli-es-system/es_systems.yml"
check knulli "$P/02-knulli/0001-libretro-makecode-arcade.patch"
echo "--- 新增文件:"; find "$T/knulli/package/cores" -type f 2>/dev/null

# ArkOS / dArkOS
for pair in "arkos:03-arkos-retroarch-cores" "darkos:04-darkos-retroarch-cores"; do
  tag=${pair%%:*}; d=${pair##*:}
  mkdir -p "$T/$tag/aarch64"
  cp "out/pr-patches/orig/$tag/aarch64-.index" "$T/$tag/aarch64/.index"
  cp "out/pr-patches/orig/$tag/aarch64-.index-extended" "$T/$tag/aarch64/.index-extended"
  check "$tag" "$P/$d/0001-index-makecode-arcade.patch"
  echo "--- 末尾两行:"; tail -1 "$T/$tag/aarch64/.index"; tail -1 "$T/$tag/aarch64/.index-extended"
done
