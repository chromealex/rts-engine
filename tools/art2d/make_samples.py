#!/usr/bin/env python3
"""Procedural placeholder art + maps for the 2D sample games (templates/tiles2d, templates/km2d).

Everything is drawn from simple shapes with a fixed seed: original, license-free (CC0), and
reproducible.  Run from the repository root:

    python3 tools/art2d/make_samples.py

Requires Pillow.  Conventions it demonstrates (see docs/game-format.md, "2D games"):
  * sprite sheets: one row per facing (row 0 = east, clockwise on screen), one column per frame;
  * team colour: shades of magenta (r == b, g = 0) are replaced by the player's colour;
  * tile atlases: blob47 / mask16 / corner16 autotile sets in canonical mask order.
"""
import json
import math
import os
import random

from PIL import Image, ImageDraw, ImageFilter

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
T = 32  # tile / sprite cell size in pixels


def magenta(k):
    """Team-colour key of brightness k (0..1)."""
    v = int(max(0, min(1, k)) * 255)
    return (v, 0, v, 255)


def save(img, rel):
    path = os.path.join(ROOT, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    img.save(path, optimize=True)
    print("wrote", rel, img.size)


def write_json(obj, rel, header=None):
    path = os.path.join(ROOT, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    text = json.dumps(obj, indent=1, ensure_ascii=False)
    with open(path, "w", encoding="utf-8") as f:
        if header:
            f.write("// " + header + "\n")
        f.write(text + "\n")
    print("wrote", rel)


# ---------------------------------------------------------------- autotile masks
# 8-neighbour bits, same as modules/rts/RtsTiles.cpp
N, NE, E, SE, S, SW, W, NW = 1, 2, 4, 8, 16, 32, 64, 128


def blob_reduce(m):
    n, e, s, w = m & N, m & E, m & S, m & W
    if not (n and e): m &= ~NE
    if not (s and e): m &= ~SE
    if not (s and w): m &= ~SW
    if not (n and w): m &= ~NW
    return m


BLOB47 = sorted({blob_reduce(m) for m in range(256)})
assert len(BLOB47) == 47


def shape_mask(zones):
    """zones: set of names among c, n, e, s, w, ne, se, sw, nw -> smooth 32x32 alpha mask.
    Drawn on a padded canvas so set edges run seamlessly into the neighbour tile."""
    P = 8
    big = Image.new("L", (T + 2 * P, T + 2 * P), 0)
    d = ImageDraw.Draw(big)
    a, b = 9, 23  # inner square of the tile
    o = P
    rects = {
        "c": (a, a, b, b), "n": (a, -P, b, a), "s": (a, b, b, T + P), "w": (-P, a, a, b), "e": (b, a, T + P, b),
        "nw": (-P, -P, a, a), "ne": (b, -P, T + P, a), "sw": (-P, b, a, T + P), "se": (b, b, T + P, T + P),
    }
    for z in zones:
        x0, y0, x1, y1 = rects[z]
        d.rectangle((x0 + o, y0 + o, x1 + o - 1, y1 + o - 1), fill=255)
    big = big.filter(ImageFilter.GaussianBlur(2.6)).point(lambda v: 255 if v > 128 else 0)
    return big.crop((P, P, P + T, P + T))


def blob_zones(m):
    z = {"c"}
    for bit, name in ((N, "n"), (E, "e"), (S, "s"), (W, "w"), (NE, "ne"), (SE, "se"), (SW, "sw"), (NW, "nw")):
        if m & bit: z.add(name)
    return z


def mask16_zones(m):  # N=1 E=2 S=4 W=8; corners only when both edges are set
    z = {"c"}
    n, e, s, w = m & 1, m & 2, m & 4, m & 8
    if n: z.add("n")
    if e: z.add("e")
    if s: z.add("s")
    if w: z.add("w")
    if n and e: z.add("ne")
    if s and e: z.add("se")
    if s and w: z.add("sw")
    if n and w: z.add("nw")
    return z


def corner16_mask(m, rng):  # NE=1 SE=2 SW=4 NW=8: quadrant filled when its corner is set
    img = Image.new("L", (T, T), 0)
    d = ImageDraw.Draw(img)
    h = T // 2
    quads = {1: (h, 0, T, h), 2: (h, h, T, T), 4: (0, h, h, T), 8: (0, 0, h, h)}
    for bit, (x0, y0, x1, y1) in quads.items():
        if m & bit: d.rectangle((x0, y0, x1 - 1, y1 - 1), fill=255)
    d.ellipse((h - 8, h - 8, h + 8, h + 8), fill=255)
    return img


def textured(mask, color_fn, outline=None, seed=0):
    """Fill `mask` with a procedural texture; optional outline colour on the inner border."""
    rng = random.Random(seed)
    img = Image.new("RGBA", (T, T), (0, 0, 0, 0))
    px, mp = img.load(), mask.load()
    for y in range(T):
        for x in range(T):
            if mp[x, y] == 0: continue
            c = color_fn(x, y, rng)
            if outline:
                edge = any(0 <= x + dx < T and 0 <= y + dy < T and mp[x + dx, y + dy] == 0 for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)))
                if edge: c = outline
            px[x, y] = c
    return img


