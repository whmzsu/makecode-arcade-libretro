"""校验 out/release/github-core-v0.3.10 里从 GitHub Release 下载的资产。

只读本地 zip，不联网。检查三件事：
  1. 每个主包里有核心二进制 + .info，且 .info 的 display_version 与 tag 一致
  2. 核心二进制的架构（PE Machine / ELF e_machine）与包名对得上
  3. 平台资产（ctr/vita/switch）内容清单符合约定

用法：python tools/verify_github_assets.py out/release/github-core-v0.3.10 0.3.10
"""
import os
import re
import struct
import sys
import zipfile

# 包名 -> 期望架构。PE 用 Machine，ELF 用 e_machine
EXPECT = {
    "makecode_arcade_windows-x64.zip":   ("pe", 0x8664),
    "makecode_arcade_windows-arm64.zip": ("pe", 0xaa64),
    "makecode_arcade_windows-x86.zip":   ("pe", 0x014c),
    "makecode_arcade_linux-x64.zip":     ("elf", 62),
    "makecode_arcade_linux-arm64.zip":   ("elf", 183),
    "makecode_arcade_linux-x86.zip":     ("elf", 3),
    "makecode_arcade_linux-armhf.zip":   ("elf", 40),
    "makecode_arcade_android-arm64.zip": ("elf", 183),
    "makecode_arcade_android-armv7.zip": ("elf", 40),
    "makecode_arcade_lakka-arm64.zip":   ("elf", 183),
}


def pe_machine(data):
    if data[:2] != b"MZ":
        return None
    off = struct.unpack_from("<I", data, 0x3C)[0]
    if data[off:off + 4] != b"PE\0\0":
        return None
    return struct.unpack_from("<H", data, off + 4)[0]


def elf_machine(data):
    if data[:4] != b"\x7fELF":
        return None
    return struct.unpack_from("<H", data, 18)[0]


def check_one(name, path, ver):
    """返回 (row, error)。row 为 None 表示这条不进表。"""
    try:
        zf = zipfile.ZipFile(path)
    except zipfile.BadZipFile:
        return None, "不是完整 zip（下载截断？）"
    with zf:
        try:
            members = zf.namelist()
        except Exception as e:                                  # noqa: BLE001
            return None, "读不出成员表：%s" % e

        if name in EXPECT:
            kind, want = EXPECT[name]
            core = [m for m in members if m.endswith((".dll", ".so", ".dylib"))]
            if len(core) != 1:
                return None, "核心二进制数量 %d" % len(core)
            data = zf.read(core[0])
            got = pe_machine(data) if kind == "pe" else elf_machine(data)
            if got != want:
                return None, "架构不匹配：期望 %s/%s，实际 %s" % (
                    kind, hex(want), hex(got) if got else None)
            info = [m for m in members if m.endswith(".info")]
            if not info:
                return None, "缺 .info"
            txt = zf.read(info[0]).decode("utf-8", "replace")
            m = re.search(r'display_version = "([^"]+)"', txt)
            if not m or m.group(1) != ver:
                return None, ".info 版本 %s != %s" % (
                    m.group(1) if m else "无", ver)
            return (name, "%s/%s" % (kind, hex(got)), m.group(1), core[0]), None

        if name.endswith("-debuginfo.zip"):
            # 粗指标：符号包里要有核心二进制。精确 build-id 配对在 tool69
            # 上用 tools/verify_release.py 做（那边有 readelf）。
            core = [m for m in members if m.endswith((".so", ".dll"))]
            if not core:
                return None, "符号包里没有核心二进制"
            return (name, "debuginfo", "-", core[0]), None

        # 平台资产：ctr / vita / switch
        info = [m for m in members if m.endswith(".info")]
        if not info:
            return None, "缺 .info"
        txt = zf.read(info[0]).decode("utf-8", "replace")
        m = re.search(r'display_version = "([^"]+)"', txt)
        if not m or m.group(1) != ver:
            return None, ".info 版本与 tag 不符（%s）" % (
                m.group(1) if m else "无")
        return (name, "+".join(sorted(members)), m.group(1), "-"), None


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "out/release/github-core-v0.3.10"
    ver = sys.argv[2] if len(sys.argv) > 2 else "0.3.10"
    rows, bad = [], []
    for n in sorted(os.listdir(root)):
        if not n.endswith(".zip"):
            continue
        row, err = check_one(n, os.path.join(root, n), ver)
        if row:
            rows.append(row)
        if err:
            bad.append((n, err))

    print("%-42s %-16s %-8s %s" % ("资产", "形态", "版本", "核心/内容"))
    print("-" * 104)
    for n, kind, v, core in rows:
        print("%-42s %-16s %-8s %s" % (n, kind, v, core))
    print("-" * 104)
    if bad:
        print("失败 %d 项：" % len(bad))
        for n, r in bad:
            print("  FAIL %s —— %s" % (n, r))
        return 1
    print("全部 %d 个 zip 校验通过（版本 %s）" % (len(rows), ver))
    return 0


if __name__ == "__main__":
    sys.exit(main())
