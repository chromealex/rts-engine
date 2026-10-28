#!/usr/bin/env python3
"""Writes a small Warcraft III style test unit (CC0, made from scratch) as .mdx, .mdl and a .blp texture.

It exercises the MDX / MDL / BLP loaders (engine/render/ModelMdx.cpp, ImageBlp.cpp) without any
Blizzard asset:
  footguy.mdx / footguy.mdl   same content in binary and text form: 5 bones + 2 attachments
                              ("Hand Right Ref", "Overhead Ref"), vertex groups with 1 and 2
                              bones, sequences Stand (scale + rotation, linear), Walk (rotations),
                              Attack - 1 (Hermite rotations, Bezier translation), Death
                              (non-looping); material = team colour layer under an alpha-tested
                              texture; Z up, facing +X, the nose points forward.
  Textures/FootGuy.blp        BLP1 paletted, 8-bit alpha (alpha 0 = team colour band on the chest)

Usage: python3 tools/models/gen_mdx_sample.py OUTDIR   (stdlib only; output is test data, not committed)
"""
import math, os, struct, sys

OUT = sys.argv[1] if len(sys.argv) > 1 else "/tmp/mdx-sample"
os.makedirs(os.path.join(OUT, "Textures"), exist_ok=True)

# ------------------------------------------------------------------ model data (WC3 axes: X forward, Y left, Z up)

# uv regions of the 64x64 texture
BODY, HEAD, ARM, NOSE = (0, 0, .5, .5), (.5, 0, 1, .5), (0, .5, .5, 1), (.5, .5, 1, 1)

def box(lo, hi, region, group_of):
    """24 vertices (pos, nrm, uv, group) + 12 triangles, counter-clockwise seen from outside."""
    V, T = [], []
    faces = [((1, 0, 0), 0), ((-1, 0, 0), 0), ((0, 1, 0), 1), ((0, -1, 0), 1), ((0, 0, 1), 2), ((0, 0, -1), 2)]
    for n, axis in faces:
        # two in-plane axes forming a right-handed frame with n
        a = [(1, 2), (0, 2), (0, 1)][axis]
        u = [0, 0, 0]; v = [0, 0, 0]
        u[a[0]] = 1; v[a[1]] = 1
        cr = (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0])
        if sum(cr[i] * n[i] for i in range(3)) < 0: u, v = v, u
        base = len(V)
        for su, sv in ((0, 0), (1, 0), (1, 1), (0, 1)):
            p = [0, 0, 0]
            for i in range(3):
                if n[i] != 0: p[i] = hi[i] if n[i] > 0 else lo[i]
                elif u[i]: p[i] = hi[i] if su else lo[i]
                elif v[i]: p[i] = hi[i] if sv else lo[i]
            uv = (region[0] + (region[2] - region[0]) * su, region[3] - (region[3] - region[1]) * sv)
            V.append((tuple(p), n, uv, group_of(p)))
        T += [(base, base + 1, base + 2), (base, base + 2, base + 3)]
    return V, T

# matrix groups (lists of bone object ids)
GROUPS = [[0, 1], [1], [2], [3], [4]]
verts, tris = [], []
def add(part):
    V, T = part
    b = len(verts)
    verts.extend(V)
    tris.extend((b + i, b + j, b + k) for i, j, k in T)
add(box((-12, -15, 0), (12, 15, 64), BODY, lambda p: 0 if p[2] < 30 else 1))
add(box((-10, -10, 64), (10, 10, 86), HEAD, lambda p: 2))
add(box((10, -3, 72), (18, 3, 78), NOSE, lambda p: 2))
add(box((-5, -26, 24), (5, -16, 62), ARM, lambda p: 3))    # right arm (-Y)
add(box((-5, 16, 24), (5, 26, 62), ARM, lambda p: 4))      # left arm (+Y)

def qaxis(axis, deg):
    s, c = math.sin(math.radians(deg) / 2), math.cos(math.radians(deg) / 2)
    return (axis[0] * s, axis[1] * s, axis[2] * s, c)

