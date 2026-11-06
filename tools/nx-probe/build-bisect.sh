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
# 复用 build-diag.sh 已经克隆的那棵树（省一次网络与磁盘）：
RA_DIR="${RA_DIR:-/tmp/ra}"

NX_CFLAGS=(-O2 -fPIE -march=armv8-a -mtune=cortex-a57 -mtp=soft
           -D__SWITCH__ -DHAVE_LIBNX -D_GNU_SOURCE)

# ------------------------------------------------------------- 1. 空核心 .a
echo "==================== 空核心 stub_core.a ===================="
if ! aarch64-none-elf-gcc "${NX_CFLAGS[@]}" -I "$WS/core" \
        -c "$WS/tools/nx-probe/stub_core.c" -o "$OUT_ABS/stub_core.o"; then
    echo "!! stub_core 编译失败"
    exit 1
fi
if ! aarch64-none-elf-ar rcs "$OUT_ABS/libretro_stub.a" "$OUT_ABS/stub_core.o"; then
    echo "!! stub 归档失败"
    exit 1
fi
echo "  stub 导出 retro_* 符号: $(aarch64-none-elf-nm --defined-only -g "$OUT_ABS/libretro_stub.a" | grep -c ' T retro_')"

# 完整性自检：libretro 的 25 个入口一个都不能少。
# 少了不会在编译期报错，而是等到 5 分钟后的链接才炸（上次就是漏了
# retro_set_controller_port_device / retro_get_memory_{data,size}），
# 且错误信息只给 runloop.c 的行号，很难一眼看出是"桩没写全"。
LIBRETRO_API="retro_api_version retro_get_system_info retro_get_system_av_info
retro_set_environment retro_set_video_refresh retro_set_audio_sample
retro_set_audio_sample_batch retro_set_input_poll retro_set_input_state
retro_set_controller_port_device retro_init retro_deinit retro_reset retro_run
retro_serialize_size retro_serialize retro_unserialize retro_cheat_reset
retro_cheat_set retro_load_game retro_load_game_special retro_unload_game
retro_get_region retro_get_memory_data retro_get_memory_size"
missing=""
for s in $LIBRETRO_API; do
    aarch64-none-elf-nm --defined-only -g "$OUT_ABS/libretro_stub.a" \
        | grep -qE " T $s\$" || missing="$missing $s"
done
if [ -n "$missing" ]; then
    echo "!! 空核心缺少入口:$missing"
    exit 1
fi
echo "  空核心入口完整: $(echo $LIBRETRO_API | wc -w) 个"

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

# ------------------------------------------------------ 3. RetroArch 源码树
#
# 只要一份源码树：两个 NRO 的 RetroArch 目标文件完全相同，只有 libretro_libnx.a
# 不同，所以编一次、换 .a 重链即可（换 .a 会让 make 认为目标过期）。
# 容器里 git 拉 GitHub 偶发中断（实测停在 checkout 87%），所以带重试。
echo
echo "==================== 取 RetroArch 源码 ===================="
if [ ! -f "$RA_DIR/Makefile.libnx" ]; then
    rm -rf "$RA_DIR"
    ok=0
    for i in 1 2 3; do
        echo "  第 $i 次克隆 $RA_REF ..."
        if git clone --depth 1 --branch "$RA_REF" --recursive \
                https://github.com/libretro/RetroArch.git "$RA_DIR" \
                > "$OUT_ABS/ra-clone.log" 2>&1 \
           && [ -f "$RA_DIR/Makefile.libnx" ]; then
            ok=1
            break
        fi
        echo "  克隆没成功，尾部输出："
        tail -5 "$OUT_ABS/ra-clone.log" | sed 's/^/    /'
        rm -rf "$RA_DIR"
        sleep 10
    done
    [ "$ok" = 1 ] || { echo "!! RetroArch 源码取不到（见 ra-clone.log）"; exit 1; }
fi
echo "  源码就绪: $RA_DIR"

# 换 .a 重链：必须删掉旧产物，否则 make 认为已是最新
link_nro() {   # $1=静态库路径 $2=输出名（不含 .nro）
    local a="$1" name="$2"
    cp "$a" "$RA_DIR/libretro_libnx.a" || return 1
    rm -f "$RA_DIR/retroarch_switch.nro" "$RA_DIR/retroarch_switch.elf"
    if ! ( cd "$RA_DIR" && make -f Makefile.libnx -j"$(nproc)" ) \
            > "$OUT_ABS/$name-build.log" 2>&1; then
        echo "!! $name 链接失败"
        grep -nE 'error:|Error [0-9]|undefined reference|cannot find' \
            "$OUT_ABS/$name-build.log" | head -15
        tail -5 "$OUT_ABS/$name-build.log" | sed 's/^/    /'
        return 1
    fi
    cp "$RA_DIR/retroarch_switch.nro" "$OUT_ABS/$name.nro" || return 1
    # 符号表：真机崩溃报告只给模块内偏移，有它就能对到函数
    aarch64-none-elf-nm -n "$RA_DIR/retroarch_switch.elf" 2>/dev/null \
        | grep -E ' [TtWw] ' > "$OUT_ABS/$name.symbols.txt"
    echo "  $name.nro: $(stat -c %s "$OUT_ABS/$name.nro") 字节，符号 $(wc -l < "$OUT_ABS/$name.symbols.txt") 条"
    # 链接成功 ≠ 核心被链进去：用只属于核心的符号做实证
    if ! aarch64-none-elf-nm "$RA_DIR/retroarch_switch.elf" | grep -qE ' T retro_load_game$'; then
        echo "  !! ELF 里没有 retro_load_game（归档成员被丢掉了）"
        return 1
    fi
}

# ra_plain 那棵树的对象是 HAVE_STATIC_DUMMY=1 编的，配置不同不能复用：
# 清掉对象重编一次（两个 NRO 共用这一次编译，只有 .a 不同）。
echo
echo "==================== 清理 RetroArch 目标文件 ===================="
( cd "$RA_DIR" && rm -rf obj && find . -name '*.o' -delete \
    && rm -f retroarch_switch.nro retroarch_switch.elf ) || {
    echo "!! 清理失败"; exit 1; }
echo "  已清理"

echo
echo "==================== 链 ra_stubcore.nro ===================="
link_nro "$OUT_ABS/libretro_stub.a" ra_stubcore
rc_stub=$?

echo
echo "==================== 链 mca_boot.nro ===================="
link_nro "$CORE_A" mca_boot
rc_boot=$?

echo
echo "==================== 产物 ===================="
ls -l "$OUT_ABS"

# 两个链接各自独立收尾：一个失败不要连带另一个也不产出，
# 否则每次都要再等一轮五分钟的 CI 才能拿到另一个对照。
if [ "$rc_stub" -ne 0 ] || [ "$rc_boot" -ne 0 ]; then
    echo "!! 失败：ra_stubcore=$rc_stub mca_boot=$rc_boot"
    exit 1
fi
