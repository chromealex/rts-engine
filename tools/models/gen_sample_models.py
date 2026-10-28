#!/usr/bin/env python3
"""Generates the sample models of templates/showcase/models (CC0, written from scratch).

They exercise every path of the model pipeline:
  golem.glb      skinned character, 8 joints, clips Idle / Walk / Attack / Death, base colour +
                 normal + metallic-roughness textures (no TANGENT: the loader generates them),
                 emissive eyes (KHR_materials_emissive_strength), "TeamCloth" material (team colour)
  tank.glb       rigid hierarchy (Hull, Turret, Barrel, 4 wheels) for procedural aim / recoil /
                 wheel spin, KHR_materials_variants (desert, winter)
  windmill.gltf  .gltf + external .bin + external .png: alpha-masked double-sided sails on a
                 "Sails" node (procedural spin), emissive windows, KHR_texture_transform
  crystal.obj    OBJ + MTL: translucent emissive crystals (blend) on a rock textured with a KTX2
                 file (ETC2 blocks + mips, stays compressed on GPUs with ETC2, CPU-decoded elsewhere)
  tank_team.png  team mask for the tank hull (materials override "teamMask")
  golem_lod.glb  far LOD of the golem (same skeleton and clips, fewer parts); golem also has Run
                 (blend space) and Breathe (additive) clips
  sword.glb, sack.glb, logs.glb   socket attachments (weapon, carried gold / wood)
  scaffold.glb   construction stage of the windmill; windmill.gltf has a "burnt" material variant
  slime.glb      morph targets (Squash, Stretch) animated by a weights channel (Wobble)

Usage: python3 tools/models/gen_sample_models.py templates/showcase/models   (needs Pillow)
"""
import json, math, os, struct, sys
from PIL import Image, ImageDraw

OUT = sys.argv[1] if len(sys.argv) > 1 else "templates/showcase/models"
os.makedirs(OUT, exist_ok=True)

# ------------------------------------------------------------------ geometry helpers

def box(center, size, uvscale=1.0):
    """24 vertices (pos, nrm, uv) and 36 indices of an axis-aligned box."""
    cx, cy, cz = center; hx, hy, hz = size[0] / 2, size[1] / 2, size[2] / 2
    faces = [((1, 0, 0), (0, 0, -1), (0, 1, 0)), ((-1, 0, 0), (0, 0, 1), (0, 1, 0)),
             ((0, 1, 0), (1, 0, 0), (0, 0, -1)), ((0, -1, 0), (1, 0, 0), (0, 0, 1)),
             ((0, 0, 1), (1, 0, 0), (0, 1, 0)), ((0, 0, -1), (-1, 0, 0), (0, 1, 0))]
    V, I = [], []
    for n, u, v in faces:
        base = len(V)
        for su, sv in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
            p = [center[k] + (n[k] + u[k] * su + v[k] * sv) * (hx, hy, hz)[k] for k in range(3)]
            V.append((p, list(n), [(su + 1) / 2 * uvscale, (1 - (sv + 1) / 2) * uvscale]))
        I += [base, base + 1, base + 2, base, base + 2, base + 3]
    return V, I

def cylinder(center, radius, length, axis, seg=14):
    """Closed cylinder along axis 'x' or 'y' or 'z'."""
    def place(a, r, h):  # a: angle, r: radius, h: along axis
        c, s = math.cos(a) * r, math.sin(a) * r
        if axis == "x": return [center[0] + h, center[1] + c, center[2] + s]
        if axis == "y": return [center[0] + c, center[1] + h, center[2] + s]
        return [center[0] + c, center[1] + s, center[2] + h]
    def nrm(a):
        c, s = math.cos(a), math.sin(a)
        return [0, c, s] if axis == "x" else [c, 0, s] if axis == "y" else [c, s, 0]
    ax = {"x": [1, 0, 0], "y": [0, 1, 0], "z": [0, 0, 1]}[axis]
    V, I = [], []
    h = length / 2
    for i in range(seg + 1):
        a = 2 * math.pi * i / seg
        V.append((place(a, radius, -h), nrm(a), [i / seg, 1]))
        V.append((place(a, radius, h), nrm(a), [i / seg, 0]))
    for i in range(seg):
        b = i * 2
        I += [b, b + 2, b + 1, b + 1, b + 2, b + 3]
    for sign in (-1, 1):
        c0 = len(V)
        V.append((place(0, 0, sign * h), [x * sign for x in ax], [0.5, 0.5]))
        for i in range(seg + 1):
            a = 2 * math.pi * i / seg
            V.append((place(a, radius, sign * h), [x * sign for x in ax], [0.5 + math.cos(a) / 2, 0.5 + math.sin(a) / 2]))
        for i in range(seg):
            I += [c0, c0 + 1 + i, c0 + 2 + i]
    return fix_winding(V, I)