# (name, kind, id, parent, pivot, tracks) ; tracks: tag -> (interp, [(frame, value, intan, outtan)])
Y = (0, 1, 0)
SEQS = [("Stand", 0, 2000, False), ("Walk", 2333, 3333, False), ("Attack - 1", 4000, 5000, False), ("Death", 6000, 7500, True)]
def lin(keys): return (1, [(f, v, None, None) for f, v in keys])
def herm(keys): return (2, keys)
OBJECTS = [
    ("Root", "bone", 0, -1, (0, 0, 0), {
        "KGRT": lin([(6000, qaxis(Y, 0)), (6800, qaxis(Y, -88)), (7500, qaxis(Y, -90))]),
        "KGTR": (3, [   # Bezier bob while walking
(2333, (0, 0, 0), (0, 0, 0), (0, 0, 4)), (2833, (0, 0, 3), (0, 0, 3), (0, 0, 3)), (3333, (0, 0, 0), (0, 0, 4), (0, 0, 0))])}),
    ("Body", "bone", 1, 0, (0, 0, 40), {
        "KGSC": lin([(0, (1, 1, 1)), (1000, (1, 1, 1.04)), (2000, (1, 1, 1))])}),
    ("Head", "bone", 2, 1, (0, 0, 66), {   # Hermite (squad) rotations: looks around in Stand, nods in Attack
        "KGRT": herm([(f, q, q, q) for f, q in [(0, qaxis((0, 0, 1), -10)), (1000, qaxis((0, 0, 1), 10)), (2000, qaxis((0, 0, 1), -10)),
                                                (4000, qaxis(Y, 0)), (4400, qaxis(Y, 20)), (5000, qaxis(Y, 0))]])}),
    ("Arm Right", "bone", 3, 1, (0, -21, 60), {
        "KGRT": (1, [(2333, qaxis(Y, -35), None, None), (2833, qaxis(Y, 35), None, None), (3333, qaxis(Y, -35), None, None)]
                 + [(4000, qaxis(Y, 0), None, None), (4400, qaxis(Y, -150), None, None), (4700, qaxis(Y, -20), None, None), (5000, qaxis(Y, 0), None, None)])}),
    ("Arm Left", "bone", 4, 1, (0, 21, 60), {
        "KGRT": lin([(2333, qaxis(Y, 35)), (2833, qaxis(Y, -35)), (3333, qaxis(Y, 35))])}),
    ("Hand Right Ref", "attachment", 5, 3, (0, -21, 24), {}),
    ("Overhead Ref", "attachment", 6, 2, (0, 0, 100), {}),
]
TEXTURES = [("Textures\\FootGuy.blp", 0), ("", 1)]

# ------------------------------------------------------------------ MDX writer

def cstr(s, n): b = s.encode("ascii"); return b + b"\0" * (n - len(b))
def chunk(tag, data): return tag.encode() + struct.pack("<I", len(data)) + data
def f3(v): return struct.pack("<3f", *v)

def track_bytes(tag, interp, keys):
    out = tag.encode() + struct.pack("<IIi", len(keys), interp, -1)
    for f, v, a, b in keys:
        out += struct.pack("<i", f) + struct.pack("<%df" % len(v), *v)
        if interp > 1: out += struct.pack("<%df" % len(v), *a) + struct.pack("<%df" % len(v), *b)
    return out

def node_bytes(name, oid, parent, flags, tracks):
    body = cstr(name, 80) + struct.pack("<iiI", oid, parent, flags)
    for tag in ("KGTR", "KGRT", "KGSC"):
        if tag in tracks: body += track_bytes(tag, *tracks[tag])
    return struct.pack("<I", len(body) + 4) + body

