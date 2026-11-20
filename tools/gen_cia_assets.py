#!/usr/bin/env python3
"""生成 3DS CIA 素材：48x48 图标 + 256x128 banner（纯 Python，无 PIL）。

用法: python tools/gen_cia_assets.py <outdir>
"""
import struct, sys, zlib

# 5x7 点阵字母（1=前景）。A-Z + 0-9 全覆盖——早期只登记了 13 个字符，
# 缺的字（如 banner 里的 'O'）会静默渲染成空白，别再按需加字。
FONT = {
    'A': ["01110","10001","10001","11111","10001","10001","10001"],
    'B': ["11110","10001","10001","11110","10001","10001","11110"],
    'C': ["01110","10001","10000","10000","10000","10001","01110"],
    'D': ["11110","10001","10001","10001","10001","10001","11110"],
    'E': ["11111","10000","10000","11110","10000","10000","11111"],
    'F': ["11111","10000","10000","11110","10000","10000","10000"],
    'G': ["01110","10001","10000","10111","10001","10001","01111"],
    'H': ["10001","10001","10001","11111","10001","10001","10001"],
    'I': ["11100","01000","01000","01000","01000","01000","11100"],
    'J': ["00111","00010","00010","00010","00010","10010","01100"],
    'K': ["10001","10010","10100","11000","10100","10010","10001"],
    'L': ["10000","10000","10000","10000","10000","10000","11111"],
    'M': ["10001","11011","10101","10101","10001","10001","10001"],
    'N': ["10001","11001","10101","10011","10001","10001","10001"],
    'O': ["01110","10001","10001","10001","10001","10001","01110"],
    'P': ["11110","10001","10001","11110","10000","10000","10000"],
    'Q': ["01110","10001","10001","10001","10101","10010","01101"],
    'R': ["11110","10001","10001","11110","10100","10010","10001"],
    'S': ["01111","10000","10000","01110","00001","00001","11110"],
    'T': ["11111","00100","00100","00100","00100","00100","00100"],
    'U': ["10001","10001","10001","10001","10001","10001","01110"],
    'V': ["10001","10001","10001","10001","10001","01010","00100"],
    'W': ["10001","10001","10001","10101","10101","11011","10001"],
    'X': ["10001","10001","01010","00100","01010","10001","10001"],
    'Y': ["10001","10001","01010","00100","00100","00100","00100"],
    'Z': ["11111","00001","00010","00100","01000","10000","11111"],
    '0': ["01110","10011","10101","10101","10101","11001","01110"],
    '1': ["00100","01100","00100","00100","00100","00100","01110"],
    '2': ["01110","10001","00001","00010","00100","01000","11111"],
    '3': ["01110","10001","00001","00110","00001","10001","01110"],
    '4': ["00010","00110","01010","10010","11111","00010","00010"],
    '5': ["11111","10000","10000","11110","00001","00001","11110"],
    '6': ["01110","10000","10000","11110","10001","10001","01110"],
    '7': ["11111","00001","00010","00100","01000","01000","01000"],
    '8': ["01110","10001","10001","01110","10001","10001","01110"],
    '9': ["01110","10001","10001","01111","00001","00001","01110"],
    ' ': ["00000"] * 7,
}

BG = (0x5C, 0x2D, 0x91)   # MakeCode 紫
FG = (0xFF, 0xFF, 0xFF)


def render_text(text, scale=3):
    """返回 (像素网格 [y][x] -> bool, 宽, 高)"""
    rows = 7 * scale
    cols = sum(6 for _ in text) * scale - scale  # 每字 5+1 空格
    grid = [[False] * cols for _ in range(rows)]
    cx = 0
    for ch in text.upper():
        glyph = FONT.get(ch, FONT[' '])
        for gy, line in enumerate(glyph):
            for gx, bit in enumerate(line):
                if bit == '1':
                    for sy in range(scale):
                        for sx in range(scale):
                            grid[gy * scale + sy][cx + gx * scale + sx] = True
        cx += 6 * scale
    return grid, cols, rows


def draw_centered(px, w, h, text, scale, y_off=0):
    grid, tw, th = render_text(text, scale)
    x0 = (w - tw) // 2
    y0 = (h - th) // 2 + y_off
    for gy, row in enumerate(grid):
        for gx, on in enumerate(row):
            if on and 0 <= y0 + gy < h and 0 <= x0 + gx < w:
                px[y0 + gy][x0 + gx] = FG


def write_png(path, px, w, h):
    raw = b''
    for row in px:
        raw += b'\x00' + b''.join(struct.pack('BBB', *p) for p in row)

    def chunk(tag, data):
        c = struct.pack('>I', len(data)) + tag + data
        return c + struct.pack('>I', zlib.crc32(tag + data) & 0xFFFFFFFF)

    png = (b'\x89PNG\r\n\x1a\n'
           + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
           + chunk(b'IDAT', zlib.compress(raw, 9))
           + chunk(b'IEND', b''))
    with open(path, 'wb') as f:
        f.write(png)
    print(f'{path}: {w}x{h} {len(png)} B')


def write_wav(path, seconds=1.0, rate=44100):
    """静音 WAV（makebanner 的 -a 必填；banner 不配声就用静音占位）"""
    import math
    n = int(seconds * rate)
    data = b''.join(struct.pack('<h', 0) for _ in range(n))
    hdr = (b'RIFF' + struct.pack('<I', 36 + len(data)) + b'WAVE'
           + b'fmt ' + struct.pack('<IHHIIHH', 16, 1, 1, rate, rate * 2, 2, 16)
           + b'data' + struct.pack('<I', len(data)))
    with open(path, 'wb') as f:
        f.write(hdr + data)
    print(f'{path}: {seconds}s {rate}Hz 静音 {len(hdr) + len(data)} B')


def main(outdir):
    # 48x48 图标：紫底 + 白色 MCA
    w, h = 48, 48
    px = [[BG for _ in range(w)] for _ in range(h)]
    draw_centered(px, w, h, 'MCA', 3)
    write_png(f'{outdir}/icon48.png', px, w, h)

    # 256x128 banner：紫底 + 白色标题两行
    w, h = 256, 128
    px = [[BG for _ in range(w)] for _ in range(h)]
    draw_centered(px, w, h, 'MCA CORE', 5, y_off=-14)
    draw_centered(px, w, h, '3DS', 3, y_off=18)
    write_png(f'{outdir}/banner256.png', px, w, h)

    # bannertool makebanner 要求 -a 音频必填（diasurgical fork，源码实测），
    # 无声 banner 用 1s 静音占位
    write_wav(f'{outdir}/banner.wav')


if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else '.')
