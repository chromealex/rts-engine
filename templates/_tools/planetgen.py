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
    # paved roads (map '+', faster with rules roadNetwork) from each base to its metal spots, mirrored on face 5
    base = [["."] * N for _ in range(N)]
    for x in range(8, 17): base[12][x] = "+"            # east-west through the base
    for y in range(5, 20): base[y][12] = "+"            # north-south, towards the spots at y 4 and the far pair
    for x in range(7, 18): base[15][x] = "+"            # between the spot pairs, clear of their footprints
    for x in range(7, 18): base[10][x] = "+"
    rows4 = ["".join(r) for r in base]
    rows5 = ["".join(reversed(r)) for r in base]       # mirror(): x -> N - x
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
                 "terrain": {"corners": corners(N, terra_height),
                             "faces": [{}, {}, {}, {}, {"rows": rows4}, {"rows": rows5}]}},
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

if __name__ == "__main__":
    path = os.path.join(ROOT, "planets", "maps", "two_worlds.json")
    with open(path, "w", encoding="utf-8") as f: f.write(compact(build()) + "\n")
    print("wrote", path)