def mdx():
    d = b"MDLX" + chunk("VERS", struct.pack("<I", 800))
    d += chunk("MODL", cstr("FootGuy", 80) + cstr("", 260) + struct.pack("<f3f3fI", 60, -20, -30, 0, 20, 30, 100, 150))
    d += chunk("SEQS", b"".join(cstr(n, 80) + struct.pack("<iifIfI", s, e, 0, 1 if nl else 0, 0, 0) + struct.pack("<f3f3f", 60, -20, -30, 0, 20, 30, 100) for n, s, e, nl in SEQS))
    layers = [(0, 0, 1), (1, 0, 0)]   # (filter, shading, texture): team colour under the alpha-tested skin
    lays = b"".join(struct.pack("<IIIiiIf", 28, f, sh, t, -1, 0, 1.0) for f, sh, t in layers)
    mat = struct.pack("<iI", 0, 0) + b"LAYS" + struct.pack("<I", len(layers)) + lays
    d += chunk("MTLS", struct.pack("<I", len(mat) + 4) + mat)
    d += chunk("TEXS", b"".join(struct.pack("<I", rid) + cstr(f, 260) + struct.pack("<I", 0) for f, rid in TEXTURES))
    g = b"VRTX" + struct.pack("<I", len(verts)) + b"".join(f3(v[0]) for v in verts)
    g += b"NRMS" + struct.pack("<I", len(verts)) + b"".join(f3(v[1]) for v in verts)
    g += b"PTYP" + struct.pack("<II", 1, 4) + b"PCNT" + struct.pack("<II", 1, len(tris) * 3)
    g += b"PVTX" + struct.pack("<I", len(tris) * 3) + b"".join(struct.pack("<3H", *t) for t in tris)
    g += b"GNDX" + struct.pack("<I", len(verts)) + bytes(v[3] for v in verts)
    g += b"MTGC" + struct.pack("<I", len(GROUPS)) + b"".join(struct.pack("<I", len(x)) for x in GROUPS)
    mats = [i for x in GROUPS for i in x]
    g += b"MATS" + struct.pack("<I", len(mats)) + b"".join(struct.pack("<I", i) for i in mats)
    g += struct.pack("<III", 0, 0, 0) + struct.pack("<f3f3f", 60, -20, -30, 0, 20, 30, 100) + struct.pack("<I", 0)
    g += b"UVAS" + struct.pack("<I", 1) + b"UVBS" + struct.pack("<I", len(verts)) + b"".join(struct.pack("<2f", *v[2]) for v in verts)
    d += chunk("GEOS", struct.pack("<I", len(g) + 4) + g)
    bones = b"".join(node_bytes(n, i, p, 256, tr) + struct.pack("<ii", 0, -1) for n, k, i, p, pv, tr in OBJECTS if k == "bone")
    d += chunk("BONE", bones)
    atch = b""
    for n, k, i, p, pv, tr in OBJECTS:
        if k != "attachment": continue
        body = node_bytes(n, i, p, 2048, tr) + cstr("", 260) + struct.pack("<i", i - 5)
        atch += struct.pack("<I", len(body) + 4) + body
    d += chunk("ATCH", atch)
    d += chunk("PIVT", b"".join(f3(o[4]) for o in OBJECTS))
    return d

# ------------------------------------------------------------------ MDL writer

def fl(x): return ("%.6f" % x).rstrip("0").rstrip(".") if isinstance(x, float) else str(x)
def vec(v): return "{ " + ", ".join(fl(float(x)) for x in v) + " }"

