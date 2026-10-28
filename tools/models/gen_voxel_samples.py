#!/usr/bin/env python3
"""Generates voxel sample models (CC0, written from scratch, no third-party data).

  voxel_tank.vox   MagicaVoxel scene: "Hull" + "Turret" (group: "TurretBody", "Barrel"), own RGBA
                   palette, team-colour stripes in the team slots (colour indices 248..255, see
                   engine/render/ModelVox.cpp), emissive headlights (MATL _emit). Faces +Z in the
                   engine (MagicaVoxel front = -Y).
  --vxl DIR        also writes voxel_tank.vxl + voxel_tank.hva (Westwood format: sections Body,
                   Turret, Barrel; remap colours 16..31 as team stripes; 8 HVA frames turning the
                   turret). Only for checking the loader; not used by any game.

Usage: python3 tools/models/gen_voxel_samples.py templates/showcase/models [--vxl /tmp/vxl]
"""
import math, os, struct, sys

# ------------------------------------------------------------------ MagicaVoxel writer

def chunk(cid, content=b"", children=b""):
    return cid.encode() + struct.pack("<ii", len(content), len(children)) + content + children

def vstr(s):
    b = s.encode()
    return struct.pack("<i", len(b)) + b

def vdict(d):
    return struct.pack("<i", len(d)) + b"".join(vstr(k) + vstr(str(v)) for k, v in d.items())

def ntrn(nid, child, attrs=None, t=None):
    frame = {"_t": "%d %d %d" % t} if t else {}
    return chunk("nTRN", struct.pack("<i", nid) + vdict(attrs or {}) + struct.pack("<iiii", child, -1, 0, 1) + vdict(frame))

def ngrp(nid, kids):
    return chunk("nGRP", struct.pack("<i", nid) + vdict({}) + struct.pack("<i", len(kids)) + b"".join(struct.pack("<i", k) for k in kids))

def nshp(nid, model):
    return chunk("nSHP", struct.pack("<i", nid) + vdict({}) + struct.pack("<ii", 1, model) + vdict({}))

class Grid:
    def __init__(self, sx, sy, sz):
        self.size = (sx, sy, sz)
        self.v = {}
    def box(self, x0, x1, y0, y1, z0, z1, c):  # inclusive ranges
        for x in range(x0, x1 + 1):
            for y in range(y0, y1 + 1):
                for z in range(z0, z1 + 1):
                    if 0 <= x < self.size[0] and 0 <= y < self.size[1] and 0 <= z < self.size[2]:
                        if c: self.v[(x, y, z)] = c
                        else: self.v.pop((x, y, z), None)
    def chunks(self):
        xyzi = struct.pack("<i", len(self.v)) + b"".join(struct.pack("<BBBB", x, y, z, c) for (x, y, z), c in sorted(self.v.items()))
        return chunk("SIZE", struct.pack("<iii", *self.size)) + chunk("XYZI", xyzi)

OLIVE, DARK, TRACK, WHEEL, STEEL, LIGHT, HATCH = 1, 2, 3, 4, 5, 6, 7
TEAM = 250   # one of the team slots 248..255

