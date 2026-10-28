"""Unity serialized YAML (scenes, prefabs, materials, .meta) without PyYAML.

Unity writes a small, regular subset of YAML 1.1: one document per object (`--- !u!<classID> &<fileID>`),
block mappings indented by two spaces, sequences whose dashes sit at the parent key's indentation,
flow mappings for references (`{fileID: 0, guid: ..., type: 2}`) and vectors, and long plain scalars
wrapped onto deeper-indented lines. That is all this parser understands, and all it needs.

Also: MinMaxCurve / MinMaxGradient / AnimationCurve evaluation (Unity's Hermite keyframes).
"""
import math
import re

_DOC = re.compile(r'^--- !u!(\d+) &(-?\d+)(.*)$')


class UnityObject:
    __slots__ = ('class_id', 'file_id', 'stripped', 'type', 'data')

    def __init__(self, class_id, file_id, stripped, type_name, data):
        self.class_id, self.file_id, self.stripped, self.type, self.data = class_id, file_id, stripped, type_name, data

    def __repr__(self):
        return '<%s %d &%d>' % (self.type, self.class_id, self.file_id)


# ------------------------------------------------------------------------------------------ scalars

def scalar(s):
    s = s.strip()
    if s == '' or s == '~' or s == 'null':
        return None
    if s[0] == "'" and s[-1:] == "'" and len(s) >= 2:
        return s[1:-1].replace("''", "'")
    if s[0] == '"' and s[-1:] == '"' and len(s) >= 2:
        return bytes(s[1:-1], 'utf-8').decode('unicode_escape', errors='replace')
    if s in ('Infinity', '.inf', '+Infinity'):
        return math.inf
    if s in ('-Infinity', '-.inf'):
        return -math.inf
    if s in ('NaN', '.nan'):
        return 0.0
    try:
        if re.fullmatch(r'[-+]?\d+', s):
            return int(s)
        if re.fullmatch(r'[-+]?(\d+\.?\d*|\.\d+)([eE][-+]?\d+)?', s):
            return float(s)
    except ValueError:
        pass
    return s


class _Flow:
    """`{a: 1, b: {c: 2}}` / `[1, 2]`."""

    def __init__(self, text):
        self.t, self.i = text, 0

    def ws(self):
        while self.i < len(self.t) and self.t[self.i] in ' \t\r\n':
            self.i += 1

    def value(self):
        self.ws()
        if self.i >= len(self.t):
            return None
        c = self.t[self.i]
        if c == '{':
            self.i += 1
            out = {}
            while True:
                self.ws()
                if self.i < len(self.t) and self.t[self.i] == '}':
                    self.i += 1
                    return out
                k = self.token(':')
                self.i += 1  # ':'
                out[k.strip()] = self.value()
                self.ws()
                if self.i < len(self.t) and self.t[self.i] == ',':
                    self.i += 1
                elif self.i >= len(self.t):
                    return out
        if c == '[':
            self.i += 1
            out = []
            while True:
                self.ws()
                if self.i < len(self.t) and self.t[self.i] == ']':
                    self.i += 1
                    return out
                if self.i >= len(self.t):
                    return out
                out.append(self.value())
                self.ws()
                if self.i < len(self.t) and self.t[self.i] == ',':
                    self.i += 1
        if c in '\'"':
            q = c
            j = self.i + 1
            while j < len(self.t):
                if self.t[j] == q and not (q == "'" and j + 1 < len(self.t) and self.t[j + 1] == "'"):
                    break
                j += 2 if (q == "'" and self.t[j] == "'") else 1
            s = self.t[self.i:j + 1]
            self.i = j + 1
            return scalar(s)
        return scalar(self.token(',}]'))

    def token(self, stops):
        j = self.i
        while j < len(self.t) and self.t[j] not in stops:
            j += 1
        s = self.t[self.i:j]
        self.i = j
        return s


def flow(text):
    return _Flow(text).value()


def _balanced(s):
    depth, q = 0, None
    for ch in s:
        if q:
            if ch == q:
                q = None
        elif ch in '\'"':
            q = ch
        elif ch in '{[':
            depth += 1
        elif ch in '}]':
            depth -= 1
    return depth <= 0


def _value(text):
    t = text.strip()
    if t.startswith('{') or t.startswith('['):
        return flow(t)
    return scalar(t)


# ------------------------------------------------------------------------------------------ blocks

class _Lines:
    def __init__(self, raw):
        self.items = []
        for line in raw:
            if not line.strip() or line.lstrip().startswith('#'):
                continue
            stripped = line.rstrip('\r\n')
            ind = len(stripped) - len(stripped.lstrip(' '))
            self.items.append([ind, stripped.strip()])
        self.i = 0

    def peek(self):
        return self.items[self.i] if self.i < len(self.items) else None


