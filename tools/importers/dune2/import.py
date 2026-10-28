#!/usr/bin/env python3
"""Dune II importer: turns the user's own copy of Dune II into an RTSEngine game folder.

    python3 tools/importers/dune2/import.py --source "~/Downloads/Dune II" --out ~/RTSGames/dune2 [--json]

Reads every .PAK under --source (Westwood PAK, see tools/unpackers/wwpak) and writes:
  game.json (top-down ortho camera), rules.json (our balance, tools/importers/dune2/rules.json), lang/{en,ru}.json,
  maps/scen{a,h,o}NNN.json   66 campaign scenarios with a tile layer (terrain approximated, see below)
  tilesets/dune.json + .png  ICON.ICN landscape: sand, rock, dunes, mountains, spice (mask16 autotiling)
  sprites/<type>.png         units (SHP, 5 facings mirrored, turrets composited) and structures (ICN tile
                             groups: built / rubble / animation), house colour as magenta shades;
                             added to rules.json as `sprite` blocks (docs/game-format.md, "2D games")
  pictures/*.png, pictures/anim/*.png   CPS pictures and WSA animation sheets
  audio/*.wav + audio.json   VOC sounds and voices mapped to engine sound events

Everything written is derived from copyrighted data: game.json is marked "external", so rtsbuild
flags the pack, and the output must never be committed or redistributed.

Terrain: Dune II scenarios store only a seed for the original procedural landscape generator. The
importer builds its own seeded landscape (sand, rock plateaus, dunes, mountains) and guarantees
rock under every base, so missions are playable but the terrain is not the original one.
"""
import argparse
import json
import math
import os
import struct
import sys

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'lib'))
import westwood as ww  # noqa: E402
from png import write_png  # noqa: E402

# ------------------------------------------------------------------ name tables

UNITS = {
    'Soldier': 'soldier', 'Infantry': 'infantry', 'Trooper': 'trooper', 'Troopers': 'troopers', 'Fremen': 'fremen',
    'Sardaukar': 'sardaukar', 'Saboteur': 'saboteur', 'Trike': 'trike', 'Raider Trike': 'raider', 'Quad': 'quad',
    'Tank': 'tank', 'Siege Tank': 'siege_tank', 'Launcher': 'launcher', 'Sonic Tank': 'sonic_tank',
    'Devastator': 'devastator', 'Deviator': 'deviator', 'Harvester': 'harvester', 'MCV': 'mcv', 'Carryall': 'carryall',
    "'Thopter": 'ornithopter', 'Ornithopter': 'ornithopter', 'Sandworm': 'sandworm',
}
STRUCTURES = {
    'Const Yard': ('const_yard', 2, 2), 'Windtrap': ('windtrap', 2, 2), 'Refinery': ('refinery', 3, 2),
    'Spice Silo': ('silo', 2, 2), 'Barracks': ('barracks', 2, 2), 'WOR': ('wor', 2, 2), 'Light Fctry': ('light_factory', 2, 2),
    'Heavy Fctry': ('heavy_factory', 3, 2), 'Hi-Tech': ('hightech_factory', 3, 2), 'Outpost': ('outpost', 2, 2),
    'Repair': ('repair', 3, 2), 'IX': ('ix', 2, 2), 'Starport': ('starport', 3, 3), 'Palace': ('palace', 3, 3),
    'Turret': ('turret', 1, 1), 'R-Turret': ('rocket_turret', 1, 1), 'Wall': ('wall', 1, 1),
}
STRUCTURE_IDS = {v[0] for v in STRUCTURES.values()}
HOUSES = ['Harkonnen', 'Atreides', 'Ordos', 'Fremen', 'Sardaukar', 'Mercenary']
HOUSE_RU = {'Harkonnen': 'Харконнены', 'Atreides': 'Атрейдесы', 'Ordos': 'Ордосы', 'Fremen': 'Фримены',
            'Sardaukar': 'Сардаукары', 'Mercenary': 'Наёмники'}
SCALES = {0: (1, 1, 62, 62), 1: (16, 16, 32, 32), 2: (21, 21, 21, 21)}   # MapScale -> x0, y0, w, h

TYPE_RU = {
    'const_yard': 'Сборочный цех', 'windtrap': 'Ветроловушка', 'refinery': 'Перерабатывающий завод', 'silo': 'Хранилище специи',
    'barracks': 'Казармы', 'wor': 'Казармы WOR', 'light_factory': 'Лёгкий завод', 'heavy_factory': 'Тяжёлый завод',
    'hightech_factory': 'Завод высоких технологий', 'outpost': 'Радарный пост', 'repair': 'Ремонтная мастерская',
    'ix': 'Дом IX', 'starport': 'Космопорт', 'palace': 'Дворец', 'turret': 'Пушечная турель', 'rocket_turret': 'Ракетная турель',
    'wall': 'Стена', 'spice_bloom': 'Цветение специи', 'soldier': 'Солдат', 'infantry': 'Пехота', 'trooper': 'Штурмовик',
    'troopers': 'Штурмовики', 'fremen': 'Фримены', 'sardaukar': 'Сардаукары', 'saboteur': 'Диверсант', 'trike': 'Трайк',
    'raider': 'Рейдер', 'quad': 'Квадроцикл', 'tank': 'Боевой танк', 'siege_tank': 'Осадный танк', 'launcher': 'Ракетная установка',
    'sonic_tank': 'Звуковой танк', 'devastator': 'Девастатор', 'deviator': 'Девиатор', 'harvester': 'Харвестер', 'mcv': 'MCV',
    'carryall': 'Транспортник', 'ornithopter': 'Орнитоптер', 'sandworm': 'Песчаный червь',
}


# ------------------------------------------------------------------ source files

