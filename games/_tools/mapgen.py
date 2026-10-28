#!/usr/bin/env python3
"""Map generator for the sample games (deterministic, no randomness).

    python3 games/_tools/mapgen.py            # writes every sample map
    python3 games/_tools/mapgen.py frostcrown  # one game

Maps are 1v1 and point-symmetric: everything placed for player 1 is mirrored for player 2,
so the generic AI plays a fair match. Each generator returns the map dict written to
games/<id>/maps/<name>.json.
"""
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


class Map:
    def __init__(self, name, w, h, players, fill='.'):
        self.name, self.w, self.h = name, w, h
        self.rows = [[fill] * w for _ in range(h)]
        self.levels = None
        self.regions = None
        self.region_names = {}
        self.territory = None
        self.players = players
        self.entities = []
        self.fields = []
        self.extra = {}
        self.corners = None   # heightfield: (w+1)*(h+1) ints, 1/16 units

    # ---- terrain
    def heightfield(self, f):
        """Corner heights from f(x, y) -> world units (rounded to 1/16)."""
        self.corners = [int(round(f(x, y) * 16)) for y in range(self.h + 1) for x in range(self.w + 1)]

    def cell(self, x, y, c):
        if 0 <= x < self.w and 0 <= y < self.h:
            self.rows[y][x] = c

    def rect(self, x, y, w, h, c, sym=True):
        for yy in range(y, y + h):
            for xx in range(x, x + w):
                self.cell(xx, yy, c)
                if sym:
                    self.cell(self.w - 1 - xx, self.h - 1 - yy, c)

    def disc(self, cx, cy, r, c, sym=True):
        for yy in range(int(cy - r), int(cy + r) + 1):
            for xx in range(int(cx - r), int(cx + r) + 1):
                if (xx - cx) ** 2 + (yy - cy) ** 2 <= r * r:
                    self.cell(xx, yy, c)
                    if sym:
                        self.cell(self.w - 1 - xx, self.h - 1 - yy, c)

    def border(self, c='#'):
        for x in range(self.w):
            self.rows[0][x] = self.rows[self.h - 1][x] = c
        for y in range(self.h):
            self.rows[y][0] = self.rows[y][self.w - 1] = c

    def level_rect(self, x, y, w, h, lv, sym=True):
        if self.levels is None:
            self.levels = [[0] * self.w for _ in range(self.h)]
        for yy in range(y, y + h):
            for xx in range(x, x + w):
                for (px, py) in ([(xx, yy), (self.w - 1 - xx, self.h - 1 - yy)] if sym else [(xx, yy)]):
                    if 0 <= px < self.w and 0 <= py < self.h:
                        self.levels[py][px] = lv

    def region_rect(self, x, y, w, h, letter, mirror_letter=None):
        if self.regions is None:
            self.regions = [['.'] * self.w for _ in range(self.h)]
        for yy in range(y, y + h):
            for xx in range(x, x + w):
                if 0 <= xx < self.w and 0 <= yy < self.h:
                    self.regions[yy][xx] = letter
                if mirror_letter:
                    mx, my = self.w - 1 - xx, self.h - 1 - yy
                    if 0 <= mx < self.w and 0 <= my < self.h:
                        self.regions[my][mx] = mirror_letter

    # ---- objects
    def mirror(self, pos):
        return [round(self.w - pos[0], 2), round(self.h - pos[1], 2)]

    def ent(self, type_, owner, pos, sym=True, **kw):
        e = {"type": type_, "owner": owner, "pos": [pos[0], pos[1]]}
        e.update(kw)
        self.entities.append(e)
        if sym:
            o2 = {1: 2, 2: 1}.get(owner, owner)
            e2 = dict(e)
            e2["owner"] = o2
            e2["pos"] = self.mirror(pos)
            if "id" in kw:
                e2["id"] = kw["id"] + "_b"
            self.entities.append(e2)

    def field(self, resource, cx, cy, r, amount=None, sym=True):
        f = {"resource": resource, "center": [cx, cy], "radius": r}
        if amount is not None:
            f["amount"] = amount
        self.fields.append(f)
        if sym:
            f2 = dict(f)
            f2["center"] = self.mirror([cx, cy])
            self.fields.append(f2)

    def to_json(self):
        m = {"name": self.name, "size": [self.w, self.h], "players": self.players,
             "terrain": {"rows": ["".join(r) for r in self.rows]}}
        if self.levels:
            m["terrain"]["levels"] = ["".join(str(v) for v in r) for r in self.levels]
        if self.corners and (self.w + 1) * (self.h + 1) <= 257 * 257:
            m["terrain"]["corners"] = self.corners   # big maps: write_corners_file() + terrain.cornersFile
        if self.fields:
            m["fields"] = self.fields
        if self.regions:
            m["regions"] = {"rows": ["".join(r) for r in self.regions], "names": self.region_names}
        if self.territory:
            m["territory"] = {"rows": ["".join(r) for r in self.territory]}
        m["entities"] = self.entities
        m.update(self.extra)
        return m


