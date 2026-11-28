#!/usr/bin/env bash
# armhf_smoke.sh —— 32 位 Linux ARM（armhf）运行时冒烟，在 tool69 上运行
#
# 补齐 32 位探针唯一没有脚本的测试路径：docs/PXT32-A-SCHEME.md §7.1/§8.3
# 当年是手工命令实测，本脚本把它固化成可重跑的回归。
#
# 用法（在 tool69 上）：
#   source /opt/mca/env.sh
#   $MCA_BIN/armhf_smoke.sh                # 用现有 core/dist 里的 armhf core
#   $MCA_BIN/armhf_smoke.sh --build-core   # 先用 build_target.sh 重编 armhf core
#   $MCA_BIN/armhf_smoke.sh --core /path/to/core.so
#
# 六例：golden 三例（ping300 / probe120 / probe120+A@4）
#       + --reject-rgb565 三例（同卡带同帧数，模拟只接受 0RGB1555 的前端）。
# 期望哈希来自 2026-11-16 实测，与 docs/PXT32-A-SCHEME.md §7.1/§8.3 一致；
# 改核心、改宿主参数、改帧数后要同步更新下面的 EXPECTED 表。
#
# 注意：core 是 32 位 ELF，CI 的 64 位 Python 宿主载不了，只能在这里用
# zig 编的 32 位 C 宿主 + qemu-arm-static 跑（不是 ARM32 真机）。
# 退出码：0 全一致 / 1 有差异或缺哈希 / 2 环境问题

set -uo pipefail
source /opt/mca/env.sh

REPO=$MCA_SRC
ZIG=${ZIG:-$MCA_TOOLCHAIN/bin/zig}
QEMU=${QEMU:-qemu-arm-static}
SYSROOT=$MCA_SYSROOT_ARMHF
TRIPLE=arm-linux-gnueabihf.2.17
CORE=$REPO/core/dist/$TRIPLE/makecode_arcade_libretro.so
OUT=$REPO/out/armhf_smoke
HOST=$OUT/mca_host_armhf

BUILD_CORE=0
while [ $# -gt 0 ]; do
  case "$1" in
    --build-core) BUILD_CORE=1; shift ;;
    --core)       CORE="$2"; shift 2 ;;
    -h|--help)    sed -n '2,22p' "${BASH_SOURCE[0]}"; exit 0 ;;
    *)            echo "未知参数: $1" >&2; exit 2 ;;
  esac
done

fail() { echo "ERROR: $*" >&2; exit 2; }
[ -x "$QEMU" ] || command -v "$QEMU" >/dev/null 2>&1 || fail "找不到 $QEMU"
[ -x "$ZIG" ] || fail "找不到 zig: $ZIG"
[ -f "$SYSROOT/lib/ld-linux-armhf.so.3" ] || fail "sysroot 缺 loader: $SYSROOT/lib/ld-linux-armhf.so.3"
[ -f "$REPO/tools/mca_host_android.c" ] || fail "找不到 tools/mca_host_android.c"
for c in ping probe; do
  [ -f "$REPO/tools/fixtures/$c.pxt64" ] || fail "缺卡带 tools/fixtures/$c.pxt64"
done
mkdir -p "$OUT" || fail "建不了 $OUT"

# ---- core：默认复用构建产物，--build-core 时用统一构建脚本重编 ----
if [ "$BUILD_CORE" = 1 ]; then
  echo "=== 重编 armhf core ($TRIPLE) ==="
  ( cd "$REPO" && ZIG="$ZIG" "$REPO/tools/build_target.sh" "$TRIPLE" ) || fail "build_target.sh 失败"
fi
[ -f "$CORE" ] || fail "缺 core: $CORE（加 --build-core 重编）"

# ---- 宿主：每次重编，避免测到旧 host ----
echo "=== 编译 32 位 C 宿主 ==="
"$ZIG" cc "$REPO/tools/mca_host_android.c" -I"$REPO/core" \
  -target "$TRIPLE" -O2 -o "$HOST" -ldl || fail "编译宿主失败"
file "$HOST" | sed 's/^/  /'

# 用例：标签|卡带|帧数|额外宿主参数|期望哈希（前 8 位即可，完整 32 位也写全）
CASES=(
  "ping300|ping.pxt64|300|-|0556e78f7ce0e828f2dc922d210dc047"
  "probe120|probe.pxt64|120|-|e82844f7160449d1767aa69afe788800"
  "probe120-A|probe.pxt64|120|--tap=A@4 --tap-dur=100000|0eb7884cb40cef0cb549946af3a942ab"
  "reject565-ping300|ping.pxt64|300|--reject-rgb565|aec6c1c80bc75cdcab7a26b527b88136"
  "reject565-probe120|probe.pxt64|120|--reject-rgb565|af451361f744bd6e9763e921352d8fee"
  "reject565-probe120-A|probe.pxt64|120|--reject-rgb565 --tap=A@4 --tap-dur=100000|181cc2bb2313931d367e3bf0553246b7"
)

printf '%-24s %-34s %-34s %s\n' 用例 实测 期望 结果
printf '%s\n' "-----------------------------------------------------------------------------------------------"
pass=0; failed=0
i=0
for spec in "${CASES[@]}"; do
  IFS='|' read -r tag cart frames extra want <<< "$spec"
  i=$((i + 1))
  args=( "$HOST" "$CORE" "$REPO/tools/fixtures/$cart" "$frames" "--data-dir=$OUT/data-$tag" )
  [ "$extra" != "-" ] && read -r -a xtra <<< "$extra" && args+=( "${xtra[@]}" )
  log="$OUT/$tag.log"
  "$QEMU" -L "$SYSROOT" "${args[@]}" > "$log" 2>&1
  rc=$?
  got=$(grep -o 'FINAL-FRAME-HASH [0-9a-f]\{32\}' "$log" | head -1 | awk '{print $2}')
  if [ "$rc" -eq 0 ] && [ "$got" = "$want" ]; then
    result=PASS; pass=$((pass + 1))
  else
    result=FAIL; failed=$((failed + 1))
  fi
  printf '%-24s %-34s %-34s %s\n' "$tag" "${got:-MISSING(rc=$rc)}" "${want:0:8}…" "$result"
done
printf '%s\n' "-----------------------------------------------------------------------------------------------"
echo "合计 $pass PASS / $failed FAIL；日志与宿主在 $OUT"
[ "$failed" -eq 0 ]
