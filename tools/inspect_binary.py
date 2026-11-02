#!/usr/bin/env python3
"""inspect_binary.py — 交叉编译产物自检（纯标准库，不依赖 objdump/readelf）

对每个产物打印：格式（ELF/PE/Mach-O）、位宽、机器架构（e_machine 等）
以及是否导出了 libretro 必需符号（retro_init / retro_run / ...）。

用法：
  python tools/inspect_binary.py core/dist/*/makecode_arcade_libretro.*
"""

import glob
import os
import re
import struct
import sys

# RetroArch 的 runloop_init_libretro_symbols() 逐个解析这 25 个符号，
# 缺任何一个就直接放弃加载 core（GUI 下无提示，表现为"无声退出"）。
# 这份清单与 core/src/libretro_core.cpp 末尾的 kCoreAbi 守卫表一一对应。
REQUIRED = [
    "retro_init", "retro_deinit", "retro_api_version", "retro_get_system_info",
    "retro_get_system_av_info", "retro_set_environment", "retro_set_video_refresh",
    "retro_set_audio_sample", "retro_set_audio_sample_batch",
    "retro_set_input_poll", "retro_set_input_state",
    "retro_set_controller_port_device", "retro_reset", "retro_run",
    "retro_load_game", "retro_load_game_special", "retro_unload_game",
    "retro_get_region", "retro_serialize_size", "retro_serialize",
    "retro_unserialize", "retro_cheat_reset", "retro_cheat_set",
    "retro_get_memory_data", "retro_get_memory_size",
]

ELF_MACHINES = {
    3: "x86 (i386)", 8: "MIPS", 10: "MIPSel", 20: "PowerPC", 21: "PowerPC64",
    22: "S/390", 40: "ARM", 62: "x86_64", 183: "AArch64", 243: "RISC-V",
    258: "LoongArch",
}
PE_MACHINES = {
    0x014C: "x86 (i386)", 0x8664: "x86_64", 0x01C0: "ARM",
    0x01C4: "ARMv7 (thumb2)", 0xAA64: "AArch64",
}
MACHO_CPUS = {
    0x01000007: "x86_64", 0x0100000C: "AArch64",
    7: "x86 (i386)", 12: "ARM",
}


def elf_info(d):
    cls = d[4]                      # 1=32bit 2=64bit
    endian = "<" if d[5] == 1 else ">"
    machine = struct.unpack_from(endian + "H", d, 18)[0]
    return ("ELF", "64-bit" if cls == 2 else "32-bit",
            ELF_MACHINES.get(machine, f"machine#{machine}"), endian)


def elf_symbols(d):
    """从 .dynsym / .symtab 里取全局符号名（兼容 32/64 位与两种字节序）。"""
    is64 = d[4] == 2
    endian = "<" if d[5] == 1 else ">"
    if is64:
        shoff, = struct.unpack_from(endian + "Q", d, 0x28)
        shentsize, shnum = struct.unpack_from(endian + "HH", d, 0x3A)
    else:
        shoff, = struct.unpack_from(endian + "I", d, 0x20)
        shentsize, shnum = struct.unpack_from(endian + "HH", d, 0x2E)
    if not shoff or not shnum:
        return set()
    names = set()
    for i in range(shnum):
        base = shoff + i * shentsize
        if is64:
            _name, sh_type, _, _, sh_offset, sh_size, sh_link, _, _, sh_entsize = \
                struct.unpack_from(endian + "IIQQQQIIQQ", d, base)
        else:
            _name, sh_type, _, _, sh_offset, sh_size, sh_link, _, _, sh_entsize = \
                struct.unpack_from(endian + "IIIIIIIIII", d, base)
        if sh_type not in (2, 11):  # SHT_SYMTAB / SHT_DYNSYM
            continue
        if not sh_entsize:
            continue
        strtab_off = strtab_size = 0
        sbase = shoff + sh_link * shentsize
        if is64:
            strtab_off, strtab_size = struct.unpack_from(endian + "QQ", d, sbase + 0x18)
        else:
            strtab_off, strtab_size = struct.unpack_from(endian + "II", d, sbase + 0x10)
        strtab = d[strtab_off:strtab_off + strtab_size]
        count = sh_size // sh_entsize
        for k in range(count):
            off = sh_offset + k * sh_entsize
            if is64:
                st_name, = struct.unpack_from(endian + "I", d, off)
            else:
                st_name, = struct.unpack_from(endian + "I", d, off)
            if st_name:
                end = strtab.find(b"\0", st_name)
                names.add(strtab[st_name:end].decode("latin1"))
    return names


