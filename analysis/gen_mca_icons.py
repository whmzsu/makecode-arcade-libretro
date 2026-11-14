#!/usr/bin/env python3
# Generate watermark-free MakeCode Arcade ES theme icons at the exact sizes
# used by es-theme-nes-box:
#   system.png        1000 x 287  (wide banner / wordmark)
#   background_icon.png 960 x 540  (faint backdrop mark)
# Pure stdlib (zlib) PNG writer + RGBA canvas. No external deps, no watermark.

import zlib, struct, os

# ---- MakeCode Arcade palette (vibrant brand blocks) ----
PURPLE = (92, 45, 145)      # MakeCode purple (badge)
PURPLE_L = (150, 100, 210)  # light border
PALETTE = [
    (229, 37, 33),   # red
    (255, 127, 0),   # orange
    (255, 214, 0),   # yellow
    (0, 184, 80),    # green
    (0, 178, 201),   # cyan
    (0, 90, 220),    # blue
    (150, 60, 220),  # purple/magenta
]
WHITE = (255, 255, 255)


def write_png(path, w, h, rgba):
    sig = b"\x89PNG\r\n\x1a\n"

    def chunk(typ, data):
        c = typ + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)  # 8-bit RGBA
    raw = bytearray()
    stride = w * 4
    for y in range(h):
        raw.append(0)  # filter type 0
        raw += rgba[y * stride:(y + 1) * stride]
    idat = zlib.compress(bytes(raw), 9)
    with open(path, "wb") as f:
        f.write(sig)
        f.write(chunk(b"IHDR", ihdr))
        f.write(chunk(b"IDAT", idat))
        f.write(chunk(b"IEND", b""))


