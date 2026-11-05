"""把 muOS 的 /dev/fb0 原始转 PNG（32bpp, 720x1440 双缓冲）。"""

import struct
import sys
import zlib

W, H = 720, 720


def write_png(path, pixels, w, h):
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        raw += pixels[y * w * 3:(y + 1) * w * 3]

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr)
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b"")
    open(path, "wb").write(png)


src = open(sys.argv[1], "rb").read()
order = sys.argv[2] if len(sys.argv) > 2 else "bgra"
stride = W * 4
for idx in range(min(2, len(src) // (W * H * 4))):
    base = idx * W * H * 4
    buf = src[base:base + W * H * 4]
    nz = sum(1 for i in range(0, len(buf), 4000) if buf[i] or buf[i + 1] or buf[i + 2])
    out = bytearray(W * H * 3)
    for y in range(H):
        row = buf[y * stride:(y + 1) * stride]
        for x in range(W):
            b, g, r = row[x * 4], row[x * 4 + 1], row[x * 4 + 2]
            o = (y * W + x) * 3
            if order == "bgra":
                out[o], out[o + 1], out[o + 2] = r, g, b
            else:
                out[o], out[o + 1], out[o + 2] = b, g, r
    name = "out/deploy/muos/fb%d_%s.png" % (idx, order)
    write_png(name, out, W, H)
    print(name, "nonzero_samples=", nz, "/", len(buf) // 4000)