def _split_key(content):
    # 'key: value' | 'key:' ; keys never contain ': '
    if content.endswith(':') and ': ' not in content:
        return content[:-1], None
    k, sep, v = content.partition(': ')
    if not sep:
        return None, None
    return k, v


def _gather(L, base_indent, text):
    """Continuation lines of a wrapped scalar or an unbalanced flow mapping."""
    if text.lstrip().startswith(('{', '[')):
        while not _balanced(text) and L.peek() is not None:
            text += ' ' + L.peek()[1]
            L.i += 1
        return text
    while L.peek() is not None and L.peek()[0] > base_indent and _split_key(L.peek()[1])[0] is None and not L.peek()[1].startswith('- '):
        text += ' ' + L.peek()[1]
        L.i += 1
    return text


def _block(L, indent):
    cur = L.peek()
    if cur is None:
        return None
    if cur[1].startswith('- ') or cur[1] == '-':
        return _sequence(L, cur[0])
    return _mapping(L, cur[0])


def _mapping(L, indent):
    out = {}
    while True:
        cur = L.peek()
        if cur is None or cur[0] < indent:
            return out
        if cur[0] > indent:  # stray deeper line (wrapped value we did not attach): skip
            L.i += 1
            continue
        if cur[1].startswith('- ') or cur[1] == '-':
            return out
        k, v = _split_key(cur[1])
        L.i += 1
        if k is None:
            continue
        if v is None:
            nxt = L.peek()
            if nxt is not None and (nxt[0] > indent or (nxt[0] == indent and (nxt[1].startswith('- ') or nxt[1] == '-'))):
                out[k] = _block(L, nxt[0])
            else:
                out[k] = None
        else:
            out[k] = _value(_gather(L, indent, v))


def _sequence(L, indent):
    out = []
    while True:
        cur = L.peek()
        if cur is None or cur[0] != indent or not (cur[1].startswith('- ') or cur[1] == '-'):
            return out
        item = cur[1][2:] if cur[1] != '-' else ''
        if item == '':
            L.i += 1
            nxt = L.peek()
            out.append(_block(L, nxt[0]) if nxt is not None and nxt[0] > indent else None)
            continue
        k, v = _split_key(item)
        if k is not None and not item.lstrip().startswith(('{', '[', '"', "'")):
            cur[0], cur[1] = indent + 2, item  # the item is a mapping whose first key shares the dash line
            out.append(_mapping(L, indent + 2))
        else:
            L.i += 1
            out.append(_value(_gather(L, indent, item)))


def parse_block_text(text):
    L = _Lines(text.splitlines())
    return _block(L, 0) if L.peek() is not None else {}


def parse_documents(text):
    """Unity YAML file -> [UnityObject]. Non-Unity YAML (a .meta file) -> one object with class_id -1."""
    docs = []
    cur = None
    body = []

    def flush():
        if cur is None:
            return
        data = parse_block_text('\n'.join(body))
        type_name, payload = None, data
        if isinstance(data, dict) and len(data) == 1:
            type_name, payload = next(iter(data.items()))
        docs.append(UnityObject(cur[0], cur[1], cur[2], type_name, payload if payload is not None else {}))

    for line in text.splitlines():
        m = _DOC.match(line)
        if m:
            flush()
            cur = (int(m.group(1)), int(m.group(2)), 'stripped' in m.group(3))
            body = []
        elif line.startswith('%') or line.startswith('---'):
            continue
        elif cur is not None:
            body.append(line)
        else:
            body.append(line)
    if cur is None:
        data = parse_block_text('\n'.join(body))
        return [UnityObject(-1, 0, False, None, data if isinstance(data, dict) else {})]
    flush()
    return docs


# ------------------------------------------------------------------------------------------ helpers

def get(d, *path, default=None):
    for p in path:
        if isinstance(d, dict) and p in d:
            d = d[p]
        elif isinstance(d, list) and isinstance(p, int) and 0 <= p < len(d):
            d = d[p]
        else:
            return default
    return d if d is not None else default


def num(v, default=0.0):
    if isinstance(v, bool):
        return float(v)
    if isinstance(v, (int, float)):
        return float(v)
    return default


def vec3(d, default=(0.0, 0.0, 0.0)):
    if not isinstance(d, dict):
        return default
    return (num(d.get('x'), default[0]), num(d.get('y'), default[1]), num(d.get('z'), default[2]))