def read_pak(data):
    """Westwood PAK -> {NAME: bytes} (same layout as the wwpak unpacker)."""
    n, p, recs = len(data), 0, []
    while p + 4 <= n:
        off = struct.unpack_from('<I', data, p)[0]
        if off == 0 or (recs and off == n and (p + 4 >= n or data[p + 4] == 0)):
            break
        q = data.find(b'\0', p + 4, p + 68)
        if q < 0:
            raise ww.FormatError('pak: bad header')
        recs.append((data[p + 4:q].decode('ascii', 'replace').upper(), off))
        p = q + 1
        if recs and p >= min(o for _, o in recs):
            break
    out = {}
    for k, (name, off) in enumerate(recs):
        end = recs[k + 1][1] if k + 1 < len(recs) else n
        out.setdefault(name, data[off:end])
    return out


def load_sources(src):
    files, paks = {}, []
    for root, _dirs, names in os.walk(src):
        for nm in sorted(names):
            if nm.upper().endswith('.PAK'):
                paks.append(os.path.join(root, nm))
    if not paks:
        raise SystemExit('error: no .PAK files under %s (point --source at the Dune II folder)' % src)
    for path in sorted(paks):
        with open(path, 'rb') as f:
            for name, blob in read_pak(f.read()).items():
                files.setdefault(name, blob)
    return files, paks


# ------------------------------------------------------------------ helpers

class Rng:
    """xorshift32 — our own deterministic generator for the landscape (not the original one)."""

    def __init__(self, seed):
        self.s = (seed * 2654435761 + 0x9E3779B9) & 0xFFFFFFFF or 1

    def next(self):
        s = self.s
        s ^= (s << 13) & 0xFFFFFFFF
        s ^= s >> 17
        s ^= (s << 5) & 0xFFFFFFFF
        self.s = s
        return s

    def unit(self):
        return self.next() / 4294967296.0

    def range(self, a, b):
        return a + self.next() % (b - a + 1)


def wjson(path, obj, compact_rows=False):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    text = json.dumps(obj, ensure_ascii=False, indent=1 if not compact_rows else None, separators=(',', ': ') if not compact_rows else (',', ':'))
    with open(path, 'w', encoding='utf-8') as f:
        f.write(text + '\n')


def parse_ini(text):
    sections, cur = {}, None
    for line in text.splitlines():
        line = line.split(';', 1)[0].strip()
        if not line:
            continue
        if line.startswith('[') and line.endswith(']'):
            cur = sections.setdefault(line[1:-1], [])
        elif cur is not None and '=' in line:
            k, v = line.split('=', 1)
            cur.append((k.strip(), v.strip()))
    return sections


def num(s, default=0):
    try:
        return int(s)
    except ValueError:
        return default


# ------------------------------------------------------------------ landscape (our generator)

def smooth(kinds, fixed=()):
    """Dune II tiles only know the 4 edge neighbours (mask16): one-cell notches and spikes would show
    as square holes. Smooth every kind like the original maps: fill cells enclosed on 3 sides, drop
    cells held by at most one side. Mountains count as rock (they stand on it). `fixed` cells stay."""
    fixed = set(fixed)

    def group(k):
        return ('r', 'm') if k == 'r' else (k,)
    # Pruning mountains off the sand can leave new spikes: repeat until stable (twice is enough).
    for _pass in range(3):
        for kind, lower in (('r', 's'), ('d', 's'), ('m', 'r')):
            for _ in range(4):
                changed = False
                for y in range(64):
                    for x in range(64):
                        if (x, y) in fixed:
                            continue
                        n = sum(1 for dx, dy in ((0, -1), (1, 0), (0, 1), (-1, 0))
                                if not (0 <= x + dx < 64 and 0 <= y + dy < 64) or kinds[y + dy][x + dx] in group(kind))
                        here = kinds[y][x]
                        if here not in group(kind) and n >= 3 and (kind != 'm' or here == 'r'):
                            kinds[y][x] = kind
                            changed = True
                        elif here == kind and n <= 1:
                            kinds[y][x] = lower
                            changed = True
                if not changed:
                    break
        # Dune II mountains always rise from rock: a mountain cell touching sand becomes rock.
        for y in range(64):
            for x in range(64):
                if kinds[y][x] == 'm' and any(kinds[yy][xx] in ('s', 'd') for yy in range(max(0, y - 1), min(64, y + 2))
                                              for xx in range(max(0, x - 1), min(64, x + 2))):
                    kinds[y][x] = 'r'


def landscape(seed, protect):
    """64x64 kinds: 's' sand, 'd' dunes, 'r' rock, 'm' mountain. `protect`: cells that must be rock."""
    rng = Rng(seed)

    def grid(cells):
        return [[rng.unit() for _ in range(cells + 1)] for _ in range(cells + 1)]

    octaves = [(grid(8), 8, 1.0), (grid(16), 16, 0.5), (grid(32), 32, 0.25)]
    dune = grid(16)

    def sample(g, cells, x, y):
        fx, fy = x * cells / 64.0, y * cells / 64.0
        ix, iy = int(fx), int(fy)
        tx, ty = fx - ix, fy - iy
        tx, ty = tx * tx * (3 - 2 * tx), ty * ty * (3 - 2 * ty)
        a = g[iy][ix] * (1 - tx) + g[iy][ix + 1] * tx
        b = g[iy + 1][ix] * (1 - tx) + g[iy + 1][ix + 1] * tx
        return a * (1 - ty) + b * ty

    total = sum(w for _, _, w in octaves)
    kinds = [['s'] * 64 for _ in range(64)]
    for y in range(64):
        for x in range(64):
            v = sum(sample(g, c, x, y) * w for g, c, w in octaves) / total
            if v > 0.66:
                kinds[y][x] = 'm'
            elif v > 0.55:
                kinds[y][x] = 'r'
            elif sample(dune, 16, x, y) > 0.62:
                kinds[y][x] = 'd'
    smooth(kinds)
    # Bases sit on rock in the original missions: grow a plateau around every protected cell.
    for (x, y) in protect:
        for dy in range(-2, 3):
            for dx in range(-2, 3):
                xx, yy = x + dx, y + dy
                if 0 <= xx < 64 and 0 <= yy < 64 and (abs(dx) + abs(dy) <= 3 or kinds[yy][xx] != 's'):
                    kinds[yy][xx] = 'r'
    return kinds


