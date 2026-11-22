#!/usr/bin/env bash
# 从 GitHub Release 下载 CI 发版资产到 work/release/ci/<tag>/。
# 用法: fetch_ci_release.sh core-v0.3.11
set -euo pipefail
source /opt/mca/env.sh

TAG="${1:?usage: fetch_ci_release.sh <release-tag>}"
REPO="${MCA_GH_REPO:-whmzsu/makecode-arcade-libretro}"
API="https://api.github.com/repos/$REPO"
D="$MCA_RELEASE_CI/$TAG"
ASSETS="$D/assets"

mkdir -p "$ASSETS"
curl -fsSL "$API/releases/tags/$TAG" -o "$D/release.json"

python3 - "$D/release.json" > "$D/assets.tsv" <<'PY'
import json, sys
with open(sys.argv[1], encoding="utf-8") as f:
    data = json.load(f)
for a in data.get("assets", []):
    print(f'{a["id"]}\t{a["name"]}')
PY

while IFS=$'\t' read -r id name; do
  [ -n "$id" ] || continue
  echo "download: $name"
  curl -fsSL --retry 3 -H 'Accept: application/octet-stream' \
    -o "$ASSETS/$name" "$API/releases/assets/$id"
done < "$D/assets.tsv"

python3 - "$D/release.json" "$ASSETS" <<'PY'
import json, os, sys
with open(sys.argv[1], encoding="utf-8") as f:
    data = json.load(f)
root = sys.argv[2]
for a in data.get("assets", []):
    p = os.path.join(root, a["name"])
    if not os.path.isfile(p):
        sys.exit(f"missing: {a['name']}")
    size = os.path.getsize(p)
    if size != a["size"]:
        sys.exit(f"size mismatch: {a['name']} local={size} api={a['size']}")
print(f"size check OK: {len(data.get('assets', []))} assets")
PY

( cd "$ASSETS" && sha256sum * > "$D/SHA256SUMS" )
printf 'tag=%s\nrepo=%s\nrelease_api=%s/releases/tags/%s\ndownloaded=%s\nasset_count=%s\n' \
  "$TAG" "$REPO" "$API" "$TAG" "$(date -Is)" "$(wc -l < "$D/assets.tsv")" > "$D/SOURCE.txt"
echo "downloaded $(wc -l < "$D/assets.tsv") assets -> $ASSETS"
