#include "RtsSurface.h"
#include "engine/core/FixedMath.h"
#include <algorithm>

namespace rts::game {

void Surface::build() {
    auto t = std::make_shared<SphereTables>();
    W = 0; H = 0;
    size_t cornerTotal = 0;
    for (auto& b : bodies) {
        b.rowBase = H;
        b.cornerBase = cornerTotal;
        if (!b.hasSurface()) continue;
        W = std::max(W, b.faceSize);
        H += 6 * b.faceSize;
        cornerTotal += size_t(6) * size_t(b.faceSize + 1) * size_t(b.faceSize + 1);
    }
    if (corners.size() != cornerTotal) corners.assign(cornerTotal, 0);
    t->bodyOfRow.assign(size_t(H), 0);
    t->dir.assign(size_t(W) * size_t(H), Vec3{});
    t->nb.assign(size_t(W) * size_t(H) * 8, -1);
    t->coarseBase.push_back(0);
    for (size_t bi = 0; bi < bodies.size(); ++bi) {
        const BodyDef& b = bodies[bi];
        const int N = b.hasSurface() ? b.faceSize : 1;
        t->cs.emplace_back(N);
        t->cellSize.push_back(b.radius * kFxTwoPi / (4 * N));
        const int M = b.hasSurface() ? std::max(1, (N + 3) / 4) : 1;
        t->coarse.emplace_back(M);
        t->coarseSize.push_back(b.radius * kFxTwoPi / (4 * M));
        t->coarseBase.push_back(t->coarseBase.back() + 6 * M * M);
        if (!b.hasSurface()) continue;
        const CubeSphere& cs = t->cs.back();
        for (int r = 0; r < 6 * N; ++r) t->bodyOfRow[size_t(b.rowBase + r)] = int16_t(bi);
        for (int id = 0; id < cs.cellCount(); ++id) {
            const SphereCell c = cs.cell(id);
            const size_t g = size_t((b.rowBase + c.face * N + c.y) * W + c.x);
            t->dir[g] = cs.cellCenterDir(id);
            for (int k = 0; k < 8; ++k) {
                const int n = cs.neighbor(id, kDx[k], kDy[k]);
                if (n < 0) continue;
                const SphereCell d = cs.cell(n);
                t->nb[g * 8 + size_t(k)] = (b.rowBase + d.face * N + d.y) * W + d.x;
            }
        }
    }
    tab = t;
}

int Surface::findBody(const std::string& id) const {
    for (size_t i = 0; i < bodies.size(); ++i) if (bodies[i].id == id) return int(i);
    return -1;
}

int Surface::bodyAt(Vec3 p) const {
    if (p.x.raw < kSphereMinRaw || bodies.empty()) return 0;
    const int64_t b = (p.x.raw - kSphereMinRaw) / (int64_t(kBodySpacing) << Fixed::kFracBits);
    return int(std::min<int64_t>(b, int64_t(bodies.size()) - 1));
}

bool Surface::cellFace(int c, int& body, int& face, int& x, int& y) const {
    if (c < 0 || c >= W * H) return false;
    body = tab->bodyOfRow[size_t(c / W)];
    const BodyDef& b = bodies[size_t(body)];
    const int r = c / W - b.rowBase;
    x = c % W;
    face = r / b.faceSize;
    y = r % b.faceSize;
    return x < b.faceSize;
}

int Surface::cellAt(Vec3 p) const {
    const int bi = bodyAt(p);
    const BodyDef& b = bodies[size_t(bi)];
    if (!b.hasSurface()) return -1;
    const SphereCell c = tab->cs[size_t(bi)].cell(tab->cs[size_t(bi)].cellFromDir(p - anchor(bi)));
    return gridCell(bi, c.face, c.x, c.y);
}

int Surface::offset(int c, int dx, int dy) const {
    int b, f, x, y;
    if (!cellFace(c, b, f, x, y)) return -1;
    const CubeSphere& cs = tab->cs[size_t(b)];
    const int o = cs.offset(cs.cellId(f, x, y), dx, dy);
    if (o < 0) return -1;
    const SphereCell d = cs.cell(o);
    return gridCell(b, d.face, d.x, d.y);
}

Vec3 Surface::cellCenter(int c) const {
    const int b = bodyOfCell(c);
    return anchor(b) + cellDir(c) * (bodies[size_t(b)].radius + cellHeight(c));
}

int16_t Surface::corner(int body, int face, int i, int j) const {
    const BodyDef& b = bodies[size_t(body)];
    const int n1 = b.faceSize + 1;
    return corners[b.cornerBase + size_t(face) * size_t(n1 * n1) + size_t(j * n1 + i)];
}

Fixed Surface::cellHeight(int c) const {
    int b, f, x, y;
    if (!cellFace(c, b, f, x, y)) return Fixed{};
    const int64_t s = int64_t(corner(b, f, x, y)) + corner(b, f, x + 1, y) + corner(b, f, x, y + 1) + corner(b, f, x + 1, y + 1);
    return Fixed::fromRaw(s << (Fixed::kFracBits - 6));
}

namespace {
// bilinear on the face corners, same rounding scheme as MapGrid::surfaceHeight
Fixed bilinear(int64_t a, int64_t b, int64_t c, int64_t d, Fixed fx, Fixed fz) {
    const int64_t one = Fixed::kOne, x = fx.raw, z = fz.raw;
    const int64_t top = a * (one - x) + b * x, bot = c * (one - x) + d * x;
    const int64_t v = (top >> 4) * (one - z) + (bot >> 4) * z;
    return Fixed::fromRaw(v >> (28 - Fixed::kFracBits + 4));
}
} // namespace

Fixed Surface::heightAt(Vec3 p) const {
    const int bi = bodyAt(p);
    const BodyDef& b = bodies[size_t(bi)];
    if (!b.hasSurface()) return Fixed{};
    int face; Fixed a, bb;
    tab->cs[size_t(bi)].faceFromDir(p - anchor(bi), face, a, bb);
    const int N = b.faceSize;
    int cx = int(a.floorInt()), cy = int(bb.floorInt());
    if (cx >= N) cx = N - 1;
    if (cy >= N) cy = N - 1;
    return bilinear(corner(bi, face, cx, cy), corner(bi, face, cx + 1, cy), corner(bi, face, cx, cy + 1), corner(bi, face, cx + 1, cy + 1),
                    a - Fixed::fromInt(cx), bb - Fixed::fromInt(cy));
}

Vec3 Surface::up(Vec3 p) const { return fxNormalize(p - anchor(bodyAt(p))); }

Fixed Surface::altitude(Vec3 p) const {
    const int b = bodyAt(p);
    return (p - anchor(b)).length() - bodies[size_t(b)].radius;
}

Vec3 Surface::project(Vec3 p, Fixed alt) const {
    const int bi = bodyAt(p);
    const Vec3 o = anchor(bi), l = p - o;
    if (l.lengthSq().raw == 0) return p;
    const Vec3 d = fxNormalize(l);
    return o + d * (bodies[size_t(bi)].radius + heightAt(p) + alt);
}

Vec3 Surface::toShell(Vec3 p, Fixed r) const {
    const int bi = bodyAt(p);
    const Vec3 o = anchor(bi), l = p - o;
    if (l.lengthSq().raw == 0) return p;
    return o + fxNormalize(l) * r;
}

Fixed Surface::groundDist(Vec3 a, Vec3 b) const {
    const int ba = bodyAt(a), bb = bodyAt(b);
    if (ba != bb) return Fixed::fromInt(1 << 20);
    const Vec3 o = anchor(ba);
    return (fxNormalize(a - o) - fxNormalize(b - o)).length() * bodies[size_t(ba)].radius;
}

void Surface::frame(Vec3 p, Vec3& east, Vec3& south) const {
    const int bi = bodyAt(p);
    const Vec3 l = p - anchor(bi);
    int face; Fixed a, b;
    tab->cs[size_t(bi)].faceFromDir(l, face, a, b);
    CubeSphere::faceTangents(face, fxNormalize(l), east, south);
}

Vec3 Surface::toPlane(Vec3 p, Vec3 v) const {
    Vec3 e, s;
    frame(p, e, s);
    return {v.dot(e), Fixed{}, v.dot(s)};
}

Vec3 Surface::fromPlane(Vec3 p, Vec3 q) const {
    Vec3 e, s;
    frame(p, e, s);
    return e * q.x + s * q.z;
}

Vec3 Surface::tangent(Vec3 p, Vec3 v) const {
    const Vec3 u = up(p);
    return v - u * v.dot(u);
}

int Surface::bucketOf(Vec3 p) const {
    const int bi = bodyAt(p);
    const Vec3 l = p - anchor(bi);
    return tab->coarseBase[size_t(bi)] + tab->coarse[size_t(bi)].cellFromDir(l);
}

void Surface::bucketsAround(Vec3 p, Fixed r, std::vector<int32_t>& out) const {
    out.clear();
    const int bi = bodyAt(p);
    const CubeSphere& cs = tab->coarse[size_t(bi)];
    const int base = tab->coarseBase[size_t(bi)];
    const int c = cs.cellFromDir(p - anchor(bi));
    // buckets shrink to ~0.7 of the mean edge near face corners (tangent warp): pad the radius
    const Fixed edge = tab->coarseSize[size_t(bi)] * Fixed::fromRatio(7, 10);
    int k = int((r / fxMax(edge, Fixed::fromRatio(1, 16))).floorInt()) + 1;
    const int M = cs.faceSize();
    if (k >= M) { // whole body
        for (int i = 0; i < cs.cellCount(); ++i) out.push_back(base + i);
        return;
    }
    for (int dy = -k; dy <= k; ++dy)
        for (int dx = -k; dx <= k; ++dx) {
            int o = cs.offset(c, dx, dy);
            if (o < 0) { // past a cube corner: the walk in the other order usually gets there
                const int mid = cs.offset(c, 0, dy);
                o = mid < 0 ? -1 : cs.offset(mid, dx, 0);
            }
            if (o >= 0) out.push_back(base + o);
        }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
}

Vec3 Surface::facePoint(int body, int face, Fixed a, Fixed b, bool withHeight) const {
    const BodyDef& bd = bodies[size_t(body)];
    const Vec3 d = tab->cs[size_t(body)].dirFromFace(face, a, b);
    Fixed h{};
    if (withHeight && bd.hasSurface()) {
        const int N = bd.faceSize;
        int cx = std::clamp(int(a.floorInt()), 0, N - 1), cy = std::clamp(int(b.floorInt()), 0, N - 1);
        h = bilinear(corner(body, face, cx, cy), corner(body, face, cx + 1, cy), corner(body, face, cx, cy + 1), corner(body, face, cx + 1, cy + 1),
                     fxClamp(a - Fixed::fromInt(cx), Fixed{}, Fixed::fromInt(1)), fxClamp(b - Fixed::fromInt(cy), Fixed{}, Fixed::fromInt(1)));
    }
    return d * (bd.radius + h);
}

Vec3 Surface::faceNormal(int body, int face, Fixed a, Fixed b) const {
    const Fixed e = Fixed::fromRatio(1, 4);
    const Vec3 pa0 = facePoint(body, face, a - e, b), pa1 = facePoint(body, face, a + e, b);
    const Vec3 pb0 = facePoint(body, face, a, b - e), pb1 = facePoint(body, face, a, b + e);
    Vec3 n = fxNormalize(fxCross(pb1 - pb0, pa1 - pa0));
    const Vec3 d = tab->cs[size_t(body)].dirFromFace(face, a, b);
    if (n.dot(d).raw < 0) n = n * Fixed::fromInt(-1);
    return n;
}

Vec3 Surface::bodyPosition(int body, Fixed seconds) const {
    if (body < 0 || size_t(body) >= bodies.size()) return {};
    const BodyDef& b = bodies[size_t(body)];
    if (b.parent < 0 || b.parent == body || b.orbitPeriod.raw <= 0) return b.pos;
    // parents come before children (checked by the loader), so this recursion ends
    const Vec3 c = bodyPosition(b.parent, seconds);
    const Fixed turns = b.orbitPhase + Fixed::fromRaw((seconds / b.orbitPeriod).raw & 0xFFFF);
    return c + Vec3{fxCosTurns(turns) * b.orbitRadius, Fixed{}, fxSinTurns(turns) * b.orbitRadius};
}

} // namespace rts::game
