#!/usr/bin/env python3
"""合成一张符合 docs/PNG-FORMAT.md 的测试卡带 PNG（编解码回环自测用）。

生成 → 用 extract_png_meta.py 解码，两者对上即说明位流编解码逻辑一致。
注意：这只验证自洽性；对真实 pxt 卡带的符合性验证仍需真实样本（M1）。

用法: python tools/make_test_cartridge.py out.png
"""

import lzma
import struct
import sys
import zlib

MAGIC = 0x59347A7D
HEADER_SIZE = 36
BPP = 2  # 每 RGBA 像素 2 bit，一个输出字节占 4 个像素


def write_png(path, w, h, rgba):
    def chunk(ctype, body):
        return (struct.pack(">I", len(body)) + ctype + body +
                struct.pack(">I", zlib.crc32(ctype + body) & 0xFFFFFFFF))

    ihdr = struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)  # RGBA8 无隔行
    raw = b"".join(b"\x00" + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", ihdr))
        f.write(chunk(b"IDAT", zlib.compress(raw)))
        f.write(chunk(b"IEND", b""))


def encode_bits(d: bytearray, ptr: int, bpp: int, payload: bytes) -> int:
    """blob_decode 的逆变换：LSB-first、逢 alpha 字节跳过。"""
    mask = (1 << bpp) - 1
    for byte in payload:
        for k in range(8 // bpp):
            if ptr & 3 == 3:
                ptr += 1
            if ptr >= len(d):
                raise ValueError("image too small")
            d[ptr] |= ((byte >> (k * bpp)) & mask)
            ptr += 1
    return ptr


def build_cartridge(project: dict, w=512, h=512):
    content = '{"source": %s}' % ('"' + project["source"].replace("\\", "\\\\")
                                  .replace('"', '\\"') + '"')
    payload = lzma.compress(content.encode("utf-8"), format=lzma.FORMAT_ALONE)
    header = struct.pack("<III", MAGIC, len(payload), 0) + bytes(HEADER_SIZE - 12)

    bits_needed = (len(header) + len(payload)) * 8
    pixels_needed = 4 + bits_needed // BPP + 4
    if pixels_needed > w * h:
        raise ValueError("image too small for payload")

    d = bytearray(w * h * 4)  # 全 0 = 透明黑，低位可自由使用
    d[0] |= BPP & 1            # bpp 编码在前 3 个像素字节的低位（解码端由此探测）
    d[1] |= (BPP >> 1) & 1
    d[2] |= (BPP >> 2) & 1
    ptr = encode_bits(d, 4, BPP, header)
    encode_bits(d, ptr, BPP, payload)
    return bytes(d)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "test_cartridge.png"
    project = {"source": '{"main.ts": "game.splash(42)\\nbasic.forever(function() {})\\n"}'}
    w = h = 512
    rgba = build_cartridge(project, w, h)
    write_png(out, w, h, rgba)
    print(f"wrote {out} ({w}x{h})")


if __name__ == "__main__":
    main()