def fix_winding(V, I):
    """Make every triangle counter-clockwise as seen from its vertex normals' side."""
    out = []
    for t in range(0, len(I), 3):
        a, b, c = I[t:t + 3]
        pa, pb, pc = V[a][0], V[b][0], V[c][0]
        e1 = [pb[k] - pa[k] for k in range(3)]; e2 = [pc[k] - pa[k] for k in range(3)]
        cr = [e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]]
        n = [V[a][1][k] + V[b][1][k] + V[c][1][k] for k in range(3)]
        out += [a, b, c] if sum(cr[k] * n[k] for k in range(3)) >= 0 else [a, c, b]
    return V, out

def pyramid(base_center, half, height):
    cx, cy, cz = base_center
    apex = [cx, cy + height, cz]
    corners = [[cx - half, cy, cz - half], [cx + half, cy, cz - half], [cx + half, cy, cz + half], [cx - half, cy, cz + half]]
    V, I = [], []
    for i in range(4):
        a, b = corners[i], corners[(i + 1) % 4]
        e1 = [b[k] - a[k] for k in range(3)]; e2 = [apex[k] - a[k] for k in range(3)]
        n = [e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]]
        l = math.sqrt(sum(x * x for x in n)); n = [x / l for x in n]
        if n[1] < 0: n = [-x for x in n]
        base = len(V)
        V += [(a, n, [0, 1]), (b, n, [1, 1]), (apex, n, [0.5, 0])]
        I += [base, base + 1, base + 2]
    return fix_winding(V, I)

def quat_x(deg):
    a = math.radians(deg) / 2
    return [math.sin(a), 0, 0, math.cos(a)]

def quat_z(deg):
    a = math.radians(deg) / 2
    return [0, 0, math.sin(a), math.cos(a)]

# ------------------------------------------------------------------ glTF writer

class Gltf:
    def __init__(self):
        self.doc = {"asset": {"version": "2.0", "generator": "RTSEngine gen_sample_models.py (CC0)"},
                    "scene": 0, "scenes": [{"nodes": []}], "nodes": [], "meshes": [], "materials": [],
                    "accessors": [], "bufferViews": [], "buffers": []}
        self.bin = bytearray()
        self.used = set()

    def ext(self, name):
        self.used.add(name)

    def view(self, data, target=None):
        while len(self.bin) % 4: self.bin.append(0)
        v = {"buffer": 0, "byteOffset": len(self.bin), "byteLength": len(data)}
        if target: v["target"] = target
        self.bin += data
        self.doc["bufferViews"].append(v)
        return len(self.doc["bufferViews"]) - 1

    def accessor(self, values, comps, ctype=5126, target=None, minmax=False, normalized=False):
        fmt = {5126: "f", 5125: "I", 5123: "H", 5121: "B"}[ctype]
        flat = [x for v in values for x in (v if isinstance(v, (list, tuple)) else [v])]
        data = struct.pack("<%d%s" % (len(flat), fmt), *flat)
        a = {"bufferView": self.view(data, target), "componentType": ctype, "count": len(values),
             "type": {1: "SCALAR", 2: "VEC2", 3: "VEC3", 4: "VEC4", 16: "MAT4"}[comps]}
        if normalized: a["normalized"] = True
        if minmax:
            a["min"] = [min(v[k] for v in values) for k in range(comps)]
            a["max"] = [max(v[k] for v in values) for k in range(comps)]
        self.doc["accessors"].append(a)
        return len(self.doc["accessors"]) - 1

    def image(self, png_bytes=None, uri=None):
        imgs = self.doc.setdefault("images", [])
        if uri: imgs.append({"uri": uri})
        else: imgs.append({"bufferView": self.view(png_bytes), "mimeType": "image/png"})
        tex = self.doc.setdefault("textures", [])
        tex.append({"source": len(imgs) - 1, "sampler": 0})
        self.doc.setdefault("samplers", [{"magFilter": 9729, "minFilter": 9987, "wrapS": 10497, "wrapT": 10497}])
        return len(tex) - 1

    def material(self, m):
        self.doc["materials"].append(m)
        return len(self.doc["materials"]) - 1

    def mesh(self, name, prims):
        """prims: list of (vertices, indices, material, joints or None, extra dict)"""
        out = []
        for V, I, mat, joints, extra in prims:
            attrs = {"POSITION": self.accessor([v[0] for v in V], 3, target=34962, minmax=True),
                     "NORMAL": self.accessor([v[1] for v in V], 3, target=34962),
                     "TEXCOORD_0": self.accessor([v[2] for v in V], 2, target=34962)}
            if joints is not None:
                attrs["JOINTS_0"] = self.accessor([j[0] for j in joints], 4, 5121, target=34962)
                attrs["WEIGHTS_0"] = self.accessor([j[1] for j in joints], 4, target=34962)
            p = {"attributes": attrs, "indices": self.accessor(I, 1, 5125, target=34963), "material": mat}
            p.update(extra or {})
            out.append(p)
        self.doc["meshes"].append({"name": name, "primitives": out})
        return len(self.doc["meshes"]) - 1

    def node(self, n, root=False):
        self.doc["nodes"].append(n)
        i = len(self.doc["nodes"]) - 1
        if root: self.doc["scenes"][0]["nodes"].append(i)
        return i

    def finish(self):
        if self.used: self.doc["extensionsUsed"] = sorted(self.used)
        while len(self.bin) % 4: self.bin.append(0)
        self.doc["buffers"] = [{"byteLength": len(self.bin)}]

    def write_glb(self, path):
        self.finish()
        js = json.dumps(self.doc, separators=(",", ":")).encode()
        while len(js) % 4: js += b" "
        total = 12 + 8 + len(js) + 8 + len(self.bin)
        with open(path, "wb") as f:
            f.write(struct.pack("<4sII", b"glTF", 2, total))
            f.write(struct.pack("<I4s", len(js), b"JSON")); f.write(js)
            f.write(struct.pack("<I4s", len(self.bin), b"BIN\0")); f.write(self.bin)

    def write_gltf(self, path, bin_name):
        self.finish()
        self.doc["buffers"][0]["uri"] = bin_name
        with open(os.path.join(os.path.dirname(path), bin_name), "wb") as f: f.write(self.bin)
        with open(path, "w") as f: json.dump(self.doc, f, indent=1)

