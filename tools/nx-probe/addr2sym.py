#!/usr/bin/env python3
"""把 Switch 崩溃报告里的 "retroarch_switch + 0xXXXX" 偏移翻成函数名。

崩溃报告（sdmc:/atmosphere/crash_reports/*.log）只给模块内偏移，没有任何
符号信息；而 CI 里的 build-bisect.sh 会把 `nm -n` 输出一起发布成
`*.symbols.txt`。拿这两个一配，`PC` / `LR` / 每一层 `ReturnAddress` 都能直接
落到函数名（含静态函数，因为 ELF 没 strip，局部符号 t 也在）。

用法：
    python tools/nx-probe/addr2sym.py mca_boot.symbols.txt 0x7d20c4 0x594460
    python tools/nx-probe/addr2sym.py mca_boot.symbols.txt --from-crash report.log

--from-crash 会把报告里所有 "retroarch_switch + 0x..." 抓出来一次性转换。
注意报告里的模块偏移是相对**模块基址**（= text 段起点），与 nm 的地址同一
套口径，不需要再加任何 offset。
"""
import argparse
import bisect
import re
import sys


def load_symbols(path):
    addrs, names = [], []
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            parts = line.split()
            if len(parts) < 3:
                continue
            try:
                addr = int(parts[0], 16)
            except ValueError:
                continue
            addrs.append(addr)
            names.append(f"{parts[1]} {parts[2]}")
    order = sorted(range(len(addrs)), key=lambda i: addrs[i])
    return [addrs[i] for i in order], [names[i] for i in order]


def lookup(addrs, names, off):
    i = bisect.bisect_right(addrs, off) - 1
    if i < 0:
        return None, off
    return names[i], off - addrs[i]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("symbols")
    ap.add_argument("offsets", nargs="*", help="0x7d20c4 ...")
    ap.add_argument("--from-crash", metavar="LOG", help="直接从崩溃报告里取所有偏移")
    args = ap.parse_args()

    addrs, names = load_symbols(args.symbols)
    if not addrs:
        sys.exit(f"{args.symbols} 里没有可用的符号行")

    offs = list(args.offsets)
    if args.from_crash:
        text = open(args.from_crash, "r", encoding="utf-8", errors="replace").read()
        offs += re.findall(r"retroarch_switch \+ (0x[0-9a-fA-F]+)", text)

    if not offs:
        sys.exit("没给偏移（位置参数或 --from-crash 二选一）")

    seen = set()
    for s in offs:
        off = int(s, 16) if isinstance(s, str) else s
        if off in seen:
            continue
        seen.add(off)
        sym, delta = lookup(addrs, names, off)
        if sym is None:
            print(f"{off:#x}  (低于最低符号地址)")
        else:
            print(f"{off:#x}  ->  {sym} + {delta:#x}")


if __name__ == "__main__":
    main()
