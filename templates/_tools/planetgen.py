#!/usr/bin/env python3
"""Map generator for templates/planets (deterministic, no randomness): a planet and a moon (docs/planets.md).

    python3 templates/_tools/planetgen.py

Heights are a smooth function of the 3D direction, so corners shared by faces get (almost) the same value;
the engine averages seam duplicates on load anyway. Everything placed for player 1 on face 4 (+Z) is
mirrored for player 2 on face 5 (-Z).
"""
import json, math, os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FACES = [((1,0,0),(0,0,-1),(0,-1,0)), ((-1,0,0),(0,0,1),(0,-1,0)), ((0,1,0),(1,0,0),(0,0,1)),
         ((0,-1,0),(1,0,0),(0,0,-1)), ((0,0,1),(1,0,0),(0,-1,0)), ((0,0,-1),(-1,0,0),(0,-1,0))]

def direction(face, a, b, n):
    nn, u, v = FACES[face]
    s = math.tan((2 * a / n - 1) * math.pi / 4); t = math.tan((2 * b / n - 1) * math.pi / 4)
    p = [nn[i] + u[i] * s + v[i] * t for i in range(3)]
    l = math.sqrt(sum(c * c for c in p)); return [c / l for c in p]

def corners(n, height):
    out = []
    for f in range(6):
        for j in range(n + 1):
            for i in range(n + 1):
                out.append(int(round(height(direction(f, i, j, n)) * 16)))
    return out

def terra_height(d):
    x, y, z = d
    # rolling hills, a ridge ring around the equator (y ~ 0) with passes, flat bases at the poles of z
    h = 0.6 * math.sin(3 * x + 1.0) * math.cos(2 * y) + 0.4 * math.sin(4 * y + 2 * x)
    ridge = max(0.0, 1.0 - abs(z) / 0.18) * 2.2 * (0.5 + 0.5 * math.cos(6 * math.atan2(y, x)))
    base = min(1.0, max(0.0, (abs(z) - 0.75) / 0.15))      # 1 near the base centres
    return (h + ridge) * (1 - base)

def luna_height(d):
    x, y, z = d
    return 0.5 * math.sin(5 * x) * math.sin(4 * z) + 0.3 * math.cos(6 * y)

def at(body, face, x, y): return {"body": body, "face": face, "x": x, "y": y}

def mirror(face, x, y, n):
    # face 4 (+Z) <-> face 5 (-Z): u flips sign, v stays: point-symmetric through the planet centre
    return (5 if face == 4 else 4), n - x, y

def build():
    N, M = 24, 12
    ents = []
    def both(t, x, y, owner=0):
        ents.append({"type": t, "owner": owner, "at": at("terra", 4, x, y)})
        f, mx, my = mirror(4, x, y, N)
        ents.append({"type": t, "owner": (3 - owner) if owner else 0, "at": at("terra", f, mx, my)})
    both("commander", 12.5, 12.5, 1)
    for (x, y) in [(7, 8), (17, 8), (7, 17), (17, 17), (12, 4)]: both("metal_spot", x, y)
    for (x, y) in [(4.5, 12.5), (20.5, 12.5)]: both("wreck_rock", x, y)
    # contested spots on the equator faces and the moon (reachable by orbit units only)
    for f in (0, 1, 2, 3):
        ents.append({"type": "metal_spot", "owner": 0, "at": at("terra", f, 12, 12)})
    for f in range(6):
        ents.append({"type": "wreck_rock", "owner": 0, "at": at("luna", f, 6.5, 6.5)})
    return {
        "name": "Two Worlds",
        "players": [{"name": "Синие", "team": 1, "start": at("terra", 4, 12.5, 12.5)},
                    {"name": "Красные", "team": 2, "start": at("terra", 5, 11.5, 12.5)}],
        "world": {
            "start": "terra",
            "bodies": [
                {"id": "sun", "kind": "star", "radius": 40, "visual": {"color": [255, 220, 160]}},
                {"id": "terra", "kind": "planet", "radius": 16, "faceSize": N, "seaLevel": -2, "orbitAltitude": 4,
                 "orbit": {"around": "sun", "radius": 400, "period": 1200, "phase": 0.1},
                 "terrain": {"corners": corners(N, terra_height)}},
                {"id": "luna", "kind": "moon", "radius": 8, "faceSize": M, "orbitAltitude": 3,
                 "orbit": {"around": "terra", "radius": 60, "period": 300},
                 "terrain": {"corners": corners(M, luna_height)}},
            ],
        },
        "entities": ents,
    }