def png(img):
    import io
    b = io.BytesIO(); img.save(b, "PNG"); return b.getvalue()

def noise(x, y, seed):
    h = (x * 374761393 + y * 668265263 + seed * 2147483647) & 0xffffffff
    h = ((h ^ (h >> 13)) * 1274126177) & 0xffffffff
    return (h & 0xffff) / 65535.0

# ------------------------------------------------------------------ golem (skinned)

def golem(lod=False):
    g = Gltf()
    g.ext("KHR_materials_emissive_strength")
    S = 64
    stone = Image.new("RGB", (S, S)); nm = Image.new("RGB", (S, S)); mr = Image.new("RGB", (S, S))
    for y in range(S):
        for x in range(S):
            crack = (x % 16 == 0) or (y % 16 == 0)
            v = 0.55 + 0.25 * noise(x // 2, y // 2, 1) - (0.3 if crack else 0)
            stone.putpixel((x, y), (int(150 * v), int(140 * v), int(128 * v)))
            # bevelled tiles: normals tilt towards the tile edges
            fx, fy = (x % 16) / 15.0 - 0.5, (y % 16) / 15.0 - 0.5
            nx = 0.5 + (0.35 * fx if abs(fx) > 0.4 else 0.0) + 0.06 * (noise(x, y, 2) - 0.5)
            ny = 0.5 - (0.35 * fy if abs(fy) > 0.4 else 0.0) + 0.06 * (noise(x, y, 3) - 0.5)
            nm.putpixel((x, y), (int(255 * nx), int(255 * ny), 235))
            mr.putpixel((x, y), (255, int(255 * (0.75 + 0.2 * noise(x, y, 4))), 0))
    base_t, norm_t, mr_t = g.image(png(stone)), g.image(png(nm)), g.image(png(mr))
    body = g.material({"name": "Stone", "pbrMetallicRoughness": {"baseColorTexture": {"index": base_t},
                       "metallicRoughnessTexture": {"index": mr_t}, "metallicFactor": 0.0, "roughnessFactor": 1.0},
                       "normalTexture": {"index": norm_t, "scale": 1.0}})
    cloth = g.material({"name": "TeamCloth", "pbrMetallicRoughness": {"baseColorFactor": [0.9, 0.9, 0.9, 1], "metallicFactor": 0, "roughnessFactor": 0.7}, "doubleSided": True})
    eyes = g.material({"name": "Eyes", "pbrMetallicRoughness": {"baseColorFactor": [0.1, 0.1, 0.1, 1]},
                       "emissiveFactor": [1.0, 0.55, 0.1], "extensions": {"KHR_materials_emissive_strength": {"emissiveStrength": 2.5}}})
    # joints (world bind positions), parents
    J = [("root", None, (0, 0, 0)), ("hips", 0, (0, 0.9, 0)), ("spine", 1, (0, 1.15, 0)), ("head", 2, (0, 1.65, 0)),
         ("arm.L", 2, (0.42, 1.55, 0)), ("arm.R", 2, (-0.42, 1.55, 0)), ("leg.L", 1, (0.16, 0.85, 0)), ("leg.R", 1, (-0.16, 0.85, 0))]
    parts = [  # (center, size, joint, material)
        ((0, 0.97, 0), (0.5, 0.3, 0.32), 1, body), ((0, 1.38, 0), (0.62, 0.55, 0.38), 2, body),
        ((0, 1.86, 0), (0.34, 0.34, 0.34), 3, body),
        ((0.42, 1.26, 0), (0.18, 0.62, 0.18), 4, body), ((-0.42, 1.26, 0), (0.18, 0.62, 0.18), 5, body),
        ((0.42, 0.88, 0), (0.24, 0.2, 0.24), 4, body), ((-0.42, 0.88, 0), (0.24, 0.2, 0.24), 5, body),
        ((0.16, 0.43, 0), (0.2, 0.84, 0.22), 6, body), ((-0.16, 0.43, 0), (0.2, 0.84, 0.22), 7, body),
        ((0, 1.3, 0.2), (0.44, 0.56, 0.03), 2, cloth), ((0, 1.3, -0.2), (0.44, 0.56, 0.03), 2, cloth),
        ((0.08, 1.9, 0.172), (0.06, 0.04, 0.01), 3, eyes), ((-0.08, 1.9, 0.172), (0.06, 0.04, 0.01), 3, eyes),
    ]
    if lod:  # far LOD: no fists, back cloth or eyes (same skeleton and clips)
        parts = [p for i, p in enumerate(parts) if i not in (5, 6, 10, 11, 12)]
    prims = {}
    for c, s, j, mat in parts:
        V, I = box(c, s)
        P = prims.setdefault(mat, [[], [], []])
        base = len(P[0])
        P[0] += V; P[1] += [i + base for i in I]; P[2] += [([j, 0, 0, 0], [1.0, 0, 0, 0])] * len(V)
    mesh = g.mesh("Golem", [(V, I, mat, W, None) for mat, (V, I, W) in prims.items()])
    nodes = []
    for name, parent, pos in J:
        local = pos if parent is None else tuple(pos[k] - J[parent][2][k] for k in range(3))
        nodes.append(g.node({"name": name, "translation": list(local)}, root=(parent is None)))
    for i, (name, parent, pos) in enumerate(J):
        kids = [k for k, jj in enumerate(J) if jj[1] == i]
        if kids: g.doc["nodes"][nodes[i]]["children"] = [nodes[k] for k in kids]
    ibm = []
    for name, parent, pos in J:  # column-major translate(-pos)
        ibm.append([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -pos[0], -pos[1], -pos[2], 1])
    g.doc["skins"] = [{"name": "GolemSkin", "joints": nodes, "inverseBindMatrices": g.accessor(ibm, 16), "skeleton": nodes[0]}]
    g.node({"name": "GolemMesh", "mesh": mesh, "skin": 0}, root=True)

    anims = []
    def clip(name, channels):
        samplers, chans = [], []
        for node, path, times, values, interp in channels:
            samplers.append({"input": g.accessor(times, 1, minmax=False), "output": g.accessor(values, 4 if path == "rotation" else 3), "interpolation": interp})
            g.doc["accessors"][samplers[-1]["input"]]["min"] = [min(times)]
            g.doc["accessors"][samplers[-1]["input"]]["max"] = [max(times)]
            chans.append({"sampler": len(samplers) - 1, "target": {"node": nodes[node], "path": path}})
        anims.append({"name": name, "samplers": samplers, "channels": chans})
    t = [0, 0.5, 1.0, 1.5, 2.0]
    clip("Idle", [(2, "rotation", t, [quat_x(0), quat_x(3), quat_x(0), quat_x(-2), quat_x(0)], "LINEAR"),
                  (4, "rotation", t, [quat_z(4), quat_z(7), quat_z(4), quat_z(6), quat_z(4)], "LINEAR"),
                  (5, "rotation", t, [quat_z(-4), quat_z(-7), quat_z(-4), quat_z(-6), quat_z(-4)], "LINEAR")])
    t = [0, 0.25, 0.5, 0.75, 1.0]
    sw = [0, 30, 0, -30, 0]
    clip("Walk", [(6, "rotation", t, [quat_x(a) for a in sw], "LINEAR"), (7, "rotation", t, [quat_x(-a) for a in sw], "LINEAR"),
                  (4, "rotation", t, [quat_x(-a * 0.8) for a in sw], "LINEAR"), (5, "rotation", t, [quat_x(a * 0.8) for a in sw], "LINEAR"),
                  (1, "translation", t, [[0, 0.9, 0], [0, 0.95, 0], [0, 0.9, 0], [0, 0.95, 0], [0, 0.9, 0]], "LINEAR"),
                  (2, "rotation", t, [quat_x(6)] * 5, "STEP")])
    t = [0, 0.3, 0.45, 0.8]
    sw = [0, 45, 0, -45, 0]
    t = [0, 0.15, 0.3, 0.45, 0.6]
    clip("Run", [(6, "rotation", t, [quat_x(a) for a in sw], "LINEAR"), (7, "rotation", t, [quat_x(-a) for a in sw], "LINEAR"),
                 (4, "rotation", t, [quat_x(-a) for a in sw], "LINEAR"), (5, "rotation", t, [quat_x(a) for a in sw], "LINEAR"),
                 (1, "translation", t, [[0, 0.9, 0], [0, 1.0, 0], [0, 0.9, 0], [0, 1.0, 0], [0, 0.9, 0]], "LINEAR"),
                 (2, "rotation", t, [quat_x(14)] * 5, "STEP")])
    t = [0, 1.0, 2.0]   # additive: chest heaves (applied on top of any action)
    clip("Breathe", [(2, "scale", t, [[1, 1, 1], [1.06, 1.03, 1.08], [1, 1, 1]], "LINEAR"),
                     (3, "rotation", t, [quat_x(0), quat_x(-6), quat_x(0)], "LINEAR")])
    t = [0, 0.3, 0.45, 0.8]
    clip("Attack", [(5, "rotation", t, [quat_x(0), quat_x(-150), quat_x(20), quat_x(0)], "LINEAR"),
                    (2, "rotation", t, [quat_x(0), quat_x(-10), quat_x(18), quat_x(0)], "LINEAR")])
    # Death uses CUBICSPLINE (in-tangent, value, out-tangent per key)
    t = [0, 0.7, 1.2]
    rot = [quat_x(0), quat_x(80), quat_x(90)]
    pos = [[0, 0.9, 0], [0, 0.45, 0.3], [0, 0.25, 0.45]]
    zero4, zero3 = [0, 0, 0, 0], [0, 0, 0]
    clip("Death", [(1, "rotation", t, [v for r in rot for v in (zero4, r, zero4)], "CUBICSPLINE"),
                   (1, "translation", t, [v for p in pos for v in (zero3, p, zero3)], "CUBICSPLINE")])
    g.doc["animations"] = anims
    g.write_glb(os.path.join(OUT, "golem_lod.glb" if lod else "golem.glb"))

# ------------------------------------------------------------------ tank (rigid hierarchy, variants)

def tank():
    g = Gltf()
    g.ext("KHR_materials_variants")
    g.doc["extensions"] = {"KHR_materials_variants": {"variants": [{"name": "desert"}, {"name": "winter"}]}}
    hull = g.material({"name": "Hull", "pbrMetallicRoughness": {"baseColorFactor": [0.32, 0.38, 0.24, 1], "metallicFactor": 0.5, "roughnessFactor": 0.45}})
    desert = g.material({"name": "HullDesert", "pbrMetallicRoughness": {"baseColorFactor": [0.72, 0.6, 0.38, 1], "metallicFactor": 0.4, "roughnessFactor": 0.55}})
    winter = g.material({"name": "HullWinter", "pbrMetallicRoughness": {"baseColorFactor": [0.86, 0.88, 0.9, 1], "metallicFactor": 0.3, "roughnessFactor": 0.5}})
    steel = g.material({"name": "Steel", "pbrMetallicRoughness": {"baseColorFactor": [0.55, 0.56, 0.58, 1], "metallicFactor": 1.0, "roughnessFactor": 0.3}})
    rubber = g.material({"name": "Rubber", "pbrMetallicRoughness": {"baseColorFactor": [0.05, 0.05, 0.05, 1], "metallicFactor": 0.0, "roughnessFactor": 0.9}})
    variants = {"extensions": {"KHR_materials_variants": {"mappings": [{"material": desert, "variants": [0]}, {"material": winter, "variants": [1]}]}}}
    V, I = box((0, 0.42, 0), (0.9, 0.34, 1.3))
    hull_mesh = g.mesh("Hull", [(V, I, hull, None, variants)])
    V, I = box((0, 0.1, 0), (0.6, 0.22, 0.6))
    turret_mesh = g.mesh("Turret", [(V, I, hull, None, variants)])
    V, I = cylinder((0, 0, 0.35), 0.05, 0.7, "z", 10)
    barrel_mesh = g.mesh("Barrel", [(V, I, steel, None, None)])
    V, I = cylinder((0, 0, 0), 0.2, 0.14, "x", 14)
    wheel_mesh = g.mesh("Wheel", [(V, I, rubber, None, None)])
    root = g.node({"name": "Tank"}, root=True)
    hull_n = g.node({"name": "Hull", "mesh": hull_mesh})
    turret_n = g.node({"name": "Turret", "mesh": turret_mesh, "translation": [0, 0.59, -0.05]})
    barrel_n = g.node({"name": "Barrel", "mesh": barrel_mesh, "translation": [0, 0.11, 0.25]})
    wheels = [g.node({"name": "Wheel%d" % i, "mesh": wheel_mesh, "translation": [x, 0.2, z]})
              for i, (x, z) in enumerate([(0.52, 0.42), (-0.52, 0.42), (0.52, -0.42), (-0.52, -0.42)])]
    g.doc["nodes"][root]["children"] = [hull_n]
    g.doc["nodes"][hull_n]["children"] = [turret_n] + wheels
    g.doc["nodes"][turret_n]["children"] = [barrel_n]
    g.write_glb(os.path.join(OUT, "tank.glb"))
    # team mask: stripes on the hull (red channel = team colour)
    m = Image.new("RGB", (32, 32), (0, 0, 0))
    d = ImageDraw.Draw(m)
    d.rectangle([0, 10, 31, 21], fill=(255, 255, 255))
    m.save(os.path.join(OUT, "tank_team.png"))

# ------------------------------------------------------------------ windmill (.gltf + .bin + .png)

def windmill():
    g = Gltf()
    g.ext("KHR_texture_transform")
    g.ext("KHR_materials_variants")
    g.doc["extensions"] = {"KHR_materials_variants": {"variants": [{"name": "burnt"}]}}
    # sail lattice: alpha 0 holes in a wooden frame
    S = 64
    sail = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    for y in range(S):
        for x in range(S):
            frame = x < 4 or x >= S - 4 or y % 16 < 3 or x % 21 < 3
            cloth = (x + y) % 2 == 0 or True
            if frame: sail.putpixel((x, y), (120, 84, 50, 255))
            elif cloth and noise(x // 8, y // 8, 9) > 0.15: sail.putpixel((x, y), (236, 228, 205, 255))
    sail.save(os.path.join(OUT, "windmill_sail.png"))
    sail_t = g.image(uri="windmill_sail.png")
    wall = g.material({"name": "Wall", "pbrMetallicRoughness": {"baseColorFactor": [0.86, 0.82, 0.74, 1], "metallicFactor": 0, "roughnessFactor": 0.9}})
    roof = g.material({"name": "TeamRoof", "pbrMetallicRoughness": {"baseColorFactor": [1, 1, 1, 1], "metallicFactor": 0, "roughnessFactor": 0.6}})
    window = g.material({"name": "Window", "pbrMetallicRoughness": {"baseColorFactor": [0.2, 0.15, 0.1, 1]}, "emissiveFactor": [1.0, 0.75, 0.35]})
    sailm = g.material({"name": "Sail", "alphaMode": "MASK", "alphaCutoff": 0.5, "doubleSided": True,
                        "pbrMetallicRoughness": {"baseColorTexture": {"index": sail_t, "extensions": {"KHR_texture_transform": {"scale": [1, 2]}}},
                                                 "metallicFactor": 0, "roughnessFactor": 0.85}})
    V, I = box((0, 0.7, 0), (0.9, 1.4, 0.9))
    V2, I2 = pyramid((0, 1.4, 0), 0.6, 0.6)
    W = []
    for x in (-0.15, 0.15):
        Vw, Iw = box((x, 0.95, 0.455), (0.14, 0.2, 0.02))
        W.append((Vw, Iw))
    Vw = [v for w in W for v in w[0]]
    Iw = W[0][1] + [i + len(W[0][0]) for i in W[1][1]]
    wall_b = g.material({"name": "WallBurnt", "pbrMetallicRoughness": {"baseColorFactor": [0.18, 0.16, 0.15, 1], "metallicFactor": 0, "roughnessFactor": 1}})
    roof_b = g.material({"name": "RoofBurnt", "pbrMetallicRoughness": {"baseColorFactor": [0.12, 0.1, 0.1, 1], "metallicFactor": 0, "roughnessFactor": 1},
                         "emissiveFactor": [0.35, 0.08, 0.0]})
    vb = lambda m: {"extensions": {"KHR_materials_variants": {"mappings": [{"material": m, "variants": [0]}]}}}
    body = g.mesh("Body", [(V, I, wall, None, vb(wall_b)), (V2, I2, roof, None, vb(roof_b)), (Vw, Iw, window, None, None)])
    # four sails as quads in the XY plane, around the hub
    SV, SI = [], []
    for k in range(4):
        a = k * math.pi / 2
        c, s = math.cos(a), math.sin(a)
        quad = [(-0.12, 0.12), (0.12, 0.12), (0.12, 1.0), (-0.12, 1.0)]
        base = len(SV)
        for (qx, qy), uv in zip(quad, ([0, 1], [1, 1], [1, 0], [0, 0])):
            SV.append(([qx * c - qy * s, qx * s + qy * c, 0], [0, 0, 1], uv))
        SI += [base, base + 1, base + 2, base, base + 2, base + 3]
    sails = g.mesh("Sails", [(SV, SI, sailm, None, None)])
    root = g.node({"name": "Windmill", "mesh": body}, root=True)
    hub = g.node({"name": "Sails", "mesh": sails, "translation": [0, 1.25, 0.5]})
    g.doc["nodes"][root]["children"] = [hub]
    g.write_gltf(os.path.join(OUT, "windmill.gltf"), "windmill.bin")

# ------------------------------------------------------------------ crystal (OBJ + MTL)

def etc1_block(rgb):
    """Flat 4x4 ETC1/ETC2 block (individual mode, table 0, all pixels +2)."""
    q = [max(0, min(15, int(round((c - 2) / 17.0)))) for c in rgb]
    hi = (q[0] << 28) | (q[0] << 24) | (q[1] << 20) | (q[1] << 16) | (q[2] << 12) | (q[2] << 8)  # tables 0, individual, no flip
    return struct.pack(">II", hi, 0)

def write_ktx2_etc2(img, path):
    levels = []
    im = img.convert("RGB")
    while True:
        w, h = im.size
        data = bytearray()
        for by in range(0, h, 4):
            for bx in range(0, w, 4):
                px = [im.getpixel((min(bx + i, w - 1), min(by + j, h - 1))) for j in range(4) for i in range(4)]
                data += etc1_block([sum(p[k] for p in px) // 16 for k in range(3)])
        levels.append(bytes(data))
        if w == 1 and h == 1: break
        im = im.resize((max(1, w // 2), max(1, h // 2)), Image.BOX)
    header = bytes([0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A])
    W, H = img.size
    header += struct.pack("<9I", 147, 1, W, H, 0, 0, 1, len(levels), 0)   # VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK
    index_end = 80 + 24 * len(levels)
    header += struct.pack("<4I2Q", 0, 0, 0, 0, 0, 0)                       # no DFD / KVD / SGD (readers here ignore them)
    offsets, at = [], index_end
    for l in levels:
        at = (at + 7) & ~7
        offsets.append(at); at += len(l)
    for o, l in zip(offsets, levels): header += struct.pack("<3Q", o, len(l), len(l))
    out = bytearray(header)
    for o, l in zip(offsets, levels):
        out += b"\0" * (o - len(out)); out += l
    with open(path, "wb") as f: f.write(out)

def crystal():
    S = 32
    rock = Image.new("RGB", (S, S))
    for y in range(S):
        for x in range(S):
            v = 0.45 + 0.4 * noise(x // 4, y // 4, 5)
            rock.putpixel((x, y), (int(110 * v), int(104 * v), int(112 * v)))
    write_ktx2_etc2(rock, os.path.join(OUT, "crystal_rock.ktx2"))
    lines = ["# crystal cluster (CC0), generated by tools/models/gen_sample_models.py", "mtllib crystal.mtl"]
    vi = vt = vn = 0
    def octa(cx, cz, r, h, tilt):
        nonlocal vi, vn
        out = []
        top = (cx + tilt, h, cz); bot = (cx, 0.0, cz)
        ring = [(cx + r * math.cos(a), h * 0.35, cz + r * math.sin(a)) for a in [i * math.pi / 3 for i in range(6)]]
        pts = [top, bot] + ring
        for p in pts: out.append("v %.4f %.4f %.4f" % p)
        base = vi + 1
        vi += len(pts)
        faces = []
        centre = (cx, h * 0.4, cz)
        def oriented(i0, i1, i2):  # counter-clockwise seen from outside
            p0, p1, p2 = pts[i0], pts[i1], pts[i2]
            e1 = [p1[k] - p0[k] for k in range(3)]; e2 = [p2[k] - p0[k] for k in range(3)]
            n = [e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]]
            out_dir = [(p0[k] + p1[k] + p2[k]) / 3 - centre[k] for k in range(3)]
            ok = sum(n[k] * out_dir[k] for k in range(3)) > 0
            return (base + i0, base + i1, base + i2) if ok else (base + i0, base + i2, base + i1)
        for i in range(6):
            a, b = 2 + i, 2 + (i + 1) % 6
            faces.append(oriented(0, a, b))
            faces.append(oriented(1, a, b))
        return out, faces
    lines.append("o Rock")
    V, I = box((0, 0.1, 0), (1.3, 0.2, 1.3), 2.0)
    for p, n, uv in V: lines.append("v %.4f %.4f %.4f" % tuple(p))
    for p, n, uv in V: lines.append("vt %.4f %.4f" % (uv[0], 1 - uv[1]))
    for p, n, uv in V: lines.append("vn %.4f %.4f %.4f" % tuple(n))
    lines.append("usemtl Rock")
    for t in range(0, len(I), 3):
        a, b, c = [i + 1 for i in I[t:t + 3]]
        lines.append("f %d/%d/%d %d/%d/%d %d/%d/%d" % (a, a, a, b, b, b, c, c, c))
    vi = len(V)
    lines.append("o Crystals")
    lines.append("usemtl Crystal")
    for cx, cz, r, h, tilt in [(0, 0, 0.22, 1.5, 0.05), (0.35, 0.2, 0.15, 0.9, 0.15), (-0.3, 0.25, 0.14, 1.0, -0.1), (0.05, -0.35, 0.13, 0.8, 0.0)]:
        v, f = octa(cx, cz, r, h, tilt)
        lines += v
        lines += ["f %d %d %d" % t for t in f]   # no normals: flat shading
    with open(os.path.join(OUT, "crystal.obj"), "w") as fh: fh.write("\n".join(lines) + "\n")
    with open(os.path.join(OUT, "crystal.mtl"), "w") as fh:
        fh.write("newmtl Rock\nKd 1 1 1\nNs 10\nmap_Kd crystal_rock.ktx2\n\n"
                 "newmtl Crystal\nKd 0.45 0.75 1.0\nd 0.7\nNs 400\nKe 0.15 0.35 0.6\n")

# ------------------------------------------------------------------ props: sword, cargo, scaffold

def simple(name, parts, materials, out):
    """Rigid single-node model from boxes / cylinders: parts = [(kind, args, material name)]."""
    g = Gltf()
    mats = {n: g.material(m) for n, m in materials.items()}
    prims = {}
    for kind, args, mat in parts:
        V, I = box(*args) if kind == "box" else cylinder(*args) if kind == "cyl" else pyramid(*args)
        P = prims.setdefault(mat, [[], []])
        base = len(P[0]); P[0] += V; P[1] += [i + base for i in I]
    mesh = g.mesh(name, [(V, I, mats[m], None, None) for m, (V, I) in prims.items()])
    g.node({"name": name, "mesh": mesh}, root=True)
    g.write_glb(os.path.join(OUT, out))

def props():
    steel = {"pbrMetallicRoughness": {"baseColorFactor": [0.75, 0.76, 0.8, 1], "metallicFactor": 1, "roughnessFactor": 0.25}}
    wood = {"pbrMetallicRoughness": {"baseColorFactor": [0.42, 0.27, 0.15, 1], "metallicFactor": 0, "roughnessFactor": 0.8}}
    gold = {"pbrMetallicRoughness": {"baseColorFactor": [1.0, 0.75, 0.2, 1], "metallicFactor": 1, "roughnessFactor": 0.35}}
    cloth = {"pbrMetallicRoughness": {"baseColorFactor": [0.55, 0.45, 0.3, 1], "metallicFactor": 0, "roughnessFactor": 0.95}}
    gem = {"pbrMetallicRoughness": {"baseColorFactor": [1, 1, 1, 1]}, "emissiveFactor": [0.3, 0.3, 0.3]}
    # sword hangs along -Y from the hand (socket origin)
    simple("Sword", [("box", ((0, -0.55, 0), (0.05, 0.75, 0.02)), "Blade"), ("box", ((0, -0.14, 0), (0.22, 0.04, 0.05)), "Hilt"),
                     ("cyl", ((0, -0.05, 0), 0.025, 0.14, "y", 8), "Hilt"), ("box", ((0, -0.14, 0.03), (0.05, 0.05, 0.02)), "TeamGem")],
           {"Blade": steel, "Hilt": wood, "TeamGem": gem}, "sword.glb")
    simple("Sack", [("cyl", ((0, 0.12, 0), 0.13, 0.24, "y", 10), "Cloth"), ("pyr", ((0, 0.24, 0), 0.08, 0.1), "Cloth"),
                    ("box", ((0, 0.27, 0), (0.08, 0.05, 0.08)), "Gold")], {"Cloth": cloth, "Gold": gold}, "sack.glb")
    simple("Logs", [("cyl", ((0, 0.06, z), 0.05, 0.42, "x", 8), "Wood") for z in (-0.06, 0.06)] + [("cyl", ((0, 0.15, 0), 0.05, 0.42, "x", 8), "Wood")],
           {"Wood": wood}, "logs.glb")
    poles = []
    for x in (-0.45, 0.45):
        for z in (-0.45, 0.45):
            poles.append(("box", ((x, 0.7, z), (0.06, 1.4, 0.06)), "Wood"))
    for y in (0.35, 0.8, 1.25):
        poles += [("box", ((0, y, z), (0.96, 0.05, 0.05)), "Wood") for z in (-0.45, 0.45)]
        poles += [("box", ((x, y, 0), (0.05, 0.05, 0.96)), "Wood") for x in (-0.45, 0.45)]
    poles += [("box", ((0, 0.05, 0), (0.9, 0.1, 0.9)), "Stone")]
    simple("Scaffold", poles, {"Wood": wood, "Stone": {"pbrMetallicRoughness": {"baseColorFactor": [0.6, 0.58, 0.55, 1], "metallicFactor": 0, "roughnessFactor": 0.9}}},
           "scaffold.glb")

# ------------------------------------------------------------------ slime (morph targets + weights animation)

def slime():
    g = Gltf()
    rings, seg, R = 10, 16, 0.4
    V, I, sq, st = [], [], [], []
    for y in range(rings + 1):
        phi = y / rings * math.pi
        for x in range(seg + 1):
            th = x / seg * 2 * math.pi
            n = [math.sin(phi) * math.cos(th), math.cos(phi), math.sin(phi) * math.sin(th)]
            p = [n[0] * R, n[1] * R * 0.8 + R * 0.8, n[2] * R]
            V.append((p, n, [x / seg, y / rings]))
            sq.append([n[0] * R * 0.35, -(p[1]) * 0.45, n[2] * R * 0.35])        # squash: wider and flatter
            st.append([-n[0] * R * 0.2, p[1] * 0.4, -n[2] * R * 0.2])           # stretch: taller and thinner
    for y in range(rings):
        for x in range(seg):
            a = y * (seg + 1) + x; b = a + seg + 1
            I += [a, a + 1, b, a + 1, b + 1, b]
    V, I = fix_winding(V, I)
    goo = g.material({"name": "TeamGoo", "pbrMetallicRoughness": {"baseColorFactor": [0.9, 0.95, 0.9, 1], "metallicFactor": 0, "roughnessFactor": 0.15}})
    targets = [{"POSITION": g.accessor(sq, 3, minmax=True)}, {"POSITION": g.accessor(st, 3, minmax=True)}]
    mesh = g.mesh("Slime", [(V, I, goo, None, {"targets": targets})])
    g.doc["meshes"][mesh]["weights"] = [0, 0]
    g.doc["meshes"][mesh]["extras"] = {"targetNames": ["Squash", "Stretch"]}
    node = g.node({"name": "Slime", "mesh": mesh}, root=True)
    t = [0, 0.25, 0.5, 0.75, 1.0]
    w = [0.0, 0.0, 0.8, 0.0, 0.0, 0.6, 0.5, 0.0, 0.0, 0.0]   # Squash, Stretch per key (scalar output)
    samp = {"input": g.accessor(t, 1), "output": g.accessor(w, 1), "interpolation": "LINEAR"}
    g.doc["accessors"][samp["input"]]["min"] = [0]; g.doc["accessors"][samp["input"]]["max"] = [1]
    g.doc["animations"] = [{"name": "Wobble", "samplers": [samp], "channels": [{"sampler": 0, "target": {"node": node, "path": "weights"}}]}]
    g.write_glb(os.path.join(OUT, "slime.glb"))

golem(); golem(lod=True); tank(); windmill(); crystal(); props(); slime()
print("models written to", OUT)
