"""Unity scene side of the effect importer: vector/quaternion math, the GUID index of an asset tree,
.unitypackage extraction, prefab hierarchy (world transforms) and the import report."""
import copy
import math
import os
import re
import tarfile

import unity_yaml as uy
from unity_yaml import get

CLASS_GAMEOBJECT, CLASS_TRANSFORM, CLASS_RECT_TRANSFORM = 1, 4, 224
CLASS_PS, CLASS_PSR, CLASS_MATERIAL, CLASS_LIGHT, CLASS_MONO = 198, 199, 21, 108, 114
CLASS_PREFAB_INSTANCE = 1001
CLASS_VISUAL_EFFECT = 2083052967

# ------------------------------------------------------------------------------------------ math

def q_mul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz)


def q_rot(q, v):
    x, y, z, w = q
    vx, vy, vz = v
    # v + 2 w (q x v) + 2 q x (q x v)
    cx, cy, cz = y * vz - z * vy, z * vx - x * vz, x * vy - y * vx
    cx2, cy2, cz2 = y * cz - z * cy, z * cx - x * cz, x * cy - y * cx
    return (vx + 2 * (w * cx + cx2), vy + 2 * (w * cy + cy2), vz + 2 * (w * cz + cz2))


def q_inv(q):
    return (-q[0], -q[1], -q[2], q[3])


def q_axis(axis, deg):
    h = math.radians(deg) * 0.5
    s = math.sin(h)
    return (axis[0] * s, axis[1] * s, axis[2] * s, math.cos(h))


def q_euler(e):
    """Unity Euler angles (degrees): applied Z, then X, then Y."""
    return q_mul(q_axis((0, 1, 0), e[1]), q_mul(q_axis((1, 0, 0), e[0]), q_axis((0, 0, 1), e[2])))


def v_len(v):
    return math.sqrt(sum(x * x for x in v))


def v_norm(v, default=(0.0, 1.0, 0.0)):
    n = v_len(v)
    return tuple(x / n for x in v) if n > 1e-9 else default


def v_mul(a, b):
    return tuple(x * y for x, y in zip(a, b))


def u2rel(v):
    """Unity vector (Y up, left-handed) -> engine effect space: engine world is (x, y, -z), and the effect
    frame for the default play direction (+Y) maps world (x, y, z) to relative (z, y, x)."""
    return (-v[2], v[1], v[0])


def u2world(v):
    return (v[0], v[1], -v[2])


def r(x, d=3):
    v = round(float(x), d)
    return 0.0 if v == 0 else v


def rng(lo, hi, d=3):
    lo, hi = r(lo, d), r(hi, d)
    return lo if lo == hi else [lo, hi]


# ------------------------------------------------------------------------------------------ report

class Report:
    def __init__(self):
        self.effects, self.textures = [], []
        self.mapped, self.approx, self.dropped, self.warnings, self.notes = [], [], [], [], []
        self.systems = []

    def as_json(self):
        return {'ok': not any(w.startswith('error:') for w in self.warnings), 'effects': self.effects, 'textures': self.textures,
                'systems': self.systems, 'approximate': self.approx, 'dropped': self.dropped, 'warnings': self.warnings, 'notes': self.notes}

    def text(self):
        out = []
        for e in self.effects:
            out.append('effect %s: %d emitter(s) -> %s' % (e['name'], e['emitters'], e['file']))
        for t in self.textures:
            out.append('texture %s -> %s%s' % (t['source'], t['file'] or '(not copied)', (' (' + t['note'] + ')') if t.get('note') else ''))
        for title, items in (('approximate', self.approx), ('dropped', self.dropped), ('warnings', self.warnings), ('notes', self.notes)):
            if items:
                out.append('%s:' % title)
                out += ['  - ' + x for x in items]
        return '\n'.join(out)


class SysLog:
    """Per-system module status for the report table."""

    def __init__(self, report, where):
        self.rep, self.where, self.modules = report, where, {}

    def ok(self, module, what):
        self.modules.setdefault(module, []).append(what)

    def approx(self, module, what):
        self.modules.setdefault(module, []).append('~ ' + what)
        self.rep.approx.append('%s: %s: %s' % (self.where, module, what))

    def drop(self, module, what):
        self.modules.setdefault(module, []).append('x ' + what)
        self.rep.dropped.append('%s: %s: %s' % (self.where, module, what))


# ------------------------------------------------------------------------------------------ assets