def vox_tank():
    # MagicaVoxel axes: x right, y back (front = -y), z up
    hull = Grid(14, 20, 6)
    hull.box(0, 2, 0, 19, 0, 3, TRACK)            # tracks
    hull.box(11, 13, 0, 19, 0, 3, TRACK)
    for y in range(2, 19, 4):                     # wheels on the track sides
        hull.box(0, 0, y, y + 1, 1, 2, WHEEL)
        hull.box(13, 13, y, y + 1, 1, 2, WHEEL)
    hull.box(2, 11, 1, 18, 1, 5, OLIVE)           # body
    hull.box(3, 10, 0, 0, 1, 3, DARK)             # front plate
    hull.box(3, 4, 0, 0, 3, 3, LIGHT)             # headlights
    hull.box(9, 10, 0, 0, 3, 3, LIGHT)
    hull.box(2, 2, 2, 17, 4, 4, TEAM)             # team stripes along both sides
    hull.box(11, 11, 2, 17, 4, 4, TEAM)
    hull.box(2, 11, 15, 16, 5, 5, TEAM)           # and across the rear deck
    turret = Grid(8, 8, 3)
    turret.box(0, 7, 0, 7, 0, 2, OLIVE)
    turret.box(0, 0, 0, 0, 0, 2, 0)               # chamfered corners
    turret.box(7, 7, 0, 0, 0, 2, 0)
    turret.box(0, 0, 7, 7, 0, 2, 0)
    turret.box(7, 7, 7, 7, 0, 2, 0)
    turret.box(1, 6, 1, 6, 2, 2, DARK)
    turret.box(4, 5, 4, 5, 2, 2, HATCH)
    turret.box(0, 0, 2, 5, 1, 1, TEAM)
    turret.box(7, 7, 2, 5, 1, 1, TEAM)
    barrel = Grid(2, 8, 2)
    barrel.box(0, 1, 0, 7, 0, 1, STEEL)
    barrel.box(0, 1, 0, 0, 0, 1, DARK)            # muzzle
    pal = [(0, 0, 0, 255)] * 256                  # entry i -> colour index i + 1
    def setc(i, rgb): pal[i - 1] = rgb + (255,)
    setc(OLIVE, (104, 116, 64)); setc(DARK, (66, 74, 44)); setc(TRACK, (44, 44, 46)); setc(WHEEL, (92, 92, 96))
    setc(STEEL, (84, 88, 82)); setc(LIGHT, (255, 236, 170)); setc(HATCH, (120, 104, 70))
    for i in range(248, 256): setc(i, (200, 200, 200))   # team slots: neutral grey in tools
    rgba = chunk("RGBA", b"".join(struct.pack("<BBBB", *c) for c in pal))
    matl = chunk("MATL", struct.pack("<i", LIGHT) + vdict({"_type": "_emit", "_emit": "1", "_flux": "1"}))
    scene = (ntrn(0, 1) + ngrp(1, [2, 4]) +
             ntrn(2, 3, {"_name": "Hull"}, (0, 0, 3)) + nshp(3, 0) +
             ntrn(4, 5, {"_name": "Turret"}, (0, 2, 7)) + ngrp(5, [6, 8]) +
             ntrn(6, 7, {"_name": "TurretBody"}) + nshp(7, 1) +
             ntrn(8, 9, {"_name": "Barrel"}, (0, -8, 0)) + nshp(9, 2))
    body = hull.chunks() + turret.chunks() + barrel.chunks() + scene + rgba + matl
    return b"VOX " + struct.pack("<i", 150) + chunk("MAIN", b"", body)

# ------------------------------------------------------------------ Westwood VXL / HVA writer

def vxl_section_body(grid):
    sx, sy, sz = grid.size
    starts, ends, data = [], [], b""
    for y in range(sy):
        for x in range(sx):
            zs = [z for z in range(sz) if (x, y, z) in grid.v]
            if not zs:
                starts.append(-1); ends.append(-1); continue
            starts.append(len(data))
            col, z = b"", 0
            while z < sz:
                if (x, y, z) not in grid.v:
                    z0 = z
                    while z < sz and (x, y, z) not in grid.v: z += 1
                    if z == sz:  # trailing gap
                        col += struct.pack("<BBB", z - z0, 0, 0)
                        break
                    skip = z - z0
                else:
                    skip = 0
                run = []
                while z < sz and (x, y, z) in grid.v:
                    run.append(grid.v[(x, y, z)]); z += 1
                col += struct.pack("<BB", skip, len(run)) + b"".join(struct.pack("<BB", c, 0) for c in run) + struct.pack("<B", len(run))
            data += col
            ends.append(len(data) - 1)
    return starts, ends, data

