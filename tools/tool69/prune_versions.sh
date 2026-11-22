#!/usr/bin/env bash
# 按版本保留策略归档旧目录：
#   release/local  完整标记 BUILD-INFO.txt              默认保留 5
#   release/ci     完整标记 SOURCE.txt + SHA256SUMS      默认保留 5
#   build/test     完整标记 BUILD-INFO.txt              默认保留 3
# 不完整目录只告警，不自动归档。用法：prune_versions.sh [--dry-run]
set -euo pipefail
source /opt/mca/env.sh

DRY_RUN=0
[[ "${1:-}" == "--dry-run" ]] && DRY_RUN=1
STAMP="$(date +%Y%m%d-%H%M%S)"
DAY="$(date +%Y%m%d)"
MANIFEST="$MCA_ARCHIVE/$DAY/MANIFEST.md"
HEADER_WRITTEN=0

ensure_header() {
  if [[ "$DRY_RUN" == "1" || "$HEADER_WRITTEN" == "1" ]]; then
    return
  fi
  mkdir -p "$(dirname "$MANIFEST")"
  if [[ ! -f "$MANIFEST" ]]; then
    printf '# %s 归档清单\n\n' "$DAY" > "$MANIFEST"
  fi
  printf '\n## 版本保留归档（%s）\n| 归档名 | 原路径 | 说明 |\n|---|---|---|\n' "$STAMP" >> "$MANIFEST"
  HEADER_WRITTEN=1
}

archive_one() {
  local src="$1" group="$2" version="$3" keep="$4"
  local dest_root="$MCA_ARCHIVE/$DAY/$group"
  local dest="$dest_root/$version"
  if [[ -e "$dest" ]]; then
    dest="$dest_root/${version}-$STAMP"
  fi
  if [[ "$DRY_RUN" == "1" ]]; then
    echo "DRY-RUN: mv $src -> $dest"
    return
  fi
  ensure_header
  mkdir -p "$dest_root"
  mv "$src" "$dest"
  printf '| `%s/%s` | `%s` | 超过保留上限 %s 个完整版本 |\n' "$group" "$version" "$src" "$keep" >> "$MANIFEST"
  echo "ARCHIVED: $src -> $dest"
}

prune() {
  local group="$1" root="$2" keep="$3"
  shift 3
  local -a markers=("$@")
  local -a dirs=()
  local -a complete=()

  if [[ ! -d "$root" ]]; then
    echo "SKIP: $root 不存在"
    return 0
  fi

  while IFS= read -r d; do
    dirs+=("$d")
  done < <(find "$root" -mindepth 1 -maxdepth 1 -type d -printf '%f\n' | sort -V)

  local d m ok
  for d in "${dirs[@]}"; do
    ok=1
    for m in "${markers[@]}"; do
      if [[ ! -f "$root/$d/$m" ]]; then
        ok=0
        break
      fi
    done
    if [[ "$ok" == "1" ]]; then
      complete+=("$d")
    else
      echo "INCOMPLETE(skip): $group/$d 缺少标记 ${markers[*]}"
    fi
  done

  local n=${#complete[@]}
  echo "INFO: $group 完整版本 $n 个，保留上限 $keep"
  if (( n <= keep )); then
    return 0
  fi

  local i
  for (( i=0; i<n-keep; i++ )); do
    archive_one "$root/${complete[$i]}" "$group" "${complete[$i]}" "$keep"
  done
}

prune "release/local" "$MCA_RELEASE_LOCAL" "${MCA_KEEP_RELEASE:-5}" BUILD-INFO.txt
prune "release/ci"   "$MCA_RELEASE_CI"   "${MCA_KEEP_RELEASE:-5}" SOURCE.txt SHA256SUMS
prune "build/test"   "$MCA_BUILD_TEST"   "${MCA_KEEP_TEST:-3}"    BUILD-INFO.txt
