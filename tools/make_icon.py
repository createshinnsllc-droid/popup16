#!/usr/bin/env python3
"""Draws the launcher icon: three stacked, offset layer cards in SNES-like colours (no Nintendo
artwork). usage: make_icon.py <out.png> [size]"""
import struct, sys, zlib

out, N = sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 192
px = [[(24, 20, 36)] * N for _ in range(N)]

def rect(x0, y0, x1, y1, c):
    for y in range(max(0, int(y0)), min(N, int(y1))):
        for x in range(max(0, int(x0)), min(N, int(x1))):
            px[y][x] = c

u = N / 24.0
cards = [((96, 84, 200), (190, 180, 255)), ((60, 160, 90), (150, 230, 160)), ((220, 60, 70), (255, 170, 160))]
for i, (fill, edge) in enumerate(cards):          # back to front, each nearer card shifted down-right
    x0, y0 = (3 + 3 * i) * u, (3 + 3 * i) * u
    rect(x0, y0, x0 + 12 * u, y0 + 12 * u, edge)
    rect(x0 + u, y0 + u, x0 + 11 * u, y0 + 11 * u, fill)

raw = b"".join(b"\x00" + bytes(v for p in row for v in p) for row in px)
def chunk(t, d): return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
open(out, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", N, N, 8, 2, 0, 0, 0)) +
                      chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))
