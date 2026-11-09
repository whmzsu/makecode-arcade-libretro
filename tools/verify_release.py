#!/usr/bin/env python3
"""发行包「装一遍再发」自动体检 —— 在 Linux（binutils）上跑。

用法: tools/verify_release.py <release 目录> <libretro_core.cpp 路径>

检查两类包：

主包 makecode_arcade_<platform>.zip
  * 核心二进制按扩展名显式挑选，包里绝不能有 .pdb 之类调试符号；
  * ELF/PE 机器类型与包名平台一致（血泪：x86 包混入 arm64、Win 包只有 .pdb）；
  * .info 的 display_version == 源码 kCoreVersion；
  * ELF 动态依赖白名单、glibc 基线、retro_* 导出数量；
  * Android 无 libc++_shared、LOAD 段 16KB 对齐；
  * **ELF 必须已 strip**（无 .debug_info）—— 0.3.7 之前 linux/arm64 带着
    4.3 MB 调试信息发给玩家，zip 从 287 KB 涨到 1.38 MB，固件仓库也拒收。

调试包 makecode_arcade_<platform>-debuginfo.zip
  * 必须真的带 .debug_info（否则等于发了个空壳）；
  * build-id 必须与同平台主包一致（这是"符号对得上二进制"的唯一凭据）；
  * 只对需要 strip 的平台存在：linux-* / lakka-*（Android 的 NDK 打包时
    已经 strip，没有符号可留；Windows PE 本来就不带符号表）。

输出 markdown 报告；有 FAIL 时退出码 1。
"""
from __future__ import annotations

import re
import struct
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent

# zip 平台短名 -> (期望格式, 机器类型)
# ELF e_machine: 62=x86_64, 183=aarch64；PE Machine: 0x8664=x64, 0xaa64=ARM64
EXPECT = {
    "windows-x64":   ("PE", 0x8664),
    "windows-arm64": ("PE", 0xAA64),
    "linux-x64":     ("ELF", 62),
    "linux-arm64":   ("ELF", 183),
    "android-arm64": ("ELF", 183),
    "lakka-arm64":   ("ELF", 183),
}
# 需要 strip（因而需要 -debuginfo 包）的平台前缀
NEED_DEBUGINFO = ("linux-", "lakka-")

# glibc 带 .so.6 后缀；Android bionic 就是 libc.so / libm.so / libdl.so（无版本号）
LIB_OK = re.compile(
    r"^(libc|libm|libdl|libpthread|librt|libstdc\+\+|libgcc_s|libatomic)"
    r"\.so(\..*)?$|^ld-linux.*\.so(\..*)?$")


def sh(cmd: list[str]) -> str:
    p = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
    return p.stdout + p.stderr


def machine_of(data: bytes) -> tuple[str, int]:
    if data[:2] == b"MZ":
        off = struct.unpack_from("<I", data, 0x3C)[0]
        if data[off:off + 4] == b"PE\0\0":
            return "PE", struct.unpack_from("<H", data, off + 4)[0]
        return "PE?", 0
    if data[:4] == b"\x7fELF":
        return "ELF", struct.unpack_from("<H", data, 18)[0]
    return "?", 0


def has_debug_info(path: str) -> bool:
    return ".debug_info" in sh(["readelf", "-S", path])


def build_id(path: str) -> str:
    m = re.search(r"Build ID:\s*([0-9a-fA-F]+)", sh(["readelf", "-n", path]))
    return m.group(1) if m else ""


def core_name(names: list[str]) -> str | None:
    for e in ("dll", "so", "dylib"):
        n = f"makecode_arcade_libretro.{e}"
        if n in names:
            return n
    return None


def plat_of(zp: Path, debuginfo: bool = False) -> str:
    p = zp.stem.replace("makecode_arcade_", "")
    if debuginfo:
        p = p[:-len("-debuginfo")]
    return p