class Assets:
    """GUID index of a Unity asset tree: every *.meta next to its asset."""

    def __init__(self, roots, report):
        self.by_guid, self.cache, self.rep, self.guid_by_path = {}, {}, report, {}
        seen = set()
        for root in roots:
            root = os.path.abspath(root)
            if root in seen or not os.path.isdir(root):
                continue
            seen.add(root)
            for dp, dns, fns in os.walk(root):
                dns[:] = [d for d in dns if d not in ('Library', 'Temp', 'Logs', 'obj', '.git')]
                for fn in fns:
                    if not fn.endswith('.meta'):
                        continue
                    asset = os.path.join(dp, fn[:-5])
                    try:
                        with open(os.path.join(dp, fn), 'r', encoding='utf-8', errors='replace') as f:
                            head = f.read(4096)
                    except OSError:
                        continue
                    m = re.search(r'^guid:\s*([0-9a-fA-F]{32})', head, re.M)
                    if m and os.path.exists(asset):
                        self.by_guid.setdefault(m.group(1).lower(), asset)
                        self.guid_by_path.setdefault(os.path.abspath(asset), m.group(1).lower())

    def path(self, ref):
        g = ref.get('guid') if isinstance(ref, dict) else ref
        return self.by_guid.get(str(g).lower()) if g else None

    def docs(self, path):
        if path not in self.cache:
            try:
                with open(path, 'r', encoding='utf-8', errors='replace') as f:
                    self.cache[path] = uy.parse_documents(f.read())
            except OSError as e:
                self.rep.warnings.append('cannot read %s: %s' % (path, e))
                self.cache[path] = []
        return self.cache[path]

    def guid_of(self, path):
        return self.guid_by_path.get(os.path.abspath(path), '')

    def object(self, ref):
        """The object a {fileID, guid} reference points at in another asset (or None)."""
        path = self.path(ref)
        if not path or os.path.splitext(path)[1].lower() not in ('.prefab', '.asset', '.mat', '.unity'):
            return None, path
        fid = get(ref, 'fileID')
        return next((d for d in self.docs(path) if d.file_id == fid), None), path

    def meta(self, path):
        docs = self.docs(path + '.meta') if os.path.exists(path + '.meta') else []
        return docs[0].data if docs else {}


def project_roots(source):
    """The source folder plus the enclosing Assets folder (materials and textures often live elsewhere)."""
    src = os.path.abspath(source if os.path.isdir(source) else os.path.dirname(source))
    roots = [src]
    p = src
    while True:
        if os.path.basename(p) == 'Assets':
            roots.append(p)
            break
        parent = os.path.dirname(p)
        if parent == p:
            break
        p = parent
    return roots


def extract_unitypackage(path, tmp):
    """A .unitypackage is a tar.gz of <guid>/{asset, asset.meta, pathname}: rebuild the asset tree."""
    out = os.path.join(tmp, 'Assets_pkg')
    with tarfile.open(path, 'r:*') as tf:
        entries = {}
        for m in tf.getmembers():
            parts = m.name.strip('./').split('/')
            if len(parts) == 2 and m.isfile():
                entries.setdefault(parts[0], {})[parts[1]] = m
        for guid, files in entries.items():
            if 'pathname' not in files or 'asset' not in files:
                continue
            rel = tf.extractfile(files['pathname']).read().decode('utf-8', 'replace').splitlines()[0].strip()
            rel = os.path.normpath(rel)
            if rel.startswith('..') or os.path.isabs(rel):
                continue
            dst = os.path.join(out, rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst, 'wb') as f:
                f.write(tf.extractfile(files['asset']).read())
            meta = tf.extractfile(files['asset.meta']).read() if 'asset.meta' in files else ('guid: %s\n' % guid).encode()
            with open(dst + '.meta', 'wb') as f:
                f.write(meta)
    return out


# ------------------------------------------------------------------------------------------ prefab

MASK64 = 0x7FFFFFFFFFFFFFFF
MAX_NESTING = 4
PREFAB_EXT = ('.prefab',)


def remap_refs(obj, fn):
    """Local references ({fileID: n} without a guid) through fn, in place."""
    if isinstance(obj, dict):
        if 'fileID' in obj and not obj.get('guid') and isinstance(obj.get('fileID'), int) and obj['fileID']:
            obj['fileID'] = fn(obj['fileID'])
        for v in obj.values():
            if isinstance(v, (dict, list)):
                remap_refs(v, fn)
    elif isinstance(obj, list):
        for v in obj:
            if isinstance(v, (dict, list)):
                remap_refs(v, fn)


