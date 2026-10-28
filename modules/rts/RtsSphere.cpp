#include "RtsSphere.h"
#include "engine/core/FixedMath.h"

namespace rts::game {

const SphereFace kSphereFaces[6] = {
    {{+1, 0, 0}, {0, 0, -1}, {0, -1, 0}},
    {{-1, 0, 0}, {0, 0, +1}, {0, -1, 0}},
    {{0, +1, 0}, {+1, 0, 0}, {0, 0, +1}},
    {{0, -1, 0}, {+1, 0, 0}, {0, 0, -1}},
    {{0, 0, +1}, {+1, 0, 0}, {0, -1, 0}},
    {{0, 0, -1}, {-1, 0, 0}, {0, -1, 0}},
};

namespace {
Vec3 toVec(IVec3 v) { return {Fixed::fromInt(v.x), Fixed::fromInt(v.y), Fixed::fromInt(v.z)}; }
Fixed dotI(Vec3 a, IVec3 b) { // exact: components of b are -1/0/+1
    Fixed r{};
    if (b.x) r += b.x > 0 ? a.x : -a.x;
    if (b.y) r += b.y > 0 ? a.y : -a.y;
    if (b.z) r += b.z > 0 ? a.z : -a.z;
    return r;
}
IVec3 add3(IVec3 a, IVec3 b, IVec3 c) { return {a.x + b.x + c.x, a.y + b.y + c.y, a.z + b.z + c.z}; }
IVec3 mul(IVec3 a, int k) { return {a.x * k, a.y * k, a.z * k}; }
int faceOfNormal(IVec3 n) {
    for (int f = 0; f < 6; ++f) if (kSphereFaces[f].n == n) return f;
    return 0;
}
// tan(s / 8 turn) for s = (2a - N) / N, computed as one truncating division so that a and N - a give
// exactly opposite values (seam vertices then match bit for bit on every face that shares them).
Fixed warp(Fixed a, int n) {
    const int64_t num = 2 * a.raw - int64_t(n) * Fixed::kOne;
    return fxTanTurns(Fixed::fromRaw(num / (8 * int64_t(n))));
}
} // namespace

CubeSphere::CubeSphere(int faceSize) : n_(faceSize < 1 ? 1 : faceSize) {
    warp_.resize(size_t(n_) + 1);
    for (int k = 0; k <= n_; ++k) warp_[size_t(k)] = warp(Fixed::fromInt(k), n_);
}

Vec3 CubeSphere::dirFromFace(int face, Fixed a, Fixed b) const {
    const SphereFace& f = kSphereFaces[face < 0 || face > 5 ? 0 : face];
    const Fixed sw = warp(a, n_), tw = warp(b, n_);
    return fxNormalize(toVec(f.n) + toVec(f.u) * sw + toVec(f.v) * tw);
}

Vec3 CubeSphere::cellCenterDir(int id) const {
    const SphereCell c = cell(id);
    return dirFromFace(c.face, Fixed::fromInt(c.x) + Fixed::fromRatio(1, 2), Fixed::fromInt(c.y) + Fixed::fromRatio(1, 2));
}

void CubeSphere::faceFromDir(Vec3 d, int& face, Fixed& a, Fixed& b) const {
    const Fixed ax = fxAbs(d.x), ay = fxAbs(d.y), az = fxAbs(d.z);
    if (ax.raw == 0 && ay.raw == 0 && az.raw == 0) { face = 0; a = b = Fixed::fromRatio(n_, 2); return; }
    if (ax >= ay && ax >= az) face = d.x.raw >= 0 ? 0 : 1;
    else if (ay >= az) face = d.y.raw >= 0 ? 2 : 3;
    else face = d.z.raw >= 0 ? 4 : 5;
    const SphereFace& f = kSphereFaces[face];
    const Fixed dn = dotI(d, f.n);
    // inverse warp: the cell comes from the cell-edge table (the values dirFromFace uses, so membership is
    // consistent with the forward map), the position inside it from atan (clamped into that cell)
    auto unwarp = [&](Fixed du) {
        const Fixed sw = fxClamp(du / dn, warp_.front(), warp_.back());
        int lo = 0, hi = n_;                       // warp_[lo] <= sw < warp_[hi]
        while (hi - lo > 1) { const int mid = (lo + hi) / 2; if (warp_[size_t(mid)] <= sw) lo = mid; else hi = mid; }
        const Fixed ang = fxAtan2Turns(du, dn);    // in [-1/8, 1/8]
        const Fixed r = Fixed::fromRaw(((8 * ang.raw + Fixed::kOne) * n_) / 2);
        return fxClamp(r, Fixed::fromInt(lo), Fixed::fromInt(lo + 1) - Fixed::fromRaw(1));
    };
    a = unwarp(dotI(d, f.u));
    b = unwarp(dotI(d, f.v));
}

int CubeSphere::cellFromDir(Vec3 d) const {
    // same face choice and cell-edge table as faceFromDir, without the atan for the position inside the cell
    const Fixed ax = fxAbs(d.x), ay = fxAbs(d.y), az = fxAbs(d.z);
    if (ax.raw == 0 && ay.raw == 0 && az.raw == 0) return cellId(0, n_ / 2, n_ / 2);
    int face;
    if (ax >= ay && ax >= az) face = d.x.raw >= 0 ? 0 : 1;
    else if (ay >= az) face = d.y.raw >= 0 ? 2 : 3;
    else face = d.z.raw >= 0 ? 4 : 5;
    const SphereFace& f = kSphereFaces[face];
    const Fixed dn = dotI(d, f.n);
    auto cellOf = [&](Fixed du) {
        const Fixed sw = fxClamp(du / dn, warp_.front(), warp_.back());
        int lo = 0, hi = n_;
        while (hi - lo > 1) { const int mid = (lo + hi) / 2; if (warp_[size_t(mid)] <= sw) lo = mid; else hi = mid; }
        return lo;
    };
    return cellId(face, cellOf(dotI(d, f.u)), cellOf(dotI(d, f.v)));
}

int CubeSphere::neighbor(int id, int dx, int dy) const {
    if (id < 0 || id >= cellCount()) return -1;
    const SphereCell c = cell(id);
    const int N = n_;
    int p = 2 * c.x + 1 - N + 2 * dx, q = 2 * c.y + 1 - N + 2 * dy; // odd lattice offsets
    const bool outP = p > N || p < -N, outQ = q > N || q < -N;
    if (!outP && !outQ) return cellId(c.face, (p + N - 1) / 2, (q + N - 1) / 2);
    if (outP && outQ) return -1; // diagonal through a cube corner
    const SphereFace& f = kSphereFaces[c.face];
    // Fold the overflow over the edge: the part beyond the edge goes down the adjacent face (along -n).
    IVec3 P;
    if (outP) {
        const int sgn = p > 0 ? 1 : -1, over = p * sgn - N;
        P = add3(mul(f.u, sgn * N), mul(f.n, N - over), mul(f.v, q));
    } else {
        const int sgn = q > 0 ? 1 : -1, over = q * sgn - N;
        P = add3(mul(f.v, sgn * N), mul(f.n, N - over), mul(f.u, p));
    }
    IVec3 nn{};
    if (P.x == N || P.x == -N) nn = {P.x / N, 0, 0};
    else if (P.y == N || P.y == -N) nn = {0, P.y / N, 0};
    else nn = {0, 0, P.z / N};
    const int g = faceOfNormal(nn);
    const SphereFace& h = kSphereFaces[g];
    const int pp = P.dot(h.u), qq = P.dot(h.v);
    return cellId(g, (pp + N - 1) / 2, (qq + N - 1) / 2);
}

namespace {
// Fold a lattice point that overflowed one face edge onto the adjacent face (see CubeSphere::neighbor).
bool foldAxis(int N, int& face, int& p, int& q, bool alongP) {
    const SphereFace& f = kSphereFaces[face];
    const int c = alongP ? p : q;
    if (c <= N && c >= -N) return true;
    const int sgn = c > 0 ? 1 : -1, over = c * sgn - N;
    if (over >= 2 * N) return false;
    const IVec3 P = alongP ? add3(mul(f.u, sgn * N), mul(f.n, N - over), mul(f.v, q))
                           : add3(mul(f.v, sgn * N), mul(f.n, N - over), mul(f.u, p));
    const IVec3 nn = alongP ? mul(f.u, sgn) : mul(f.v, sgn);
    face = faceOfNormal(nn);
    const SphereFace& h = kSphereFaces[face];
    p = P.dot(h.u); q = P.dot(h.v);
    return true;
}
} // namespace

int CubeSphere::offset(int id, int dx, int dy) const {
    if (id < 0 || id >= cellCount() || dx >= n_ || dx <= -n_ || dy >= n_ || dy <= -n_) return -1;
    const SphereCell c = cell(id);
    const int N = n_;
    int face = c.face, p = 2 * c.x + 1 - N, q = 2 * c.y + 1 - N;
    // step 1: along the face's u axis; the walked direction stays the same 3D vector after a fold
    const SphereFace& f0 = kSphereFaces[face];
    p += 2 * dx;
    if (!foldAxis(N, face, p, q, true)) return -1;
    // step 2: along the original v axis, expressed in the (possibly new) face
    const SphereFace& f1 = kSphereFaces[face];
    const int vu = f0.v.dot(f1.u), vv = f0.v.dot(f1.v);
    if (vu == 0 && vv == 0) return -1;
    if (vu) { p += 2 * dy * vu; if (q > N || q < -N) return -1; if (!foldAxis(N, face, p, q, true)) return -1; }
    else { q += 2 * dy * vv; if (p > N || p < -N) return -1; if (!foldAxis(N, face, p, q, false)) return -1; }
    if (p > N || p < -N || q > N || q < -N) return -1;
    return cellId(face, (p + N - 1) / 2, (q + N - 1) / 2);
}

int CubeSphere::neighbors8(int id, int out[8]) const {
    static const int kD[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {-1, 1}, {1, -1}, {-1, -1}};
    int k = 0;
    for (auto& d : kD) {
        const int nb = neighbor(id, d[0], d[1]);
        if (nb >= 0) out[k++] = nb;
    }
    return k;
}

int CubeSphere::cornerDuplicates(int face, int i, int j, int out[3]) const {
    const int N = n_;
    const SphereFace& f = kSphereFaces[face < 0 || face > 5 ? 0 : face];
    const IVec3 P = add3(mul(f.n, N), mul(f.u, 2 * i - N), mul(f.v, 2 * j - N));
    int k = 0;
    for (int g = 0; g < 6; ++g) {
        const SphereFace& h = kSphereFaces[g];
        if (P.dot(h.n) != N) continue;
        out[k++] = cornerId(g, (P.dot(h.u) + N) / 2, (P.dot(h.v) + N) / 2);
    }
    // ascending (faces are visited in order and ids grow with the face, so already sorted)
    return k;
}

void CubeSphere::faceTangents(int face, Vec3 dir, Vec3& east, Vec3& south) {
    const SphereFace& f = kSphereFaces[face < 0 || face > 5 ? 0 : face];
    const Vec3 u = toVec(f.u), v = toVec(f.v);
    east = fxNormalize(u - dir * dir.dot(u));
    south = fxNormalize(v - dir * dir.dot(v));
}

Fixed sphereArcTurns(Vec3 a, Vec3 b) {
    const Fixed s = fxCross(a, b).length(), c = a.dot(b);
    return fxAtan2Turns(s, c);
}

Vec3 sphereTangentToward(Vec3 dir, Vec3 to) {
    return fxNormalize(to - dir * dir.dot(to));
}

Vec3 sphereRotateToward(Vec3 from, Vec3 to, Fixed maxTurns, bool* reached) {
    const Fixed arc = sphereArcTurns(from, to);
    if (arc <= maxTurns) { if (reached) *reached = true; return fxNormalize(to); }
    if (reached) *reached = false;
    Vec3 t = sphereTangentToward(from, to);
    if (t.lengthSq().raw == 0) { // antiparallel: any deterministic perpendicular
        t = fxNormalize(fxCross(from, fxAbs(from.x) < Fixed::fromRatio(9, 10) ? Vec3{Fixed::fromInt(1), {}, {}}
                                                                               : Vec3{{}, Fixed::fromInt(1), {}}));
    }
    return fxNormalize(from * fxCosTurns(maxTurns) + t * fxSinTurns(maxTurns));
}

} // namespace rts::game
