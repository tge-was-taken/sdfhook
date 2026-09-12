#!/usr/bin/env python3
"""Crops the same region out of several PNGs and lays them side by side, scaled up.

    cropcmp.py out.png x y w h scale a.png b.png [more.png]

For comparing a worst tile from refdiff.py across shots without an image library.
"""
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.expanduser("~/projects/pcsx2-hook/pcsx2-hook/scripts"))
from refdiff import read_png  # noqa: E402


def write_png(path, w, h, rgb):
    rows = b"".join(b"\x00" + rgb[y * w * 3 : (y + 1) * w * 3] for y in range(h))

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))

    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def main():
    out, x, y, w, h, scale = sys.argv[1], *map(int, sys.argv[2:7])
    shots = [read_png(p) for p in sys.argv[7:]]
    gap = 4
    cw, ch = w * scale, h * scale
    total_w = len(shots) * cw + (len(shots) - 1) * gap
    canvas = bytearray(total_w * ch * 3)
    for i, (sw, sh, pixels) in enumerate(shots):
        left = i * (cw + gap)
        for cy in range(ch):
            sy = min(y + cy // scale, sh - 1)
            for cx in range(cw):
                sx = min(x + cx // scale, sw - 1)
                src = (sy * sw + sx) * 3
                dst = (cy * total_w + left + cx) * 3
                canvas[dst : dst + 3] = pixels[src : src + 3]
    write_png(out, total_w, ch, bytes(canvas))


if __name__ == "__main__":
    main()