def _path_tokens(path):
    """'a.b.Array.data[2].c' -> ['a', 'b', 2, 'c'];  'a.Array.size' -> ['a', '#size']."""
    out, parts, i = [], path.split('.'), 0
    while i < len(parts):
        p = parts[i]
        if p == 'Array' and i + 1 < len(parts):
            nxt = parts[i + 1]
            m = re.fullmatch(r'data\[(\d+)\]', nxt)
            if m:
                out.append(int(m.group(1)))
            elif nxt == 'size':
                out.append('#size')
            i += 2
            continue
        out.append(p)
        i += 1
    return out


def set_property(data, path, value):
    """Applies one prefab modification (propertyPath + value) to a document's data; False if impossible."""
    toks = _path_tokens(path)
    if not toks or not isinstance(data, dict):
        return False
    cur = data
    for n, tok in enumerate(toks):
        last = n == len(toks) - 1
        nxt = toks[n + 1] if not last else None
        if nxt == '#size':
            lst = cur.get(tok) if isinstance(cur, dict) else None
            if not isinstance(lst, list):
                lst = []
                if isinstance(cur, dict):
                    cur[tok] = lst
                else:
                    return False
            size = int(num_or(value, len(lst)))
            del lst[size:]
            while len(lst) < size:
                lst.append(copy.deepcopy(lst[-1]) if lst else {})
            return True
        if isinstance(tok, int):
            if not isinstance(cur, list):
                return False
            while len(cur) <= tok:
                cur.append({})
            if last:
                cur[tok] = value
                return True
            if not isinstance(cur[tok], (dict, list)):
                cur[tok] = [] if isinstance(nxt, int) else {}
            cur = cur[tok]
        else:
            if not isinstance(cur, dict):
                return False
            if last:
                cur[tok] = value
                return True
            if not isinstance(cur.get(tok), (dict, list)):
                cur[tok] = [] if isinstance(nxt, int) else {}
            cur = cur[tok]
    return False


def num_or(v, default):
    return v if isinstance(v, (int, float)) and not isinstance(v, bool) else default


