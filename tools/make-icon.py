#!/usr/bin/env python3
"""Generate assets/icon.png -- the icon Sileo shows for the package.

Pure stdlib: renders at 4x and box-downsamples for antialiasing, then writes a
PNG by hand, so the build needs no image libraries.
"""
import os
import struct
import zlib

S = 256          # final size
SS = 4           # supersample factor
N = S * SS

BG = (0x1E, 0x1D, 0x1B)      # charcoal
FG = (0xE0, 0x86, 0x62)      # clay
DIM = (0x8A, 0x52, 0x3C)

R = int(N * 0.225)           # corner radius


def inside_rounded(x, y):
    if R <= x < N - R or R <= y < N - R:
        return 0 <= x < N and 0 <= y < N
    cx = R if x < R else N - R - 1
    cy = R if y < R else N - R - 1
    return (x - cx) ** 2 + (y - cy) ** 2 <= R * R


def stroke(buf, x0, y0, x1, y1, w, color):
    """Filled capsule from (x0,y0) to (x1,y1), radius w."""
    dx, dy = x1 - x0, y1 - y0
    L2 = dx * dx + dy * dy
    lo_x, hi_x = int(min(x0, x1) - w - 1), int(max(x0, x1) + w + 2)
    lo_y, hi_y = int(min(y0, y1) - w - 1), int(max(y0, y1) + w + 2)
    for y in range(max(0, lo_y), min(N, hi_y)):
        for x in range(max(0, lo_x), min(N, hi_x)):
            t = 0.0 if L2 == 0 else ((x - x0) * dx + (y - y0) * dy) / L2
            t = 0.0 if t < 0 else (1.0 if t > 1 else t)
            px, py = x0 + t * dx, y0 + t * dy
            if (x - px) ** 2 + (y - py) ** 2 <= w * w:
                buf[y * N + x] = color


def main():
    buf = [None] * (N * N)
    for y in range(N):
        for x in range(N):
            if inside_rounded(x, y):
                buf[y * N + x] = BG

    u = N / 100.0
    w = 5.5 * u
    # chevron  >
    stroke(buf, 30 * u, 32 * u, 52 * u, 50 * u, w, FG)
    stroke(buf, 52 * u, 50 * u, 30 * u, 68 * u, w, FG)
    # cursor underscore
    stroke(buf, 58 * u, 68 * u, 76 * u, 68 * u, w, DIM)

    # downsample with alpha from coverage
    out = bytearray()
    k = SS * SS
    for y in range(S):
        out.append(0)
        for x in range(S):
            r = g = b = a = 0
            for j in range(SS):
                row = (y * SS + j) * N + x * SS
                for i in range(SS):
                    p = buf[row + i]
                    if p is not None:
                        r += p[0]; g += p[1]; b += p[2]; a += 255
            if a:
                n = a // 255
                out += bytes((r // n, g // n, b // n, a // k))
            else:
                out += b"\0\0\0\0"

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", S, S, 8, 6, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(bytes(out), 9))
           + chunk(b"IEND", b""))

    dst = os.path.join(os.path.dirname(__file__), "..", "assets", "icon.png")
    with open(dst, "wb") as f:
        f.write(png)
    print("wrote %s (%d bytes, %dx%d)" % (os.path.normpath(dst), len(png), S, S))


main()