def pe_info(d):
    pe_off, = struct.unpack_from("<I", d, 0x3C)
    machine, nsec = struct.unpack_from("<HH", d, pe_off + 4)
    magic, = struct.unpack_from("<H", d, pe_off + 24)
    return ("PE", "64-bit" if magic == 0x20B else "32-bit",
            PE_MACHINES.get(machine, f"machine#{machine:#x}"), "<")


def pe_exports(d):
    pe_off, = struct.unpack_from("<I", d, 0x3C)
    magic, = struct.unpack_from("<H", d, pe_off + 24)
    opt = pe_off + 24
    dd = opt + (112 if magic == 0x20B else 96)  # DataDirectory 起点
    exp_rva, exp_size = struct.unpack_from("<II", d, dd)
    if not exp_rva:
        return set()
    # 用节表把 RVA 映射到文件偏移
    nsec, = struct.unpack_from("<H", d, pe_off + 6)
    sect = {}
    base = opt + struct.unpack_from("<H", d, pe_off + 20)[0]
    for i in range(nsec):
        s = base + i * 40
        name = d[s:s + 8].rstrip(b"\0").decode("latin1")
        vsize, va, raw_size, raw_ptr = struct.unpack_from("<IIII", d, s + 8)
        sect[name] = (va, max(vsize, raw_size), raw_ptr)

    def rva2off(rva):
        for va, size, ptr in sect.values():
            if va <= rva < va + size:
                return ptr + (rva - va)
        return None

    off = rva2off(exp_rva)
    if off is None:
        return set()
    nnames, = struct.unpack_from("<I", d, off + 24)
    names_rva, = struct.unpack_from("<I", d, off + 32)
    names_off = rva2off(names_rva)
    out = set()
    if names_off is None:
        return out
    for i in range(nnames):
        nrva, = struct.unpack_from("<I", d, names_off + i * 4)
        o = rva2off(nrva)
        if o is None:
            continue
        end = d.find(b"\0", o)
        out.add(d[o:end].decode("latin1"))
    return out


def macho_info(d):
    magic, = struct.unpack_from("<I", d, 0)
    cputype, _sub = struct.unpack_from("<II", d, 4)
    return ("Mach-O", "64-bit" if magic in (0xFEEDFACF, 0xCFFAEDFE) else "32-bit",
            MACHO_CPUS.get(cputype, f"cpu#{cputype:#x}"), "<")


def elf_sections(d):
    """返回 {节名: (offset, size)}，用于找 .note.android.ident 之类。"""
    is64 = d[4] == 2
    endian = "<" if d[5] == 1 else ">"
    if is64:
        shoff, = struct.unpack_from(endian + "Q", d, 0x28)
        shentsize, shnum, shstrndx = struct.unpack_from(endian + "HHH", d, 0x3A)
    else:
        shoff, = struct.unpack_from(endian + "I", d, 0x20)
        shentsize, shnum, shstrndx = struct.unpack_from(endian + "HHH", d, 0x2E)
    if not shoff or not shnum:
        return {}
    def raw(i):
        b = shoff + i * shentsize
        if is64:
            return struct.unpack_from(endian + "IIQQQQIIQQ", d, b)
        return struct.unpack_from(endian + "IIIIIIIIII", d, b)
    str_off, str_size = raw(shstrndx)[4], raw(shstrndx)[5]
    shstr = d[str_off:str_off + str_size]
    out = {}
    for i in range(shnum):
        f = raw(i)
        nm_off = f[0]
        end = shstr.find(b"\0", nm_off)
        name = shstr[nm_off:end].decode("latin1") if nm_off < len(shstr) else ""
        out[name] = (f[4], f[5])
    return out


def elf_android_api(d):
    """读 .note.android.ident 里的 API 级别；非 Android 产物返回 None。"""
    sec = elf_sections(d).get(".note.android.ident")
    if not sec:
        return None
    off, size = sec
    note = d[off:off + size]
    # 结构：namesz(4) descsz(4) type(4) name("Android\0" 补齐到 4) desc(3×uint32)
    if len(note) < 16:
        return None
    namesz, descsz, _ntype = struct.unpack_from("<III", note, 0)
    desc_off = 12 + ((namesz + 3) & ~3)
    if descsz >= 4 and desc_off + 4 <= len(note):
        return struct.unpack_from("<I", note, desc_off)[0]
    return None


