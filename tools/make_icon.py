#!/usr/bin/env python3
"""Generate assets/icon0.png (512x512): three palette swatches on a dark tile.

   make_icon.py [out.png]           the tile's icon (square: the console rounds it)
   make_icon.py --rounded out.png   the same with rounded, transparent corners,
                                    for places that don't round it (the README)
"""
import struct
import sys
import zlib

W = H = 512
BG = (24, 26, 31)
SWATCHES = [  # (x0, y0, x1, y1, colour)
    (112, 128, 400, 192, (236, 233, 226)),
    (112, 224, 344, 288, (226, 167, 64)),
    (112, 320, 288, 384, (88, 140, 196)),
]
RADIUS = 16
TILE_RADIUS = 112   # the rounded version's corners
SAMPLES = 4         # per axis, for smooth corner edges


def inside(x, y, x0, y0, x1, y1, r):
    if not (x0 <= x < x1 and y0 <= y < y1):
        return False
    cx = min(max(x, x0 + r), x1 - r - 1)
    cy = min(max(y, y0 + r), y1 - r - 1)
    return (x - cx) ** 2 + (y - cy) ** 2 <= r * r


def pixel(x, y):
    for x0, y0, x1, y1, c in SWATCHES:
        if inside(x, y, x0, y0, x1, y1, RADIUS):
            return c
    return BG


def chunk(kind, data):
    body = kind + data
    return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body))


def coverage(x, y):
    """How much of pixel (x, y) is inside the rounded tile, 0 to 1."""
    r = TILE_RADIUS
    if r <= x < W - r or r <= y < H - r:
        return 1.0
    hits = 0
    for i in range(SAMPLES):
        for j in range(SAMPLES):
            sx, sy = x + (i + 0.5) / SAMPLES, y + (j + 0.5) / SAMPLES
            cx = min(max(sx, r), W - r)
            cy = min(max(sy, r), H - r)
            hits += (sx - cx) ** 2 + (sy - cy) ** 2 <= r * r
    return hits / (SAMPLES * SAMPLES)


rounded = sys.argv[1:2] == ["--rounded"]
args = sys.argv[2:] if rounded else sys.argv[1:]
if rounded:
    raw = b"".join(b"\0" + bytes(v for x in range(W) for v in (*pixel(x, y), round(255 * coverage(x, y))))
                   for y in range(H))
else:
    raw = b"".join(b"\0" + bytes(v for x in range(W) for v in pixel(x, y)) for y in range(H))
png = (b"\x89PNG\r\n\x1a\n"
       + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 6 if rounded else 2, 0, 0, 0))
       + chunk(b"IDAT", zlib.compress(raw, 9))
       + chunk(b"IEND", b""))
with open(args[0] if args else "assets/icon0.png", "wb") as f:
    f.write(png)