def quat(d):
    if not isinstance(d, dict):
        return (0.0, 0.0, 0.0, 1.0)
    return (num(d.get('x')), num(d.get('y')), num(d.get('z')), num(d.get('w'), 1.0))


def color(d, default=(1.0, 1.0, 1.0, 1.0)):
    if not isinstance(d, dict):
        return default
    return (num(d.get('r'), default[0]), num(d.get('g'), default[1]), num(d.get('b'), default[2]), num(d.get('a'), default[3]))


# ------------------------------------------------------------------------------------------ curves

def _finite(x):
    return x if math.isfinite(x) else None


def curve_keys(curve):
    keys = []
    for k in get(curve, 'm_Curve', default=[]) or []:
        if not isinstance(k, dict):
            continue
        keys.append((num(k.get('time')), num(k.get('value')), num(k.get('inSlope')), num(k.get('outSlope'))))
    keys.sort(key=lambda k: k[0])
    return keys


def curve_eval(keys, t):
    """Unity AnimationCurve (Hermite, non-weighted). Infinite tangents are steps."""
    if not keys:
        return 1.0
    if t <= keys[0][0]:
        return keys[0][1]
    if t >= keys[-1][0]:
        return keys[-1][1]
    for a, b in zip(keys, keys[1:]):
        if t <= b[0]:
            dt = b[0] - a[0]
            if dt <= 1e-9:
                return b[1]
            m0, m1 = _finite(a[3]), _finite(b[2])
            if m0 is None or m1 is None:
                return a[1]
            s = (t - a[0]) / dt
            s2, s3 = s * s, s * s * s
            return ((2 * s3 - 3 * s2 + 1) * a[1] + (s3 - 2 * s2 + s) * dt * m0 + (-2 * s3 + 3 * s2) * b[1] + (s3 - s2) * dt * m1)
    return keys[-1][1]


def mmc_state(m):
    return int(num(get(m, 'minMaxState'), 0))


def mmc_eval(m, t, which=0.5):
    """Value of a MinMaxCurve at normalized time t; `which` 0..1 between the min and max curve/constant."""
    if not isinstance(m, dict):
        return num(m, 0.0)
    st = mmc_state(m)
    sc = num(m.get('scalar'), 0.0)
    if st == 0:
        return sc
    if st == 3:
        lo = num(m.get('minScalar'), sc)
        return lo + (sc - lo) * which
    hi = sc * curve_eval(curve_keys(m.get('maxCurve')), t)
    if st == 1:
        return hi
    lo = sc * curve_eval(curve_keys(m.get('minCurve')), t)
    return lo + (hi - lo) * which


def mmc_range(m):
    """(min, max) over time and randomness."""
    if not isinstance(m, dict):
        v = num(m, 0.0)
        return v, v
    st = mmc_state(m)
    sc = num(m.get('scalar'), 0.0)
    if st == 0:
        return sc, sc
    if st == 3:
        lo = num(m.get('minScalar'), sc)
        return min(lo, sc), max(lo, sc)
    vals = []
    for i in range(33):
        t = i / 32.0
        vals.append(mmc_eval(m, t, 0.0))
        vals.append(mmc_eval(m, t, 1.0))
    return min(vals), max(vals)


def mmc_mean(m):
    if not isinstance(m, dict):
        return num(m, 0.0)
    st = mmc_state(m)
    if st in (0, 3):
        lo, hi = mmc_range(m)
        return (lo + hi) * 0.5
    return sum(mmc_eval(m, i / 32.0, 0.5) for i in range(33)) / 33.0


def mmc_is_curve(m):
    return isinstance(m, dict) and mmc_state(m) in (1, 2)


def simplify(points, tol):
    """Drop keys that linear interpolation of their neighbours reproduces within tol."""
    pts = list(points)
    changed = True
    while changed and len(pts) > 2:
        changed = False
        best, bi = None, -1
        for i in range(1, len(pts) - 1):
            (t0, v0), (t1, v1), (t2, v2) = pts[i - 1], pts[i], pts[i + 1]
            f = (t1 - t0) / (t2 - t0) if t2 > t0 else 0
            err = abs(v0 + (v2 - v0) * f - v1)
            if best is None or err < best:
                best, bi = err, i
        if best is not None and best <= tol:
            del pts[bi]
            changed = True
    if len(pts) == 2 and abs(pts[0][1] - pts[1][1]) <= tol:
        pts = [pts[0], (1.0, pts[0][1])]
    return pts


