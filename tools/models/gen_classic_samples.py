#!/usr/bin/env python3
"""Writes small synthetic Total Annihilation (.3do + .gaf + palette) and C&C Generals (.w3d + .dds)
models for checking the classic loaders (CC0, written from scratch; no game data involved).

  ta/tank.3do              pieces base -> turret -> sleeve -> barrel -> flare, palette-coloured and
                           textured quads, a selection primitive, a duplicate face, a line
  ta/palette.pal           256 x RGBX (a generic palette, not TA's)
  ta/textures/armplate.gaf one-frame texture (run-length compressed)
  ta/textures/armlogo.gaf  10-frame team-colour texture (frame per player)
  w3d/tank.w3d             hierarchy ROOTTRANSFORM / TURRET / BARREL / FIREFX01, HLOD with two LODs,
                           rigid meshes on bones, a HOUSECOLOR mesh, a skinned ANTENNA, animation
                           TANK.TURN (classic channels: turret rotation, barrel translation)
  w3d/tank.anims.txt       -> tank_skl_recoil.w3d (compressed time-coded) + tank_skl_wobble.w3d
                           (adaptive delta)
  w3d/textures/hull.dds    DXT1, w3d/textures/turret.dds A8R8G8B8, w3d/textures/zhca_stripe.dds DXT5

The models face +Z after loading (TA: native -Z, W3D: native +X), upright.
Usage: python3 tools/models/gen_classic_samples.py OUTDIR
"""
import math, os, struct, sys

OUT = sys.argv[1] if len(sys.argv) > 1 else "/tmp/classic_samples"

# ------------------------------------------------------------------ TA