def duel(f1="", f2="", **kw):
    a = {"name": "Синие", "team": 1}
    b = {"name": "Красные", "team": 2}
    if f1:
        a["faction"] = f1
    if f2:
        b["faction"] = f2
    a.update(kw.get("a", {}))
    b.update(kw.get("b", {}))
    return [a, b]


def write_big_heightfield(path, size, seed=7):
    """Benchmark map for the heightfield simulation: size x size cells, procedural hills and ridges, corners in
    a raw int16 LE file next to the JSON (terrain.cornersFile). Not a sample game map: write it to
    <scratch game>/maps/ (copy game.json + rules.json of games/metaltide next to it)."""
    import math
    import random
    import struct
    rnd = random.Random(seed)
    waves = [(rnd.uniform(0.004, 0.03), rnd.uniform(0.004, 0.03), rnd.uniform(0, 6.28), rnd.uniform(0.5, 3.0)) for _ in range(6)]
    n = size + 1
    vals = []
    for y in range(n):
        for x in range(n):
            v = sum(a * math.sin(x * fx + y * fy + ph) for (fx, fy, ph, a) in waves)
            # long steep ridges (two crossing families) with periodic passes: a maze for slope-limited units
            for (fx, fy, gap) in ((0.031, 0.012, 0.017), (-0.009, 0.027, 0.021)):
                r = max(0.0, 1.0 - abs(math.sin(x * fx + y * fy)) * 6)
                if math.sin((x * fy - y * fx) * gap * 40) > 0.85:
                    r *= 0.15   # pass
                v += 7.0 * r
            vals.append(int(round(max(0.0, v + 4.0) * 16)))
    base = os.path.splitext(path)[0]
    with open(base + ".r16", "wb") as f:
        f.write(struct.pack("<%dh" % len(vals), *vals))
    m = Map("Big heightfield %d" % size, size, size, duel())
    m.border()
    a, b = size // 8, size - size // 8
    for (px, py) in ((a, a), (b, b)):
        for yy in range(py - 8, py + 9):
            for xx in range(px - 8, px + 9):
                vals[yy * n + xx] = 64
    with open(base + ".r16", "wb") as f:
        f.write(struct.pack("<%dh" % len(vals), *vals))
    m.ent("commander", 1, [a, a])
    for k in range(6):
        m.ent("metal_spot", 0, [a + 6 + 2 * (k % 3), a - 4 + 4 * (k // 3)])
    j = m.to_json()
    j["terrain"]["cornersFile"] = "maps/" + os.path.basename(base) + ".r16"   # relative to the game folder
    with open(path, "w", encoding="utf-8") as f:
        json.dump(j, f, ensure_ascii=False)
    print("wrote", path, "and", base + ".r16")


def write(game, m, fname=None):
    d = os.path.join(ROOT, game, "maps")
    os.makedirs(d, exist_ok=True)
    path = os.path.join(d, (fname or m.name.lower().replace(" ", "_")) + ".json")
    j = m.to_json()
    corners = j["terrain"].pop("corners", None)
    text = json.dumps(j, ensure_ascii=False, indent=1)
    if corners:  # one map row of corners per line instead of one number per line
        n = m.w + 1
        rows = ",\n   ".join(", ".join(str(v) for v in corners[i:i + n]) for i in range(0, len(corners), n))
        text = text.replace('"terrain": {', '"terrain": {\n  "corners": [\n   ' + rows + '\n  ],', 1)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text + "\n")
    print("wrote", os.path.relpath(path, ROOT))


# ================================================================ games

def frostcrown():
    m = Map("Frozen Pass", 64, 48, duel())
    m.border()
    m.disc(32, 24, 4, '~', sym=False)
    m.rect(20, 10, 3, 6, '#')
    m.ent("great_hall", 1, [10, 24])
    for i in range(5):
        m.ent("peon", 1, [14 + (i % 3) * 0.8, 21 + i])
    m.ent("blademaster", 1, [15, 27])  # starting hero: shows the ability kit (chain lightning, rejuvenation)
    m.ent("burrow", 1, [10, 30])
    m.ent("gold_mine", 0, [10.5, 17.5])
    for y in range(30, 44):
        for x in range(2, 6):
            m.ent("pine", 0, [x + 0.5, y + 0.5])
    # creep camps guarding expansions and the middle
    m.ent("gold_mine", 0, [22.5, 38.5])
    m.ent("troll", -1, [24, 35])
    m.ent("troll", -1, [25, 36])
    m.ent("ogre", -1, [32, 8], sym=False)
    m.ent("ogre", -1, [32, 40], sym=False)
    write("frostcrown", m, "frozen_pass")


def starfall():
    m = Map("Twin Plateaus", 64, 48, duel("guild", "swarm"))
    m.border()
    # high ground plateaus for both mains, ramps facing the centre
    m.level_rect(2, 2, 20, 20, 1)
    m.rect(22, 10, 1, 3, '/')
    m.rect(21, 10, 1, 3, '/')
    m.ent("mineral", 0, [5.5, 5.5]); m.ent("mineral", 0, [6.5, 4.5]); m.ent("mineral", 0, [4.5, 7.5])
    m.ent("mineral", 0, [8.5, 4.5]); m.ent("mineral", 0, [4.5, 9.5])
    m.ent("geyser", 0, [15, 4])
    m.ent("mineral", 0, [30.5, 40.5]); m.ent("mineral", 0, [31.5, 41.5])
    m.players[0]["start"] = [11, 12]
    m.players[1]["start"] = m.mirror([11, 12])
    write("starfall", m, "twin_plateaus")


def ironfront():
    m = Map("Ore Valley", 64, 48, duel())
    m.border()
    m.rect(28, 0, 8, 14, '~')
    m.rect(28, 18, 3, 6, ',')
    m.ent("mcv", 1, [10, 24])
    m.ent("rifleman", 1, [13, 26]); m.ent("rifleman", 1, [13, 27])
    m.field("ore", 12, 12, 4)
    m.field("ore", 26, 36, 3)
    m.ent("ore_mine", 0, [12, 12])
    write("ironfront", m, "ore_valley")


def ambersands():
    m = Map("Spice Basin", 64, 48, duel())
    # mostly sand ('.' with spice); rock plateaus for bases
    m.rect(3, 15, 16, 18, 'R')
    m.rect(28, 20, 8, 8, 'R', sym=False)
    m.ent("mcv", 1, [10, 24])
    m.ent("trooper", 1, [14, 26]); m.ent("trooper", 1, [14, 27])
    m.field("spice", 23, 18, 4)
    m.field("spice", 12, 40, 4)
    m.ent("sandworm", -1, [32, 4], sym=False)
    write("ambersands", m, "spice_basin")


def metaltide_heights(w, h, bases, spots):
    """Twin Craters relief (world units): a ridge between the bases that blocks sight, too steep for heavy
    kbots (maxSlope 0.8) but fine for light ones (1.6), two gentle saddles, rolling hills; flat pads at bases
    and metal spots. Point-symmetric like the rest of the map."""
    import math

    def tri(d, r):
        return max(0.0, 1.0 - abs(d) / r)

    def half(x, y):
        saddle = max(tri(y - 12, 6), tri(y - 36, 6))
        ridge = (4.0 - 2.6 * saddle) * tri(x - 32, 4.0 if saddle < 0.5 else 5.0)
        hills = 0.9 * max(0.0, math.sin(x * 0.35) * math.cos(y * 0.3)) + 0.5 * tri(math.hypot(x - 16, y - 40), 6)
        return ridge, hills

    def f(x, y):
        r1, h1 = half(x, y)
        r2, h2 = half(w - x, h - y)
        v = max(r1, r2) + 0.5 * (h1 + h2)
        for (px, py, r) in bases + spots:
            for (qx, qy) in ((px, py), (w - px, h - py)):
                k = min(1.0, max(0.0, (math.hypot(x - qx, y - qy) - r) / 3.0))
                v *= k
        return v
    return f


def metaltide():
    m = Map("Twin Craters", 64, 48, duel())
    m.border()
    m.disc(32, 24, 5, '#', sym=False)
    m.heightfield(metaltide_heights(64, 48, [(10, 24, 7)],
                                    [(6, 18, 2), (6, 30, 2), (14, 16, 2), (14, 32, 2), (20, 24, 2), (24, 10, 2), (26, 38, 2),
                                     (16, 20, 1), (16, 28, 1), (8, 12, 1)]))
    m.ent("commander", 1, [10, 24])
    for p in [(6, 18), (6, 30), (14, 16), (14, 32), (20, 24), (24, 10), (26, 38)]:
        m.ent("metal_spot", 0, [p[0], p[1]])
    for p in [(16, 20), (16, 28), (8, 12)]:
        m.ent("wreck_rock", 0, [p[0] + 0.5, p[1] + 0.5])
    write("metaltide", m, "twin_craters")


def fourages():
    m = Map("Highland", 64, 48, duel())
    m.border()
    m.rect(30, 4, 4, 14, '~')
    m.ent("town_center", 1, [10, 24])
    for i in range(4):
        m.ent("villager", 1, [14, 21 + i])
    m.ent("gold_pile", 0, [8.5, 16.5]); m.ent("stone_pile", 0, [16.5, 31.5])
    m.ent("berry_bush", 0, [16.5, 18.5]); m.ent("berry_bush", 0, [17.5, 19.5])
    for y in range(28, 44):
        for x in range(2, 6):
            m.ent("oak", 0, [x + 0.5, y + 0.5])
    for x in range(14, 26):
        for y in range(3, 5):
            m.ent("oak", 0, [x + 0.5, y + 0.5])
    write("fourages", m, "highland")


def holdtheline():
    m = Map("Crossroads", 64, 48, duel())
    m.border()
    m.rect(20, 8, 4, 4, '#')
    m.rect(26, 30, 3, 6, '#')
    m.ent("hq", 1, [8, 24])
    m.ent("riflemen", 1, [12, 22]); m.ent("riflemen", 1, [12, 26])
    m.ent("engineer", 1, [11, 24])
    m.ent("fuel_point", 0, [16, 12])
    m.ent("victory_point", 0, [32, 24], sym=False)
    m.ent("victory_point", 0, [24, 12])
    m.ent("munition_point", 0, [18, 36])
    write("holdtheline", m, "crossroads")


def lastcolony():
    players = [{"name": "Колония", "team": 1}, {"name": "Орда", "team": 2, "waves": True}]
    m = Map("Last Valley", 64, 48, players)
    m.border()
    m.rect(40, 4, 3, 16, '#', sym=False)
    m.rect(40, 28, 3, 16, '#', sym=False)
    m.ent("command_center", 1, [14, 24], sym=False)
    for i in range(4):
        m.ent("colonist", 1, [18, 21 + i], sym=False)
    m.ent("ranger", 1, [20, 22], sym=False); m.ent("ranger", 1, [20, 26], sym=False)
    m.ent("ranger", 1, [21, 24], sym=False)
    m.ent("stone_quarry", 0, [8.5, 14.5], sym=False)
    m.ent("stone_quarry", 0, [8.5, 33.5], sym=False)
    for y in range(8, 40, 2):
        m.ent("tree", 0, [3.5, y + 0.5], sym=False)
    m.extra["waves"] = [
        {"time": 90, "player": 2, "at": [60, 24], "spread": 3, "spawn": [{"type": "walker", "count": 5}], "every": 45, "growth": 1.25, "repeats": 14},
        {"time": 240, "player": 2, "at": [60, 6], "spread": 2, "spawn": [{"type": "runner", "count": 3}], "every": 60, "growth": 1.2, "repeats": 8},
    ]
    write("lastcolony", m, "last_valley")


def clanlands():
    m = Map("Fjords", 48, 36, duel())
    m.border()
    # 3x3 grid of regions, mirrored: A..E with C in the middle
    letters = [["A", "B", "C"], ["D", "E", "F"], ["G", "H", "I"]]
    names = {"A": "Родной фьорд", "B": "Северный лес", "C": "Ледяной склон", "D": "Западная долина",
             "E": "Сердце земли", "F": "Восточная долина", "G": "Склон теней", "H": "Южный лес", "I": "Чужой фьорд"}
    for ry in range(3):
        for rx in range(3):
            m.region_rect(rx * 16, ry * 12, 16, 12, letters[ry][rx])
    m.region_names = names
    m.rect(15, 4, 1, 5, '#'); m.rect(31, 4, 1, 5, '#')
    m.ent("longhouse", 1, [7, 6])
    for i in range(3):
        m.ent("thrall", 1, [11, 4 + i])
    m.ent("deer_herd", 0, [24, 6]); m.ent("deer_herd", 0, [8, 18])
    m.ent("deer_herd", 0, [24, 18], sym=False)
    for x in range(18, 26):
        m.ent("birch", 0, [x + 0.5, 2.5])
    write("clanlands", m, "fjords")


def flagsectors():
    m = Map("Sector Strip", 64, 32, duel())
    m.border()
    names = {}
    for i, l in enumerate("ABCDEFGH"):
        m.region_rect(i * 8, 0, 8, 32, l)
        names[l] = "Сектор " + str(i + 1)
    m.region_names = names
    m.rect(15, 6, 2, 6, '#'); m.rect(31, 20, 2, 6, '#', sym=False)
    m.ent("fort", 1, [4, 16])
    m.ent("robot", 1, [8, 14]); m.ent("robot", 1, [8, 18])
    for i, x in enumerate([12, 20, 28]):
        m.ent("flag", 0, [x, 16 + (i % 2) * 6 - 3])
        m.ent("factory", 0, [x - 2, 8 + (i % 2) * 14])
    write("flagsectors", m, "sector_strip")


def laneclash():
    m = Map("Long Lane", 20, 56, duel())
    m.border()
    m.rect(1, 20, 6, 16, '#', sym=False)
    m.rect(13, 20, 6, 16, '#', sym=False)
    m.ent("core", 1, [10, 50])
    write("laneclash", m, "long_lane")


def underlord():
    m = Map("Two Keeps", 48, 32, duel())
    m.rows = [['D'] * 48 for _ in range(32)]
    m.border('#')
    # dungeon pockets around the hearts
    m.rect(2, 10, 9, 12, '.')
    # gold veins and a gem seam
    for (x, y) in [(12, 9), (13, 9), (12, 22), (14, 15), (20, 6), (21, 6), (18, 25)]:
        m.cell(x, y, 'G'); m.cell(47 - x, 31 - y, 'G')
    m.cell(23, 15, 'J'); m.cell(24, 16, 'J')
    # neutral portals in open caves on each side of the middle
    m.rect(14, 2, 6, 5, '.')
    m.ent("dungeon_heart", 1, [6, 16])
    for i in range(4):
        m.ent("imp", 1, [9 + (i % 2) * 0.8, 13 + i])
    m.ent("portal", 0, [17, 4])
    write("underlord", m, "two_keeps")


def borderrush():
    m = Map("Five Lands", 24, 42, duel())
    m.border()
    names = {}
    # regions stacked vertically (portrait): A enemy home, B C D middle, E own home
    for i, l in enumerate("ABCDE"):
        m.region_rect(1, 1 + i * 8, 22, 8, l)
        names[l] = ["Чужбина", "Равнина", "Перевал", "Холмы", "Родина"][i]
    m.region_names = names
    m.rect(4, 20, 5, 2, '#')
    m.ent("capital", 1, [12, 37])
    m.ent("town", 0, [6, 29])
    m.ent("town", 0, [17, 24])
    write("borderrush", m, "five_lands")


def hearthhaul():
    m = Map("Green Dale", 56, 40, duel())
    m.border()
    m.rect(26, 2, 4, 12, '~')
    m.ent("storehouse", 1, [8, 20], inventory={"planks": 30, "stone": 30, "bread": 10, "sword": 2})
    m.ent("school", 1, [8, 26])
    for i in range(3):
        m.ent("serf", 1, [12, 18 + i])
    m.ent("labourer", 1, [12, 22]); m.ent("labourer", 1, [12, 23])
    m.ent("militia", 1, [13, 25]); m.ent("militia", 1, [13, 26])
    for (x, y) in [(4, 10), (5, 12), (6, 9), (4, 13), (7, 11), (5, 8), (3, 11), (6, 14)]:
        m.ent("tree", 0, [x + 0.5, y + 0.5])
    m.ent("rock", 0, [16.5, 32.5]); m.ent("rock", 0, [18.5, 33.5])
    write("hearthhaul", m, "green_dale")


def sporelanes():
    m = Map("Spore Field", 40, 24, duel())
    m.border()
    m.rect(18, 6, 2, 5, '#')
    m.disc(12, 18, 1.5, '#')
    m.ent("hive", 1, [5, 12], garrison=15)
    m.ent("hut", 0, [12, 6], garrison=6)
    m.ent("hut", 0, [14, 13], garrison=8)
    m.ent("tower", 0, [20, 18], garrison=10, sym=False)
    m.ent("hut", 0, [20, 3], garrison=12, sym=False)
    write("sporelanes", m, "spore_field")


def spellbunkers():
    blue = ["freeze", "shield", "plague", "defector", "panic", "bfg", "chain_lightning", "tornado", "mines", "rage",
            "landing", "silence", "harvest", "break_magic", "revenge", "tide"]
    red = ["freeze_wave", "invulnerability", "plague", "sabotage", "go_home", "bfg", "chain_lightning", "mines", "kill_aura",
           "resurrection", "morale_thief", "possession", "sacrifice", "break_magic", "tide", "wisdom"]
    m = Map("Rune Field", 56, 30, duel(a={"loadout": blue}, b={"loadout": red}))
    m.border()
    m.rect(26, 7, 2, 6, '#')
    m.disc(16, 22, 1.5, '#')
    m.rect(10, 4, 3, 1, '#')
    m.ent("hive", 1, [5, 15], garrison=20)
    m.ent("hut", 1, [8, 25], garrison=8)
    m.ent("hut", 0, [14, 7], garrison=10)
    m.ent("hut", 0, [19, 15], garrison=12)
    m.ent("tower", 0, [12, 18], garrison=8)
    m.ent("hut", 0, [22, 25], garrison=14)
    m.ent("tower", 0, [28, 22], garrison=12, sym=False)
    m.ent("hive", 0, [28, 4], garrison=18, sym=False)
    write("spellbunkers", m, "rune_field")


GAMES = {f.__name__: f for f in [frostcrown, starfall, ironfront, ambersands, metaltide, fourages, holdtheline,
                                   lastcolony, clanlands, flagsectors, laneclash, underlord, borderrush, hearthhaul, sporelanes,
                                   spellbunkers]}

if __name__ == "__main__":
    if sys.argv[1:2] == ["--big"]:   # mapgen.py --big 1024 /scratch/big.json
        write_big_heightfield(sys.argv[3], int(sys.argv[2]))
        sys.exit(0)
    for g in (sys.argv[1:] or GAMES):
        GAMES[g]()
