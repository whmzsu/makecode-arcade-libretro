#!/usr/bin/env bash
# fuzz_pxt.sh — 用 clang libFuzzer 喂 pxt64 镜像的解析/校验层
#
# 为什么：契约测试只喂"几种坏输入"，golden 只喂"能跑的游戏"。真正能系统性翻出
# 越界/UB 的是 fuzz——今天 PATCH 11（少分配 7 字节、每张卡带都踩）就是 ASAN 在
# loadIfaceNames 里抓到的，fuzz 能让这类问题在发布前自己浮出来。
#
# 用法：
#   tools/fuzz_pxt.sh                 # 编 + 跑 120 秒
#   tools/fuzz_pxt.sh -max_total_time=600
#   tools/fuzz_pxt.sh --rebuild       # 只重编，不跑
#   SEEDS=/path/to/carts tools/fuzz_pxt.sh
#
# 前提：clang++（带 libFuzzer，工具机 clang 18 已装）。x86_64 上跑，不需要 qemu。
# 崩溃样本：out/fuzz/crash-*、out/fuzz/timeout-*，日志 out/fuzz/fuzz.log
# 复现单例：out/fuzz/fuzz_image out/fuzz/crash-xxx

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ="$(cd "$HERE/.." && pwd)"
OUT="$PROJ/out/fuzz"
CXX="${CXX:-clang++}"

REBUILD=0
RUN_ARGS=(-max_total_time=120)
for a in "$@"; do
  case "$a" in
    --rebuild) REBUILD=1 ;;
    *)         RUN_ARGS=("$a") ;;
  esac
done

command -v "$CXX" >/dev/null 2>&1 || { echo "缺 $CXX（工具机 clang 18）" >&2; exit 2; }

# shellcheck source=tools/lib/sources.sh
source "$PROJ/tools/lib/sources.sh"
export MCA_SOURCES_MK="$PROJ/core/sources.mk"
CORE="$PROJ/core"
PXTAPP="$PROJ/runtime/pxtapp"

mkdir -p "$OUT"
BIN="$OUT/fuzz_image"

if [ "$REBUILD" = 1 ] || [ ! -x "$BIN" ]; then
  echo "== 编译 fuzz harness（clang + ASAN + libFuzzer）"
  mapfile -t SRC < <(mca_vm_sources; mca_core_sources)
  echo "   ${#SRC[@]} 个源文件"
  # 不用 UBSan：pxt 里有大量刻意的整数回绕/指针算术，UBSan 噪声会盖住真问题
  "$CXX" -g -O1 -fsanitize=fuzzer,address -fno-omit-frame-pointer \
    -fno-rtti -fno-exceptions -fno-threadsafe-statics -fwrapv -fno-strict-aliasing \
    -I"$CORE" -I"$CORE/src" -I"$PXTAPP" \
    "${SRC[@]}" "$CORE/tests/fuzz_image.cpp" -lpthread -o "$BIN"
fi

[ "$REBUILD" = 1 ] && { echo "   只编译：$BIN"; exit 0; }

# 种子：两个夹具 + 可选 SEEDS_DIR（默认 out/testcarts，有就用，没有也不挡）
SEED_DIR="$OUT/seeds"
mkdir -p "$SEED_DIR"
for f in "$PROJ"/tools/fixtures/*.pxt64 ${SEEDS_DIR:+"$SEEDS_DIR"/*.pxt64}; do
  [ -f "$f" ] && cp -n "$f" "$SEED_DIR/" 2>/dev/null || true
done

echo "== 跑 fuzz（种子 $(ls "$SEED_DIR"/*.pxt64 2>/dev/null | wc -l) 个）"
# alloc_dealloc_mismatch=0：pxt 全局重载了 operator new → xmalloc(calloc)、
# operator delete → xfree(free)。ASAN 的 new 拦截器会抢在自定义实现之前记账，
# 于是把 "operator new" 和 xfree 配成一对报 mismatch —— 生产构建里两者都是
# malloc 家族，不存在这个问题。不开这个开关，fuzz 第一秒就会停在这里。
# -rss_limit_mb：VM 镜像+GC 堆不小，给足；-timeout：单次输入 10s，超了就是可疑卡死
(cd "$OUT" && ASAN_OPTIONS="alloc_dealloc_mismatch=0:detect_leaks=0" \
  "$BIN" "${RUN_ARGS[@]}" -timeout=10 -rss_limit_mb=3072 "$SEED_DIR") 2>&1 | tee "$OUT/fuzz.log"

echo
if ls "$OUT"/crash-* >/dev/null 2>&1; then
  echo "!! 有崩溃样本：" >&2
  ls -1 "$OUT"/crash-* >&2
  echo "   复现：$BIN <样本>" >&2
  exit 1
fi
echo "无崩溃。语料已写入 $SEED_DIR（下次启动更快）"