class Canvas:
    def __init__(self, w, h):
        self.w, self.h = w, h
        self.buf = bytearray(w * h * 4)

    def set(self, x, y, rgba, a=None):
        if x < 0 or y < 0 or x >= self.w or y >= self.h:
            return
        i = (y * self.w + x) * 4
        r, g, b = rgba
        aa = a if a is not None else 255
        if aa >= 255:
            self.buf[i] = r; self.buf[i+1] = g; self.buf[i+2] = b; self.buf[i+3] = 255
        elif aa > 0:
            ba = self.buf[i+3] / 255.0
            na = aa / 255.0
            out_a = na + ba * (1 - na)
            if out_a <= 0:
                return
            orc = self.buf[i]; ogc = self.buf[i+1]; obc = self.buf[i+2]
            self.buf[i]   = int((r * na + orc * ba * (1 - na)) / out_a)
            self.buf[i+1] = int((g * na + ogc * ba * (1 - na)) / out_a)
            self.buf[i+2] = int((b * na + obc * ba * (1 - na)) / out_a)
            self.buf[i+3] = int(out_a * 255)

    def rect(self, x0, y0, x1, y1, rgba, a=255):
        for y in range(int(y0), int(y1)):
            for x in range(int(x0), int(x1)):
                self.set(x, y, rgba, a)

    def rrect(self, x0, y0, x1, y1, r, rgba, a=255):
        self.rect(x0, y0, x1, y1, rgba, a)
        for yy in range(int(y0), int(y0) + r + 1):
            for xx in range(int(x0), int(x0) + r + 1):
                if (xx - x0) ** 2 + (yy - y0) ** 2 > r * r:
                    self.set(xx, yy, (0, 0, 0), 0)
        for yy in range(int(y1) - r, int(y1)):
            for xx in range(int(x0), int(x0) + r + 1):
                if (xx - x0) ** 2 + (yy - (y1 - 1)) ** 2 > r * r:
                    self.set(xx, yy, (0, 0, 0), 0)
        for yy in range(int(y0), int(y0) + r + 1):
            for xx in range(int(x1) - r, int(x1)):
                if (xx - (x1 - 1)) ** 2 + (yy - y0) ** 2 > r * r:
                    self.set(xx, yy, (0, 0, 0), 0)
        for yy in range(int(y1) - r, int(y1)):
            for xx in range(int(x1) - r, int(x1)):
                if (xx - (x1 - 1)) ** 2 + (yy - (y1 - 1)) ** 2 > r * r:
                    self.set(xx, yy, (0, 0, 0), 0)

    def square(self, cx, cy, s, rgba, a=255):
        self.rect(cx - s // 2, cy - s // 2, cx + s // 2, cy + s // 2, rgba, a)

    def save(self, path):
        write_png(path, self.w, self.h, self.buf)


# ---- tiny 5x7 pixel font (subset for MAKECODE ARCADE) ----
FONT = {
    'M': ["10001", "11011", "10101", "10001", "10001", "10001", "10001"],
    'A': ["01110", "10001", "10001", "11111", "10001", "10001", "10001"],
    'K': ["10001", "10010", "10100", "11000", "10100", "10010", "10001"],
    'E': ["11111", "10000", "10000", "11110", "10000", "10000", "11111"],
    'C': ["01110", "10001", "10000", "10000", "10000", "10001", "01110"],
    'O': ["01110", "10001", "10001", "10001", "10001", "10001", "01110"],
    'D': ["11110", "10001", "10001", "10001", "10001", "10001", "11110"],
    'R': ["11110", "10001", "10001", "11110", "10100", "10010", "10001"],
    ' ': ["00000", "00000", "00000", "00000", "00000", "00000", "00000"],
}


def text(c, s, x, y, scale, rgba):
    cx = x
    for ch in s:
        g = FONT.get(ch)
        if g is None:
            cx += 6 * scale
            continue
        for row, line in enumerate(g):
            for col, bit in enumerate(line):
                if bit == "1":
                    c.rect(cx + col * scale, y + row * scale,
                           cx + (col + 1) * scale, y + (row + 1) * scale, rgba)
        cx += 6 * scale


def block_row(c, cy, sq, gap, colors):
    total = len(colors) * sq + (len(colors) - 1) * gap
    sx = (c.w - total) // 2
    for i, col in enumerate(colors):
        cx = sx + i * (sq + gap) + sq // 2
        c.square(cx, cy, sq, col, a=255)


# ===================== system.png (1000x287) =====================
def make_banner(path):
    c = Canvas(1000, 287)
    # solid rounded purple badge (guarantees contrast on any theme bg)
    c.rrect(16, 16, 984, 271, 34, PURPLE, a=255)
    c.rrect(20, 20, 980, 267, 30, PURPLE_L, a=70)
    # top row of 7 colored brand blocks
    block_row(c, 40, 18, 7, PALETTE)
    # two-line big wordmark
    scale = 13
    gh = 7 * scale
    gap_lines = 12
    lines = ["MAKECODE", "ARCADE"]
    top = 56
    block_h = gh * 2 + gap_lines
    start_y = top + ((271 - 16) - top - block_h) // 2
    for i, line in enumerate(lines):
        w = len(line) * 6 * scale
        x = (c.w - w) // 2
        y = start_y + i * (gh + gap_lines)
        text(c, line, x, y, scale, WHITE)
    c.save(path)
    print("banner ->", path, "text height(px in canvas)=", gh)


# ===================== background_icon.png (960x540) =====================
def make_backdrop(path):
    c = Canvas(960, 540)
    c.rrect(120, 100, 840, 440, 44, PURPLE, a=55)
    c.rrect(132, 112, 828, 428, 36, PURPLE_L, a=40)
    # big 4x2 block grid centered
    cols, rows, cell, gap = 4, 2, 96, 26
    total_w = cols * cell + (cols - 1) * gap
    total_h = rows * cell + (rows - 1) * gap
    sx = (c.w - total_w) // 2 + cell // 2
    sy = (c.h - total_h) // 2 + cell // 2
    idx = 0
    for r in range(rows):
        for col in range(cols):
            px = sx + col * (cell + gap)
            py = sy + r * (cell + gap)
            c.square(px, py, cell, PALETTE[idx % len(PALETTE)], a=200)
            idx += 1
    c.save(path)
    print("backdrop ->", path)


if __name__ == "__main__":
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "mca_icons")
    os.makedirs(out, exist_ok=True)
    make_banner(os.path.join(out, "system.png"))
    make_backdrop(os.path.join(out, "background_icon.png"))
    print("done ->", out)