def main() -> int:
    rel = Path(sys.argv[1])
    core_cpp = Path(sys.argv[2])
    m = re.search(r'kCoreVersion\s*=\s*"([^"]+)"', core_cpp.read_text(encoding="utf8"))
    version = m.group(1) if m else "?"

    zips = sorted(rel.glob("*.zip"))
    main_zips = [z for z in zips if not z.stem.endswith("-debuginfo")]
    dbg_zips = [z for z in zips if z.stem.endswith("-debuginfo")]

    lines = ["# 发行包自动体检", "",
             f"版本（源码 kCoreVersion）：`{version}`；"
             f"主包 {len(main_zips)} 个，调试包 {len(dbg_zips)} 个", "",
             "| 包 | 核心二进制 | 架构 | .info 版本 | 依赖/基线 | 导出 | 判定 |",
             "|---|---|---|---|---|---|---|"]
    all_ok = bool(main_zips)
    main_bid: dict[str, str] = {}
    main_size: dict[str, int] = {}

    for zp in main_zips:
        plat = plat_of(zp)
        exp = EXPECT.get(plat)
        with zipfile.ZipFile(zp) as z:
            names = z.namelist()
            bad = [n for n in names if n.lower().endswith((".pdb", ".lib", ".exp", ".idb"))]
            core = core_name(names)
            info = next((n for n in names if n.endswith(".info")), None)
            info_ver = "-"
            if info:
                im = re.search(r"display_version\s*=\s*\"([^\"]+)\"",
                               z.read(info).decode("utf8", "replace"))
                info_ver = im.group(1) if im else "?"
            if core is None:
                lines.append(f"| {plat} | **缺失** | - | {info_ver} | - | - | FAIL |")
                all_ok = False
                continue
            data = z.read(core)
            main_size[plat] = len(data)
            with tempfile.NamedTemporaryFile(suffix="." + core.rsplit(".", 1)[1]) as f:
                f.write(data)
                f.flush()
                fmt, mach = machine_of(data)
                arch_ok = bool(exp) and fmt == exp[0] and mach == exp[1]
                dep, exports, notes = "-", "-", []
                if fmt == "ELF":
                    out = sh(["readelf", "-d", f.name])
                    needed = re.findall(r"\(NEEDED\).*?\[(.*?)\]", out)
                    bad_libs = [x for x in needed if not LIB_OK.match(x)]
                    gv = re.findall(r"GLIBC_([0-9]+(?:\.[0-9]+)*)",
                                    sh(["readelf", "-V", f.name]))
                    glibc = sorted(set(gv), key=lambda s: tuple(map(int, s.split("."))))
                    top = "GLIBC_" + glibc[-1] if glibc else "-"
                    dep = f"NEEDED {len(needed)} 项 / GLIBC {top}"
                    if bad_libs:
                        notes.append("意外依赖 " + ",".join(bad_libs))
                    if "libc++_shared" in needed:
                        notes.append("依赖 libc++_shared")
                    if plat.startswith("linux-") and glibc:
                        # glibc[-1] 是纯版本号（"2.34"）；别对 "GLIBC_" 前缀做切片，
                        # 前缀长度是 6 不是 7，切错会变成 ".34" -> int('') 崩。
                        ver = tuple(map(int, glibc[-1].split(".")))
                        if ver > (2, 17):
                            notes.append(f"glibc 基线 {top} 超过 2.17")
                    if plat == "android-arm64":
                        aligns = set(re.findall(r"^\s*LOAD.*?(0x[0-9a-f]+)\s*$",
                                                sh(["readelf", "-lW", f.name]), re.M))
                        if aligns != {"0x4000"}:
                            notes.append("LOAD 段非 16KB 对齐: " + ",".join(sorted(aligns)))
                    n = len(re.findall(r"\bretro_\w+", sh(["nm", "-D", f.name])))
                    exports = str(n)
                    if n < 20:
                        notes.append("retro_* 导出不足")
                    # strip 断言：Android 本来就被 NDK strip 过，其余 ELF 由
                    # package_release.sh strip；两者都不该再带 .debug_info。
                    if has_debug_info(f.name):
                        notes.append("未 strip（带 .debug_info）")
                    main_bid[plat] = build_id(f.name)
                elif fmt == "PE":
                    out = sh(["objdump", "-p", f.name])
                    n = len(set(re.findall(r"\bretro_\w+", out)))
                    exports = str(n) if n else "见 check_pe"
                    if n == 0:
                        # binutils 的 x86 objdump 解不了 aarch64 PE 导出表，
                        # 必须回到 check_pe.py（pefile，架构无关）。
                        alt = sh([sys.executable, str(HERE / "check_pe.py"), f.name])
                        m = re.search(r"retro_\* 共 (\d+) 个", alt)
                        n = int(m.group(1)) if m else 0
                        exports = str(n)
                        if n < 20:
                            notes.append("PE 导出表 retro_* 不足：" + alt.strip()[:120])
                    dep = "PE 导入表"
                if bad:
                    notes.append("包内混入 " + ",".join(bad))
                ver_ok = info_ver == version
                ok = arch_ok and ver_ok and not notes
                all_ok &= ok
                detail = "；".join(notes) if notes else "干净"
                lines.append(
                    f"| {plat} | `{core}` | {fmt}:{mach} "
                    f"{'OK' if arch_ok else '**FAIL**（期望 ' + str(exp) + '）'} | "
                    f"`{info_ver}` {'OK' if ver_ok else '**FAIL**'} | {dep} | {exports} | "
                    f"{'PASS' if ok else '**FAIL**：' + detail} |")

    # ---------------------------------------------------------- 调试包
    lines += ["", "## 调试包（-debuginfo）", "",
              "| 平台 | 核心 | .debug_info | build-id | 与主包一致 | 判定 |",
              "|---|---|---|---|---|---|"]
    dbg_ok = True
    seen: set[str] = set()
    for zp in dbg_zips:
        plat = plat_of(zp, debuginfo=True)
        with zipfile.ZipFile(zp) as z:
            names = z.namelist()
            core = core_name(names)
            if core is None:
                lines.append(f"| {plat} | **缺失** | - | - | - | FAIL |")
                dbg_ok = False
                continue
            data = z.read(core)
            with tempfile.NamedTemporaryFile(suffix="." + core.rsplit(".", 1)[1]) as f:
                f.write(data)
                f.flush()
                has_dbg = has_debug_info(f.name)
                bid = build_id(f.name)
        same = bool(bid) and bid == main_bid.get(plat)
        size_ok = len(data) >= main_size.get(plat, 0)
        ok = has_dbg and same and size_ok
        notes = []
        if not has_dbg:
            notes.append("没有 .debug_info")
        if not same:
            notes.append(f"build-id 与主包不符（主包 {main_bid.get(plat, '无')[:16]}…）")
        if not size_ok:
            notes.append("比主包内的核心还小")
        dbg_ok &= ok
        seen.add(plat)
        lines.append(
            f"| {plat} | `{core}` | {'有' if has_dbg else '**无**'} | "
            f"`{bid[:16]}{'…' if bid else ''}` | {'是' if same else '**否**'} | "
            f"{'PASS' if ok else '**FAIL**：' + '；'.join(notes)} |")

    # 该有的调试包一个都不能少：需要 strip 的平台必须配对
    need = {p for p in main_bid if p.startswith(NEED_DEBUGINFO)}
    missing = sorted(need - seen)
    extra = sorted(seen - need)
    if missing:
        lines += ["", f"**缺少调试包**：{', '.join(missing)}"]
        dbg_ok = False
    if extra:
        lines += ["", f"**多余的调试包**（该平台不需要 strip）：{', '.join(extra)}"]
        dbg_ok = False

    all_ok &= dbg_ok
    lines += ["", f"## 结论：{'PASS' if all_ok else 'FAIL'}",
              "", f"主包 {len(main_zips)} 个 / 调试包 {len(dbg_zips)} 个（应为 {len(need)} 个）", ""]
    print("\n".join(lines))
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
