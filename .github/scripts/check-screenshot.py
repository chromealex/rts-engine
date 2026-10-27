#!/usr/bin/env python3
"""Converts PPM screenshots to PNG and fails if a frame looks broken (blank or a single colour).

Usage: check-screenshot.py shot-a.ppm [shot-b.ppm ...]   (also accepts .png)
No dependencies: reads PPM directly and writes PNG with zlib.
"""
import struct, sys, zlib


def read_ppm(path):
    data = open(path, "rb").read()
    parts, pos = [], 0
    while len(parts) < 4:  # magic, width, height, maxval (comments not produced by the engine)
        while data[pos:pos + 1].isspace():
            pos += 1
        end = pos
        while not data[end:end + 1].isspace():
            end += 1
        parts.append(data[pos:end]); pos = end
    if parts[0] != b"P6":
        raise ValueError(f"{path}: not a binary PPM")
    w, h = int(parts[1]), int(parts[2])
    return w, h, data[pos + 1:pos + 1 + w * h * 3]


def write_png(path, w, h, rgb):
    raw = b"".join(b"\0" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    def chunk(tag, body):
        return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def check(path):
    w, h, rgb = read_ppm(path)
    if len(rgb) != w * h * 3 or w == 0:
        return f"{path}: truncated image"
    # Sample a grid; a rendered match has terrain, units and UI, so many distinct colours.
    colours, lum = set(), []
    for y in range(0, h, max(1, h // 64)):
        for x in range(0, w, max(1, w // 64)):
            i = (y * w + x) * 3
            px = rgb[i:i + 3]
            colours.add(px)
            lum.append(px[0] * 0.3 + px[1] * 0.59 + px[2] * 0.11)
    mean = sum(lum) / len(lum)
    write_png(path.rsplit(".", 1)[0] + ".png", w, h, rgb)
    print(f"{path}: {w}x{h}, {len(colours)} distinct sampled colours, mean luminance {mean:.0f}")
    if len(colours) < 32:
        return f"{path}: frame is (almost) a single colour — rendering is broken"
    if mean < 8:
        return f"{path}: frame is black"
    return None


errors = [e for e in (check(p) for p in sys.argv[1:]) if e]
for e in errors:
    print(f"::error::{e}")
sys.exit(1 if errors or len(sys.argv) < 2 else 0)