def palette():
    pal = []
    for i in range(256):
        if i < 16:
            v = int(i / 15 * 255); pal.append((v, v, v))
        else:
            h = (i // 16 - 1) / 15 * 6; v = 0.25 + 0.75 * (i % 16) / 15
            f = h - math.floor(h); s = int(h) % 6
            q = v * (1 - 0.7 * f); t = v * (1 - 0.7 * (1 - f)); lo = v * 0.3
            r, g, b = [(v, t, lo), (q, v, lo), (lo, v, t), (lo, q, v), (t, lo, v), (v, lo, q)][s]
            pal.append((int(r * 255), int(g * 255), int(b * 255)))
    return pal

def box(x0, y0, z0, x1, y1, z1):
    """Vertices + quads wound counter-clockwise seen from outside (TA native frame)."""
    v = [(x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0), (x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)]
    q = [(0, 3, 2, 1), (4, 5, 6, 7), (0, 1, 5, 4), (3, 7, 6, 2), (0, 4, 7, 3), (1, 2, 6, 5)]  # -z +z -y +y -x +x
    return v, q

class Ta3do:
    def __init__(self):
        self.pieces = []  # name, offset, verts, prims [(color, idx, tex)], children, selection

    def piece(self, name, off, verts, prims, children=(), selection=-1):
        return dict(name=name, off=off, verts=verts, prims=prims, children=list(children), sel=selection)

    def write(self, root):
        out = bytearray()
        def alloc(n):
            at = len(out); out.extend(b"\0" * n); return at
        def string(s):
            at = len(out); out.extend(s.encode() + b"\0"); return at
        def emit(p, sibling_of=None):
            at = alloc(52)
            name = string(p["name"])
            vat = len(out)
            for x, y, z in p["verts"]:
                out.extend(struct.pack("<3i", int(x * 65536), int(y * 65536), int(z * 65536)))
            pat = alloc(32 * len(p["prims"]))
            for k, (color, idx, tex) in enumerate(p["prims"]):
                iat = len(out); out.extend(struct.pack("<%dH" % len(idx), *idx))
                tat = string(tex) if tex else 0
                struct.pack_into("<8i", out, pat + 32 * k, color, len(idx), 0, iat, tat, 0, 0, 0)
            child = 0
            prev = None
            for c in p["children"]:
                cat = emit(c)
                if prev is None: child = cat
                else: struct.pack_into("<i", out, prev + 44, cat)
                prev = cat
            ox, oy, oz = p["off"]
            struct.pack_into("<13i", out, at, 1, len(p["verts"]), len(p["prims"]), p["sel"],
                             int(ox * 65536), int(oy * 65536), int(oz * 65536), name, 0, vat, pat, 0, child)
            return at
        emit(root)
        return bytes(out)

def gaf(entries):
    """entries: [(name, [frame pixel rows (list of palette indices)...], compressed)]"""
    out = bytearray(struct.pack("<3I", 0x00010100, len(entries), 0))
    table = len(out); out.extend(b"\0" * 4 * len(entries))
    for e, (name, frames, compressed) in enumerate(entries):
        struct.pack_into("<I", out, table + 4 * e, len(out))
        ent = len(out)
        out.extend(struct.pack("<HHI", len(frames), 1, 0) + name.encode().ljust(32, b"\0"))
        ft = len(out); out.extend(b"\0" * 8 * len(frames))
        for f, rows in enumerate(frames):
            h = len(rows); w = len(rows[0])
            fat = len(out); out.extend(b"\0" * 24)
            dat = len(out)
            if compressed:
                for row in rows:
                    line = bytearray(); x = 0
                    while x < w:
                        n = 1
                        while x + n < w and row[x + n] == row[x] and n < 64: n += 1
                        if row[x] == 9:  # transparent run
                            line.append((n << 1) | 1)
                        elif n > 1:
                            line.append(((n - 1) << 2) | 2); line.append(row[x])
                        else:
                            line.append(0); line.append(row[x])
                        x += n
                    out.extend(struct.pack("<H", len(line)) + line)
            else:
                for row in rows: out.extend(bytes(row))
            struct.pack_into("<HHhhBBHIII", out, fat, w, h, 0, 0, 9, 1 if compressed else 0, 0, 0, dat, 0)
            struct.pack_into("<II", out, ft + 8 * f, fat, 1)
    return bytes(out)

def write_ta(d):
    os.makedirs(os.path.join(d, "textures"), exist_ok=True)
    with open(os.path.join(d, "palette.pal"), "wb") as f:
        for r, g, b in palette(): f.write(bytes((r, g, b, 0)))
    plate = [[(16 * 3 + 12) if (x // 4 + y // 4) % 2 else (16 * 3 + 6) for x in range(16)] for y in range(16)]
    plate[0][0] = 9  # one transparent pixel exercises the skip runs
    with open(os.path.join(d, "textures", "armplate.gaf"), "wb") as f:
        f.write(gaf([("armplate", [plate], True)]))
    frames = []
    for player in range(10):
        frames.append([[(16 * (1 + player) + 12) if 4 <= x < 12 and 4 <= y < 12 else 15 for x in range(16)] for y in range(16)])
    with open(os.path.join(d, "textures", "armlogo.gaf"), "wb") as f:
        f.write(gaf([("armlogo", frames, False)]))
    t = Ta3do()
    bv, bq = box(-6, 0, -9, 6, 4, 9)
    base_prims = [(16 * 9 + 8, q, None) for q in bq]
    base_prims[3] = (0, bq[3], "armplate")                 # top: textured
    base_prims[1] = (0, bq[1], "armlogo")                  # back (+z native = engine -z): team logo
    base_prims.append((16 * 9 + 8, bq[0], None))           # duplicate of the front face (dropped)
    base_prims.append((0, (0, 1), None))                   # a line (skipped)
    sel = len(base_prims)
    sv = len(bv)
    bv += [(-8, 0.01, -11), (8, 0.01, -11), (8, 0.01, 11), (-8, 0.01, 11)]
    base_prims.append((0, (sv, sv + 1, sv + 2, sv + 3), None))   # selection primitive (skipped)
    tv, tq = box(-3.5, 0, -3.5, 3.5, 3, 3.5)
    sleeve_v, sleeve_q = box(-1.2, -1, -1.5, 1.2, 1, 1.5)
    barrel_v, barrel_q = box(-0.5, -0.5, -9, 0.5, 0.5, 0)        # points along native -z = engine +z
    flare = t.piece("flare", (0, 0, -9), [], [])
    barrel = t.piece("barrel", (0, 0, -1.5), barrel_v, [(16 * 0 + 6, q, None) for q in barrel_q], [flare])
    sleeve = t.piece("sleeve", (0, 1.5, -2), sleeve_v, [(16 * 12 + 9, q, None) for q in sleeve_q], [barrel])
    turret = t.piece("turret", (0, 4, 1), tv, [(16 * 5 + 10, q, None) for q in tq], [sleeve])
    base = t.piece("base", (0, 0, 0), bv, base_prims, [turret], selection=sel)
    with open(os.path.join(d, "tank.3do"), "wb") as f:
        f.write(t.write(base))

# ------------------------------------------------------------------ DDS

def dds_header(w, h, fourcc=None, bits=0, masks=(0, 0, 0, 0), mips=1, alpha=False):
    flags = 0x1 | 0x2 | 0x4 | 0x1000 | (0x20000 if mips > 1 else 0)
    if fourcc:
        pf = struct.pack("<II4sI4I", 32, 0x4, fourcc, 0, 0, 0, 0, 0)
    else:
        pf = struct.pack("<IIII4I", 32, 0x40 | (0x1 if alpha else 0), 0, bits, *masks)
    hdr = struct.pack("<7I", 124, flags, h, w, 0, 0, mips) + b"\0" * 44 + pf + struct.pack("<5I", 0x1000, 0, 0, 0, 0)
    return b"DDS " + hdr

def rgb565(r, g, b):
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)

def dxt1_solid(w, h, color_at, mips=True):
    data = bytearray(); levels = 0
    while True:
        for by in range((h + 3) // 4):
            for bx in range((w + 3) // 4):
                c = rgb565(*color_at(bx * 4 * 1.0 / w, by * 4 * 1.0 / h))
                data.extend(struct.pack("<HHI", c | 0, c, 0))
        levels += 1
        if not mips or (w == 1 and h == 1): break
        w, h = max(1, w // 2), max(1, h // 2)
    return data, levels

def write_dds(d):
    os.makedirs(os.path.join(d, "textures"), exist_ok=True)
    data, levels = dxt1_solid(32, 32, lambda u, v: (150, 140, 90) if (int(u * 8) + int(v * 8)) % 2 else (110, 105, 70))
    with open(os.path.join(d, "textures", "hull.dds"), "wb") as f:
        f.write(dds_header(32, 32, b"DXT1", mips=levels) + data)
    px = bytearray()
    for y in range(16):
        for x in range(16):
            r, g, b = (90, 110, 80) if (x // 2) % 2 else (70, 90, 60)
            px.extend(bytes((b, g, r, 255)))
    with open(os.path.join(d, "textures", "turret.dds"), "wb") as f:
        f.write(dds_header(16, 16, bits=32, masks=(0xff0000, 0xff00, 0xff, 0xff000000), alpha=True) + px)
    # DXT5 white with alpha 0 (= house colour) in the middle band
    blocks = bytearray()
    for by in range(4):
        for bx in range(4):
            a = 0 if 1 <= by <= 2 else 255
            blocks.extend(struct.pack("<BB6s", a, a, b"\0" * 6))
            c = rgb565(230, 230, 230)
            blocks.extend(struct.pack("<HHI", c, c, 0))
    with open(os.path.join(d, "textures", "zhca_stripe.dds"), "wb") as f:
        f.write(dds_header(16, 16, b"DXT5") + blocks)

# ------------------------------------------------------------------ W3D

def chunk(cid, payload, container=False):
    return struct.pack("<II", cid, len(payload) | (0x80000000 if container else 0)) + payload

def name(s, n=16):
    return s.encode().ljust(n, b"\0")[:n]

def w3d_box(x0, y0, z0, x1, y1, z1):
    """24 vertices (flat faces), 12 CCW triangles in the right-handed Z-up frame."""
    faces = [((1, 0, 0), [(x1, y0, z0), (x1, y1, z0), (x1, y1, z1), (x1, y0, z1)]),
             ((-1, 0, 0), [(x0, y1, z0), (x0, y0, z0), (x0, y0, z1), (x0, y1, z1)]),
             ((0, 1, 0), [(x1, y1, z0), (x0, y1, z0), (x0, y1, z1), (x1, y1, z1)]),
             ((0, -1, 0), [(x0, y0, z0), (x1, y0, z0), (x1, y0, z1), (x0, y0, z1)]),
             ((0, 0, 1), [(x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)]),
             ((0, 0, -1), [(x0, y1, z0), (x1, y1, z0), (x1, y0, z0), (x0, y0, z0)])]
    pos, nrm, uv, tris = [], [], [], []
    for n, quad in faces:
        b = len(pos)
        pos += quad; nrm += [n] * 4; uv += [(0, 0), (1, 0), (1, 1), (0, 1)]
        tris += [(b, b + 1, b + 2), (b, b + 2, b + 3)]
    return pos, nrm, uv, tris

def w3d_mesh(mname, container, geo, texture=None, diffuse=(255, 255, 255), skin_bones=None, flags=0, blend=False):
    pos, nrm, uv, tris = geo
    attrs = flags | (0x00020000 if skin_bones else 0)
    hdr = struct.pack("<II", 0x00040002, attrs) + name(mname) + name(container)
    hdr += struct.pack("<7I", len(tris), len(pos), 1, 0, 0, 0, 0) + struct.pack("<II", 3, 1) + b"\0" * 40
    c = chunk(0x1F, hdr)
    c += chunk(0x02, b"".join(struct.pack("<3f", *p) for p in pos))
    c += chunk(0x03, b"".join(struct.pack("<3f", *n) for n in nrm))
    c += chunk(0x20, b"".join(struct.pack("<3II4f", *t, 0, 0, 0, 1, 0) for t in tris))
    if skin_bones:
        c += chunk(0x0E, b"".join(struct.pack("<H6x", b) for b in skin_bones))
    c += chunk(0x28, struct.pack("<4I", 1, 1, 1, 1 if texture else 0))
    vm = struct.pack("<I", 0) + bytes((255, 255, 255, 0)) + bytes(diffuse + (0,)) + bytes(4) + bytes(4) + struct.pack("<3f", 1, 1, 0)
    c += chunk(0x2A, chunk(0x2B, chunk(0x2C, mname.encode() + b"\0") + chunk(0x2D, vm), True), True)
    shader = bytes((3, 1, 0, 5 if blend else 0, 0, 1, 0, 2 if blend else 1, 1 if texture else 0, 0, 0, 0, 0, 0, 0, 0))
    c += chunk(0x29, shader)
    if texture:
        c += chunk(0x30, chunk(0x31, chunk(0x32, texture.encode() + b"\0"), True), True)
    mp = chunk(0x39, struct.pack("<I", 0)) + chunk(0x3A, struct.pack("<I", 0))
    if texture:
        mp += chunk(0x48, chunk(0x49, struct.pack("<I", 0)) + chunk(0x4A, b"".join(struct.pack("<2f", u, 1 - v) for u, v in uv)), True)
    c += chunk(0x38, mp, True)
    return chunk(0x00, c, True)

def quat_z(a):
    return (0, 0, math.sin(a / 2), math.cos(a / 2))

PIVOTS = [("ROOTTRANSFORM", -1, (0, 0, 0)), ("TURRET", 0, (0, 0, 0.6)), ("BARREL", 1, (0.5, 0, 0.35)), ("FIREFX01", 2, (1.6, 0, 0))]

def w3d_hierarchy(hname):
    hdr = struct.pack("<I", 0x00040001) + name(hname) + struct.pack("<I3f", len(PIVOTS), 0, 0, 0)
    piv = b""
    for pn, parent, t in PIVOTS:
        piv += name(pn) + struct.pack("<I3f3f4f", parent & 0xffffffff, *t, 0, 0, 0, 0, 0, 0, 1)
    return chunk(0x100, chunk(0x101, hdr) + chunk(0x102, piv), True)

def w3d_anim_classic(aname, hname, frames, fps):
    hdr = struct.pack("<I", 0x00040001) + name(aname) + name(hname) + struct.pack("<II", frames, fps)
    c = chunk(0x201, hdr)
    q = b"".join(struct.pack("<4f", *quat_z(2 * math.pi * f / (frames - 1))) for f in range(frames))
    c += chunk(0x202, struct.pack("<HHHHHH", 0, frames - 1, 4, 6, 1, 0) + q)
    x = b"".join(struct.pack("<f", -0.15 * math.sin(math.pi * f / (frames - 1)) ** 8) for f in range(frames))
    c += chunk(0x202, struct.pack("<HHHHHH", 0, frames - 1, 1, 0, 2, 0) + x)
    return chunk(0x200, c, True)

def w3d_anim_timecoded(aname, hname):
    frames, fps = 16, 30
    hdr = struct.pack("<I", 0x00000001) + name(aname) + name(hname) + struct.pack("<IHH", frames, fps, 0)
    keys = [(0, 0.0), (2, -0.3), (15, 0.0)]
    data = b"".join(struct.pack("<If", t, v) for t, v in keys)
    ch = struct.pack("<IHBB", len(keys), 2, 1, 0) + data
    return chunk(0x280, chunk(0x281, hdr) + chunk(0x282, ch), True)

def w3d_anim_adaptive(aname, hname):
    frames, fps = 33, 15
    hdr = struct.pack("<I", 0x00000001) + name(aname) + name(hname) + struct.pack("<IHH", frames, fps, 1)
    # turret yaw quaternion (z, w components drift): x, y stay 0; filter index 8 -> 1.0 * scale
    init = struct.pack("<4f", 0, 0, 0, 1)
    packets = b""
    for blk in range(2):
        for comp in range(4):
            nyb = [0] * 16
            if comp == 2: nyb = [3 if blk == 0 else -3] * 16
            if comp == 3: nyb = [-1 if blk == 0 else 1] * 16
            body = bytes(((nyb[i] & 15) | ((nyb[i + 1] & 15) << 4)) for i in range(0, 16, 2))
            packets += bytes((8,)) + body
    ch = struct.pack("<IHBBf", frames, 1, 4, 6, 0.01) + init + packets
    return chunk(0x280, chunk(0x281, hdr) + chunk(0x282, ch), True)

def write_w3d(d):
    write_dds(d)
    hname = "TANK_SKL"
    meshes = [
        ("HULL", w3d_box(-1.2, -0.7, 0, 1.2, 0.7, 0.6), "hull.tga", (255, 255, 255), 0),
        ("HOUSECOLOR01", w3d_box(-1.25, -0.72, 0.35, 0.6, 0.72, 0.45), "zhca_stripe.tga", (255, 255, 255), 0),
        ("TURRET", w3d_box(-0.5, -0.45, 0, 0.5, 0.45, 0.4), "turret.tga", (255, 255, 255), 1),
        ("BARREL", w3d_box(0, -0.08, -0.08, 1.6, 0.08, 0.08), None, (60, 60, 60), 2),
        ("HULL_LOW", w3d_box(-1.2, -0.7, 0, 1.2, 0.7, 0.6), None, (255, 0, 255), 0),
    ]
    body = w3d_hierarchy(hname)
    for mname, geo, tex, diff, bone in meshes:
        body += w3d_mesh(mname, "TANK", geo, tex, diff)
    # skinned antenna: bottom on ROOTTRANSFORM, top on TURRET (each vertex in its bone's space)
    apos, anrm, auv, atris = w3d_box(-0.03, -0.03, 0, 0.03, 0.03, 0.9)
    bones = []
    for i, p in enumerate(apos):
        if p[2] > 0.5:
            bones.append(1); apos[i] = (p[0] - 0.3, p[1] - 0.3, p[2])  # TURRET pivot at z 0.6; antenna at x=-0.3,y=-0.3, z 0.6..1.5
        else:
            bones.append(0); apos[i] = (p[0] - 0.3, p[1] - 0.3, p[2] + 0.6)
    body += w3d_mesh("ANTENNA", "TANK", (apos, anrm, auv, atris), None, (40, 40, 40), skin_bones=bones)
    # HLOD: LOD 0 (low detail) and LOD 1 (highest, max screen size = huge)
    hl = chunk(0x701, struct.pack("<II", 0x00010000, 2) + name("TANK") + name(hname))
    low = chunk(0x703, struct.pack("<If", 1, 0.3)) + chunk(0x704, struct.pack("<I", 0) + name("TANK.HULL_LOW", 32))
    high = chunk(0x703, struct.pack("<If", 6, 3.4e38))
    for mname, _, _, _, bone in meshes[:4]:
        high += chunk(0x704, struct.pack("<I", bone) + name("TANK." + mname, 32))
    high += chunk(0x704, struct.pack("<I", 0) + name("TANK.ANTENNA", 32))
    hl += chunk(0x702, low, True) + chunk(0x702, high, True)
    body += chunk(0x700, hl, True)
    body += w3d_anim_classic("TANK_SKL.TURN", hname, 31, 15)
    with open(os.path.join(d, "tank.w3d"), "wb") as f: f.write(body)
    with open(os.path.join(d, "tank_skl_recoil.w3d"), "wb") as f: f.write(w3d_anim_timecoded("TANK_SKL.RECOIL", hname))
    with open(os.path.join(d, "tank_skl_wobble.w3d"), "wb") as f: f.write(w3d_anim_adaptive("TANK_SKL.WOBBLE", hname))
    with open(os.path.join(d, "tank.anims.txt"), "w") as f:
        f.write("# extra animation files for tank.w3d\ntank_skl_recoil.w3d\ntank_skl_wobble.w3d\n")

if __name__ == "__main__":
    write_ta(os.path.join(OUT, "ta"))
    write_w3d(os.path.join(OUT, "w3d"))
    print("wrote", OUT)