def mdl():
    L = ["// FootGuy: CC0 test unit written by tools/models/gen_mdx_sample.py", "Version {", "\tFormatVersion 800,", "}",
         'Model "FootGuy" {', "\tBlendTime 150,", "}", "Sequences %d {" % len(SEQS)]
    for n, s, e, nl in SEQS:
        L += ['\tAnim "%s" {' % n, "\t\tInterval { %d, %d }," % (s, e)] + (["\t\tNonLooping,"] if nl else []) + ["\t}"]
    L += ["}", "Textures %d {" % len(TEXTURES)]
    for f, rid in TEXTURES:
        L += ["\tBitmap {", '\t\tImage "%s",' % f.replace("\\", "\\")] + (["\t\tReplaceableId %d," % rid] if rid else []) + ["\t}"]
    L += ["}", "Materials 1 {", "\tMaterial {",
          "\t\tLayer {", "\t\t\tFilterMode None,", "\t\t\tstatic TextureID 1,", "\t\t}",
          "\t\tLayer {", "\t\t\tFilterMode Transparent,", "\t\t\tstatic TextureID 0,", "\t\t}", "\t}", "}"]
    L += ["Geoset {", "\tVertices %d {" % len(verts)] + ["\t\t%s," % vec(v[0]) for v in verts] + ["\t}"]
    L += ["\tNormals %d {" % len(verts)] + ["\t\t%s," % vec(v[1]) for v in verts] + ["\t}"]
    L += ["\tTVertices %d {" % len(verts)] + ["\t\t%s," % vec(v[2]) for v in verts] + ["\t}"]
    L += ["\tVertexGroup {"] + ["\t\t%d," % v[3] for v in verts] + ["\t}"]
    L += ["\tFaces 1 %d {" % (len(tris) * 3), "\t\tTriangles {", "\t\t\t{ " + ", ".join(str(i) for t in tris for i in t) + " },", "\t\t}", "\t}"]
    L += ["\tGroups %d %d {" % (len(GROUPS), sum(len(x) for x in GROUPS))] + ["\t\tMatrices { %s }," % ", ".join(map(str, x)) for x in GROUPS] + ["\t}"]
    L += ["\tMaterialID 0,", "\tSelectionGroup 0,", "}"]
    names = {"KGTR": "Translation", "KGRT": "Rotation", "KGSC": "Scaling"}
    interps = ["DontInterp", "Linear", "Hermite", "Bezier"]
    for n, k, i, p, pv, tr in OBJECTS:
        L += ['%s "%s" {' % ("Bone" if k == "bone" else "Attachment", n), "\tObjectId %d," % i] + (["\tParent %d," % p] if p >= 0 else [])
        for tag in ("KGTR", "KGRT", "KGSC"):
            if tag not in tr: continue
            interp, keys = tr[tag]
            L += ["\t%s %d {" % (names[tag], len(keys)), "\t\t%s," % interps[interp]]
            for f, v, a, b in keys:
                L.append("\t\t%d: %s," % (f, vec(v)))
                if interp > 1: L += ["\t\t\tInTan %s," % vec(a), "\t\t\tOutTan %s," % vec(b)]
            L.append("\t}")
        L.append("}")
    L += ["PivotPoints %d {" % len(OBJECTS)] + ["\t%s," % vec(o[4]) for o in OBJECTS] + ["}"]
    return "\n".join(L) + "\n"

# ------------------------------------------------------------------ BLP1 paletted texture

def blp():
    W = H = 64
    pal, px, alpha = [], [], []
    def colour(c):
        if c not in pal: pal.append(c)
        return pal.index(c)
    for y in range(H):
        for x in range(W):
            u, v = (x + .5) / W, (y + .5) / H
            a = 255
            if u < .5 and v < .5:   # body: steel with a team colour band and a belt
                c = (150, 155, 165)
                if .2 < v < .32: a = 0
                if .4 < v < .45: c = (90, 60, 30)
            elif v < .5: c = (230, 190, 150)                       # head: skin
            elif u < .5: c = (120, 85, 50)                         # arms: leather
            else: c = (200, 40, 40)                                # nose: red
            if (x % 8 == 0 or y % 8 == 0) and a: c = tuple(int(k * .85) for k in c)   # seams
            px.append(colour(c)); alpha.append(a)
    palette = b"".join(struct.pack("<4B", c[2], c[1], c[0], 0) for c in pal) + b"\0" * (4 * (256 - len(pal)))
    data = bytes(px) + bytes(alpha)
    offset = 156 + 1024
    offs = [offset] + [0] * 15; sizes = [len(data)] + [0] * 15
    hdr = b"BLP1" + struct.pack("<6I", 1, 8, W, H, 4, 0) + struct.pack("<16I", *offs) + struct.pack("<16I", *sizes)
    return hdr + palette + data

open(os.path.join(OUT, "footguy.mdx"), "wb").write(mdx())
open(os.path.join(OUT, "footguy.mdl"), "w").write(mdl())
open(os.path.join(OUT, "Textures", "FootGuy.blp"), "wb").write(blp())
print("wrote", OUT)
