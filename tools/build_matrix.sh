#!/usr/bin/env bash
# build_matrix.sh — 交叉编译矩阵
#
# 档位：
#   ./build_matrix.sh            # 主流目标（发版用，5 个：Win x64/arm64、Linux x86_64/aarch64、Android arm64）
#   ./build_matrix.sh --all      # 全架构（验证用，含 riscv/loongarch/ppc/s390x/musl/macOS/大端…，均 64 位）
#   ./build_matrix.sh <triple>…  # 只跑指定目标
#
# 附加参数：
#   --prune                      # 删掉不在本次目标列表里的 core/dist/<t> 与 core/build/<t>
#                                # （跑完 --all 再回来打正式包时用；release/ 与 native 缓存不受影响）
#
# 结果：core/dist/<triple>/makecode_arcade_libretro.{so,dll,dylib}
#       每个目标的编译日志 core/dist/<triple>/build.log
#       汇总表：core/dist/MATRIX.md

set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
PROJ="$(cd "$HERE/.." && pwd)"
DIST="$PROJ/core/dist"

# —— 主流目标：发版矩阵 ——
# Linux glibc 基线钉在 2.17（覆盖 2012 年后所有掌机固件；默认基线会到 2.29~2.34）
# Android API 21（RetroArch Android 最低支持；NDK r27 的下限即 21）
MAIN_TARGETS=(
  # Windows
  x86_64-windows-gnu
  aarch64-windows-gnu
  # Linux
  x86_64-linux-gnu.2.17
  aarch64-linux-gnu.2.17
  # Android
  aarch64-linux-android21
)

# 32 位目标（armhf / armv7 / x86 / i686-android…）**一律不构建**：
# 卡带镜像是 PXT64，官方 pxt 编译器没有 32 位镜像输出，runtime 的 PXT32
# 镜像补丁是上游未实现的 TODO（verify.cpp: "TODO: NumberBoxes"），所以
# 32 位核心能装能识别但跑不了任何游戏。详见 README「只支持 64 位前端」。

# —— 扩展目标：--all 时追加 ——
EXTRA_TARGETS=(
  # musl 发行版（Alpine / 部分静态固件）
  aarch64-linux-musl
  x86_64-linux-musl
  # 其它 64 位架构（社区向）
  riscv64-linux-gnu
  loongarch64-linux-gnu
  powerpc64le-linux-gnu
  mips64-linux-gnuabi64
  mips64el-linux-gnuabi64
  s390x-linux-gnu
  # 大端
  aarch64_be-linux-gnu
  # macOS
  aarch64-macos
  x86_64-macos
  # Android 其它 64 位 ABI
  x86_64-linux-android21
)

# 已知不可行（记录在案，见 docs/CROSS-COMPILE.md）：
#   mips/mipsel-linux-*   zig 0.17 未提供 32 位 MIPS 的 glibc/musl math.h
#   m68k-linux-gnu        无 LLVM 后端
#   sparc64-linux-gnu     zig/LLVM 的 SPARC 后端编译期崩溃

# 参数解析：--all / --prune 为开关，其余视为目标三元组
ALL=0; PRUNE=0; ARGS=()
for a in "$@"; do
  case "$a" in
    --all)   ALL=1 ;;
    --prune) PRUNE=1 ;;
    -h|--help) sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *)       ARGS+=("$a") ;;
  esac
done

if [ "$ALL" = 1 ]; then
  TARGETS=("${MAIN_TARGETS[@]}" "${EXTRA_TARGETS[@]}")
elif [ "${#ARGS[@]}" -gt 0 ]; then
  TARGETS=("${ARGS[@]}")
else
  TARGETS=("${MAIN_TARGETS[@]}")
fi

# --prune：清掉不在本次目标列表里的产物目录（只动产物，release/ 与 native 的 build/vm_objs 保留）
if [ "$PRUNE" = 1 ]; then
  in_list() { local x="$1" t; for t in "${TARGETS[@]}"; do [ "$x" = "$t" ] && return 0; done; return 1; }
  for d in "$DIST"/*/ "$PROJ/core/build"/*/; do
    [ -d "$d" ] || continue
    n="$(basename "$d")"
    if [ "$n" = "release" ] || [ "$n" = "vm_objs" ]; then continue; fi
    if ! in_list "$n"; then
      rm -rf "$d"
      echo "prune  ${d#$PROJ/}"
    fi
  done
fi

mkdir -p "$DIST"
SUMMARY="$DIST/MATRIX.md"
: > "$SUMMARY"
{
  echo "# 交叉编译矩阵"
  echo
  echo "> 由 tools/build_matrix.sh 自动生成。zig $( "$PROJ/../common tools/zig/zig.exe" version 2>/dev/null || echo "?" )；Android 走 NDK clang"
  echo
  echo "| 目标三元组 | 结果 | 大小 | 耗时 |"
  echo "|---|---|---|---|"
} >> "$SUMMARY"

fail=0; ok=0
ok_dirs=()
for t in "${TARGETS[@]}"; do
  log="$DIST/$t/build.log"
  mkdir -p "$DIST/$t"
  start=$(date +%s)
  if "$HERE/build_target.sh" "$t" > "$log" 2>&1; then
    size=$(stat -c%s "$DIST/$t"/makecode_arcade_libretro.* 2>/dev/null | head -1)
    dur=$(( $(date +%s) - start ))
    ok_dirs+=("$DIST/$t/makecode_arcade_libretro."*)
    printf '%-32s OK   %8s bytes  %ss\n' "$t" "$size" "$dur"
    echo "| \`$t\` | ✅ | $((size / 1024)) KB | ${dur}s |" >> "$SUMMARY"
    ok=$((ok + 1))
  else
    dur=$(( $(date +%s) - start ))
    rm -f "$DIST/$t"/makecode_arcade_libretro.*   # 残缺产物会误导自检，直接清掉
    err=$(grep -m1 -E "error:" "$log" | sed 's/.*error: //' | cut -c1-60)
    printf '%-32s FAIL %ss  %s\n' "$t" "$dur" "$err"
    echo "| \`$t\` | ❌ ${err:-见 build.log} | — | ${dur}s |" >> "$SUMMARY"
    fail=$((fail + 1))
  fi
done

echo
echo "完成：成功 $ok / 失败 $fail"
echo "产物：$DIST"

# 追加二进制自检（架构 / glibc 基线 / 动态依赖 / retro_* 导出）
{
  echo
  echo "## 二进制自检"
  echo
  echo '```'
  python "$HERE/inspect_binary.py" ${ok_dirs[@]+"${ok_dirs[@]}"} 2>&1 || true
  echo '```'
} >> "$SUMMARY"

[ "$fail" -eq 0 ]
