"""Unity values -> engine values for the effect importer.

MinMaxCurve (constant / two constants / curve / two curves) -> engine range (number, [min, max] or
{"curve", "curveMin"}), linear combinations of per-axis curves (axes mapped through a rotation), adaptive
sampling of Unity's Hermite / weighted curves into <= 16 linear keys, and the 3x3 frame math between
Unity space, engine world and the effect frame (engine/fx/FxEffect.h, docs/particles.md "Unity import").
"""
import math

import unity_yaml as uy
from unity_yaml import get, num

RAD = 180.0 / math.pi

# ------------------------------------------------------------------------------------------ frames
# Unity is Y-up left-handed, engine world = Unity (x, y, -z) (F). The effect frame for the default play
# direction (+Y) maps effect (x, y, z) to world (z, y, x) (W, its own inverse). Unity -> effect: P = W F,
# the importer's u2rel. Axial vectors (angular velocities) pick up the mirror's sign: -P.
F = ((1, 0, 0), (0, 1, 0), (0, 0, -1))
W = ((0, 0, 1), (0, 1, 0), (1, 0, 0))
P = ((0, 0, -1), (0, 1, 0), (1, 0, 0))
I3 = ((1, 0, 0), (0, 1, 0), (0, 0, 1))
# Engine shape-local frames whose axis is +Y (hemisphere, donut, ...) -> Unity shape-local frames whose axis is +Z
A_Y_TO_Z = ((1, 0, 0), (0, 0, -1), (0, 1, 0))


def mm(a, b):
    return tuple(tuple(sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)) for i in range(3))


def mv(a, v):
    return tuple(sum(a[i][k] * v[k] for k in range(3)) for i in range(3))


def mt(a):
    return tuple(tuple(a[j][i] for j in range(3)) for i in range(3))


def neg(a):
    return tuple(tuple(-x for x in row) for row in a)


def q_mat(q):
    """Rotation matrix of a quaternion (x, y, z, w), column j = image of axis j."""
    x, y, z, w = q
    n = math.sqrt(x * x + y * y + z * z + w * w) or 1.0
    x, y, z, w = x / n, y / n, z / n, w / n
    return ((1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)),
            (2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)),
            (2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)))


def signed_perm(m, eps=1e-4):
    """True when every row and column has exactly one +-1 (axis-aligned rotation)."""
    for row in m:
        big = [abs(x) for x in row if abs(x) > eps]
        if len(big) != 1 or abs(big[0] - 1) > eps:
            return False
    return True


def is_identity(m, eps=1e-4):
    return all(abs(m[i][j] - (1 if i == j else 0)) <= eps for i in range(3) for j in range(3))


def rot_between(a, b):
    """Smallest rotation taking unit vector a to unit vector b."""
    c = sum(x * y for x, y in zip(a, b))
    v = (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])
    if c < -0.999999:   # opposite: half turn around any axis perpendicular to a
        p = (1.0, 0.0, 0.0) if abs(a[0]) < 0.9 else (0.0, 0.0, 1.0)
        v = (a[1] * p[2] - a[2] * p[1], a[2] * p[0] - a[0] * p[2], a[0] * p[1] - a[1] * p[0])
        n = math.sqrt(sum(x * x for x in v))
        x, y, z = (q / n for q in v)
        return tuple(tuple(2 * u * w - (1 if i == j else 0) for j, w in enumerate((x, y, z))) for i, u in enumerate((x, y, z)))
    k = 1.0 / (1.0 + c)
    vx = ((0, -v[2], v[1]), (v[2], 0, -v[0]), (-v[1], v[0], 0))
    vx2 = mm(vx, vx)
    return tuple(tuple((1 if i == j else 0) + vx[i][j] + vx2[i][j] * k for j in range(3)) for i in range(3))


def euler_zxy(m):
    """Degrees (x, y, z) with m = Ry(y) Rx(x) Rz(z) (right-hand rule, applied Z, then X, then Y)."""
    sx = max(-1.0, min(1.0, -m[1][2]))
    x = math.asin(sx)
    if abs(sx) < 0.99999:
        y = math.atan2(m[0][2], m[2][2])
        z = math.atan2(m[1][0], m[1][1])
    else:
        y, z = math.atan2(-m[2][0], m[0][0]), 0.0
    return tuple(0.0 if abs(v) < 1e-9 else v * RAD for v in (x, y, z))


def mat_zxy(deg):
    x, y, z = (math.radians(v) for v in deg)
    rx = ((1, 0, 0), (0, math.cos(x), -math.sin(x)), (0, math.sin(x), math.cos(x)))
    ry = ((math.cos(y), 0, math.sin(y)), (0, 1, 0), (-math.sin(y), 0, math.cos(y)))
    rz = ((math.cos(z), -math.sin(z), 0), (math.sin(z), math.cos(z), 0), (0, 0, 1))
    return mm(ry, mm(rx, rz))


# ------------------------------------------------------------------------------------------ numbers