def elf_needed(d):
    """DT_NEEDED 列表（动态库依赖），用于判断固件/容器是否缺库。"""
    is64 = d[4] == 2
    endian = "<" if d[5] == 1 else ">"
    secs = elf_sections(d)
    dyn = secs.get(".dynamic")
    dynstr = secs.get(".dynstr")
    if not dyn or not dynstr:
        return []
    doff, dsize = dyn
    soff, ssize = dynstr
    strtab = d[soff:soff + ssize]
    ent = 16 if is64 else 8
    fmt = endian + ("qQ" if is64 else "iI")
    out = []
    for k in range(dsize // ent):
        tag, val = struct.unpack_from(fmt, d, doff + k * ent)
        if tag == 0:
            break
        if tag == 1:
            end = strtab.find(b"\0", val)
            out.append(strtab[val:end].decode("latin1"))
    return out


def elf_glibc_min(d):
    """产物要求的 libc 基线：
    - Android（bionic）：报 API 级别（RetroArch Android 最低 21）
    - glibc：报符号版本里的最大值（掌机固件普遍 ≥2.17，2.29+ 老固件装不上）
    - musl/静态：无版本
    """
    api = elf_android_api(d)
    if api is not None:
        return f"bionic API{api}"
    vers = set()
    for m in re.finditer(rb"GLIBC_(2\.\d+)", d):
        vers.add(m.group(1).decode())
    if not vers:
        return "—（musl）"
    key = lambda v: tuple(int(x) for x in v.split("."))
    return max(vers, key=key)


def main():
    EXTS = (".so", ".dll", ".dylib")
    paths = []
    for a in sys.argv[1:]:
        paths.extend(sorted(p for p in glob.glob(a) if p.endswith(EXTS)))
    if not paths:
        paths = sorted(p for p in glob.glob(os.path.join(
            os.path.dirname(__file__), "..", "core", "dist", "*", "makecode_arcade_libretro.*"))
            if p.endswith(EXTS))
    ok_all = True
    print(f"{'target':<30} {'fmt':<6} {'bits':<7} {'arch':<12} {'libc':<13} {'size':>9}  symbols")
    print("-" * 110)
    deps_report = []
    for p in paths:
        with open(p, "rb") as f:
            d = f.read()
        deps = []
        try:
            if d[:4] == b"\x7fELF":
                fmt, bits, arch, _e = elf_info(d)
                syms = elf_symbols(d)
                glibc = elf_glibc_min(d)
                deps = elf_needed(d)
            elif d[:2] == b"MZ":
                fmt, bits, arch, _e = pe_info(d)
                syms = pe_exports(d)
                glibc = "—"
            elif struct.unpack_from("<I", d, 0)[0] in (0xFEEDFACE, 0xFEEDFACF, 0xCEFAEDFE, 0xCFFAEDFE):
                fmt, bits, arch, _e = macho_info(d)
                syms = set()  # Mach-O 导出表解析略；用 nm 更省事
                glibc = "系统自带"
            else:
                raise ValueError("unknown format")
        except Exception as exc:  # noqa: BLE001
            print(f"{os.path.basename(os.path.dirname(p)):<30} ERROR {os.path.basename(p)}: {exc}")
            ok_all = False
            continue
        if fmt == "Mach-O":
            # 无法解析导出表时，退化为字符串检索（__LINKEDIT 中的符号名是明文）
            found = [s for s in REQUIRED if s.encode() in d]
            missing = [s for s in REQUIRED if s not in found]
        else:
            missing = [s for s in REQUIRED if s not in syms]
        status = "OK" if not missing else f"缺 {len(missing)}: {','.join(missing[:3])}"
        if missing:
            ok_all = False
        print(f"{os.path.basename(os.path.dirname(p)):<30} {fmt:<6} {bits:<7} "
              f"{arch:<12} {glibc:<13} {len(d):>9}  {status}")
        if deps:
            deps_report.append((os.path.basename(os.path.dirname(p)), deps))
    if deps_report:
        print()
        print("动态依赖（DT_NEEDED；插件型 core 越少越好）")
        for tag, deps in deps_report:
            print(f"  {tag:<30} {' '.join(deps)}")
    return 0 if ok_all else 1


if __name__ == "__main__":
    sys.exit(main())