def mmc_keys(m, multiplier=1.0, samples=33):
    """MinMaxCurve over a normalized lifetime -> [[t, v], ...] (two curves / constants: the middle)."""
    pts = [(i / (samples - 1), mmc_eval(m, i / (samples - 1), 0.5) * multiplier) for i in range(samples)]
    lo, hi = min(p[1] for p in pts), max(p[1] for p in pts)
    return [[round(t, 3), round(v, 4)] for t, v in simplify(pts, max(1e-4, (hi - lo) * 0.01))]


# ------------------------------------------------------------------------------------------ gradients

def _gradient_keys(g):
    """Gradient -> (color keys [(t, rgb)], alpha keys [(t, a)], fixed mode)."""
    if not isinstance(g, dict):
        return [(0.0, (1, 1, 1)), (1.0, (1, 1, 1))], [(0.0, 1.0), (1.0, 1.0)], False
    nc = int(num(g.get('m_NumColorKeys'), 2))
    na = int(num(g.get('m_NumAlphaKeys'), 2))
    ck, ak = [], []
    for i in range(min(8, max(1, nc))):
        c = color(g.get('key%d' % i))
        ck.append((num(g.get('ctime%d' % i), 0) / 65535.0, c[:3]))
    for i in range(min(8, max(1, na))):
        c = color(g.get('key%d' % i))
        ak.append((num(g.get('atime%d' % i), 0) / 65535.0, c[3]))
    ck.sort(key=lambda k: k[0])
    ak.sort(key=lambda k: k[0])
    return ck, ak, int(num(g.get('m_Mode'), 0)) == 1


def _lerp_keys(keys, t, fixed):
    if t <= keys[0][0]:
        return keys[0][1]
    if t >= keys[-1][0]:
        return keys[-1][1]
    for a, b in zip(keys, keys[1:]):
        if t <= b[0]:
            if fixed:
                return b[1]  # Unity fixed mode: the next key's value holds until that key
            f = (t - a[0]) / (b[0] - a[0]) if b[0] > a[0] else 1.0
            if isinstance(a[1], tuple):
                return tuple(x + (y - x) * f for x, y in zip(a[1], b[1]))
            return a[1] + (b[1] - a[1]) * f
    return keys[-1][1]


def gradient_eval(g, t):
    ck, ak, fixed = _gradient_keys(g)
    rgb = _lerp_keys(ck, t, fixed)
    return (rgb[0], rgb[1], rgb[2], _lerp_keys(ak, t, fixed))


def gradient_times(g):
    ck, ak, fixed = _gradient_keys(g)
    ts = sorted(set([round(k[0], 4) for k in ck] + [round(k[0], 4) for k in ak] + [0.0, 1.0]))
    return ts, fixed


def mmg_state(m):
    return int(num(get(m, 'minMaxState'), 0))


def mmg_eval(m, t, which=0.5):
    """MinMaxGradient at t: 0 colour, 1 gradient, 2 two colours, 3 two gradients, 4 random colour."""
    if not isinstance(m, dict):
        return (1.0, 1.0, 1.0, 1.0)
    st = mmg_state(m)
    if st == 0:
        return color(m.get('maxColor'))
    if st in (1, 4):
        return gradient_eval(m.get('maxGradient'), t)
    if st == 2:
        a, b = color(m.get('minColor')), color(m.get('maxColor'))
    else:
        a, b = gradient_eval(m.get('minGradient'), t), gradient_eval(m.get('maxGradient'), t)
    return tuple(x + (y - x) * which for x, y in zip(a, b))


def mmg_keys(m, mult=(1, 1, 1, 1)):
    """MinMaxGradient over a lifetime -> [(t, rgba)] at the gradient's own key times (fixed mode: steps)."""
    st = mmg_state(m) if isinstance(m, dict) else 0
    if st in (0, 2):
        c = mmg_eval(m, 0)
        c = tuple(min(1.0, x * y) for x, y in zip(c, mult))
        return [(0.0, c), (1.0, c)]
    g = m.get('maxGradient')
    ts, fixed = gradient_times(g)
    if st == 3:
        t2, _ = gradient_times(m.get('minGradient'))
        ts = sorted(set(ts + t2))
    out = []
    for t in ts:
        out.append((t, mmg_eval(m, t)))
        if fixed and t < 1:  # a step right after the key: same time, next colour (stable sort keeps the order)
            out.append((t, mmg_eval(m, min(1.0, t + 1e-4))))
    return [(t, tuple(min(1.0, max(0.0, x * y)) for x, y in zip(c, mult))) for t, c in out]


def hex_color(c):
    return '#%02x%02x%02x%02x' % tuple(int(round(min(1.0, max(0.0, x)) * 255)) for x in c)
