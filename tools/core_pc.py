"""从 core dump 里读出崩溃线程的 PC/LR，并映射到具体库 + 偏移。

用途：设备上没有 gdb 时定位 SIGSEGV 位置。
用法: python3 core_pc.py <core文件>

原理：解析 PT_NOTE 里的 NT_PRSTATUS（arm64 user_pt_regs）与 NT_FILE（映射表）。
arm64 prstatus: pr_reg 起于偏移 112，x0-x30 是 [0..30]，sp=[31]，pc=[32]，pstate=[33]。
"""

import struct
import sys

PT_NOTE = 4
NT_PRSTATUS = 1
NT_FILE = 0x46494C45
AARCH64 = 183


def read_elf(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"\x7fELF":
        raise SystemExit("not an ELF core")
    is64 = data[4] == 2
    if not is64:
        raise SystemExit("only 64-bit cores supported")
    e_machine = struct.unpack_from("<H", data, 18)[0]
    if e_machine != AARCH64:
        raise SystemExit("expected AArch64 core, got e_machine=%d" % e_machine)
    e_phoff = struct.unpack_from("<Q", data, 32)[0]
    e_phentsize = struct.unpack_from("<H", data, 54)[0]
    e_phnum = struct.unpack_from("<H", data, 56)[0]

    phdrs = []
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        p_type, p_flags = struct.unpack_from("<II", data, off)
        p_offset, p_vaddr, p_paddr, p_filesz = struct.unpack_from("<QQQQ", data, off + 8)
        phdrs.append((p_type, p_offset, p_vaddr, p_filesz))
    return data, phdrs


def parse_notes(data, phdrs):
    prstatus = []
    files = []
    for p_type, p_offset, p_vaddr, p_filesz in phdrs:
        if p_type != PT_NOTE:
            continue
        pos = p_offset
        end = p_offset + p_filesz
        while pos + 12 <= end:
            namesz, descsz, ntype = struct.unpack_from("<III", data, pos)
            pos += 12
            name = data[pos:pos + namesz].rstrip(b"\0")
            pos += (namesz + 3) & ~3
            desc = data[pos:pos + descsz]
            pos += (descsz + 3) & ~3
            if ntype == NT_PRSTATUS:
                # pr_reg 偏移 112，arm64 有 34 个 8 字节寄存器
                if len(desc) >= 112 + 34 * 8:
                    regs = struct.unpack_from("<34Q", desc, 112)
                    pid = struct.unpack_from("<i", desc, 32)[0]
                    prstatus.append({"pid": pid, "regs": regs})
            elif ntype == NT_FILE:
                if len(desc) < 16:
                    continue
                # 头部只有 16 字节：count + page_size，之后紧跟三元组
                count, page_size = struct.unpack_from("<QQ", desc, 0)
                pos2 = 16
                entries = []
                for _i in range(count):
                    if pos2 + 24 > len(desc):
                        break
                    start, endaddr, file_ofs = struct.unpack_from("<QQQ", desc, pos2)
                    pos2 += 24
                    entries.append([start, endaddr, file_ofs * page_size, None])
                names = desc[pos2:].split(b"\0")
                for e, nm in zip(entries, names):
                    e[3] = nm.decode("utf-8", "replace")
                    files.append(e)
    return prstatus, files


def which(files, addr):
    for start, endaddr, file_ofs, name in files:
        if start <= addr < endaddr:
            return name, addr - start + file_ofs
    return None, None


def main():
    path = sys.argv[1]
    data, phdrs = read_elf(path)
    prstatus, files = parse_notes(data, phdrs)
    print("threads with NT_PRSTATUS: %d, mapped files: %d" % (len(prstatus), len(files)))
    if not prstatus:
        raise SystemExit("no prstatus found")

    # 第一个通常是崩溃线程
    for idx, st in enumerate(prstatus):
        r = st["regs"]
        pc, lr, sp = r[32], r[30], r[31]
        print("\n--- thread[%d] pid=%s" % (idx, st["pid"]))
        print("  pc = 0x%016x" % pc)
        print("  lr = 0x%016x" % lr)
        print("  sp = 0x%016x" % sp)
        for label, addr in (("pc", pc), ("lr", lr), ("sp", sp)):
            name, off = which(files, addr)
            if name:
                print("  %s -> %s + 0x%x" % (label, name, off))
            else:
                print("  %s -> (unmapped)" % label)
        if idx >= 2:
            break


if __name__ == "__main__":
    main()