class Prefab:
    """A prefab with its nested prefab instances / variant base flattened in: the objects of every source
    prefab are cloned under Unity's own ids (instance id XOR source id), its property modifications applied
    and its root placed under m_TransformParent."""

    def __init__(self, path, assets, report, depth=0, stack=()):
        self.path, self.assets, self.rep = path, assets, report
        self.guid = assets.guid_of(path)
        own = assets.docs(path)
        # class 1001 is also the legacy (pre-2018.3) "Prefab" asset document of the file itself: not an instance
        self.nested = [d for d in own if d.class_id == CLASS_PREFAB_INSTANCE and get(d.data, 'm_SourcePrefab', 'guid')]
        self.nested_notes, self.nested_failed = [], []
        self.origin = {}  # (asset guid, file id in that asset) -> id here
        docs = [d for d in own if d.class_id != CLASS_PREFAB_INSTANCE]
        for d in docs:
            if not d.stripped:
                self.origin[(self.guid, d.file_id)] = d.file_id
        for inst in self.nested:
            docs = self._flatten(inst, docs, depth, stack)
        self.docs = docs
        self.by_id = {d.file_id: d for d in self.docs}
        self.gos = {d.file_id: d for d in self.docs if d.class_id == CLASS_GAMEOBJECT and not d.stripped}
        self.transforms = {d.file_id: d for d in self.docs if d.class_id in (CLASS_TRANSFORM, CLASS_RECT_TRANSFORM) and not d.stripped}
        self.comp_by_go = {}
        for d in self.docs:
            go = get(d.data, 'm_GameObject', 'fileID')
            if go and d.class_id not in (CLASS_GAMEOBJECT,) and not d.stripped:
                self.comp_by_go.setdefault(go, []).append(d)
        self.tr_by_go = {get(t.data, 'm_GameObject', 'fileID'): t for t in self.transforms.values()}

    def _flatten(self, inst, docs, depth, stack):
        rel = os.path.basename(self.path)
        src_ref = get(inst.data, 'm_SourcePrefab') or {}
        src = self.assets.path(src_ref)
        label = os.path.basename(src) if src else str(src_ref.get('guid', '?'))
        if not src:
            self.nested_failed.append('%s: nested prefab %s not found in the source folder (its systems are missing)' % (rel, label))
            return docs
        if not src.lower().endswith(PREFAB_EXT):
            self.nested_failed.append('%s: model instance %s (meshes in the scene are not particles)' % (rel, label))
            return docs
        if depth >= MAX_NESTING or os.path.abspath(src) in stack:
            self.nested_failed.append('%s: nested prefab %s: nesting deeper than %d levels or recursive (skipped)' % (rel, label, MAX_NESTING))
            return docs
        sub = Prefab(src, self.assets, self.rep, depth + 1, stack + (os.path.abspath(self.path),))
        self.nested_failed += sub.nested_failed
        iid = inst.file_id

        def idmap(f):
            return (iid ^ f) & MASK64

        clones = {}
        for d in sub.docs:
            if d.stripped:
                continue
            nd = uy.UnityObject(d.class_id, idmap(d.file_id), False, d.type, copy.deepcopy(d.data))
            remap_refs(nd.data, idmap)
            clones[nd.file_id] = nd
        for key, lid in sub.origin.items():
            self.origin.setdefault(key, idmap(lid))
        mod = get(inst.data, 'm_Modification') or {}
        applied = failed = 0
        for m in mod.get('m_Modifications') or []:
            if not isinstance(m, dict):
                continue
            tgt = m.get('target') or {}
            key = (str(tgt.get('guid', '')).lower() or sub.guid, get(tgt, 'fileID'))
            lid = sub.origin.get(key)
            doc = clones.get(idmap(lid)) if lid is not None else None
            ref = m.get('objectReference')
            val = ref if isinstance(ref, dict) and get(ref, 'fileID') else m.get('value')
            if isinstance(val, str):
                val = uy.scalar(val)
            if doc is not None and set_property(doc.data, str(m.get('propertyPath', '')), val):
                applied += 1
            else:
                failed += 1
        for rc in mod.get('m_RemovedComponents') or []:
            key = (str(get(rc, 'guid', default='')).lower() or sub.guid, get(rc, 'fileID'))
            lid = sub.origin.get(key)
            if lid is not None:
                clones.pop(idmap(lid), None)
        parent = get(mod, 'm_TransformParent', 'fileID') or 0
        for d in clones.values():
            if d.class_id in (CLASS_TRANSFORM, CLASS_RECT_TRANSFORM) and not get(d.data, 'm_Father', 'fileID'):
                d.data['m_Father'] = {'fileID': parent}
        note = '%s: nested prefab %s: %d system(s), %d modification(s) applied' % (
            rel, label, sum(1 for d in clones.values() if d.class_id == CLASS_PS), applied)
        if failed:
            note += ', %d not applicable' % failed
        self.nested_notes.append(note)
        self.nested_notes += sub.nested_notes
        out = [d for d in docs if d.file_id not in clones]  # stripped placeholders give way to the clones
        return out + list(clones.values())

    def name(self, go_id):
        return str(get(self.gos.get(go_id).data if go_id in self.gos else {}, 'm_Name', default='?'))

    def local(self, tr):
        d = tr.data
        return uy.vec3(d.get('m_LocalPosition')), uy.quat(d.get('m_LocalRotation')), uy.vec3(d.get('m_LocalScale'), (1, 1, 1))

    def roots(self):
        return [t for t in self.transforms.values() if get(t.data, 'm_Father', 'fileID') not in self.transforms]

    def children(self, tr):
        listed = [self.transforms[i] for i in (get(c, 'fileID') for c in get(tr.data, 'm_Children', default=[]) or [])
                  if i in self.transforms and get(self.transforms[i].data, 'm_Father', 'fileID') == tr.file_id]
        ids = {t.file_id for t in listed}
        extra = [t for t in self.transforms.values() if get(t.data, 'm_Father', 'fileID') == tr.file_id and t.file_id not in ids]
        return listed + extra

    def walk(self):
        """Depth-first (transform, go, world pos, world rot, world scale, path) from the roots."""
        if getattr(self, '_walk', None) is not None:
            return self._walk
        out = []

        def rec(tr, ppos, prot, pscale, path, depth):
            lp, lr, ls = self.local(tr)
            pos = tuple(a + b for a, b in zip(ppos, q_rot(prot, v_mul(pscale, lp))))
            rot = q_mul(prot, lr)
            scale = v_mul(pscale, ls)
            go = get(tr.data, 'm_GameObject', 'fileID')
            p = path + [self.name(go)]
            out.append((tr, go, pos, rot, scale, '/'.join(p)))
            if depth < 64:
                for ch in self.children(tr):
                    rec(ch, pos, rot, scale, p, depth + 1)

        for root in self.roots():
            rec(root, (0.0, 0.0, 0.0), (0.0, 0.0, 0.0, 1.0), (1.0, 1.0, 1.0), [], 0)
        self._walk = out
        return out

    def world_of(self, tr):
        """(pos, rot, scale) of a transform in the prefab."""
        return next(((p, q, s) for (t, _, p, q, s, _) in self.walk() if t is tr), ((0.0, 0.0, 0.0), (0.0, 0.0, 0.0, 1.0), (1.0, 1.0, 1.0)))
