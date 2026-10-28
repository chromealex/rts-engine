#!/usr/bin/env python3
"""Regenerates the synthetic textures of this Unity test project (all made here, nothing downloaded).

    python3 tests/data/unity_vfx/make_textures.py
"""
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', '..', '..', 'tools', 'importers', 'unity_vfx'))
import images  # noqa: E402

TEX = os.path.join(HERE, 'Assets', 'FX', 'Textures')


def make(w, h, fn):
    px = bytearray(w * h * 4)
    for y in range(h):
        for x in range(w):
            px[(y * w + x) * 4:(y * w + x) * 4 + 4] = bytes(max(0, min(255, int(v))) for v in fn(x, y))
    return images.Image(w, h, px)


def blob(u, v, r):
    d = math.sqrt(u * u + v * v) / r
    t = max(0.0, 1 - d)
    return t * t * (3 - 2 * t)


def fire_sheet(x, y):  # 4x4 flipbook: a flame blob that grows and fades over 16 frames
    cell, cx, cy = 32, x % 32, y % 32
    f = (y // cell) * 4 + x // cell
    k = blob(cx - 15.5, (cy - 15.5) * 0.8 - f * 0.3, 6 + f * 0.6) * (1 - f / 20)
    return (255, 200 + 55 * k, 120 * k, 255 * k)


def smoke(x, y):  # RGB only, black background: alpha comes from grayscale (import setting)
    n = (math.sin(x * 0.7) * math.cos(y * 0.5) + math.sin((x + y) * 0.31)) * 0.15 + 0.7
    g = 255 * blob(x - 31.5, y - 31.5, 30) * n
    return (g, g, g, 255)


def spark(x, y):
    k = blob(x - 15.5, y - 15.5, 15) ** 2
    return (255, 255, 255, 255 * k)


def ring(x, y):  # premultiplied alpha
    d = math.sqrt((x - 31.5) ** 2 + (y - 31.5) ** 2) / 31.5
    a = math.exp(-((d - 0.7) / 0.12) ** 2)
    return (120 * a, 200 * a, 255 * a, 255 * a)


if __name__ == '__main__':
    os.makedirs(TEX, exist_ok=True)
    images.write_png(os.path.join(TEX, 'fire_sheet.png'), make(128, 128, fire_sheet))
    images.write_tga(os.path.join(TEX, 'smoke.tga'), make(64, 64, smoke), rle=True)
    images.write_png(os.path.join(TEX, 'spark.png'), make(32, 32, spark))
    images.write_png(os.path.join(TEX, 'ring.png'), make(64, 64, ring))
    print('textures written to', TEX)