# ------------------------------------------------------------------ scenarios -> maps

def convert_scenario(name, text, rules_types, report):
    ini = parse_ini(text)
    basic = dict(ini.get('BASIC', []))
    mp = dict(ini.get('MAP', []))
    scale = num(basic.get('MapScale', '0'))
    x0, y0, w, h = SCALES.get(scale, SCALES[0])

    def cell(pos):
        p = num(pos, -1)
        return (p % 64, p // 64) if p >= 0 else None

    houses = [hname for hname in HOUSES if hname in ini]
    human = next((hname for hname in houses if dict(ini[hname]).get('Brain', '').lower() == 'human'), houses[0] if houses else 'Atreides')
    order = [human] + [hname for hname in houses if hname != human]
    entities, protect, skipped = [], [], {}

    def owner_of(house, unit_type):
        if house in order:
            return order.index(house) + 1
        if unit_type == 'sandworm':
            return -1
        order.append(house)             # a house with units but no [House] section: plain CPU player
        return len(order)

    for key, val in ini.get('STRUCTURES', []):
        parts = [s.strip() for s in val.split(',')]
        if key.startswith('GEN'):
            house, kind = parts[0], parts[1] if len(parts) > 1 else ''
            c = cell(key[3:])
            if c is None:
                continue
            protect.append(c)
            if kind == 'Wall':
                entities.append(('wall', owner_of(house, 'wall'), c[0] + 0.5, c[1] + 0.5))
            continue                    # concrete slabs: rock floor, nothing to place
        if len(parts) < 4:
            continue
        house, kind, _hp, pos = parts[:4]
        if kind not in STRUCTURES:
            skipped[kind] = skipped.get(kind, 0) + 1
            continue
        tid, fw, fh = STRUCTURES[kind]
        c = cell(pos)
        if c is None:
            continue
        for dy in range(fh):
            for dx in range(fw):
                protect.append((c[0] + dx, c[1] + dy))
        entities.append((tid, owner_of(house, tid), c[0] + fw / 2.0, c[1] + fh / 2.0))

    for _key, val in ini.get('UNITS', []):
        parts = [s.strip() for s in val.split(',')]
        if len(parts) < 4:
            continue
        house, kind, _hp, pos = parts[:4]
        tid = UNITS.get(kind)
        if tid is None or tid not in rules_types:
            skipped[kind] = skipped.get(kind, 0) + 1
            continue
        c = cell(pos)
        if c is None:
            continue
        entities.append((tid, owner_of(house, tid), c[0] + 0.5, c[1] + 0.5))

    seed = num(mp.get('Seed', '0'))
    kinds = landscape(seed, [c for c in protect if x0 <= c[0] < x0 + w and y0 <= c[1] < y0 + h])
    # Units never start inside mountains; sandworms need sand.
    def nearest_sand(c, taken=()):
        for r in range(0, 8):
            ring = [(c[0] + dx, c[1] + dy) for dy in range(-r, r + 1) for dx in range(-r, r + 1)
                    if max(abs(dx), abs(dy)) == r and 0 <= c[0] + dx < 64 and 0 <= c[1] + dy < 64
                    and kinds[c[1] + dy][c[0] + dx] in ('s', 'd') and (c[0] + dx, c[1] + dy) not in taken]
            if ring:
                return min(ring, key=lambda p: (abs(p[0] - c[0]) + abs(p[1] - c[1]), p[1], p[0]))
        kinds[c[1]][c[0]] = 's'      # no sand nearby: dig one cell rather than lose the entity
        return c

    for i, (tid, o, ex, ey) in enumerate(entities):
        cx, cy = int(ex), int(ey)
        if not (0 <= cx < 64 and 0 <= cy < 64):
            continue
        if tid == 'sandworm' and kinds[cy][cx] not in ('s', 'd'):
            # Sandworms need sand: move the worm, don't dig a hole in the rock.
            sx, sy = nearest_sand((cx, cy))
            entities[i] = (tid, o, sx + 0.5, sy + 0.5)
        elif kinds[cy][cx] == 'm':
            kinds[cy][cx] = 'r'

    # Spice fields (Field=) and hidden blooms (Bloom=) — spice only lies on sand.
    rng = Rng(seed ^ 0x5A5A)
    density = [[0] * 64 for _ in range(64)]
    for pos in [p for p in mp.get('Field', '').split(',') if p.strip()]:
        c = cell(pos.strip())
        if c is None:
            continue
        radius = rng.range(4, 6)
        for dy in range(-radius, radius + 1):
            for dx in range(-radius, radius + 1):
                xx, yy = c[0] + dx, c[1] + dy
                if not (0 <= xx < 64 and 0 <= yy < 64):
                    continue
                d = math.hypot(dx, dy) / radius
                if d <= 0.75 + 0.25 * rng.unit():
                    if kinds[yy][xx] in ('r', 'm') and d < 0.6:
                        kinds[yy][xx] = 's'
                    if kinds[yy][xx] in ('s', 'd'):
                        density[yy][xx] = max(density[yy][xx], max(1, min(9, int(round(9 * (1.0 - d) + 2)))))
    blooms = set()
    for pos in [p for p in mp.get('Bloom', '').split(',') if p.strip()]:
        c = cell(pos.strip())
        if c is None:
            continue
        # A bloom lies on sand: move it to the nearest sand cell rather than dig a hole in the rock.
        best = nearest_sand(c, blooms)
        blooms.add(best)
        entities.append(('spice_bloom', 0, best[0] + 0.5, best[1] + 0.5))
    # Field carving and unit fixes leave notches again: smooth once more, keeping what must stay.
    keep = set(blooms) | set(protect)
    keep |= {(x, y) for y in range(64) for x in range(64) if density[y][x]}
    keep |= {(int(ex), int(ey)) for tid, _o, ex, ey in entities if tid == 'sandworm'}
    for (x, y) in keep:
        if density[y][x] and kinds[y][x] == 'd':
            kinds[y][x] = 's'           # the spice tile covers it; a dune under it would leave dune spikes
    smooth(kinds, keep)
    for y in range(64):
        for x in range(64):
            if density[y][x] and kinds[y][x] not in ('s', 'd'):
                density[y][x] = 0

    # Spice uses the same 4-neighbour tiles: trim one-cell spikes and fill one-cell gaps of the fields.
    for _ in range(8):
        changed = False
        for y in range(64):
            for x in range(64):
                near = [density[y + dy][x + dx] for dx, dy in ((0, -1), (1, 0), (0, 1), (-1, 0))
                        if 0 <= x + dx < 64 and 0 <= y + dy < 64 and density[y + dy][x + dx]]
                if density[y][x] and len(near) <= 1 and (x, y) not in blooms:
                    density[y][x] = 0
                    changed = True
                elif not density[y][x] and len(near) >= 3 and kinds[y][x] in ('s', 'd'):
                    density[y][x] = min(near)
                    changed = True
        if not changed:
            break

    # Trimmed spice may leave bare sand pockets in the rock: close them.
    keep = set(blooms) | set(protect) | {(x, y) for y in range(64) for x in range(64) if density[y][x]}
    keep |= {(int(ex), int(ey)) for tid, _o, ex, ey in entities if tid == 'sandworm'}
    smooth(kinds, keep)

    # Tile layer (tilesets/dune.json): the engine derives terrain cells and spice fields from it.
    code = {'s': ',', 'd': ',', 'r': 'R', 'm': '#'}
    rows, tile_rows, field_rows = [], [], []
    for y in range(y0, y0 + h):
        rows.append(''.join(code[kinds[y][x]] for x in range(x0, x0 + w)))
        field_rows.append(''.join(str(density[y][x]) if density[y][x] else '0' for x in range(x0, x0 + w)))
        tile_rows.append(''.join(('O' if density[y][x] >= 7 else 'o') if density[y][x] else kinds[y][x] for x in range(x0, x0 + w)))

    players = []
    for i, house in enumerate(order):
        sec = dict(ini.get(house, []))
        team = 1 if i == 0 or (house == 'Fremen' and human == 'Atreides') else 2
        pl = {'name': house, 'team': team, 'resources': {'spice': num(sec.get('Credits', '0'))}}
        if i > 0:
            pl['ai'] = True
        players.append(pl)

    ents = []
    for tid, owner, ex, ey in entities:
        if not (x0 <= ex < x0 + w and y0 <= ey < y0 + h):
            continue
        ents.append({'type': tid, 'owner': owner, 'pos': [round(ex - x0, 2), round(ey - y0, 2)]})
    # Win conditions: spice quota (WinFlags bit 4) becomes a trigger; if an enemy house starts without
    # structures, "destroy all structures" would end the mission at once, so units count too.
    triggers, victory = [], None
    win_flags = num(basic.get('WinFlags', '0'))
    quota = num(dict(ini.get(human, [])).get('Quota', '0'))
    if quota > 0 and win_flags & 4:
        # The quota is spice harvested during the mission; the engine tracks the stock, so the
        # target is the starting credits plus the quota.
        target = quota + num(dict(ini.get(human, [])).get('Credits', '0'))
        triggers.append({'id': 'quota', 'once': True, 'event': {'type': 'resource', 'resource': 'spice', 'player': 1, '>=': target},
                         'actions': [{'type': 'message', 'text': 'Spice quota reached'}, {'type': 'victory', 'player': 1}]})
    owners_with_structures = {o for t, o, _x, _y in entities if t in STRUCTURE_IDS and t != 'wall'}
    if any(i + 1 not in owners_with_structures for i in range(1, len(order))):
        victory = 'noEntities'
    house_letter = name[4]
    mission = int(name[5:8])
    data = {
        'name': '%s %d' % ({'A': 'Atreides', 'H': 'Harkonnen', 'O': 'Ordos'}.get(house_letter, 'Dune'), mission),
        'size': [w, h],
        'players': players,
        'terrain': {'rows': rows},
        # Plain fields for builds without the tileset; with tiles the engine takes spice from them.
        'fields': [{'resource': 'spice', 'rows': field_rows}],
        'tiles': {'tileset': 'tilesets/dune.json', 'rows': tile_rows},
        'entities': ents,
        'triggers': triggers,
        'source': {'scenario': name, 'seed': seed, 'mapScale': scale, 'terrain': 'approximated'},
    }
    if victory:
        data['victory'] = victory
    for k, n in skipped.items():
        report['skipped'][k] = report['skipped'].get(k, 0) + n
    return data, human


# ------------------------------------------------------------------ graphics (2D format of docs/game-format.md)

# Unit sheets: (sheet, first frame) per layer; Dune II stores 5 facings N, NE, E, SE, S and mirrors
# the west side, which is exactly the engine's `mirror: true` layout. Turrets are composited on
# their chassis. Pairs marked "guess" are inferred from the look of the frames.
UNIT_ART = {
    'quad': [('UNITS.SHP', 0)], 'trike': [('UNITS.SHP', 5)], 'raider': [('UNITS.SHP', 5)],
    'mcv': [('UNITS.SHP', 10)], 'harvester': [('UNITS.SHP', 15)],
    'tank': [('UNITS2.SHP', 0), ('UNITS2.SHP', 5)], 'siege_tank': [('UNITS2.SHP', 10), ('UNITS2.SHP', 15)],
    'devastator': [('UNITS2.SHP', 20), ('UNITS2.SHP', 25)],          # guess: turret
    'sonic_tank': [('UNITS2.SHP', 0), ('UNITS2.SHP', 30)],           # guess
    'launcher': [('UNITS2.SHP', 0), ('UNITS2.SHP', 35)],             # guess
    'deviator': [('UNITS2.SHP', 0), ('UNITS2.SHP', 35)],             # guess
}
# Structures are ICON.ICN tile groups (ICON.MAP): frame 0 = being built, 1 = rubble, 2.. = alive.
STRUCTURE_ART = {
    'palace': (11, 3, 3), 'light_factory': (12, 2, 2), 'heavy_factory': (13, 3, 2), 'hightech_factory': (14, 3, 2),
    'ix': (15, 2, 2), 'wor': (16, 2, 2), 'const_yard': (17, 2, 2), 'barracks': (18, 2, 2), 'windtrap': (19, 2, 2),
    'starport': (20, 3, 3), 'refinery': (21, 3, 2), 'repair': (22, 3, 2), 'turret': (23, 1, 1), 'rocket_turret': (24, 1, 1),
    'silo': (25, 2, 2), 'outpost': (26, 2, 2),
}
TEAM_INDICES = range(144, 151)      # Harkonnen-red ramp that the game remaps per house


def to_team_rgba(px, pal, transparent=0):
    """Palette indices -> RGBA where house-coloured pixels become magenta shades (r = b, g = 0),
    the engine's team-colour convention; other pixels are nudged off that exact hue."""
    out = bytearray(len(px) * 4)
    for k, v in enumerate(px):
        if v == transparent:
            continue
        r, g, b = pal[v]
        if v in TEAM_INDICES:
            lum = max(r, g, b)
            r, g, b = lum, 0, lum
        elif r == b and g == 0 and r > 0:
            g = 1
        out[4 * k:4 * k + 4] = bytes((r, g, b, 255))
    return out


def blit(dst, dw, src, sw, sh, x0, y0):
    """Alpha-tested copy of an RGBA image into a bigger RGBA buffer."""
    for y in range(sh):
        for x in range(sw):
            s = 4 * (y * sw + x)
            if src[s + 3]:
                d = 4 * ((y0 + y) * dw + x0 + x)
                dst[d:d + 4] = src[s:s + 4]


def write_unit_sheets(files, pal, out, report):
    """-> {type: sprite block}."""
    shp = {}
    blocks = {}
    for tid, layers in UNIT_ART.items():
        frames = []
        try:
            for sheet, first in layers:
                if sheet not in shp:
                    shp[sheet] = ww.read_shp(files[sheet]) if sheet in files else []
                if first + 4 >= len(shp[sheet]):
                    raise KeyError(sheet)
                frames.append(shp[sheet][first:first + 5])
        except (KeyError, ww.FormatError, struct.error, IndexError):
            continue
        fw = max(f['width'] for layer in frames for f in layer)
        fh = max(f['height'] for layer in frames for f in layer)
        W, H = fw, fh * 5                       # one column, five rows (N, NE, E, SE, S)
        sheet_px = bytearray(W * H * 4)
        for row in range(5):
            for layer in frames:
                f = layer[row]
                blit(sheet_px, W, to_team_rgba(f['pixels'], pal), f['width'], f['height'],
                     (fw - f['width']) // 2, row * fh + (fh - f['height']) // 2)
        rel = 'sprites/%s.png' % tid
        write_png(os.path.join(out, rel), W, H, sheet_px)
        blocks[tid] = {'sheet': rel, 'frame': [fw, fh], 'facings': 8, 'mirror': True, 'mode': 'ground',
                       'pixelsPerCell': 16, 'filter': 'nearest', 'team': 'magenta', 'actions': {'idle': [0, 1]}}
        report['sprites'] += 1
    return blocks


def write_structure_sheets(files, pal, out, report):
    tw, th, tiles = ww.read_icn(files['ICON.ICN'])
    groups = ww.read_icon_map(files['ICON.MAP'])
    blocks = {}
    for tid, (g, w, h) in STRUCTURE_ART.items():
        if g >= len(groups):
            continue
        t = groups[g]
        per = w * h
        n = len(t) // per
        if n < 3:
            continue
        fw, fh = w * tw, h * th
        is_turret = w == 1 and h == 1 and n >= 10
        if is_turret:
            # frames 2..9 aim N, NE, E, SE, S, SW, W, NW; engine rows start east and go clockwise
            order = [4, 5, 6, 7, 8, 9, 2, 3]
            cols, rows, cells = 1, 8, [[f] for f in order]
        else:
            cols, rows, cells = n, 1, [list(range(n))]
        W, H = cols * fw, rows * fh
        img = bytearray(W * H * 4)
        for r, row in enumerate(cells):
            for c, f in enumerate(row):
                for k in range(per):
                    rgba = to_team_rgba(tiles[t[f * per + k]], pal, transparent=-1)
                    x0, y0 = c * fw + (k % w) * tw, r * fh + (k // w) * th
                    for y in range(th):
                        o = (y0 + y) * W + x0
                        img[4 * o:4 * (o + tw)] = rgba[4 * y * tw:4 * (y + 1) * tw]
        rel = 'sprites/%s.png' % tid
        write_png(os.path.join(out, rel), W, H, img)
        block = {'sheet': rel, 'frame': [fw, fh], 'mode': 'ground', 'pixelsPerCell': 16, 'filter': 'nearest',
                 'team': 'magenta', 'shadow': False}
        if is_turret:
            block.update({'facings': 8, 'actions': {'idle': [0, 1]}})
        else:
            block.update({'facings': 1, 'corpseTime': 8,
                          'actions': {'idle': [2, n - 2, 2] if n > 3 else [2, 1], 'build': {'start': 0, 'count': 1, 'loop': False},
                                      'die': [1, 1, 1]}})
        blocks[tid] = block
        report['sprites'] += 1
    # Spice bloom: first tile of group 10 (a sand mound).
    if len(groups) > 10 and groups[10]:
        img = to_team_rgba(tiles[groups[10][0]], pal, transparent=-1)
        write_png(os.path.join(out, 'sprites', 'spice_bloom.png'), tw, th, img)
        blocks['spice_bloom'] = {'sheet': 'sprites/spice_bloom.png', 'frame': [tw, th], 'mode': 'ground', 'pixelsPerCell': 16,
                                 'filter': 'nearest', 'team': 'none', 'shadow': False, 'facings': 1, 'actions': {'idle': [0, 1]}}
    return blocks


def write_tileset(files, pal, out):
    """ICON.ICN -> tilesets/dune.png + tilesets/dune.json (terrains with mask16 autotiling)."""
    tw, th, tiles = ww.read_icn(files['ICON.ICN'])
    groups = ww.read_icon_map(files['ICON.MAP']) if 'ICON.MAP' in files else []
    cols = 32
    rows = (len(tiles) + cols - 1) // cols
    W, H = cols * tw, rows * th
    sheet = bytearray(W * H * 4)
    for t, px in enumerate(tiles):
        rgba = ww.to_rgba(px, pal, transparent=-1)
        x0, y0 = (t % cols) * tw, (t // cols) * th
        for y in range(th):
            o = (y0 + y) * W + x0
            sheet[4 * o:4 * (o + tw)] = rgba[4 * y * tw:4 * (y + 1) * tw]
    write_png(os.path.join(out, 'tilesets', 'dune.png'), W, H, sheet)
    land = next((g for g in groups if len(g) == 81 and g == list(range(g[0], g[0] + 81))), None)
    if land is None:
        raise ww.FormatError('ICON.MAP: landscape group (81 consecutive tiles) not found')
    L = land[0]
    def auto(first):   # Dune II: 16 tiles per kind, index = N1 E2 S4 W8 neighbours of the same kind
        return {'type': 'mask16', 'first': L + first}
    ts = {
        'name': 'Dune II', 'atlas': 'tilesets/dune.png', 'tileSize': [tw, th], 'columns': cols, 'filter': 'nearest',
        'terrains': [
            {'id': 'sand', 'char': 's', 'cell': ',', 'color': [0.84, 0.6, 0.35], 'tiles': [L]},
            # mountains stand on rock: for rock edges they count as rock (no sand rims inside massifs)
            {'id': 'rock', 'char': 'r', 'cell': 'R', 'color': [0.45, 0.42, 0.36], 'under': 'sand', 'tiles': [L + 16],
             'autotile': dict(auto(1), **{'with': ['mountain']})},
            {'id': 'dunes', 'char': 'd', 'cell': ',', 'color': [0.8, 0.55, 0.3], 'under': 'sand', 'tiles': [L + 32], 'autotile': auto(17)},
            {'id': 'mountain', 'char': 'm', 'cell': '#', 'color': [0.35, 0.25, 0.15], 'under': 'rock', 'tiles': [L + 48], 'autotile': auto(33)},
            {'id': 'spice', 'char': 'o', 'cell': ',', 'color': [0.85, 0.45, 0.25], 'field': {'resource': 'spice', 'density': 5},
             'under': 'sand', 'depleted': 'sand', 'tiles': [L + 64], 'autotile': dict(auto(49), **{'with': ['thick_spice']})},
            {'id': 'thick_spice', 'char': 'O', 'cell': ',', 'color': [0.7, 0.3, 0.15], 'field': {'resource': 'spice', 'density': 9},
             'under': 'spice', 'depleted': 'sand', 'tiles': [L + 80], 'autotile': auto(65)},
        ],
    }
    wjson(os.path.join(out, 'tilesets', 'dune.json'), ts)
    return len(tiles)


def write_cps(name, data, pal, out):
    px, own = ww.read_cps(data)
    p = own or pal
    write_png(os.path.join(out, 'pictures', name.rsplit('.', 1)[0].lower() + '.png'), 320, 200,
              ww.to_rgba(px[:64000], p, transparent=-1))


def write_wsa(name, data, pal, out):
    """WSA animation -> one PNG sheet (8 frames per row) for menus, briefings and cutscenes."""
    w, h, frames, _loops = ww.read_wsa(data)
    if not frames:
        return False
    cols = min(len(frames), 8)
    rows = (len(frames) + cols - 1) // cols
    W, H = cols * w, rows * h
    sheet = bytearray(W * H * 4)
    for k, fr in enumerate(frames):
        rgba = ww.to_rgba(fr, pal, transparent=-1)
        x0, y0 = (k % cols) * w, (k // cols) * h
        for y in range(h):
            o = (y0 + y) * W + x0
            sheet[4 * o:4 * (o + w)] = rgba[4 * y * w:4 * (y + 1) * w]
    write_png(os.path.join(out, 'pictures', 'anim', name.rsplit('.', 1)[0].lower() + '.png'), W, H, sheet)
    return True


# ------------------------------------------------------------------ audio

SOUND_EVENTS = {
    'select': ['ZREPORT1', 'ZREPORT2', 'ZREPORT3'], 'move': ['ZAFFIRM', 'ZMOVEOUT'], 'attackOrder': ['ZOVEROUT', 'ZAFFIRM'],
    'buildOrder': ['ZAFFIRM'], 'buildDone': ['ACONST'], 'ready': ['AUNIT'], 'underAttack': ['AATTACK'],
    'victory': ['AWIN'], 'defeat': ['ALOSE'], 'click': ['BUTTON'], 'error': ['BUTTON'],
}
WEAPON_SOUNDS = {'bullet': ('GUNSHOT', None), 'cannon': ('EXCANNON', 'EXSMALL'), 'rocket': ('ROCKET', 'EXMED'),
                 'sonic': ('BLASTER', None), 'worm': ('WORMET3P', None)}


def write_audio(files, out, rules, report):
    have = set()
    for name, data in sorted(files.items()):
        if not name.endswith('.VOC'):
            continue
        try:
            rate, ch, bits, pcm = ww.read_voc(data)
        except ww.FormatError as ex:
            report['warnings'].append('%s: %s' % (name, ex))
            continue
        base = name[:-4].lower().replace('~', '_')
        path = os.path.join(out, 'audio', base + '.wav')
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, 'wb') as f:
            f.write(ww.wav_bytes(rate, ch, bits, pcm))
        have.add(name[:-4])
        report['sounds'] += 1

    def snd(names, bus=None):
        fl = ['audio/%s.wav' % n.lower() for n in names if n in have]
        if not fl:
            return None
        s = {'files': fl} if len(fl) > 1 else {'file': fl[0]}
        if bus:
            s['bus'] = bus
        return s

    sounds, events, weapons, types = {}, {}, {}, {}
    for ev, names in SOUND_EVENTS.items():
        s = snd(names, 'voice' if ev in ('buildDone', 'ready', 'underAttack', 'victory', 'defeat') else 'ui')
        if s:
            sounds[ev] = s
            events[ev] = ev
    for wid, (fire, hit) in WEAPON_SOUNDS.items():
        entry = {}
        for role, n in (('attack', fire), ('hit', hit)):
            s = snd([n]) if n else None
            if s:
                sounds[wid + '_' + role] = s
                entry[role] = wid + '_' + role
        if entry:
            weapons[wid] = entry
    scream = snd(['VSCREAM1', 'VSCREAM2', 'VSCREAM3', 'VSCREAM4', 'VSCREAM5'])
    boom = snd(['EXMED', 'EXLARGE'])
    crumble = snd(['CRUMBLE', 'EXLARGE'])
    for k, s in (('scream', scream), ('vehicleDies', boom), ('crumble', crumble)):
        if s:
            sounds[k] = s
    for tid, t in rules['types'].items():
        tags = t.get('tags', [])
        died = 'scream' if 'infantry' in tags else 'crumble' if 'building' in tags else 'vehicleDies'
        if died in sounds:
            types[tid] = {'died': died}
    wjson(os.path.join(out, 'audio.json'), {'volume': {'sfx': 0.9, 'ui': 0.7, 'voice': 1.0, 'music': 0.6},
                                            'sounds': sounds, 'events': events, 'weapons': weapons, 'types': types})


# ------------------------------------------------------------------ main

def load_rules():
    text = open(os.path.join(HERE, 'rules.json'), encoding='utf-8').read()
    lines = [ln for ln in text.splitlines() if not ln.lstrip().startswith('//')]
    clean = []
    for ln in lines:   # drop trailing // comments outside strings (the template keeps them simple)
        i = ln.find(' //')
        clean.append(ln if i < 0 or ln.count('"', 0, i) % 2 else ln[:i])
    return text, json.loads('\n'.join(clean))


def run(src, out, only_maps=False):
    report = {'source': src, 'out': out, 'paks': 0, 'files': 0, 'maps': 0, 'sprites': 0, 'tiles': 0,
              'pictures': 0, 'animations': 0, 'sounds': 0, 'skipped': {}, 'warnings': []}
    files, paks = load_sources(src)
    report['paks'], report['files'] = len(paks), len(files)
    _rules_text, rules = load_rules()
    os.makedirs(out, exist_ok=True)
    pal = ww.read_pal(files['IBM.PAL']) if 'IBM.PAL' in files else None
    has_tiles = pal is not None and 'ICON.ICN' in files and 'ICON.MAP' in files

    maps, lang_en, lang_ru = [], {}, {}
    for name in sorted(n for n in files if n.startswith('SCEN') and n.endswith('.INI')):
        stem = name[:-4].lower()
        try:
            data, human = convert_scenario(name, files[name].decode('cp437', 'replace'), rules['types'], report)
        except (ValueError, IndexError, KeyError) as ex:
            report['warnings'].append('%s: %s' % (name, ex))
            continue
        wjson(os.path.join(out, 'maps', stem + '.json'), data, compact_rows=True)
        title = data['name']
        maps.append({'file': 'maps/%s.json' % stem, 'title': title})
        lang_en['map.%s.title' % stem] = lang_en['map.%s.name' % stem] = title
        num_ = title.split()[-1]
        ru = '%s — миссия %s' % (HOUSE_RU.get(human, human), num_)
        lang_ru['map.%s.title' % stem] = lang_ru['map.%s.name' % stem] = ru
        for i, p in enumerate(data['players']):
            lang_en['map.%s.player.%d' % (stem, i + 1)] = p['name']
            lang_ru['map.%s.player.%d' % (stem, i + 1)] = HOUSE_RU.get(p['name'], p['name'])
        report['maps'] += 1
    order = {'A': 0, 'H': 1, 'O': 2}
    maps.sort(key=lambda m: (order.get(os.path.basename(m['file'])[4].upper(), 9), m['file']))

    for tid, t in rules['types'].items():
        lang_en['type.' + tid] = t.get('name', tid)
        lang_ru['type.' + tid] = TYPE_RU.get(tid, t.get('name', tid))
    for rid, r in rules['resources'].items():
        lang_en['resource.' + rid] = r.get('name', rid)
    lang_ru['resource.spice'] = 'Специя'
    lang_ru['resource.power'] = 'Энергия'
    lang_en['game.name'] = 'Dune II (imported)'
    lang_ru['game.name'] = 'Дюна II (импорт)'
    wjson(os.path.join(out, 'lang', 'en.json'), dict(sorted(lang_en.items())))
    wjson(os.path.join(out, 'lang', 'ru.json'), dict({'@language': 'Русский'}, **dict(sorted(lang_ru.items()))))

    # Graphics: sprite sheets become `sprite` blocks on the types; the tileset is needed by the
    # maps, so it is written even with --maps-only.
    sprites = {}
    if has_tiles:
        try:
            report['tiles'] = write_tileset(files, pal, out)
        except (ww.FormatError, struct.error, IndexError) as ex:
            report['warnings'].append('ICON.ICN: %s' % ex)
            has_tiles = False
    if not only_maps and pal:
        sprites.update(write_unit_sheets(files, pal, out, report))
        if has_tiles:
            sprites.update(write_structure_sheets(files, pal, out, report))
        for name in sorted(n for n in files if n.endswith('.CPS')):
            try:
                write_cps(name, files[name], pal, out)
                report['pictures'] += 1
            except (ww.FormatError, struct.error, IndexError) as ex:
                report['warnings'].append('%s: %s' % (name, ex))
        for name in sorted(n for n in files if n.endswith('.WSA')):
            try:
                if write_wsa(name, files[name], pal, out):
                    report['animations'] += 1
            except (ww.FormatError, struct.error, IndexError) as ex:
                report['warnings'].append('%s: %s' % (name, ex))
        write_audio(files, out, rules, report)
    for tid, block in sprites.items():
        if tid in rules['types']:
            rules['types'][tid]['sprite'] = block
    with open(os.path.join(out, 'rules.json'), 'w', encoding='utf-8') as f:
        f.write('// Dune II rules (tools/importers/dune2/rules.json) + sprite sheets converted from your copy.\n')
        json.dump(rules, f, ensure_ascii=False, indent=1)
        f.write('\n')
    if not has_tiles:   # no tileset: keep plain terrain rows
        for m in maps:
            path = os.path.join(out, m['file'])
            with open(path, encoding='utf-8') as f:
                data = json.load(f)
            data.pop('tiles', None)
            wjson(path, data, compact_rows=True)

    wjson(os.path.join(out, 'game.json'), {
        'name': 'Dune II (imported)', 'id': 'com.rtsengine.import.dune2', 'version': '0.1.0', 'build': 1, 'module': 'rts',
        'sourceLanguage': 'en', 'languages': ['en', 'ru'], 'rules': 'rules.json', 'maps': maps, 'orientation': 'landscape',
        'camera': {'projection': 'ortho', 'pitch': 90, 'yaw': 0, 'pixelsPerCell': 32, 'zoomMin': 8, 'zoomMax': 40, 'rotate': False},
        'external': True,
        'importedFrom': {'game': 'Dune II', 'importer': 'tools/importers/dune2', 'paks': [os.path.basename(p) for p in paks]},
    })
    with open(os.path.join(out, 'README.txt'), 'w', encoding='utf-8') as f:
        f.write('Imported from your own copy of Dune II by tools/importers/dune2.\n'
                'This folder contains copyrighted game data: keep it private, never commit or redistribute it.\n'
                'Terrain is approximated (the original landscape generator is not used); see maps/*.json "source".\n')
    return report


def main():
    ap = argparse.ArgumentParser(description='Import Dune II into an RTSEngine game folder')
    ap.add_argument('--source', required=True, help='Dune II folder (contains DUNE.PAK, SCENARIO.PAK, ...)')
    ap.add_argument('--out', required=True, help='output game folder (outside the repository)')
    ap.add_argument('--maps-only', action='store_true', help='rules, maps and lang only (fast)')
    ap.add_argument('--json', action='store_true', help='one JSON report on stdout (MCP / editor)')
    a = ap.parse_args()
    src, out = os.path.expanduser(a.source), os.path.expanduser(a.out)
    try:
        rep = run(src, out, a.maps_only)
    except SystemExit as ex:
        if a.json:
            print(json.dumps({'ok': False, 'error': str(ex)}, ensure_ascii=False))
            sys.exit(1)
        raise
    if a.json:
        print(json.dumps(dict({'ok': True}, **rep), ensure_ascii=False))
        return
    print('Dune II -> %s' % out)
    print('  PAK: %d, файлов: %d' % (rep['paks'], rep['files']))
    print('  карт: %d, спрайтов: %d, тайлов: %d, картинок: %d, анимаций: %d, звуков: %d' % (
        rep['maps'], rep['sprites'], rep['tiles'], rep['pictures'], rep['animations'], rep['sounds']))
    if rep['skipped']:
        print('  пропущено объектов: ' + ', '.join('%s ×%d' % kv for kv in sorted(rep['skipped'].items())))
    for wmsg in rep['warnings']:
        print('  предупреждение: ' + wmsg)
    print('  ВНИМАНИЕ: папка содержит данные Dune II — только для личного использования, не коммитить.')


if __name__ == '__main__':
    main()