def rv(v):
    """Compact float for JSON: 5 significant digits, integers as ints."""
    if not math.isfinite(v):
        return 0
    f = float('%.5g' % v)
    if f == 0:
        return 0
    return int(f) if f.is_integer() and abs(f) < 1e9 else f


# ------------------------------------------------------------------------------------------ curves

class Curve:
    """A MinMaxCurve parsed once: state, scalar, min/max functions of t and the key times."""

    def __init__(self, m, mult=1.0):
        self.m = m if isinstance(m, dict) else {'minMaxState': 0, 'scalar': num(m, 0.0)}
        self.state = uy.mmc_state(self.m)
        sc = num(self.m.get('scalar'), 0.0)
        self.mult = mult
        if self.state == 3:
            self.c_lo, self.c_hi = num(self.m.get('minScalar'), sc) * mult, sc * mult
        else:
            self.c_lo = self.c_hi = sc * mult
        self.k_hi = uy.curve_keys(self.m.get('maxCurve')) if self.state in (1, 2) else []
        self.k_lo = uy.curve_keys(self.m.get('minCurve')) if self.state == 2 else []
        self.sc = sc * mult

    @property
    def curved(self):
        return self.state in (1, 2)

    @property
    def random(self):
        return self.state in (2, 3)

    def hi(self, t):
        return self.sc * uy.curve_eval(self.k_hi, t) if self.curved else self.c_hi

    def lo(self, t):
        if self.state == 2:
            return self.sc * uy.curve_eval(self.k_lo, t)
        return self.hi(t) if self.state == 1 else self.c_lo

    def times(self):
        return [k[0] for k in self.k_hi + self.k_lo]

    def mean(self):
        if not self.curved:
            return (self.c_lo + self.c_hi) * 0.5
        return sum((self.lo(i / 32.0) + self.hi(i / 32.0)) * 0.5 for i in range(33)) / 33.0

    def bounds(self):
        if not self.curved:
            return min(self.c_lo, self.c_hi), max(self.c_lo, self.c_hi)
        vals = [f(i / 64.0) for i in range(65) for f in (self.lo, self.hi)]
        return min(vals), max(vals)

    def is_zero(self):
        lo, hi = self.bounds()
        return abs(lo) < 1e-9 and abs(hi) < 1e-9


def _simplify(pts, tol):
    """Greedy key removal; the error of a removal is checked against every original sample it spans."""
    keep = list(range(len(pts)))

    def err(i):  # error of removing keep[i]
        a, b = pts[keep[i - 1]], pts[keep[i + 1]]
        worst = 0.0
        for j in range(keep[i - 1] + 1, keep[i + 1]):
            t, v = pts[j]
            f = (t - a[0]) / (b[0] - a[0]) if b[0] > a[0] else 0.0
            worst = max(worst, abs(a[1] + (b[1] - a[1]) * f - v))
        return worst

    while len(keep) > 2:
        best, bi = None, -1
        for i in range(1, len(keep) - 1):
            e = err(i)
            if best is None or e < best:
                best, bi = e, i
        if best is None or best > tol:
            break
        del keep[bi]
    return [pts[i] for i in keep]


def _max_error(pts, keys):
    err, k = 0.0, 0
    for t, v in pts:
        while k < len(keys) - 2 and t > keys[k + 1][0]:
            k += 1
        a, b = keys[k], keys[k + 1]
        f = (t - a[0]) / (b[0] - a[0]) if b[0] > a[0] else 0.0
        err = max(err, abs(a[1] + (b[1] - a[1]) * f - v))
    return err


def _refine(pts, tol, max_keys):
    """Top-down: keep inserting the sample farthest from the polyline until within tol or max_keys."""
    keep = [0, len(pts) - 1]

    def worst(i0, i1):
        a, b = pts[i0], pts[i1]
        best, bi = 0.0, -1
        for j in range(i0 + 1, i1):
            t, v = pts[j]
            f = (t - a[0]) / (b[0] - a[0]) if b[0] > a[0] else 0.0
            e = abs(a[1] + (b[1] - a[1]) * f - v)
            if e > best:
                best, bi = e, j
        return best, bi

    while len(keep) < max_keys:
        best, bi = 0.0, -1
        for i0, i1 in zip(keep, keep[1:]):
            e, j = worst(i0, i1)
            if e > best:
                best, bi = e, j
        if best <= tol or bi < 0:
            break
        keep.append(bi)
        keep.sort()
    return [pts[i] for i in keep]


