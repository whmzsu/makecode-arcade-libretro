#!/usr/bin/env python3
"""MakeCode Arcade .png 卡带解码验证工具（纯标准库）。

按 docs/PNG-FORMAT.md 解码 .png 卡带，提取项目 JSON 与源码文件，
用于在接 C++ 实现前验证格式（对应 core/src/png_loader.cpp 的 M1）。

用法:
    python tools/extract_png_meta.py game.png            # 打印格式信息
    python tools/extract_png_meta.py game.png --dump-ts  # 导出全部源码文件
"""

import json
import lzma
import struct
import sys
import zlib

MAGIC = 0x59347A7D
IMG_HEADER_SIZE = 36


# ---------------- PNG 解码（bit depth 8，无隔行） ----------------

def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def png_to_rgba(data: bytes):
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG file")
    pos, ihdr, plte, trns, idat = 8, None, b"", b"", []
    while pos + 8 <= len(data):
        (length,) = struct.unpack_from(">I", data, pos)
        ctype = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if ctype == b"IHDR":
            ihdr = struct.unpack_from(">IIBBBBB", body)
        elif ctype == b"PLTE":
            plte = body
        elif ctype == b"tRNS":
            trns = body
        elif ctype == b"IDAT":
            idat.append(body)
        elif ctype == b"IEND":
            break
    if ihdr is None:
        raise ValueError("missing IHDR")
    w, h, depth, color, comp, filt, interlace = ihdr
    if depth != 8 or interlace != 0 or comp != 0 or filt != 0:
        raise ValueError(f"unsupported PNG: depth={depth} interlace={interlace}")
    if color not in (0, 2, 3, 4, 6):
        raise ValueError(f"unsupported color type {color}")

    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[color]
    raw = zlib.decompress(b"".join(idat))
    stride = w * channels
    out = bytearray(w * h * 4)
    prev = bytearray(stride)
    for y in range(h):
        base = y * (stride + 1)
        f = raw[base]
        line = bytearray(raw[base + 1:base + 1 + stride])
        if f == 1:  # Sub
            for i in range(channels, stride):
                line[i] = (line[i] + line[i - channels]) & 0xFF
        elif f == 2:  # Up
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif f == 3:  # Average
            for i in range(stride):
                a = line[i - channels] if i >= channels else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif f == 4:  # Paeth
            for i in range(stride):
                a = line[i - channels] if i >= channels else 0
                c = prev[i - channels] if i >= channels else 0
                line[i] = (line[i] + paeth(a, prev[i], c)) & 0xFF
        elif f != 0:
            raise ValueError(f"bad filter {f} at row {y}")
        prev = bytearray(line)

        for x in range(w):
            o = (y * w + x) * 4
            i = x * channels
            if color == 0:
                g = line[i]
                out[o:o + 4] = bytes((g, g, g, 255))
            elif color == 2:
                out[o:o + 4] = bytes((line[i], line[i + 1], line[i + 2], 255))
            elif color == 3:
                idx = line[i]
                r, g, b = plte[idx * 3:idx * 3 + 3]
                a = trns[idx] if idx < len(trns) else 255
                out[o:o + 4] = bytes((r, g, b, a))
            elif color == 4:
                g, al = line[i], line[i + 1]
                out[o:o + 4] = bytes((g, g, g, al))
            else:
                out[o:o + 4] = bytes(line[i:i + 4])
    return w, h, bytes(out)


# ---------------- 卡带 blob 位流解码（移植自 jan4984-lib.rs / pxt） ----------------

def blob_decode(d: bytes, ptr: int, bpp: int, tgr_len: int):
    shift = acc = 0
    mask = (1 << bpp) - 1
    out = bytearray()
    i = 0
    n = len(d)
    while i < tgr_len:
        if ptr >= n:
            raise ValueError(f"bitstream overrun at byte {ptr}")
        acc |= (d[ptr] & mask) << shift
        ptr += 1
        if ptr & 3 == 3:
            ptr += 1
        shift += bpp
        if shift >= 8:
            out.append(acc & 0xFF)
            i += 1
            acc = 0
            shift -= 8
    return ptr, bytes(out)


def extract_payload(w, h, d: bytes):
    bpp = (d[0] & 1) | ((d[1] & 1) << 1) | ((d[2] & 1) << 2)
    if not 1 <= bpp <= 5:
        raise ValueError(f"invalid bpp {bpp}: not an arcade cartridge png")
    ptr, hd = blob_decode(d, 4, bpp, IMG_HEADER_SIZE)
    magic, ret_len, added_lines = struct.unpack_from("<III", hd, 0)
    if magic != MAGIC:
        raise ValueError(f"bad magic 0x{magic:08x} (expect 0x{MAGIC:08x})")
    if added_lines > 0:
        orig_size = (h - added_lines) * w
        img_cap = (orig_size - 1) * 3 * bpp >> 3
        ptr, res = blob_decode(d, ptr, bpp, img_cap - IMG_HEADER_SIZE)
        _, added = blob_decode(d, orig_size * 4, 8, ret_len - len(res))
        res += added
    else:
        _, res = blob_decode(d, ptr, bpp, ret_len)
    return bpp, res


def payload_to_project(res: bytes):
    if res[0:1] == b"{":
        content = res.decode("utf-8")
        compressed = False
    else:
        try:
            content = lzma.decompress(res, format=lzma.FORMAT_ALONE).decode("utf-8")
        except lzma.LZMAError:
            content = lzma.decompress(res, format=lzma.FORMAT_AUTO).decode("utf-8")
        compressed = True
    obj = json.loads(content)
    source = json.loads(obj["source"])
    return compressed, obj, source


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    path = sys.argv[1]
    dump = "--dump-ts" in sys.argv
    with open(path, "rb") as f:
        data = f.read()

    w, h, rgba = png_to_rgba(data)
    print(f"png: {w}x{h}, rgba {len(rgba)} bytes")
    bpp, res = extract_payload(w, h, rgba)
    print(f"bpp: {bpp}, payload: {len(res)} bytes")
    compressed, obj, source = payload_to_project(res)
    print(f"payload: {'lzma-compressed' if compressed else 'plain json'}")
    print(f"project keys: {sorted(obj.keys())}")
    print(f"source files: {sorted(source.keys())}")
    for name, text in source.items():
        print(f"  {name}: {len(text)} chars")
    if dump:
        for name, text in source.items():
            out = name.replace("/", "_")
            with open(out, "w", encoding="utf-8") as f:
                f.write(text)
            print(f"wrote {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