def noise_color(base, amp):
    def fn(x, y, rng):
        k = rng.uniform(-amp, amp)
        return tuple(max(0, min(255, int(c + k))) for c in base) + (255,)
    return fn


FULL = Image.new("L", (T, T), 255)


# ---------------------------------------------------------------- tiles2d: tileset
def make_desert_tileset():
    cols, rows = 8, 18
    atlas = Image.new("RGBA", (cols * T, rows * T), (0, 0, 0, 0))

    def put(i, tile):
        atlas.paste(tile, ((i % cols) * T, (i // cols) * T), tile)

    sand = (214, 180, 120)
    for v in range(4):  # 0-3 sand variants: noise + a few pebbles/dunes
        t = textured(FULL, noise_color(sand, 10), seed=10 + v)
        d = ImageDraw.Draw(t)
        r = random.Random(20 + v)
        for _ in range(3 + v):
            x, y = r.randrange(2, 30), r.randrange(2, 30)
            d.point((x, y), fill=(170, 140, 90, 255))
        if v == 3: d.arc((4, 10, 28, 26), 200, 340, fill=(190, 156, 100, 255))
        put(v, t)
    spice = (196, 98, 40)

    def spice_fn(x, y, rng):
        k = rng.random()
        return (spice[0] + int(k * 30), spice[1] + int(k * 25), spice[2], 255) if k > 0.3 else (226, 140, 60, 255)

    for v in range(2):  # 4-5 spice interior
        put(4 + v, textured(FULL, spice_fn, seed=30 + v))
    rock = (128, 112, 96)
    for v in range(2):  # 6-7 rock interior
        t = textured(FULL, noise_color(rock, 12), seed=40 + v)
        ImageDraw.Draw(t).line((5 + v * 6, 6, 14 + v * 4, 18, 12, 27), fill=(92, 80, 68, 255))
        put(6 + v, t)
    for i, m in enumerate(BLOB47):  # 8..54 rock blob47
        put(8 + i, textured(shape_mask(blob_zones(m)), noise_color(rock, 12), outline=(84, 72, 60, 255), seed=100 + i))
    water = (52, 104, 168)

    def water_fn(x, y, rng):
        wave = math.sin((x + y * 0.5) * 0.6) * 10
        return (water[0] + int(wave), water[1] + int(wave), water[2] + int(wave), 255)

    for i, m in enumerate(BLOB47):  # 56..102 water blob47 (foam shore)
        put(56 + i, textured(shape_mask(blob_zones(m)), water_fn, outline=(222, 236, 240, 255), seed=200 + i))
    for m in range(16):  # 104..119 spice corner16
        put(104 + m, textured(corner16_mask(m, None), spice_fn, seed=300 + m))
    cliff = (96, 70, 52)
    for m in range(16):  # 120..135 cliff mask16
        put(120 + m, textured(shape_mask(mask16_zones(m)), noise_color(cliff, 14), outline=(52, 36, 28, 255), seed=400 + m))
    for v in range(2):  # 136-137 deep water interior
        put(136 + v, textured(FULL, water_fn, seed=500 + v))
    save(atlas, "templates/tiles2d/tilesets/desert.png")

    ts = {
        "name": "Desert",
        "atlas": "tilesets/desert.png",
        "tileSize": [T, T], "columns": cols, "filter": "nearest",
        "terrains": [
            {"id": "sand", "name": "Sand", "char": "s", "cell": ".", "color": [0.84, 0.71, 0.47],
             "tiles": [0, 1, {"tile": 2, "weight": 0.6}, {"tile": 3, "weight": 0.25}]},
            {"id": "spice", "name": "Crystal field", "char": "o", "cell": ".", "color": [0.85, 0.45, 0.18],
             "field": {"resource": "crystal", "density": 9}, "under": "sand", "depleted": "sand",
             "tiles": [4, 5], "autotile": {"type": "corner16", "first": 104}},
            {"id": "rock", "name": "Rock", "char": "r", "cell": "R", "color": [0.5, 0.44, 0.38], "under": "sand",
             "tiles": [6, 7], "autotile": {"type": "blob47", "first": 8}},
            {"id": "cliff", "name": "Cliffs", "char": "c", "cell": "#", "color": [0.38, 0.27, 0.2], "under": "sand",
             "autotile": {"type": "mask16", "first": 120}},
            {"id": "water", "name": "Oasis", "char": "w", "cell": "~", "color": [0.2, 0.41, 0.66], "under": "sand",
             "tiles": [136, 137], "autotile": {"type": "blob47", "first": 56}},
        ],
    }
    write_json(ts, "templates/tiles2d/tilesets/desert.json", "Generated by tools/art2d/make_samples.py (placeholder art, CC0).")
    return ts


# ---------------------------------------------------------------- top-down sprites (tiles2d)
def rot(px, py, a, cx, cy):
    c, s = math.cos(a), math.sin(a)
    return (cx + px * c - py * s, cy + px * s + py * c)


def poly(d, pts, a, cx, cy, fill, outline=None):
    d.polygon([rot(x, y, a, cx, cy) for x, y in pts], fill=fill, outline=outline)


def explosion(d, cx, cy, k):
    """k in 0..1: fireball growing, then smoke."""
    r = (4 + k * 11) * 2   # sheets are drawn 2x supersampled
    if k < 0.6:
        d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=(250, 160, 40, 255))
        d.ellipse((cx - r * 0.55, cy - r * 0.55, cx + r * 0.55, cy + r * 0.55), fill=(255, 240, 150, 255))
    else:
        d.ellipse((cx - r, cy - r, cx + r, cy + r), fill=(60, 52, 46, 200))


def scorch(d, cx, cy):
    d.ellipse((cx - 18, cy - 14, cx + 18, cy + 14), fill=(50, 40, 32, 170))


def sheet(facings, frames, draw_fn, w=T, h=T):
    img = Image.new("RGBA", (frames * w, facings * h), (0, 0, 0, 0))
    for f in range(facings):
        a = f * 2 * math.pi / facings
        for k in range(frames):
            cell = Image.new("RGBA", (w * 2, h * 2), (0, 0, 0, 0))   # 2x supersampled, then reduced
            draw_fn(ImageDraw.Draw(cell), a, k, w * 2, h * 2)
            cell = cell.resize((w, h), Image.LANCZOS)
            img.paste(cell, (k * w, f * h), cell)
    return img


def tank_frame(d, a, k, W, H):
    cx, cy = W / 2, H / 2
    if k >= 5:  # die: 5..8
        explosion(d, cx, cy, (k - 5) / 3) if k < 8 else scorch(d, cx, cy)
        return
    tread = (40, 40, 40, 255)
    off = 2 if k in (2, 4) else 0
    for side in (-1, 1):
        poly(d, [(-22, side * 14 - 5), (22, side * 14 - 5), (22, side * 14 + 5), (-22, side * 14 + 5)], a, cx, cy, tread)
        for t in range(-18 + off, 20, 6):
            poly(d, [(t, side * 14 - 5), (t + 2, side * 14 - 5), (t + 2, side * 14 + 5), (t, side * 14 + 5)], a, cx, cy, (80, 80, 80, 255))
    poly(d, [(-18, -10), (18, -10), (20, 0), (18, 10), (-18, 10)], a, cx, cy, magenta(0.8), (60, 0, 60, 255))
    d.ellipse((cx - 9, cy - 9, cx + 9, cy + 9), fill=(150, 150, 140, 255), outline=(60, 60, 60, 255))
    recoil = -4 if k == 3 else 0
    poly(d, [(6 + recoil, -2.5), (30 + recoil, -2.5), (30 + recoil, 2.5), (6 + recoil, 2.5)], a, cx, cy, (110, 110, 105, 255))
    if k == 3:
        fx, fy = rot(34, 0, a, cx, cy)
        d.ellipse((fx - 5, fy - 5, fx + 5, fy + 5), fill=(255, 230, 120, 255))


def harvester_frame(d, a, k, W, H):
    cx, cy = W / 2, H / 2
    if k >= 5:
        explosion(d, cx, cy, (k - 5) / 3) if k < 8 else scorch(d, cx, cy)
        return
    poly(d, [(-24, -16), (20, -16), (20, 16), (-24, 16)], a, cx, cy, (60, 60, 56, 255))
    poly(d, [(-20, -13), (6, -13), (6, 13), (-20, 13)], a, cx, cy, magenta(0.75), (60, 0, 60, 255))
    poly(d, [(6, -11), (18, -11), (18, 11), (6, 11)], a, cx, cy, (200, 190, 150, 255))
    spin = k * 0.8
    for i in range(3):
        b = spin + i * 2.1
        poly(d, [(22, -12), (28, -12), (28, 12), (22, 12)], a, cx, cy, (120, 110, 90, 255))
        x, y = rot(25, math.sin(b) * 10, a, cx, cy)
        d.ellipse((x - 2, y - 2, x + 2, y + 2), fill=(230, 210, 120, 255))
    if k in (3, 4):  # work: crystal dust
        x, y = rot(32, 0, a, cx, cy)
        d.ellipse((x - 6, y - 6, x + 6, y + 6), fill=(230, 140, 60, 140))


def trooper_frame(d, a, k, W, H):
    cx, cy = W / 2, H / 2
    if k >= 7:  # die 7..9
        if k == 7: d.ellipse((cx - 7, cy - 7, cx + 7, cy + 7), fill=(170, 30, 30, 220))
        poly(d, [(-12, -5), (8, -5), (8, 5), (-12, 5)], a + 0.6, cx, cy, magenta(0.5))
        if k == 9: d.ellipse((cx - 10, cy - 6, cx + 10, cy + 6), fill=(110, 30, 30, 150))
        return
    step = [0, 5, 0, -5][(k - 1) % 4] if 1 <= k <= 4 else 0
    for side, ph in ((-1, step), (1, -step)):  # feet
        x, y = rot(ph, side * 6, a, cx, cy)
        d.ellipse((x - 3.5, y - 3.5, x + 3.5, y + 3.5), fill=(60, 50, 40, 255))
    poly(d, [(-6, -9), (6, -9), (6, 9), (-6, 9)], a, cx, cy, magenta(0.85), (50, 0, 50, 255))
    poly(d, [(2, 5), (20, 5), (20, 8), (2, 8)], a, cx, cy, (50, 50, 50, 255))   # rifle
    d.ellipse((cx - 5.5, cy - 5.5, cx + 5.5, cy + 5.5), fill=(230, 196, 150, 255), outline=(120, 90, 60, 255))
    if k == 6:
        x, y = rot(24, 6.5, a, cx, cy)
        d.ellipse((x - 4, y - 4, x + 4, y + 4), fill=(255, 230, 120, 255))


def building(fw, fh, style, frames=8):
    """Top-down building sheet: idle (0), build 1..4 (progress), die 5..7."""
    W, H = fw * T, fh * T
    img = Image.new("RGBA", (frames * W, H), (0, 0, 0, 0))
    for k in range(frames):
        c = Image.new("RGBA", (W, H), (0, 0, 0, 0))
        d = ImageDraw.Draw(c)
        if k >= 5:
            d.rectangle((3, 3, W - 4, H - 4), fill=(70, 60, 52, 255))
            for i in range(fw * fh):
                r = random.Random(k * 31 + i)
                x, y = r.randrange(4, W - 4), r.randrange(4, H - 4)
                d.ellipse((x - 5, y - 5, x + 5, y + 5), fill=(40, 34, 30, 255) if k == 7 else (240, 150, 50, 255))
        else:
            prog = 1.0 if k == 0 else k / 4
            d.rectangle((2, 2, W - 3, H - 3), fill=(110, 104, 96, 255), outline=(50, 46, 42, 255))  # slab
            if prog >= 0.5:
                inset = 6
                d.rectangle((inset, inset, W - inset - 1, H - inset - 1), fill=magenta(0.65), outline=(60, 0, 60, 255))
                style(d, W, H, prog)
            else:  # scaffolding
                for x in range(4, W - 4, 8):
                    d.line((x, 4, x, H - 5), fill=(160, 130, 80, 255), width=2)
                d.line((4, 4, W - 5, H - 5), fill=(160, 130, 80, 255), width=2)
        img.paste(c, (k * W, 0), c)
    return img


def yard_style(d, W, H, prog):
    d.rectangle((W * 0.3, H * 0.3, W * 0.7, H * 0.7), fill=(190, 186, 170, 255), outline=(60, 60, 60, 255))
    d.line((W * 0.5, H * 0.15, W * 0.5, H * 0.3), fill=(60, 60, 60, 255), width=3)
    d.rectangle((W * 0.1, H * 0.75, W * 0.3, H * 0.9), fill=(200, 170, 60, 255))


def refinery_style(d, W, H, prog):
    for i in range(2):
        cx = W * (0.3 + 0.4 * i)
        d.ellipse((cx - 12, H / 2 - 12, cx + 12, H / 2 + 12), fill=(210, 150, 70, 255), outline=(80, 50, 20, 255))


def windtrap_style(d, W, H, prog):
    d.ellipse((W / 2 - 14, H / 2 - 14, W / 2 + 14, H / 2 + 14), fill=(170, 190, 200, 255), outline=(60, 70, 80, 255))
    for i in range(4):
        a = i * math.pi / 2 + 0.4
        d.line((W / 2, H / 2, W / 2 + math.cos(a) * 14, H / 2 + math.sin(a) * 14), fill=(60, 70, 80, 255), width=3)


def barracks_style(d, W, H, prog):
    for y in range(int(H * 0.25), int(H * 0.8), 9):
        d.rectangle((W * 0.2, y, W * 0.8, y + 5), fill=(150, 140, 110, 255))


def factory_style(d, W, H, prog):
    d.rectangle((W * 0.15, H * 0.2, W * 0.6, H * 0.8), fill=(160, 160, 160, 255), outline=(60, 60, 60, 255))
    d.rectangle((W * 0.66, H * 0.35, W * 0.85, H * 0.65), fill=(60, 60, 60, 255))


def turret_sheet():
    def fn(d, a, k, W, H):
        cx, cy = W / 2, H / 2
        if k >= 3:
            explosion(d, cx, cy, (k - 3) / 2) if k < 5 else scorch(d, cx, cy)
            return
        d.rectangle((4, 4, W - 5, H - 5), fill=(110, 104, 96, 255), outline=(50, 46, 42, 255))
        d.ellipse((cx - 16, cy - 16, cx + 16, cy + 16), fill=magenta(0.7), outline=(60, 0, 60, 255))
        poly(d, [(4, -3), (28, -3), (28, 3), (4, 3)], a, cx, cy, (80, 80, 76, 255))
        if k == 2:
            x, y = rot(30, 0, a, cx, cy)
            d.ellipse((x - 5, y - 5, x + 5, y + 5), fill=(255, 230, 120, 255))
    return sheet(8, 6, fn)


def make_tiles2d_sprites():
    save(sheet(8, 9, tank_frame), "templates/tiles2d/sprites/tank.png")
    save(sheet(8, 9, harvester_frame), "templates/tiles2d/sprites/harvester.png")
    save(sheet(8, 10, trooper_frame), "templates/tiles2d/sprites/trooper.png")
    save(turret_sheet(), "templates/tiles2d/sprites/turret.png")
    save(building(3, 3, yard_style), "templates/tiles2d/sprites/yard.png")
    save(building(3, 2, refinery_style), "templates/tiles2d/sprites/refinery.png")
    save(building(2, 2, windtrap_style), "templates/tiles2d/sprites/windtrap.png")
    save(building(2, 2, barracks_style), "templates/tiles2d/sprites/barracks.png")
    save(building(3, 3, factory_style), "templates/tiles2d/sprites/factory.png")


# ---------------------------------------------------------------- tiles2d map
def make_tiles2d_map(ts):
    Wm, Hm = 48, 32
    grid = [["s"] * Wm for _ in range(Hm)]
    rng = random.Random(7)

    def disc(cx, cy, r, ch, jitter=0.0):
        for y in range(Hm):
            for x in range(Wm // 2):
                rr = r + (rng.uniform(-jitter, jitter) if jitter else 0)
                if (x - cx) ** 2 + (y - cy) ** 2 <= rr * rr: grid[y][x] = ch

    disc(8, 16, 6.2, "r", 0.6)                    # rock plateau for the base
    disc(15, 5, 3.2, "o", 0.5)                    # crystal fields
    disc(15, 27, 3.2, "o", 0.5)
    disc(5, 4, 2.6, "o", 0.3)
    disc(5, 28, 2.6, "o", 0.3)
    for y in list(range(2, 10)) + list(range(22, 30)):  # cliff ridges with passes
        grid[y][19] = "c"
        if y % 3: grid[y][20] = "c"
    for y in range(12, 20):
        if y not in (15, 16): grid[y][13] = "c" if grid[y][13] == "s" else grid[y][13]
    disc(23.5, 16, 3.2, "w", 0.4)                 # oasis (left half, mirrored)
    for y in range(Hm):                           # mirror to the right half
        for x in range(Wm // 2):
            grid[y][Wm - 1 - x] = grid[y][x]
    rows = ["".join(r) for r in grid]
    by_char = {t["char"]: t for t in ts["terrains"]}
    cells = ["".join(by_char[c]["cell"] for c in r) for r in rows]   # baked gameplay (the loader re-applies it)
    m = {
        "name": "Crystal Dunes",
        "size": [Wm, Hm],
        "players": [
            {"name": "Blue", "team": 1, "faction": "house", "start": [8.5, 16.5]},
            {"name": "Red", "team": 2, "faction": "house", "start": [39.5, 16.5]},
        ],
        "terrain": {"rows": cells},
        "tiles": {"tileset": "tilesets/desert.json", "rows": rows},
        # baked too, so tools that don't read tilesets see the fields (the loader re-applies them)
        "fields": [{"resource": "crystal", "fromTiles": True,
                    "rows": ["".join(str(by_char[c]["field"]["density"]) if "field" in by_char[c] else "0" for c in r) for r in rows]}],
        "entities": [],
    }
    write_json(m, "templates/tiles2d/maps/dunes.json", "Generated by tools/art2d/make_samples.py — edit in the editor (Tiles window) or regenerate.")


# ---------------------------------------------------------------- km2d: 3/4-view sprites
PITCH = math.radians(50)


def person(d, cx, cy, a, k, walk, weapon, scale=2, team=0.8):
    """3/4-view little person; facing a (screen angle, 0 = east, clockwise)."""
    fx, fy = math.cos(a), math.sin(a) * math.sin(PITCH)
    s = scale
    step = math.sin(k * math.pi / 2) * 3 * s if walk else 0
    for side in (-1, 1):  # legs
        lx = cx + side * 2.5 * s - fy * 0.5 * s
        d.line((lx, cy - 7 * s, lx + fx * step * side, cy), fill=(70, 50, 36, 255), width=int(2 * s))
    d.ellipse((cx - 5 * s, cy - 17 * s, cx + 5 * s, cy - 6 * s), fill=magenta(team), outline=(60, 0, 60, 255))  # tunic
    hx, hy = cx + fx * 1.0 * s, cy - 20 * s
    d.ellipse((hx - 3.2 * s, hy - 3.2 * s, hx + 3.2 * s, hy + 3.2 * s), fill=(232, 196, 156, 255), outline=(120, 90, 60, 255))
    if fy < 0.1:  # back of the head when walking away
        d.chord((hx - 3.2 * s, hy - 3.2 * s, hx + 3.2 * s, hy + 3.2 * s), 180, 360, fill=(110, 70, 40, 255))
    if weapon == "sword":
        swing = 0.9 if k == 1 else 0.0
        ax, ay = cx + fx * 4 * s, cy - 12 * s
        bx, by = ax + math.cos(a - 0.9 + swing * 1.8) * 9 * s, ay + math.sin(a - 0.9 + swing * 1.8) * 6 * s - 4 * s
        d.line((ax, ay, bx, by), fill=(200, 205, 215, 255), width=int(1.5 * s))
    elif weapon == "bow":
        ax, ay = cx + fx * 5 * s, cy - 13 * s
        d.arc((ax - 4 * s, ay - 6 * s, ax + 4 * s, ay + 6 * s), -70 if fx >= 0 else 110, 70 if fx >= 0 else 250, fill=(120, 80, 40, 255), width=int(1.5 * s))
    elif weapon == "tool":
        ax, ay = cx + fx * 4 * s, cy - 11 * s
        up = -4 * s if k == 1 else 0
        d.line((ax, ay, ax + fx * 6 * s, ay - 5 * s + up), fill=(120, 80, 40, 255), width=int(1.5 * s))
        d.line((ax + fx * 6 * s - 2 * s, ay - 5 * s + up, ax + fx * 6 * s + 2 * s, ay - 5 * s + up), fill=(150, 150, 160, 255), width=int(2 * s))


def km_unit_sheet(weapon):
    # columns: idle 0, move 1-4, attack 5-6 (work for the peasant), die 7-9
    def fn(d, a, k, W, H):
        cx, cy = W / 2, H - 4
        d.ellipse((cx - 10, cy - 4, cx + 10, cy + 4), fill=(0, 0, 0, 0))
        if k >= 7:
            j = k - 7
            if j < 2:
                person(d, cx + j * 6, cy, a, 0, False, weapon, team=0.6 - j * 0.2)
            else:
                d.ellipse((cx - 14, cy - 6, cx + 14, cy + 2), fill=magenta(0.45))
                d.ellipse((cx + 8, cy - 8, cx + 16, cy), fill=(232, 196, 156, 255))
            return
        if 1 <= k <= 4: person(d, cx, cy, a, k, True, weapon)
        elif k in (5, 6): person(d, cx, cy, a, k - 5, False, weapon)
        else: person(d, cx, cy, a, 0, False, weapon)
    return sheet(8, 10, fn)


def km_building(fw, fh, wall, roof_h, style):
    """3/4-view building: ground footprint (fw x fh cells seen at PITCH) + front wall + roof.
    Image width fw*32, anchor bottom centre = front edge of the footprint. Frames: idle, build 1-4, die 5-7."""
    W = fw * T
    depth = fh * T * math.sin(PITCH)
    wall_h = wall * T * math.cos(PITCH)
    H = int(math.ceil(depth + wall_h + roof_h * T * math.cos(PITCH) + 4))
    frames = 8
    img = Image.new("RGBA", (frames * W, H), (0, 0, 0, 0))
    for k in range(frames):
        c = Image.new("RGBA", (W * 2, H * 2), (0, 0, 0, 0))
        d = ImageDraw.Draw(c)
        S = 2
        base = H * S - 1
        prog = 1.0 if k == 0 else min(1.0, k / 4) if k < 5 else 1.0
        wh = wall_h * S * (prog if k < 5 else 1.0)
        if k >= 5:  # ruins
            d.polygon([(4, base), (W * S - 4, base), (W * S - 10, base - depth * S * 0.7), (10, base - depth * S * 0.7)], fill=(90, 80, 70, 255))
            for i in range(6):
                r = random.Random(k * 13 + i)
                x, y = r.randrange(8, W * S - 8), base - r.randrange(4, int(depth * S * 0.7))
                col = (240, 150, 50, 255) if k == 5 else (60, 50, 44, 255)
                d.ellipse((x - 8, y - 6, x + 8, y + 6), fill=col)
            if k == 7:
                d.rectangle((W * S * 0.3, base - depth * S * 0.6 - 10, W * S * 0.38, base - depth * S * 0.3), fill=(70, 60, 50, 255))
        else:
            front = base
            back = base - depth * S
            d.polygon([(2, front), (W * S - 2, front), (W * S - 2, back), (2, back)], fill=(122, 104, 78, 255))  # yard
            inset = 6 * S
            fl, fr = inset, W * S - inset
            fb = front - 6 * S          # front wall bottom
            bb = back + 6 * S
            if prog < 0.5:
                for x in range(int(fl), int(fr), 10 * S):
                    d.line((x, fb, x, fb - wall_h * S * 1.2), fill=(170, 130, 80, 255), width=2 * S)
                d.line((fl, fb - wall_h * S * 0.6, fr, fb - wall_h * S * 0.6), fill=(170, 130, 80, 255), width=2 * S)
            else:
                d.rectangle((fl, fb - wh, fr, fb), fill=(214, 200, 170, 255), outline=(90, 76, 60, 255))          # front wall
                d.polygon([(fl, fb - wh), (fr, fb - wh), (fr, bb - wh), (fl, bb - wh)], fill=(196, 182, 152, 255))  # wall top
                rh = roof_h * T * math.cos(PITCH) * S
                d.polygon([(fl - 3 * S, fb - wh), (fr + 3 * S, fb - wh), (fr - 8 * S, bb - wh - rh), (fl + 8 * S, bb - wh - rh)],
                          fill=magenta(0.7), outline=(70, 0, 70, 255))                                            # team roof
                style(d, fl, fr, fb, wh, S)
        c = c.resize((W, H), Image.LANCZOS)
        img.paste(c, (k * W, 0), c)
    return img, W, H


def door(d, fl, fr, fb, wh, S):
    cx = (fl + fr) / 2
    d.rectangle((cx - 5 * S, fb - min(wh, 16 * S), cx + 5 * S, fb), fill=(90, 60, 36, 255))


def windows(d, fl, fr, fb, wh, S):
    door(d, fl, fr, fb, wh, S)
    for x in (fl + 8 * S, fr - 14 * S):
        d.rectangle((x, fb - wh * 0.7, x + 6 * S, fb - wh * 0.45), fill=(60, 80, 110, 255))


def km_tree():
    W, H = T, int(T * 1.6)
    frames = 1
    img = Image.new("RGBA", (W * frames, H), (0, 0, 0, 0))
    c = Image.new("RGBA", (W * 2, H * 2), (0, 0, 0, 0))
    d = ImageDraw.Draw(c)
    d.rectangle((W - 4, H * 2 - 24, W + 4, H * 2 - 2), fill=(100, 70, 44, 255))
    for i, (r, y, col) in enumerate(((26, H * 2 - 40, (40, 100, 44)), (21, H * 2 - 62, (56, 124, 52)), (14, H * 2 - 80, (72, 146, 64)))):
        d.ellipse((W - r, y - r, W + r, y + r), fill=col + (255,))
    c = c.resize((W, H), Image.LANCZOS)
    img.paste(c, (0, 0), c)
    return img


def km_mine():
    W, H = 2 * T, int(2 * T * 0.9)
    c = Image.new("RGBA", (W * 2, H * 2), (0, 0, 0, 0))
    d = ImageDraw.Draw(c)
    d.ellipse((6, H * 0.4, W * 2 - 6, H * 2 - 4), fill=(110, 104, 98, 255), outline=(60, 56, 52, 255))
    d.ellipse((W * 0.7, H * 1.1, W * 1.3, H * 1.9), fill=(30, 26, 22, 255))
    for x, y in ((40, 70), (90, 60), (70, 95)):
        d.polygon([(x, y), (x + 8, y - 6), (x + 16, y), (x + 8, y + 6)], fill=(250, 210, 60, 255))
    return c.resize((W, H), Image.LANCZOS)


def make_km2d_sprites():
    save(km_unit_sheet("tool"), "templates/km2d/sprites/peasant.png")
    save(km_unit_sheet("sword"), "templates/km2d/sprites/swordsman.png")
    save(km_unit_sheet("bow"), "templates/km2d/sprites/archer.png")
    info = {}
    for name, fw, fh, wall, roof, style in (("hall", 3, 3, 1.3, 0.9, windows), ("house", 2, 2, 0.9, 0.7, door),
                                            ("barracks", 3, 3, 1.1, 0.5, door), ("tower", 2, 2, 2.2, 0.8, windows)):
        img, W, H = km_building(fw, fh, wall, roof, style)
        save(img, f"templates/km2d/sprites/{name}.png")
        info[name] = (W, H)
    save(km_tree(), "templates/km2d/sprites/tree.png")
    save(km_mine(), "templates/km2d/sprites/goldmine.png")
    return info


def make_km2d_map():
    Wm, Hm = 48, 36
    rng = random.Random(11)
    rows = [["."] * Wm for _ in range(Hm)]
    levels = [["0"] * Wm for _ in range(Hm)]
    heights = [0] * (Wm * Hm)
    # rolling fine heights (visual) + a central hill plateau (level 1) with ramps
    for y in range(Hm):
        for x in range(Wm):
            h = 10 + 6 * math.sin(x * 0.31) * math.cos(y * 0.27) + 4 * math.sin((x + y) * 0.17)
            heights[y * Wm + x] = max(0, int(h))  # 1/16 units; >= 0 keeps dry land above the water sheet
    for y in range(Hm):
        for x in range(Wm):
            dx, dy = (x - 23.5) / 6.5, (y - 17.5) / 4.5
            if dx * dx + dy * dy <= 1.0: levels[y][x] = "1"
    for (x, y) in ((16, 17), (16, 18), (31, 17), (31, 18), (23, 12), (24, 12), (23, 23), (24, 23)):
        rows[y][x] = "/"
    for y in range(Hm):  # pond
        for x in range(Wm):
            for py in (4, 31):
                d = (x - 23.5) ** 2 / 9 + (y - py) ** 2 / 2.5
                if d <= 1: rows[y][x] = "~"
                if d <= 2.2: heights[y * Wm + x] = 0   # shore down to the water sheet
    ents = []
    for side in (0, 1):
        def X(x): return x if side == 0 else Wm - x
        ents.append({"type": "goldmine", "owner": 0, "pos": [X(7), 8]})
        ents.append({"type": "goldmine", "owner": 0, "pos": [X(7), 28]})
        for i in range(26):  # forest along the map edge
            r = random.Random(100 + i)
            ents.append({"type": "tree", "owner": 0, "pos": [X(1.5 + r.random() * 3) + (0 if side == 0 else 0), 1.5 + i * 1.3]})
        for i in range(10):
            r = random.Random(300 + i)
            ents.append({"type": "tree", "owner": 0, "pos": [X(13 + r.random() * 3), 3 + i * 0.9]})
    for e in ents:
        e["pos"] = [math.floor(e["pos"][0]) + 0.5, math.floor(e["pos"][1]) + 0.5]
        if e["type"] == "goldmine": e["pos"] = [round(e["pos"][0] - 0.5), round(e["pos"][1] - 0.5)]
    m = {
        "name": "Green Hills",
        "size": [Wm, Hm],
        "players": [
            {"name": "Blue", "team": 1, "faction": "realm", "start": [10.5, 18.5]},
            {"name": "Red", "team": 2, "faction": "realm", "start": [37.5, 18.5]},
        ],
        "terrain": {"rows": ["".join(r) for r in rows], "levels": ["".join(r) for r in levels], "heights": heights},
        "entities": ents,
    }
    write_json(m, "templates/km2d/maps/hills.json", "Generated by tools/art2d/make_samples.py.")


if __name__ == "__main__":
    ts = make_desert_tileset()
    make_tiles2d_sprites()
    make_tiles2d_map(ts)
    make_km2d_sprites()
    make_km2d_map()