def sample(fn, times=(), max_keys=16, tol_frac=0.008):
    """f(t) on 0..1 -> [[t, v], ...]: dense samples (plus the curve's own key times, so steps stay sharp),
    within tol_frac of the value range (0.8%: under 1% between the samples too) using at most max_keys keys."""
    ts = set(i / 128.0 for i in range(129))
    for t in times:
        if 0 < t < 1:
            ts.add(t)
            ts.add(max(0.0, t - 1e-3))
    pts = [(t, fn(t)) for t in sorted(ts)]
    lo, hi = min(p[1] for p in pts), max(p[1] for p in pts)
    if hi - lo <= 1e-6 * max(1.0, abs(hi)):
        return [[0, rv(pts[0][1])], [1, rv(pts[0][1])]]
    tol = max(1e-6, (hi - lo) * tol_frac)
    keys = _simplify(pts, tol)              # bottom-up: fewest keys when the budget is not the limit
    if len(keys) > max_keys:                # over budget: the better of greedy removal with a growing tolerance / top-down insertion
        a = keys
        while len(a) > max_keys:
            tol *= 1.25
            a = _simplify(a, tol)
        b = _refine(pts, 0.0, max_keys)
        keys = min((a, b), key=lambda k: _max_error(pts, k))
    out = []
    for t, v in keys:
        k = [rv(round(t, 4)), rv(v)]
        if not out or out[-1] != k:
            out.append(k)
    return out


def flat(keys):
    return all(k[1] == keys[0][1] for k in keys)


def safe_lo(lo):
    """A positive lower bound nudged up: the engine reads numbers as 16.16 fixed point, 0.01 may land just below 0.01."""
    return lo * 1.002 + 2e-5 if lo > 0 else lo


class Value:
    """Result of a conversion: the JSON value plus what was lost on the way."""

    def __init__(self, json_value, mixed=False, clamped=None):
        self.json, self.mixed, self.clamped = json_value, mixed, clamped


def combine(terms, lim=None):
    """sum(coef * curve) -> engine range JSON. terms: [(Curve, coef)]. `mixed` is set when random values of
    several Unity axes fall into one engine axis (Unity picks them independently, the engine with one random)."""
    act = [(c, k) for c, k in terms if abs(k) > 1e-9 and not c.is_zero()]
    if not act:
        if lim is not None and not lim[0] <= 0 <= lim[1]:
            return Value(rv(safe_lo(lim[0])), False, 0.0)
        return Value(0)
    mixed = sum(1 for c, _ in act if c.random) > 0 and len(act) > 1
    curved = any(c.curved for c, _ in act)
    rnd = any(c.random for c, _ in act)
    clamped = []

    def cl(v):
        if lim is None:
            return v
        if v < lim[0] - 1e-9 or v > lim[1] + 1e-9:
            clamped.append(v)
        return min(lim[1], max(safe_lo(lim[0]), v))

    def f_lo(t):
        return sum(min(k * c.lo(t), k * c.hi(t)) for c, k in act)

    def f_hi(t):
        return sum(max(k * c.lo(t), k * c.hi(t)) for c, k in act)

    if not curved:
        lo, hi = cl(f_lo(0)), cl(f_hi(0))
        a, b = rv(lo), rv(hi)
        return Value(a if a == b else [a, b], mixed, clamped[0] if clamped else None)
    times = [t for c, _ in act for t in c.times()]
    hi = sample(lambda t: cl(f_hi(t)), times)
    lo = sample(lambda t: cl(f_lo(t)), times) if rnd else None
    cv = clamped[0] if clamped else None
    if flat(hi) and (lo is None or flat(lo)):
        a, b = (lo[0][1] if lo else hi[0][1]), hi[0][1]
        return Value(a if a == b else [min(a, b), max(a, b)], mixed, cv)
    out = {'curve': hi}
    if lo is not None and lo != hi:
        out['curveMin'] = lo
    return Value(out, mixed, cv)


def value(m, mult=1.0, lim=None):
    return combine([(Curve(m), mult)], lim)


def keys_of(m, mult=1.0, which='mid', lim=None):
    """MinMaxCurve over a normalized time -> [[t, v], ...] (`which`: 'hi', 'lo' or 'mid' of a random pair)."""
    c = Curve(m, mult)
    fn = c.hi if which == 'hi' else c.lo if which == 'lo' else (lambda t: (c.lo(t) + c.hi(t)) * 0.5)
    if lim is not None:
        f0 = fn
        fn = lambda t: min(lim[1], max(lim[0], f0(t)))  # noqa: E731
    return sample(fn, c.times())


def is_zero(v):
    return v == 0 or v == 0.0


# ------------------------------------------------------------------------------------------ gradients

def gradient_keys(g, mult=(1, 1, 1, 1)):
    """One Unity Gradient -> [(t, rgba)] (colour and alpha keys merged; fixed mode as steps)."""
    return uy.mmg_keys({'minMaxState': 1, 'maxGradient': g}, mult)


def mmg_pair(m, mult=(1, 1, 1, 1)):
    """MinMaxGradient -> (max keys, min keys or None) over a normalized time."""
    st = uy.mmg_state(m) if isinstance(m, dict) else 0
    if st == 3:
        return gradient_keys(get(m, 'maxGradient'), mult), gradient_keys(get(m, 'minGradient'), mult)
    if st == 2:
        a = tuple(min(1.0, x * y) for x, y in zip(uy.color(get(m, 'maxColor')), mult))
        b = tuple(min(1.0, x * y) for x, y in zip(uy.color(get(m, 'minColor')), mult))
        return [(0.0, a), (1.0, a)], [(0.0, b), (1.0, b)]
    return uy.mmg_keys(m, mult), None
