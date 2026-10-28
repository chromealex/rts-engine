#include "RtsPlanetGroundFx.h"
#include "RtsView.h"
#include "../RtsModule.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>

namespace rts::game {

using namespace render;

namespace {
constexpr float kTerrLift = 0.06f, kRoadLift = 0.08f, kLaneLift = 0.1f;
enum { kDT = 1, kDD = 2, kDM = 4, kDR = 8 };
uint32_t c4(float3 c, float a) { return rgbaf({c.x, c.y, c.z, a}); }
void quad(MeshBuilder& b, float3 p0, float3 p1, float3 p2, float3 p3, float3 n, uint32_t c0, uint32_t c1, uint32_t c2, uint32_t c3) {
    const uint32_t i0 = b.addVertex(p0, n, c0), i1 = b.addVertex(p1, n, c1), i2 = b.addVertex(p2, n, c2), i3 = b.addVertex(p3, n, c3);
    if (dot(cross(p1 - p0, p2 - p0), n) >= 0) { b.addTriangle(i0, i1, i2); b.addTriangle(i0, i2, i3); }
    else { b.addTriangle(i0, i2, i1); b.addTriangle(i0, i3, i2); }
}
void quad(MeshBuilder& b, float3 p0, float3 p1, float3 p2, float3 p3, float3 n, uint32_t c) { quad(b, p0, p1, p2, p3, n, c, c, c, c); }
void tri(MeshBuilder& b, float3 p0, float3 p1, float3 p2, float3 n, uint32_t c0, uint32_t c1, uint32_t c2) {
    const uint32_t i0 = b.addVertex(p0, n, c0), i1 = b.addVertex(p1, n, c1), i2 = b.addVertex(p2, n, c2);
    if (dot(cross(p1 - p0, p2 - p0), n) >= 0) b.addTriangle(i0, i1, i2); else b.addTriangle(i0, i2, i1);
}
void freeMesh(gfx::Device& dev, Mesh& m) { dev.destroyBuffer(m.vb); dev.destroyBuffer(m.ib); m = {}; }
void setMesh(gfx::Device& dev, Mesh& m, const MeshBuilder& b) { freeMesh(dev, m); if (!b.empty()) m = b.upload(dev); }
uint32_t hash2(int x, int y, uint32_t s = 0) {
    uint32_t h = uint32_t(x) * 73856093u ^ uint32_t(y) * 19349663u ^ s * 83492791u;
    h ^= h >> 13; h *= 0x5bd1e995u; return h ^ (h >> 15);
}
float rnd(int x, int y, uint32_t s) { return float(hash2(x, y, s) & 0xffff) / 65535.0f; }
float3 toLocal(const Vec3& p, int body) {
    const Vec3 l = p - Surface::anchor(body);
    return {float(l.x.toDouble()), float(l.y.toDouble()), float(l.z.toDouble())};
}

// Cell (face, x, y) of a body: points at fractional cell coordinates, lifted along the surface normal.
struct CellGeo {
    const PlanetSurface* s;
    int f, x, y;
    float3 P(float fx, float fy, float lift) const {
        const float3 d = s->dirFromFace(f, float(x) + fx, float(y) + fy);
        return s->surfacePoint(d) + d * lift;
    }
    float3 up(float fx = 0.5f, float fy = 0.5f) const { return s->dirFromFace(f, float(x) + fx, float(y) + fy); }
};
// The four edges of a cell in kDx/kDy order (+x, -x, +y, -y): endpoints a, b and the inward step (ix, iy).
struct Edge { float ax, ay, bx, by, ix, iy; };
constexpr Edge kEdges[4] = {{1, 0, 1, 1, -1, 0}, {0, 0, 0, 1, 1, 0}, {0, 1, 1, 1, 0, -1}, {0, 0, 1, 0, 0, 1}};
constexpr int kDx4[4] = {1, -1, 0, 0}, kDy4[4] = {0, 0, 1, -1};
} // namespace

void RtsPlanetGroundFx::shutdown(gfx::Device& dev) {
    for (auto& b : bodies_) {
        for (auto& t : b.tiles) for (Mesh* m : {&t.fill, &t.border, &t.road, &t.rock, &t.veins, &t.gems, &t.marks}) freeMesh(dev, *m);
        freeMesh(dev, b.lanes);
    }
    bodies_.clear();
    memT_.clear(); memR_.clear(); memD_.clear(); memM_.clear(); memP_.clear();
    S_ = nullptr;
    init_ = false;
    laneSig_ = ~0u; laneCount_ = ~size_t(0);
}

void RtsPlanetGroundFx::dirtyCell(int c, int what, bool neighbours) {
    auto mark = [&](int cc) {
        int b, f, x, y;
        if (cc < 0 || !S_->cellFace(cc, b, f, x, y) || size_t(b) >= bodies_.size()) return;
        Body& B = bodies_[size_t(b)];
        if (B.tn <= 0) return;
        Tile& t = B.tiles[size_t((f * B.tn + y / kTile) * B.tn + x / kTile)];
        if (what & kDT) t.dT = true;
        if (what & kDD) t.dD = true;
        if (what & kDM) t.dM = true;
        if (what & kDR) t.dR = true;
    };
    mark(c);
    if (neighbours) for (int k = 0; k < 8; ++k) mark(S_->neighbor(c, k));   // borders / walls / road arms across seams
}

// ------------------------------------------------------------------ territory

void RtsPlanetGroundFx::buildTerritory(gfx::Device& dev, Tile& t, int bi, int f, int tx, int ty, const PlanetRenderer& planets, const RtsGroundFx& looks) {
    t.dT = false;
    const PlanetSurface* s = planets.surface(bi);
    const int N = s->faceSize();
    const auto& tl = looks.territoryLook();
    auto ownerColor = [&](int slot) {
        if (slot >= 1 && size_t(slot - 1) < tl.colors.size()) return tl.colors[size_t(slot - 1)];
        const float4 c = RtsView::playerColor(slot);
        return float3{c.x, c.y, c.z};
    };
    MeshBuilder fb, bb;
    const float bw = std::clamp(tl.border, 0.02f, 0.5f);
    for (int y = ty * kTile; y < std::min(N, (ty + 1) * kTile); ++y)
        for (int x = tx * kTile; x < std::min(N, (tx + 1) * kTile); ++x) {
            const int c = S_->gridCell(bi, f, x, y);
            const int o = memT_[size_t(c)];
            if (o <= 0 || memD_[size_t(c)]) continue;
            const CellGeo G{s, f, x, y};
            const float3 n = G.up(), oc = ownerColor(o);
            if (tl.fill > 0.001f) quad(fb, G.P(0, 0, kTerrLift), G.P(1, 0, kTerrLift), G.P(1, 1, kTerrLift), G.P(0, 1, kTerrLift), n, c4(oc, tl.fill));
            for (int k = 0; k < 4; ++k) { // borders towards another owner, across face seams
                const int nc = S_->offset(c, kDx4[k], kDy4[k]);
                if (nc < 0 || memT_[size_t(nc)] == o) continue;
                const Edge& e = kEdges[k];
                const float3 bright = oc * 0.55f + float3{0.45f, 0.45f, 0.45f};
                const float L = kTerrLift + 0.005f;
                quad(bb, G.P(e.ax, e.ay, L), G.P(e.bx, e.by, L), G.P(e.bx + e.ix * bw, e.by + e.iy * bw, L), G.P(e.ax + e.ix * bw, e.ay + e.iy * bw, L), n,
                     c4(bright, 0.95f), c4(bright, 0.95f), c4(oc, 0.05f), c4(oc, 0.05f));
            }
        }
    setMesh(dev, t.fill, fb);
    setMesh(dev, t.border, bb);
}

// ------------------------------------------------------------------ diggable rock

void RtsPlanetGroundFx::buildDig(gfx::Device& dev, Tile& t, int bi, int f, int tx, int ty, const PlanetRenderer& planets, const RtsGroundFx& looks, bool hi) {
    t.dD = false;
    const PlanetSurface* s = planets.surface(bi);
    const int N = s->faceSize();
    const auto& dl = looks.digLook();
    if (dl.kinds.empty()) return;
    MeshBuilder rb, vb, gb;
    const float H = dl.height, bevel = hi ? 0.09f : 0.0f;
    // a small pyramid standing on the rock top (gold nugget / gem crystal) in the cell's tangent frame
    auto pyramid = [&](MeshBuilder& b, const CellGeo& G, float fx, float fy, float base, float r, float h, float a, float tilt, float3 col, float3 tipCol) {
        const float3 up = G.up(fx, fy), c = G.P(fx, fy, base);
        const float3 t1 = normalize(G.P(std::min(fx + 0.2f, 1.0f), fy, base) - G.P(std::max(fx - 0.2f, 0.0f), fy, base)), t2 = cross(up, t1);
        const float3 tip = c + up * h + (t1 * std::cos(a * 1.7f) + t2 * std::sin(a * 1.7f)) * tilt;
        float3 p[4];
        for (int q = 0; q < 4; ++q) p[q] = c + (t1 * std::cos(a + float(q) * 1.5708f) + t2 * std::sin(a + float(q) * 1.5708f)) * r;
        for (int q = 0; q < 4; ++q) {
            float3 nn = normalize(cross(p[(q + 1) % 4] - p[q], tip - p[q]));
            const float3 out = (p[q] + p[(q + 1) % 4]) * 0.5f - c;
            if (dot(nn, out + up * 0.2f) < 0) nn = nn * -1.0f;
            tri(b, p[q], p[(q + 1) % 4], tip, nn, c4(col, 1), c4(col, 1), c4(tipCol, 1));
        }
    };
    for (int y = ty * kTile; y < std::min(N, (ty + 1) * kTile); ++y)
        for (int x = tx * kTile; x < std::min(N, (tx + 1) * kTile); ++x) {
            const int c = S_->gridCell(bi, f, x, y);
            const int k = memD_[size_t(c)];
            if (!k) continue;
            const RtsGroundFx::DigKind& K = dl.kinds[std::min(size_t(k - 1), dl.kinds.size() - 1)];
            const CellGeo G{s, f, x, y};
            const int gx = f * 4099 + x;   // noise key (per face; a seam shows a slight colour step only)
            float3 rock = K.rock * (0.97f + 0.06f * rnd(gx, y, 1));
            if (K.style != 0 && !hi) rock = lerp(rock, K.vein, 0.35f);   // low quality: the vein as a tint only
            bool wall[4];
            for (int d = 0; d < 4; ++d) { const int nc = S_->offset(c, kDx4[d], kDy4[d]); wall[d] = nc >= 0 && memD_[size_t(nc)] == 0; }
            const float x0 = wall[1] ? bevel : 0, x1 = 1 - (wall[0] ? bevel : 0), y0 = wall[3] ? bevel : 0, y1 = 1 - (wall[2] ? bevel : 0);
            auto cornerC = [&](int ix, int iy) {
                float3 cc = rock * (0.9f + 0.16f * rnd(f * 4099 + ix, iy, 3));
                if (K.style != 0) cc = lerp(cc, K.vein * 0.8f, 0.18f + 0.22f * rnd(f * 4099 + ix, iy, 13));
                return c4(cc, 1);
            };
            quad(rb, G.P(x0, y0, H), G.P(x1, y0, H), G.P(x1, y1, H), G.P(x0, y1, H), G.up(), cornerC(x, y), cornerC(x + 1, y), cornerC(x + 1, y + 1), cornerC(x, y + 1));
            const float wallTop = H - bevel * 0.9f;
            const float3 mid = G.P(0.5f, 0.5f, 0);
            for (int d = 0; d < 4; ++d) {
                if (!wall[d]) continue;
                const Edge& e = kEdges[d];
                // inner (top) endpoints of the edge: the bevel inset of this side, clamped to the other insets
                const float iax = std::clamp(e.ax, x0, x1), iay = std::clamp(e.ay, y0, y1), ibx = std::clamp(e.bx, x0, x1), iby = std::clamp(e.by, y0, y1);
                const float3 oa = G.P(e.ax, e.ay, wallTop), ob = G.P(e.bx, e.by, wallTop);
                float3 nrm = G.P((e.ax + e.bx) * 0.5f, (e.ay + e.by) * 0.5f, 0) - mid;
                const float3 up = G.up();
                nrm = normalize(nrm - up * dot(nrm, up));
                if (bevel > 0) quad(rb, oa, ob, G.P(ibx, iby, H), G.P(iax, iay, H), normalize(nrm + up), c4(rock * 0.92f, 1));
                // the wall reaches down to the floor next to it (slightly below to hide seams)
                const float3 fa = G.P(e.ax, e.ay, -0.05f), fb2 = G.P(e.bx, e.by, -0.05f);
                const float3 wc = rock * 0.72f, wd = rock * 0.5f;
                quad(rb, oa, ob, fb2, fa, nrm, c4(wc, 1), c4(wc, 1), c4(wd, 1), c4(wd, 1));
                if (K.style == 1 && hi) // vein streaks on the wall face
                    for (int q = 0; q < 2; ++q) {
                        const float tt = 0.2f + 0.6f * rnd(gx * 4 + d, y, 31 + uint32_t(q)), hgt = 0.12f + 0.15f * rnd(gx, y * 4 + d, 41 + uint32_t(q));
                        const float yy = 0.1f + (wallTop - 0.3f) * rnd(gx + d, y, 51 + uint32_t(q));
                        const float px = e.ax + (e.bx - e.ax) * tt, py = e.ay + (e.by - e.ay) * tt;
                        const float ux = (e.bx - e.ax) * 0.09f, uy = (e.by - e.ay) * 0.09f;
                        const float3 off = nrm * 0.012f;
                        quad(vb, G.P(px - ux, py - uy, yy) + off, G.P(px + ux, py + uy, yy + hgt * 0.4f) + off, G.P(px + ux, py + uy, yy + hgt) + off,
                             G.P(px - ux, py - uy, yy + hgt * 0.6f) + off, nrm, c4(K.vein, 1));
                    }
            }
            if (!hi || K.style == 0) continue;
            if (K.style == 1) { // gold nuggets on the top
                for (int q = 0; q < 4; ++q) {
                    const float px = x0 + 0.12f + (x1 - x0 - 0.24f) * rnd(gx, y, 61 + uint32_t(q)), py = y0 + 0.12f + (y1 - y0 - 0.24f) * rnd(gx, y, 71 + uint32_t(q));
                    pyramid(vb, G, px, py, H + 0.006f, 0.08f + 0.08f * rnd(gx, y, 81 + uint32_t(q)), 0.045f, 6.283f * rnd(gx, y, 91 + uint32_t(q)), 0.0f, K.vein, K.vein * 1.15f);
                }
            } else { // gem crystals: tilted pyramids
                for (int q = 0; q < 3; ++q) {
                    const float px = x0 + 0.18f + (x1 - x0 - 0.36f) * rnd(gx, y, 101 + uint32_t(q)), py = y0 + 0.18f + (y1 - y0 - 0.36f) * rnd(gx, y, 111 + uint32_t(q));
                    const float3 gc = K.vein * (0.85f + 0.3f * rnd(gx, y, 151 + uint32_t(q)));
                    pyramid(gb, G, px, py, H - 0.01f, 0.07f + 0.05f * rnd(gx, y, 121 + uint32_t(q)), 0.22f + 0.25f * rnd(gx, y, 131 + uint32_t(q)),
                            6.283f * rnd(gx, y, 141 + uint32_t(q)), 0.08f, gc * 0.7f, gc * 1.2f);
                }
            }
        }
    setMesh(dev, t.rock, rb);
    setMesh(dev, t.veins, vb);
    setMesh(dev, t.gems, gb);
}

// Dig marks of the viewer on rock tops, with the digging progress (cracks + a bar) of cells being dug.
void RtsPlanetGroundFx::buildMarks(gfx::Device& dev, Tile& t, int bi, int f, int tx, int ty, const PlanetRenderer& planets, const MapGrid&,
                                   const RtsGroundFx& looks, const Rules&) {
    t.dM = false;
    const PlanetSurface* s = planets.surface(bi);
    const int N = s->faceSize();
    const auto& dl = looks.digLook();
    MeshBuilder mb;
    for (int y = ty * kTile; y < std::min(N, (ty + 1) * kTile); ++y)
        for (int x = tx * kTile; x < std::min(N, (tx + 1) * kTile); ++x) {
            const int c = S_->gridCell(bi, f, x, y);
            if (!memM_[size_t(c)] || !memD_[size_t(c)]) continue;
            const CellGeo G{s, f, x, y};
            const float L = dl.height + 0.012f, m = 0.1f, w = 0.07f, a0 = m, a1 = 1 - m;
            const float3 n = G.up();
            quad(mb, G.P(m, m, L), G.P(1 - m, m, L), G.P(1 - m, 1 - m, L), G.P(m, 1 - m, L), n, c4(dl.mark, 0.38f));
            quad(mb, G.P(a0, a0, L), G.P(a1, a0, L), G.P(a1, a0 + w, L), G.P(a0, a0 + w, L), n, c4(dl.mark, 0.9f));
            quad(mb, G.P(a0, a1 - w, L), G.P(a1, a1 - w, L), G.P(a1, a1, L), G.P(a0, a1, L), n, c4(dl.mark, 0.9f));
            quad(mb, G.P(a0, a0 + w, L), G.P(a0 + w, a0 + w, L), G.P(a0 + w, a1 - w, L), G.P(a0, a1 - w, L), n, c4(dl.mark, 0.9f));
            quad(mb, G.P(a1 - w, a0 + w, L), G.P(a1, a0 + w, L), G.P(a1, a1 - w, L), G.P(a1 - w, a1 - w, L), n, c4(dl.mark, 0.9f));
            const int pb = memP_[size_t(c)];
            if (!pb || !dl.progress) continue;
            const float p = float(pb) / 16.0f, L2 = L + 0.006f, L3 = L + 0.01f;
            quad(mb, G.P(0.1f, 0.1f, L2), G.P(0.9f, 0.1f, L2), G.P(0.9f, 0.9f, L2), G.P(0.1f, 0.9f, L2), n, c4({0.08f, 0.055f, 0.03f}, (40.0f + 110.0f * p) / 255.0f));
            quad(mb, G.P(0.15f, 0.45f, L3), G.P(0.85f, 0.45f, L3), G.P(0.85f, 0.57f, L3), G.P(0.15f, 0.57f, L3), n, c4({0, 0, 0}, 0.67f));
            const float fx = 0.15f + 0.7f * p;
            quad(mb, G.P(0.15f, 0.45f, L3 + 0.003f), G.P(fx, 0.45f, L3 + 0.003f), G.P(fx, 0.57f, L3 + 0.003f), G.P(0.15f, 0.57f, L3 + 0.003f), n, c4(dl.mark, 1));
        }
    setMesh(dev, t.marks, mb);
}

// ------------------------------------------------------------------ road cells

void RtsPlanetGroundFx::buildRoads(gfx::Device& dev, Tile& t, int bi, int f, int tx, int ty, const PlanetRenderer& planets, const RtsGroundFx& looks) {
    t.dR = false;
    const PlanetSurface* s = planets.surface(bi);
    const int N = s->faceSize();
    const auto& rl = looks.roadLook();
    MeshBuilder rb;
    const float h = std::clamp(rl.width, 0.2f, 1.0f) * 0.5f, L = kRoadLift;
    const uint32_t col = c4(rl.color, rl.alpha);
    for (int y = ty * kTile; y < std::min(N, (ty + 1) * kTile); ++y)
        for (int x = tx * kTile; x < std::min(N, (tx + 1) * kTile); ++x) {
            const int c = S_->gridCell(bi, f, x, y);
            if (!memR_[size_t(c)]) continue;
            const CellGeo G{s, f, x, y};
            const float3 n = G.up();
            quad(rb, G.P(0.5f - h, 0.5f - h, L), G.P(0.5f + h, 0.5f - h, L), G.P(0.5f + h, 0.5f + h, L), G.P(0.5f - h, 0.5f + h, L), n, col);
            for (int k = 0; k < 4; ++k) { // arms to road neighbours (across seams)
                const int nc = S_->offset(c, kDx4[k], kDy4[k]);
                if (nc < 0 || !memR_[size_t(nc)]) continue;
                if (k == 0) quad(rb, G.P(0.5f + h, 0.5f - h, L), G.P(1, 0.5f - h, L), G.P(1, 0.5f + h, L), G.P(0.5f + h, 0.5f + h, L), n, col);
                if (k == 1) quad(rb, G.P(0, 0.5f - h, L), G.P(0.5f - h, 0.5f - h, L), G.P(0.5f - h, 0.5f + h, L), G.P(0, 0.5f + h, L), n, col);
                if (k == 2) quad(rb, G.P(0.5f - h, 0.5f + h, L), G.P(0.5f + h, 0.5f + h, L), G.P(0.5f + h, 1, L), G.P(0.5f - h, 1, L), n, col);
                if (k == 3) quad(rb, G.P(0.5f - h, 0, L), G.P(0.5f + h, 0, L), G.P(0.5f + h, 0.5f - h, L), G.P(0.5f - h, 0.5f - h, L), n, col);
            }
        }
    setMesh(dev, t.road, rb);
}

// ------------------------------------------------------------------ lanes and map roads

void RtsPlanetGroundFx::buildLanes(gfx::Device& dev, const PlanetRenderer& planets, const RtsScene& s, const RtsGroundFx& looks, const Rules& rules, bool hi) {
    const auto& ll = looks.laneLook();
    std::vector<MeshBuilder> mbs(bodies_.size());
    // a ribbon along a body-local polyline, draped on the surface (4 vertices across: faded edges)
    auto ribbon = [&](int bi, const std::vector<float3>& pts, float width, float alpha, float3 colr, float3 edge, float lift, bool fadeEnds) {
        const PlanetSurface* ps = planets.surface(bi);
        if (!ps || pts.size() < 2) return;
        MeshBuilder& b = mbs[size_t(bi)];
        std::vector<float> len(pts.size(), 0);
        for (size_t k = 1; k < pts.size(); ++k) len[k] = len[k - 1] + length(pts[k] - pts[k - 1]);
        const float total = len.back();
        uint32_t prev[4] = {};
        float3 prevP[4] = {};
        for (size_t k = 0; k < pts.size(); ++k) {
            const float3 up = normalize(pts[k]);
            const float3 tg = pts[std::min(k + 1, pts.size() - 1)] - pts[k ? k - 1 : 0];
            float3 side = cross(tg, up);
            side = length(side) > 1e-5f ? normalize(side) : normalize(cross(up, float3{0, 1, 0.3f}));
            const float fade = fadeEnds ? std::clamp(std::min(len[k], total - len[k]) / 1.2f, 0.0f, 1.0f) : 1.0f;
            const float off[4] = {-0.5f, -0.32f, 0.32f, 0.5f};
            uint32_t cur[4];
            float3 curP[4];
            for (int q = 0; q < 4; ++q) {
                const float3 d = normalize(pts[k] + side * (off[q] * width));
                curP[q] = ps->surfacePoint(d) + d * lift;
                const bool rim = q == 0 || q == 3;
                cur[q] = b.addVertex(curP[q], d, c4(rim ? edge : colr, rim ? 0.0f : alpha * fade));
            }
            if (k > 0)
                for (int q = 0; q < 3; ++q) { // keep the triangles facing outwards (front faces)
                    const bool flip = dot(cross(prevP[q + 1] - prevP[q], curP[q + 1] - prevP[q]), up) < 0;
                    if (!flip) { b.addTriangle(prev[q], prev[q + 1], cur[q + 1]); b.addTriangle(prev[q], cur[q + 1], cur[q]); }
                    else { b.addTriangle(prev[q], cur[q + 1], prev[q + 1]); b.addTriangle(prev[q], cur[q], cur[q + 1]); }
                }
            std::copy(cur, cur + 4, prev);
            std::copy(curP, curP + 4, prevP);
        }
    };
    // buildings a lane must not cross (garrisons other than its own two ends), body-local
    struct Obst { int body; float3 p; float r; Entity e; };
    std::vector<Obst> obst;
    for (const SceneObject& o : s.objects)
        if (o.garrison >= 0 && o.body >= 0 && o.type < rules.types.size()) {
            const UnitType& t = rules.types[o.type];
            obst.push_back({o.body, o.pos, std::max(0.8f, float(std::max(t.footprintW, t.footprintH)) * 0.5f), o.entity});
        }
    auto blockedBy = [&](const Lane& l, int body, const std::vector<float3>& pts) {
        for (const Obst& ob : obst) {
            if (ob.body != body || ob.e == l.a || ob.e == l.b) continue;
            for (const float3& p : pts) if (length(p - ob.p) < ob.r) return true;
        }
        return false;
    };
    // MW2 shows paths between neighbouring buildings only (relative neighbourhood graph, chord distances)
    auto neighbours = [&](const Lane& l, int body, const std::vector<float3>& pts) {
        if (ll.all || pts.size() < 2) return true;
        const float3 a = pts.front(), b = pts.back();
        const float dab = length(a - b);
        for (const Obst& ob : obst) {
            if (ob.body != body || ob.e == l.a || ob.e == l.b) continue;
            if (std::max(length(a - ob.p), length(b - ob.p)) < dab * 0.97f) return false;
        }
        return true;
    };
    auto localPts = [&](const Lane& l, int& body) {
        std::vector<float3> pts;
        body = -1;
        if (l.points.empty() || !S_) return pts;
        body = S_->bodyAt(l.points.front());
        pts.reserve(l.points.size());
        for (const Vec3& p : l.points) pts.push_back(toLocal(p, body));
        return pts;
    };
    if (s.lanes && rules.lanes.enabled) {
        const LaneRules& LR = rules.lanes;
        const float full = ll.width > 0 ? ll.width : float(LR.left + LR.right) * float(LR.spacing.toDouble()) * 0.6f + 0.6f;
        for (const Lane& l : *s.lanes) {
            int body;
            std::vector<float3> pts = localPts(l, body);
            if (body < 0 || size_t(body) >= bodies_.size() || pts.size() < 2) continue;
            const bool drawn = ll.all || (!blockedBy(l, body, pts) && neighbours(l, body, pts));
            if (l.offset == 0) {
                if (!drawn) continue;
                // centre of the band between the outermost lanes
                std::vector<float3> c = pts;
                const float shift = (float(LR.right) - float(LR.left)) * 0.5f * float(LR.spacing.toDouble());
                for (size_t k = 0; k < c.size() && std::fabs(shift) > 1e-4f; ++k) {
                    const float3 tg = pts[std::min(k + 1, pts.size() - 1)] - pts[k ? k - 1 : 0];
                    const float3 sd = cross(tg, normalize(pts[k]));
                    if (length(sd) > 1e-5f) c[k] = c[k] + normalize(sd) * shift;
                }
                ribbon(body, c, full, ll.alpha, ll.color, ll.edge, kLaneLift, true);
                if (hi && ll.sideLanes) ribbon(body, pts, 0.14f, ll.alpha * 0.8f, ll.edge, ll.edge, kLaneLift + 0.004f, true);
            } else if (hi && ll.sideLanes && drawn) ribbon(body, pts, 0.1f, ll.alpha * 0.45f, ll.edge, ll.edge, kLaneLift + 0.004f, true);
        }
    }
    // hand-made map roads between buildings: great-circle ribbons (ends in sim space)
    for (auto& [a, b] : s.roads) {
        if (!onSphereF(a) || bodyAtF(a) != bodyAtF(b)) continue;
        const int body = bodyAtF(a);
        if (size_t(body) >= bodies_.size() || !planets.surface(body)) continue;
        const float3 la = a - anchorF(body), lb = b - anchorF(body), da = normalize(la), db = normalize(lb);
        const float ang = std::acos(std::clamp(dot(da, db), -1.0f, 1.0f)), arc = ang * length(la);
        const int n = std::max(2, int(arc * 2.0f) + 1);
        std::vector<float3> pts;
        for (int k = 0; k <= n; ++k) {
            const float t = float(k) / float(n);
            float3 d;
            if (ang < 1e-4f) d = da;
            else { const float sa = std::sin(ang); d = da * (std::sin((1 - t) * ang) / sa) + db * (std::sin(t * ang) / sa); }
            pts.push_back(planets.surface(body)->surfacePoint(normalize(d)));
        }
        ribbon(body, pts, ll.width > 0 ? ll.width : 0.9f, ll.alpha, ll.color, ll.edge, kLaneLift, true);
    }
    for (size_t bi = 0; bi < bodies_.size(); ++bi) setMesh(dev, bodies_[bi].lanes, mbs[bi]);
}

// ------------------------------------------------------------------ frame

bool RtsPlanetGroundFx::draw(gfx::Device& dev, Renderer3D& r, const PlanetRenderer& planets, const PlanetViewFrame& frame, const RtsScene& s,
                             double time, const std::vector<uint8_t>* vision, const MapGrid& g, const RtsGroundFx& looks, const Rules& rules) {
    const PlanetWorld& W = planets.world();
    const Surface* S = W.sim.get();
    if (!S || g.surf.get() != S) return false;
    bool hi = r.materialQuality() != MaterialQuality::Low;
    if (const char* q = std::getenv("RTS_GROUNDFX")) hi = std::string(q) != "low";
    const size_t n = g.flags.size();
    if (!init_ || S_ != S || bodies_.size() != W.bodies.size() || memR_.size() != n || hi != hi_) {
        shutdown(dev);
        S_ = S;
        hi_ = hi;
        bodies_.assign(W.bodies.size(), Body{});
        for (size_t bi = 0; bi < W.bodies.size(); ++bi)
            if (const PlanetSurface* ps = planets.surface(int(bi))) {
                Body& B = bodies_[bi];
                B.n = ps->faceSize();
                B.tn = (B.n + kTile - 1) / kTile;
                B.tiles.assign(size_t(6 * B.tn * B.tn), Tile{});
            }
        memT_.assign(n, 0); memR_.assign(n, 0); memM_.assign(n, 0); memP_.assign(n, 0);
        memD_.assign(n, 0);
        for (size_t i = 0; i < n && i < g.dig.size(); ++i) memD_[i] = g.dig[i];   // rock is known from the start, like the map
        init_ = true;
        nextScan_ = -1;
    }
    const std::vector<uint8_t>* terr = &g.territory;
    const bool useT = looks.showsTerritory() && (rules.territory.enabled || !rules.rooms.empty()) && terr->size() == n;
    const auto& dl = looks.digLook();
    const bool useD = dl.on && !rules.diggables.empty() && g.dig.size() == n;
    bool rockChanged = false;
    if (time >= nextScan_) { // fog-of-war memory: territory and rock learned by seeing them, roads and own marks known
        nextScan_ = time + 0.25;
        const bool fog = vision && vision->size() == n;
        const uint8_t bit = s.viewer >= 1 && s.viewer <= 8 ? uint8_t(1u << (s.viewer - 1)) : 0xff;
        for (size_t i = 0; i < n; ++i) {
            const int c = int(i);
            const uint8_t rd = (g.flags[i] & kCellRoad) ? 1 : 0;
            if (rd != memR_[i]) { memR_[i] = rd; dirtyCell(c, kDR, true); }
            if (useD) {
                const uint8_t m = g.dig[i] ? uint8_t(g.digMark[i] & bit) : 0;
                if (m != memM_[i]) { memM_[i] = m; dirtyCell(c, kDM, false); }
            }
            if (fog && (*vision)[i] != 2) continue;
            if (useT && (*terr)[i] != memT_[i]) { memT_[i] = (*terr)[i]; dirtyCell(c, kDT, true); }
            if (useD) {
                const uint8_t d = g.dig[i];
                if (d != memD_[i]) { memD_[i] = d; rockChanged = true; dirtyCell(c, kDD | kDT | kDM, true); }
                uint8_t pb = 0;
                if (memM_[i] && d && size_t(d - 1) < rules.diggables.size()) {
                    const Fixed hard = rules.diggables[size_t(d - 1)].hardness;
                    if (hard.raw > 0 && g.digHp[i] < hard) pb = uint8_t(1 + std::clamp(int((1.0 - (g.digHp[i] / hard).toDouble()) * 15.0), 0, 15));
                }
                if (pb != memP_[i]) { memP_[i] = pb; dirtyCell(c, kDM, false); }
            }
        }
    }
    // lanes / map roads: rebuilt when the lane set or the roads change
    if (looks.laneLook().on) {
        uint32_t sig = s.lanes ? s.laneSignature : 0u;
        const size_t count = (s.lanes ? s.lanes->size() : 0) * 131 + s.roads.size();
        for (auto& [a, b] : s.roads) sig = sig * 31u + uint32_t(int(a.x * 4) * 7 + int(a.z * 4) * 13 + int(b.x * 4) * 17 + int(b.z * 4) * 19);
        if (sig != laneSig_ || count != laneCount_) { laneSig_ = sig; laneCount_ = count; buildLanes(dev, planets, s, looks, rules, hi); }
    }
    quads_ = 0;
    const float pulse = hi && looks.territoryLook().pulse ? 0.85f + 0.15f * float(std::sin(time * 2.6)) : 1.0f;
    float gemGlow = 0.0f;
    for (auto& k : dl.kinds) gemGlow = std::max(gemGlow, k.glow);
    gemGlow *= hi ? 0.85f + 0.15f * float(std::sin(time * 2.0)) : 1.0f;
    const bool useR = looks.roadLook().on;
    for (size_t bi = 0; bi < W.bodies.size(); ++bi) {
        Body& B = bodies_[bi];
        if (!planets.surface(int(bi)) || B.tn <= 0) continue;
        const mat4 model = frame.matrix() * mat4::translate(W.center(int(bi), s.seconds));   // body-local -> view world
        for (int f = 0; f < 6; ++f)
            for (int ty = 0; ty < B.tn; ++ty)
                for (int tx = 0; tx < B.tn; ++tx) {
                    Tile& t = B.tiles[size_t((f * B.tn + ty) * B.tn + tx)];
                    if (t.dT) { if (useT) buildTerritory(dev, t, int(bi), f, tx, ty, planets, looks); else t.dT = false; }
                    if (t.dD) { if (useD) buildDig(dev, t, int(bi), f, tx, ty, planets, looks, hi); else t.dD = false; }
                    if (t.dM) { if (useD) buildMarks(dev, t, int(bi), f, tx, ty, planets, g, looks, rules); else t.dM = false; }
                    if (t.dR) { if (useR) buildRoads(dev, t, int(bi), f, tx, ty, planets, looks); else t.dR = false; }
                    if (t.rock.indexCount) r.drawMesh(t.rock, model, {1, 1, 1, 1}, 0, 0.12f);
                    if (t.veins.indexCount) r.drawMesh(t.veins, model, {1, 1, 1, 1}, kDrawNoCastShadow, 0.9f, 0.25f);
                    if (t.gems.indexCount) r.drawMesh(t.gems, model, {1, 1, 1, 1}, kDrawNoCastShadow, 1.0f, gemGlow);
                    if (t.road.indexCount) r.drawMesh(t.road, model, {1, 1, 1, 0.98f}, kDrawNoCastShadow, 0.08f);
                    if (t.fill.indexCount) r.drawMesh(t.fill, model, {1, 1, 1, 0.98f}, kDrawNoCastShadow, 0.05f, 0.05f);
                    if (t.border.indexCount) r.drawMesh(t.border, model, {1, 1, 1, 0.98f}, kDrawNoCastShadow, 0.1f, looks.territoryLook().glow * pulse);
                    if (t.marks.indexCount) r.drawMesh(t.marks, model, {1, 1, 1, 0.98f}, kDrawNoCastShadow, 0.1f, hi ? 0.4f : 0.2f);
                    quads_ += int((t.fill.indexCount + t.border.indexCount + t.road.indexCount + t.rock.indexCount + t.marks.indexCount) / 6);
                }
        if (B.lanes.indexCount) { r.drawMesh(B.lanes, model, {1, 1, 1, 0.98f}, kDrawNoCastShadow, 0.06f); quads_ += int(B.lanes.indexCount / 6); }
    }
    return rockChanged;
}

} // namespace rts::game
