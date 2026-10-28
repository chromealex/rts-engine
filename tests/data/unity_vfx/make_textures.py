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
MOD_TEX = os.path.join(HERE, 'Assets', 'Modules', 'Textures')


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


def vnoise(x, y, seed):
    def h(i, j):
        n = (i * 374761393 + j * 668265263 + seed * 2246822519) & 0xffffffff
        n = ((n ^ (n >> 13)) * 1274126177) & 0xffffffff
        return ((n ^ (n >> 16)) & 0xffff) / 65535.0
    xi, yi = math.floor(x), math.floor(y)
    fx, fy = x - xi, y - yi
    fx, fy = fx * fx * (3 - 2 * fx), fy * fy * (3 - 2 * fy)
    a, b, c, d = h(xi, yi), h(xi + 1, yi), h(xi, yi + 1), h(xi + 1, yi + 1)
    return (a + (b - a) * fx) * (1 - fy) + (c + (d - c) * fx) * fy


def fbm(x, y, seed):
    return sum(vnoise(x * f, y * f, seed + o) * a for o, (f, a) in enumerate(((1, 0.5), (2, 0.25), (4, 0.125), (8, 0.0625)))) / 0.9375


def fire_sheet(x, y):  # 4x4 flipbook: a turbulent flame ball that grows, cools and breaks up over 16 frames
    cell, cx, cy = 32, x % 32, y % 32
    f = (y // cell) * 4 + x // cell
    r = 9 + f * 0.45
    n = fbm(cx / 7.0 + f * 0.37, cy / 7.0 - f * 0.21, 3)
    k = blob(cx - 15.5, cy - 15.5, r) * (0.55 + 0.9 * n) * (1 - f / 18)
    k = max(0.0, min(1.0, k))
    return (255, 120 + 135 * k, 40 + 140 * k * k, 255 * k)


def smoke(x, y):  # RGB only, black background: alpha comes from grayscale (import setting)
    n = fbm(x / 9.0, y / 9.0, 11)
    g = 255 * blob(x - 31.5, y - 31.5, 31) * max(0.0, min(1.0, 0.2 + 1.1 * n))
    return (g, g, g, 255)


def spark(x, y):
    k = blob(x - 15.5, y - 15.5, 15) ** 2
    return (255, 255, 255, 255 * k)


def ring(x, y):  # premultiplied alpha
    d = math.sqrt((x - 31.5) ** 2 + (y - 31.5) ** 2) / 31.5
    a = math.exp(-((d - 0.7) / 0.12) ** 2)
    return (120 * a, 200 * a, 255 * a, 255 * a)


def sheet(x, y):  # two sprites side by side (multiple sprite mode): a red star left, a green disc right
    if x < 32:
        u, v = x - 15.5, y - 15.5
        a = math.atan2(v, u)
        k = blob(u, v, 9 + 5 * abs(math.cos(a * 2.5)))
        return (255, 80, 60, 255 * k)
    k = blob(x - 47.5, y - 15.5, 14)
    return (80, 255, 120, 255 * k)


def dot(x, y):  # a single sprite (sprite mode single)
    k = blob(x - 7.5, y - 7.5, 8)
    return (255, 255, 255, 255 * k)


if __name__ == '__main__':
    os.makedirs(TEX, exist_ok=True)
    images.write_png(os.path.join(TEX, 'fire_sheet.png'), make(128, 128, fire_sheet))
    images.write_tga(os.path.join(TEX, 'smoke.tga'), make(64, 64, smoke), rle=True)
    images.write_png(os.path.join(TEX, 'spark.png'), make(32, 32, spark))
    images.write_png(os.path.join(TEX, 'ring.png'), make(64, 64, ring))
    os.makedirs(MOD_TEX, exist_ok=True)
    images.write_png(os.path.join(MOD_TEX, 'sheet.png'), make(64, 32, sheet))
    images.write_png(os.path.join(MOD_TEX, 'dot.png'), make(16, 16, dot))
    print('textures written to', TEX, 'and', MOD_TEX)
