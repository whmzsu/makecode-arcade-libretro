#!/usr/bin/env bash
# macos_verify.sh — 发版 dylib 在 macOS runner 上的加载 + 像素回归
#
# 由 .github/workflows/build.yml 的 macos-verify job 调用（两条 runner：
# Apple Silicon arm64 / Intel x64）。做三件事：
#   1. ci_regress.sh 跑 golden 三例，与共享基线逐位比对
#   2. --reinit=2 三轮加载，确认 PATCH 20 的像素格式协商每轮都重做
#   3. 失败时留末帧 BMP 供排查
#
# 断言逻辑刻意独立成脚本而非写在 run: | 里：GitHub 把块标量交给 runner 的
# /bin/bash（macOS 是 3.2），中文注解紧贴 ${var} 时在该 shell 上出现过
# "unbound variable" 的解析异常；脚本用 UTF-8 落盘、bash 直接读文件无此问题。
#
# 用法：macos_verify.sh <dylib>
# 退出码：0 全通过 / 1 有断言失败 / 2 环境问题

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ="$(cd "${HERE}/.." && pwd)"

CORE="${1:-}"
[ -n "${CORE}" ] || { echo "用法: $0 <dylib>" >&2; exit 2; }
[ -f "${CORE}" ] || { echo "找不到核心: ${CORE}" >&2; exit 2; }

PY=""
for c in python3 python; do
  if command -v "${c}" >/dev/null 2>&1; then PY="${c}"; break; fi
done
[ -n "${PY}" ] || { echo "找不到 python" >&2; exit 2; }

out="${PROJ}/out/macos_verify"
rm -rf "${out}"
mkdir -p "${out}/bmp"

fail=0

# ---- 1. golden 三例 -------------------------------------------------------
echo "== golden 三例末帧哈希 =="
if ! "${PROJ}/tools/ci_regress.sh" "${CORE}" --bmp-dir "${out}/bmp"; then
  echo "::error::macOS golden 三例比对失败"
  fail=1
fi

# ---- 2. --reinit 三轮 -----------------------------------------------------
echo "== 二次加载 --reinit=2（PATCH 20 真机回归）=="
"${PY}" "${PROJ}/tools/libretro_host.py" "${CORE}" \
  "${PROJ}/tools/fixtures/ping.pxt64" 300 --turbo --reinit=2 \
  --data-dir="${out}/reinit" 2>&1 | tee "${out}/reinit.log" >/dev/null
rc=${PIPESTATUS[0]}
if [ "${rc}" -ne 0 ]; then
  echo "::error::reinit 宿主进程异常退出 rc=${rc}"
  fail=1
fi

got=$(grep -oE '(FINAL|REOPEN)-FRAME-HASH [0-9a-f]{32}' "${out}/reinit.log" \
      | awk '{print $2}' || true)
n=$(printf '%s\n' "${got}" | grep -c . || true)
uniq_n=$(printf '%s\n' "${got}" | sort -u | wc -l | tr -d ' ')
first=$(printf '%s\n' "${got}" | head -1)
want=$(awk -F'|' '$1 ~ /ping\.pxt64/ {print $4}' \
       "${PROJ}/tools/fixtures/golden-frames.txt" | tr -d '\r')
nfmt=$(grep -c 'SET_PIXEL_FORMAT = 2' "${out}/reinit.log" || true)

echo "三轮: n=${n} uniq=${uniq_n} nfmt=${nfmt}"
if [ "${n}" -ne 3 ]; then
  echo "::error::期望 3 轮哈希，实际 ${n} 轮"
  fail=1
elif [ "${uniq_n}" -ne 1 ]; then
  echo "::error::三轮末帧不一致: $(printf '%s' "${got}" | tr '\n' ' ')"
  fail=1
elif [ "${first}" != "${want}" ]; then
  echo "::error::重开哈希 ${first} 与基线 ${want} 不符"
  fail=1
elif [ "${nfmt}" -lt 6 ]; then
  echo "::error::像素格式只协商 ${nfmt} 次（应为 6）"
  fail=1
else
  echo "三轮重开一致 ${first}；SET_PIXEL_FORMAT 共 ${nfmt} 次"
fi

[ "${fail}" -eq 0 ] || exit 1
exit 0