def compact(obj):
    s = json.dumps(obj, ensure_ascii=False, indent=1)
    # keep long number arrays on few lines
    import re
    return re.sub(r'\[\s+(-?\d+(?:,\s+-?\d+)*)\s+\]', lambda m: '[' + ', '.join(m.group(1).split(',\n')).replace('\n', '').replace('  ', ' ') + ']', s)

def build_big():
    """Big Planet: faceSize 128 (98k cells: planet HPA* active), rocky obstacles and lakes, groups of 8+ and the
    scripted swarms march along shared flow fields (map movement.flowField)."""
    import random
    N = 128
    rng = random.Random(11)
    rows = [['.'] * N for _ in range(6 * N)]
    for _ in range(6 * N * N // 90):
        f, x, y, r = rng.randrange(6), rng.randrange(N), rng.randrange(N), rng.randint(1, 4)
        if f in (4, 5) and abs(x - N / 2) < 16 and abs(y - N / 2) < 16: continue   # keep the bases clear
        ch = '#' if rng.random() < 0.65 else '~'
        for dy in range(-r, r + 1):
            for dx in range(-r, r + 1):
                if 0 <= x + dx < N and 0 <= y + dy < N and dx * dx + dy * dy <= r * r: rows[f * N + y + dy][x + dx] = ch
    faces = [{"rows": [''.join(rows[f * N + y]) for y in range(N)]} for f in range(6)]
    ents = []
    c = N / 2 + 0.5
    def both(t, x, y, owner=0):
        ents.append({"type": t, "owner": owner, "at": at("terra", 4, x, y)})
        f, mx, my = mirror(4, x, y, N)
        ents.append({"type": t, "owner": (3 - owner) if owner else 0, "at": at("terra", f, mx, my)})
    both("commander", c, c, 1)
    for (dx, dy) in [(-6, -6), (6, -6), (-6, 6), (6, 6), (0, -10), (0, 10)]: both("metal_spot", c + dx, c + dy)
    for (dx, dy) in [(-11, 0), (11, 0)]: both("wreck_rock", c + dx, c + dy)
    for f in (0, 1, 2, 3):
        for (dx, dy) in [(0, 0), (-20, 20), (20, -20)]:
            ents.append({"type": "metal_spot", "owner": 0, "at": at("terra", f, c + dx, c + dy)})
    return {
        "name": "Big Planet",
        "players": [{"name": "Синие", "team": 1, "start": at("terra", 4, c, c)},
                    {"name": "Красные", "team": 2, "start": at("terra", 5, N - c, c)}],
        "movement": {"flowField": {"minGroup": 8}},
        "world": {
            "start": "terra",
            "bodies": [
                {"id": "sun", "kind": "star", "radius": 60, "visual": {"color": [255, 220, 160]}},
                {"id": "terra", "kind": "planet", "radius": 80, "faceSize": N, "seaLevel": -2, "orbitAltitude": 10,
                 "orbit": {"around": "sun", "radius": 1000, "period": 1200, "phase": 0.1},
                 "terrain": {"faces": faces, "corners": corners(N, lambda d: terra_height(d) * 3)}},
            ],
        },
        "entities": ents,
    }

if __name__ == "__main__":
    path = os.path.join(ROOT, "planets", "maps", "two_worlds.json")
    with open(path, "w", encoding="utf-8") as f: f.write(compact(build()) + "\n")
    print("wrote", path)
    path = os.path.join(ROOT, "planets", "maps", "big_planet.json")
    with open(path, "w", encoding="utf-8") as f: f.write(json.dumps(build_big(), ensure_ascii=False, separators=(",", ":")) + "\n")
    print("wrote", path)
