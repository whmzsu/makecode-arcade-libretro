#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# NX 诊断产物构建：出三个 NRO，用来把 "一启动就崩" 拆开定位
#
#   probe.nro     纯 libnx 控制台程序，打印固件/applet 模式/内存
#   ra_plain.nro  RetroArch 本体（HAVE_STATIC_DUMMY=1，不链我们的核心）
#   <release 里的> makecode_arcade_libretro_libnx.nro  前端+核心
#
# 判定逻辑：
#   probe 崩       -> 工具链/libnx 与固件 ABI 不兼容，与我们的代码无关
#   probe OK / ra_plain 崩 -> RetroArch 前端在新固件上跑不动（官方 1.22.2 的已知问题）
#   两个都 OK，只有我们的 NRO 崩 -> 核心（静态链接/线程/初始化）的问题
#
# 本脚本在 CI 的 devkitpro/devkita64 容器里跑，也可在本地同环境复现。
# ---------------------------------------------------------------------------
set -uo pipefail

DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
PORTLIBS="${PORTLIBS:-$DEVKITPRO/portlibs/switch}"
export DEVKITPRO PORTLIBS
export PATH="$DEVKITPRO/devkitA64/bin:$DEVKITPRO/tools/bin:$PATH"

OUT="${1:-out/nx-diag}"
mkdir -p "$OUT"

echo "==================== 环境 ===================="
echo "DEVKITPRO=$DEVKITPRO"
printf '%-10s %s\n' "gcc:" "$(aarch64-none-elf-gcc -dumpversion 2>/dev/null || echo '?')"
for p in libnx switch-mesa switch-libdrm_nouveau switch-ffmpeg switch-libvpx; do
    v=$(dkp-pacman -Q "$p" 2>/dev/null | awk '{print $2}')
    printf '%-26s %s\n' "$p:" "${v:-<未安装>}"
done
LIBNX_VER=$(dkp-pacman -Q libnx 2>/dev/null | awk '{print $2}')
LIBNX_VER="${LIBNX_VER:-unknown}"
echo "LIBNX_VER=$LIBNX_VER"

# ---------------------------------------------------------------- probe.nro
echo
echo "==================== 构建 probe.nro ===================="
if aarch64-none-elf-gcc \
        -O2 -Wall \
        -march=armv8-a -mtune=cortex-a57 -mtp=soft -fPIE \
        -D__SWITCH__=1 -D_GNU_SOURCE \
        -DBUILD_LIBNX_VER="\"$LIBNX_VER\"" \
        -I"$DEVKITPRO/libnx/include" -I"$PORTLIBS/include" \
        -L"$DEVKITPRO/libnx/lib" -L"$PORTLIBS/lib" \
        -specs="$DEVKITPRO/libnx/switch.specs" \
        tools/nx-probe/probe.c -o "$OUT/probe.elf" -lnx -lm; then
    # elf2nro 不是 target 前缀工具，装在 $DEVKITPRO/tools/bin（有时也以
    # aarch64-none-elf-elf2nro 的形式存在），两种都试
    ELF2NRO=$(command -v elf2nro || command -v aarch64-none-elf-elf2nro || true)
    echo "elf2nro: ${ELF2NRO:-<找不到>}"
    [ -n "$ELF2NRO" ] || { echo "!! 找不到 elf2nro"; exit 1; }
    "$ELF2NRO" "$OUT/probe.elf" "$OUT/probe.nro" && echo "probe.nro OK"
else
    echo "!! probe 编译失败"
    exit 1
fi

# ------------------------------------------------------------- ra_plain.nro
# RetroArch 本体，不链任何核心（Makefile.libnx 支持 HAVE_STATIC_DUMMY=1）。
# 这一步是"尽力而为"的控制组：编不出来不影响 probe 的结论。
echo
echo "==================== 构建 ra_plain.nro（尽力而为） ===================="
RA_REF="${RA_REF:-v1.22.2}"
# 容器里 git 拉 GitHub 偶发中断（实测停在 checkout 87% 就没了），带重试；
# 失败要留下明确记录，不能像以前那样"静默跳过控制组"。
if [ ! -f /tmp/ra/Makefile.libnx ]; then
    rm -rf /tmp/ra
    for i in 1 2 3; do
        echo "  第 $i 次克隆 $RA_REF ..."
        if git clone --depth 1 --branch "$RA_REF" --recursive \
                https://github.com/libretro/RetroArch.git /tmp/ra \
                > "$OUT/ra-clone.log" 2>&1 && [ -f /tmp/ra/Makefile.libnx ]; then
            break
        fi
        echo "  克隆没成功，尾部输出："
        tail -5 "$OUT/ra-clone.log" 2>/dev/null | sed 's/^/    /'
        rm -rf /tmp/ra
        sleep 10
    done
fi
if [ -d /tmp/ra ]; then
    ( cd /tmp/ra && make -f Makefile.libnx -j"$(nproc)" HAVE_STATIC_DUMMY=1 ) \
        >"$OUT/ra_plain-build.log" 2>&1
    if [ -f /tmp/ra/retroarch_switch.nro ]; then
        cp /tmp/ra/retroarch_switch.nro "$OUT/ra_plain.nro"
        echo "ra_plain.nro OK"
    else
        echo "!! ra_plain 没编出来（见 ra_plain-build.log）"
        grep -E 'error:|Error [0-9]|undefined reference' "$OUT/ra_plain-build.log" | head -10
    fi
else
    echo "!! 拉不到 RetroArch 源码，跳过控制组"
fi

# ------------------------------------------------------------------ 收尾
echo
echo "==================== 产物 ===================="
ls -l "$OUT"
[ -f "$OUT/probe.nro" ] || { echo "probe.nro 缺失"; exit 1; }