def vxl_file(sections):
    """sections: list of (name, grid, bmin, bmax)"""
    pal = bytearray(768)
    for i in range(256):
        g = i
        pal[i * 3:i * 3 + 3] = bytes((g, g, g))
    for i in range(16, 32):                       # remap ramp (red in the games' palettes)
        k = 255 - (i - 16) * 12
        pal[i * 3:i * 3 + 3] = bytes((k, k // 6, k // 8))
    pal[64 * 3:64 * 3 + 3] = bytes((104, 116, 64)); pal[65 * 3:65 * 3 + 3] = bytes((66, 74, 44))
    pal[66 * 3:66 * 3 + 3] = bytes((44, 44, 46)); pal[67 * 3:67 * 3 + 3] = bytes((84, 88, 82))
    body, tails = b"", b""
    for name, grid, bmin, bmax in sections:
        starts, ends, data = vxl_section_body(grid)
        s0 = len(body)
        body += struct.pack("<%di" % len(starts), *starts)
        e0 = len(body)
        body += struct.pack("<%di" % len(ends), *ends)
        d0 = len(body)
        body += data
        xf = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0]
        tails += struct.pack("<IIIf12f3f3fBBBB", s0, e0, d0, 1 / 12, *xf, *bmin, *bmax, *grid.size, 4)
    head = b"Voxel Animation\0" + struct.pack("<IIIIBB", 1, len(sections), len(sections), len(body), 16, 31) + bytes(pal)
    hdrs = b"".join(name.encode().ljust(16, b"\0") + struct.pack("<III", i, 1, 0) for i, (name, *_r) in enumerate(sections))
    return head + hdrs + body + tails

def hva_file(names, frames):
    """frames: list of per-section 3x4 row-major matrices (12 floats)"""
    out = b"voxel_tank".ljust(16, b"\0") + struct.pack("<II", len(frames), len(names))
    out += b"".join(n.encode().ljust(16, b"\0") for n in names)
    for f in frames:
        for mtx in f: out += struct.pack("<12f", *mtx)
    return out

def vxl_tank():
    BODY, DARKC, TRACKC, STEELC = 64, 65, 66, 67
    body = Grid(20, 14, 6)                         # x forward, y left, z up
    body.box(0, 19, 0, 2, 0, 3, TRACKC)
    body.box(0, 19, 11, 13, 0, 3, TRACKC)
    body.box(1, 18, 2, 11, 1, 5, BODY)
    body.box(19, 19, 3, 10, 1, 3, DARKC)
    for x in range(2, 18):
        body.box(x, x, 2, 2, 4, 4, 16 + (x % 16))  # remap stripe (gradient through 16..31)
        body.box(x, x, 11, 11, 4, 4, 16 + (x % 16))
    turret = Grid(8, 8, 3)
    turret.box(0, 7, 0, 7, 0, 2, BODY)
    turret.box(1, 6, 1, 6, 2, 2, DARKC)
    turret.box(2, 5, 0, 0, 1, 1, 20)
    turret.box(2, 5, 7, 7, 1, 1, 20)
    barrel = Grid(8, 2, 2)
    barrel.box(0, 7, 0, 1, 0, 1, STEELC)
    secs = [("Body", body, (-10, -7, 0), (10, 7, 6)), ("Turret", turret, (-4, -4, 0), (4, 4, 3)),
            ("Barrel", barrel, (0, -1, 0), (8, 1, 2))]
    frames = []
    for f in range(8):
        a = math.radians(f * 45)
        c, s = math.cos(a), math.sin(a)
        tur = (-1.0, 0.0, 6.0)                     # voxel units; HVA stores them x12 (1 / section scale)
        off = (4.0, 0.0, 0.5)                      # barrel pivot in turret space
        bar = (tur[0] + c * off[0] - s * off[1], tur[1] + s * off[0] + c * off[1], tur[2] + off[2])
        rot = lambda t: [c, -s, 0, t[0] * 12, s, c, 0, t[1] * 12, 0, 0, 1, t[2] * 12]
        frames.append([[1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0], rot(tur), rot(bar)])
    return vxl_file(secs), hva_file([s[0] for s in secs], frames)

if __name__ == "__main__":
    args = sys.argv[1:]
    vxl_dir = None
    if "--vxl" in args:
        i = args.index("--vxl"); vxl_dir = args[i + 1]; del args[i:i + 2]
    out = args[0] if args else "templates/showcase/models"
    os.makedirs(out, exist_ok=True)
    with open(os.path.join(out, "voxel_tank.vox"), "wb") as f: f.write(vox_tank())
    if vxl_dir:
        os.makedirs(vxl_dir, exist_ok=True)
        v, h = vxl_tank()
        with open(os.path.join(vxl_dir, "voxel_tank.vxl"), "wb") as f: f.write(v)
        with open(os.path.join(vxl_dir, "voxel_tank.hva"), "wb") as f: f.write(h)
