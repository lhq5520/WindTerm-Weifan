#!/usr/bin/env python3
"""Generates the application icons from one large RGBA PNG, using only the standard library.

    python3 tools/make_icons.py images/WindTerm_icon_1024x1024.png app/resources/icons

Writes hicolor-style PNGs (16..512 px), windterm-weifan.ico (Windows, PNG-compressed entries)
and windterm-weifan.icns (macOS, PNG entries). Re-run after changing the source icon.
"""

import struct
import sys
import zlib
from pathlib import Path


def read_png(path):
    data = Path(path).read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit("not a PNG file")
    pos = 8
    idat = b""
    width = height = None
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            width, height, depth, color, _, _, interlace = struct.unpack(">IIBBBBB", body)
            if depth != 8 or color != 6 or interlace != 0:
                raise SystemExit("need an 8-bit, non-interlaced RGBA PNG")
        elif kind == b"IDAT":
            idat += body
        elif kind == b"IEND":
            break
    raw = zlib.decompress(idat)
    stride = width * 4
    pixels = bytearray(width * height * 4)
    previous = bytearray(stride)
    offset = 0
    for y in range(height):
        kind = raw[offset]
        line = bytearray(raw[offset + 1:offset + 1 + stride])
        offset += 1 + stride
        for x in range(stride):
            left = line[x - 4] if x >= 4 else 0
            up = previous[x]
            upper_left = previous[x - 4] if x >= 4 else 0
            if kind == 1:
                line[x] = (line[x] + left) & 0xFF
            elif kind == 2:
                line[x] = (line[x] + up) & 0xFF
            elif kind == 3:
                line[x] = (line[x] + (left + up) // 2) & 0xFF
            elif kind == 4:
                p = left + up - upper_left
                pa, pb, pc = abs(p - left), abs(p - up), abs(p - upper_left)
                predictor = left if pa <= pb and pa <= pc else up if pb <= pc else upper_left
                line[x] = (line[x] + predictor) & 0xFF
        pixels[y * stride:(y + 1) * stride] = line
        previous = line
    return width, height, pixels


def downscale(width, height, pixels, size):
    """Box filter with premultiplied alpha (no dark fringes)."""
    factor = width // size
    out = bytearray(size * size * 4)
    for oy in range(size):
        for ox in range(size):
            r = g = b = a = 0
            for y in range(oy * factor, (oy + 1) * factor):
                row = y * width * 4
                for x in range(ox * factor, (ox + 1) * factor):
                    i = row + x * 4
                    alpha = pixels[i + 3]
                    r += pixels[i] * alpha
                    g += pixels[i + 1] * alpha
                    b += pixels[i + 2] * alpha
                    a += alpha
            count = factor * factor
            j = (oy * size + ox) * 4
            if a:
                out[j:j + 4] = bytes((round(r / a), round(g / a), round(b / a), round(a / count)))
    return out


def write_png(size, pixels):
    rows = b"".join(b"\x00" + bytes(pixels[y * size * 4:(y + 1) * size * 4]) for y in range(size))

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))


def main():
    source, target = Path(sys.argv[1]), Path(sys.argv[2])
    target.mkdir(parents=True, exist_ok=True)
    width, height, pixels = read_png(source)
    if width != height or width % 512:
        raise SystemExit("need a square icon whose size is a multiple of 512")
    pngs = {}
    for size in (16, 32, 48, 64, 128, 256, 512):
        pngs[size] = write_png(size, downscale(width, height, pixels, size))
        (target / f"windterm-weifan-{size}.png").write_bytes(pngs[size])
    pngs[1024] = source.read_bytes()

    # Windows .ico with PNG payloads (Vista and later).
    sizes = (16, 32, 48, 256)
    header = struct.pack("<HHH", 0, 1, len(sizes))
    entries = b""
    blobs = b""
    offset = 6 + 16 * len(sizes)
    for size in sizes:
        blob = pngs[size]
        entries += struct.pack("<BBBBHHII", size % 256, size % 256, 0, 0, 1, 32, len(blob), offset)
        blobs += blob
        offset += len(blob)
    (target / "windterm-weifan.ico").write_bytes(header + entries + blobs)

    # macOS .icns with PNG payloads (10.7 and later).
    types = [(b"icp4", 16), (b"icp5", 32), (b"icp6", 64), (b"ic07", 128), (b"ic08", 256), (b"ic09", 512),
             (b"ic10", 1024)]
    body = b"".join(kind + struct.pack(">I", 8 + len(pngs[size])) + pngs[size] for kind, size in types)
    (target / "windterm-weifan.icns").write_bytes(b"icns" + struct.pack(">I", 8 + len(body)) + body)
    print(f"wrote icons to {target}")


if __name__ == "__main__":
    main()
