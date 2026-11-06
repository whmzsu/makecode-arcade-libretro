#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# NX 静态核心对比构建：把"机制"和"我们的代码"分开
#
#   ra_stubcore.nro  RetroArch + 空核心（tools/nx-probe/stub_core.c）
#   mca_boot.nro     RetroArch + 我们的核心（带开机 trace）
#
# 配合 build-diag.sh 的 ra_plain.nro（RetroArch 本体）形成三级对照：
#
#   ra_plain OK / ra_stubcore 崩  -> 静态核心链接/注册机制本身有问题
#   ra_stubcore OK / mca_boot 崩  -> 问题在我们核心的目标代码
#   mca_boot 只写出前几行 trace    -> 看最后一行就知道死在哪个阶段
#
# mca_boot 的 trace 写在真机上：sdmc:/switch/mca_boot.log
#
# 本脚本在 CI 的 devkitpro/devkita64 容器里跑（需要已装好 portlibs）。
# ---------------------------------------------------------------------------
set -uo pipefail

DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
PORTLIBS="${PORTLIBS:-$DEVKITPRO/portlibs/switch}"
export DEVKITPRO PORTLIBS
export PATH="$DEVKITPRO/devkitA64/bin:$DEVKITPRO/tools/bin:$PATH"

OUT="${1:-out/nx-diag}"
mkdir -p "$OUT"
OUT_ABS=$(cd "$OUT" && pwd)
WS=$(cd "$(dirname "$0")/../.." && pwd)
RA_REF="${RA_REF:-v1.22.2}"
RA_DIR="${RA_DIR:-/tmp/ra-core}"

NX_CFLAGS=(-O2 -fPIE -march=armv8-a -mtune=cortex-a57 -mtp=soft
           -D__SWITCH__ -DHAVE_LIBNX -D_GNU_SOURCE)

# ------------------------------------------------------------- 1. 空核心 .a
echo "==================== 空核心 stub_core.a ===================="
aarch64-none-elf-gcc "${NX_CFLAGS[@]}" -I "$WS/core" \
    -c "$WS/tools/nx-probe/stub_core.c" -o "$OUT_ABS/stub_core.o" || {
    echo "!! stub_core 编译失败"; exit 1; }
aarch64-none-elf-ar rcs "$OUT_ABS/libretro_stub.a" "$OUT_ABS/stub_core.o" || {
    echo "!! stub 归档失败"; exit 1; }
aarch64-none-elf-nm --defined-only -g "$OUT_ABS/libretro_stub.a" | grep -c ' T retro_' \
    | sed 's/^/  stub 导出 retro_* 符号: /'

# --------------------------------------------------------- 2. 我们的核心 .a
echo
echo "==================== 我们的核心 .a ===================="
if ! make -C "$WS/core" platform=libnx -j"$(nproc)" > "$OUT_ABS/core-libnx-build.log" 2>&1; then
    echo "!! 核心静态库编译失败"
    grep -nE 'error:|Error [0-9]|undefined reference' "$OUT_ABS/core-libnx-build.log" | head -15
    exit 1
fi
CORE_A="$WS/core/makecode_arcade_libretro_libnx.a"
[ -f "$CORE_A" ] || { echo "!! 没生成 $CORE_A"; exit 1; }
echo "  核心静态库: $(stat -c %s "$CORE_A") 字节"

# ------------------------------------------------------ 3. RetroArch 静态核心树
# 注意：ra_plain（build-diag.sh）用的是 HAVE_STATIC_DUMMY=1 编的树，那套对象
# 与"真核心"不是同一个配置。这里单独克隆一份，保证两个 NRO 只在 .a 上不同。
echo
echo "==================== 取 RetroArch（静态核心用） ===================="
if [ ! -d "$RA_DIR" ]; then
    git clone --depth 1 --branch "$RA_REF" --recursive \
        https://github.com/libretro/RetroArch.git "$RA_DIR" 2>&1 | tail -2
fi
[ -f "$RA_DIR/Makefile.libnx" ] || { echo "!! 没有 Makefile.libnx"; exit 1; }

link_nro() {   # $1=静态库路径 $2=输出名（不含 .nro）
    local a="$1" name="$2"
    cp "$a" "$RA_DIR/libretro_libnx.a" || return 1
    # 换 .a 必须强制重链，否则 make 认为 NRO 已是最新
    rm -f "$RA_DIR/retroarch_switch.nro" "$RA_DIR/retroarch_switch.elf"
    if ! ( cd "$RA_DIR" && make -f Makefile.libnx -j"$(nproc)" ) \
            > "$OUT_ABS/$name-build.log" 2>&1; then
        echo "!! $name 链接失败"
        grep -nE 'error:|Error [0-9]|undefined reference|cannot find' \
            "$OUT_ABS/$name-build.log" | head -15
        return 1
    fi
    cp "$RA_DIR/retroarch_switch.nro" "$OUT_ABS/$name.nro" || return 1
    # 符号表：真机崩溃报告只给模块内偏移，有了它就能对到函数
    aarch64-none-elf-nm -n "$RA_DIR/retroarch_switch.elf" 2>/dev/null \
        | grep -E ' [TtWw] ' > "$OUT_ABS/$name.symbols.txt"
    echo "  $name.nro: $(stat -c %s "$OUT_ABS/$name.nro") 字节，符号 $(wc -l < "$OUT_ABS/$name.symbols.txt") 条"
    # 断言核心真的进了 ELF（链接成功 ≠ 被链进去）
    if [ -f "$RA_DIR/retroarch_switch.elf" ]; then
        aarch64-none-elf-nm "$RA_DIR/retroarch_switch.elf" \
            | grep -qE ' T retro_load_game$' || echo "  !! ELF 里没有 retro_load_game"
    fi
}

echo
echo "==================== 链 ra_stubcore.nro ===================="
link_nro "$OUT_ABS/libretro_stub.a" ra_stubcore || exit 1

echo
echo "==================== 链 mca_boot.nro ===================="
link_nro "$CORE_A" mca_boot || exit 1

echo
echo "==================== 产物 ===================="
ls -l "$OUT_ABS"
