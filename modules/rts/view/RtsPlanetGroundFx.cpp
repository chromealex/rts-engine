#include "RtsPlanetGroundFx.h"
#include "RtsView.h"
#include <algorithm>
#include <cmath>

namespace rts::game {

using namespace render;

namespace {
constexpr float kTerrLift = 0.06f, kRoadLift = 0.08f;
uint32_t c4(float3 c, float a) { return rgbaf({c.x, c.y, c.z, a}); }
void quad(MeshBuilder& b, float3 p0, float3 p1, float3 p2, float3 p3, float3 n, uint32_t c0, uint32_t c1, uint32_t c2, uint32_t c3) {
    const uint32_t i0 = b.addVertex(p0, n, c0), i1 = b.addVertex(p1, n, c1), i2 = b.addVertex(p2, n, c2), i3 = b.addVertex(p3, n, c3);
    if (dot(cross(p1 - p0, p2 - p0), n) >= 0) { b.addTriangle(i0, i1, i2); b.addTriangle(i0, i2, i3); }
    else { b.addTriangle(i0, i2, i1); b.addTriangle(i0, i3, i2); }
}
void quad(MeshBuilder& b, float3 p0, float3 p1, float3 p2, float3 p3, float3 n, uint32_t c) { quad(b, p0, p1, p2, p3, n, c, c, c, c); }
void freeMesh(gfx::Device& dev, Mesh& m) { dev.destroyBuffer(m.vb); dev.destroyBuffer(m.ib); m = {}; }
void setMesh(gfx::Device& dev, Mesh& m, const MeshBuilder& b) { freeMesh(dev, m); if (!b.empty()) m = b.upload(dev); }
} // namespace

void RtsPlanetGroundFx::shutdown(gfx::Device& dev) {
    for (auto& b : bodies_) for (Mesh* m : {&b.fill, &b.border, &b.road}) freeMesh(dev, *m);
    bodies_.clear();
    memT_.clear(); memR_.clear();
    init_ = false;
}

void RtsPlanetGroundFx::build(gfx::Device& dev, int bi, const PlanetRenderer& planets, const MapGrid&, const RtsGroundFx& looks) {
    BodyMesh& bm = bodies_[size_t(bi)];
    bm.dirty = false;
    const PlanetSurface* s = planets.surface(bi);
    const Surface* S = planets.world().sim.get();
    if (!s || !S) return;
    const int N = s->faceSize();
    const auto& tl = looks.territoryLook();
    const auto& rl = looks.roadLook();
    auto ownerColor = [&](int slot) {
        if (slot >= 1 && size_t(slot - 1) < tl.colors.size()) return tl.colors[size_t(slot - 1)];
        const float4 c = RtsView::playerColor(slot);
        return float3{c.x, c.y, c.z};
    };
    // a point of cell (face, x, y) at fractional cell coordinates (fx, fy in 0..1), lifted along the normal
    auto P = [&](int f, int x, int y, float fx, float fy, float lift) {
        const float3 d = s->dirFromFace(f, float(x) + fx, float(y) + fy);
        return s->surfacePoint(d) + d * lift;
    };
    MeshBuilder fb, bb, rb;
    const float bw = std::clamp(tl.border, 0.02f, 0.5f);
    static const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
    for (int f = 0; f < 6; ++f)
        for (int y = 0; y < N; ++y)
            for (int x = 0; x < N; ++x) {
                const int c = S->gridCell(bi, f, x, y);
                const float3 n = s->dirFromFace(f, float(x) + 0.5f, float(y) + 0.5f);
                const int o = memT_.empty() ? 0 : memT_[size_t(c)];
                if (o > 0 && looks.territoryOn()) {
                    const float3 oc = ownerColor(o);
                    if (tl.fill > 0.001f) quad(fb, P(f, x, y, 0, 0, kTerrLift), P(f, x, y, 1, 0, kTerrLift), P(f, x, y, 1, 1, kTerrLift), P(f, x, y, 0, 1, kTerrLift), n, c4(oc, tl.fill));
                    for (int k = 0; k < 4; ++k) { // borders towards another owner, across face seams
                        const int nc = S->offset(c, dx[k], dy[k]);
                        if (nc < 0 || memT_[size_t(nc)] == o) continue;
                        float ax, ay, bx, by, ix, iy;   // edge a-b, inner offset direction (ix, iy)
                        if (k == 0) { ax = 1; ay = 0; bx = 1; by = 1; ix = -bw; iy = 0; }
                        else if (k == 1) { ax = 0; ay = 0; bx = 0; by = 1; ix = bw; iy = 0; }
                        else if (k == 2) { ax = 0; ay = 1; bx = 1; by = 1; ix = 0; iy = -bw; }
                        else { ax = 0; ay = 0; bx = 1; by = 0; ix = 0; iy = bw; }
                        const float3 bright = oc * 0.55f + float3{0.45f, 0.45f, 0.45f};
                        quad(bb, P(f, x, y, ax, ay, kTerrLift + 0.005f), P(f, x, y, bx, by, kTerrLift + 0.005f), P(f, x, y, bx + ix, by + iy, kTerrLift + 0.005f),
                             P(f, x, y, ax + ix, ay + iy, kTerrLift + 0.005f), n, c4(bright, 0.95f), c4(bright, 0.95f), c4(oc, 0.05f), c4(oc, 0.05f));
                    }
                }
                if (!memR_.empty() && memR_[size_t(c)] && rl.on) { // road: centre square + arms to road neighbours
                    const float h = std::clamp(rl.width, 0.2f, 1.0f) * 0.5f;
                    const uint32_t col = c4(rl.color, rl.alpha);
                    quad(rb, P(f, x, y, 0.5f - h, 0.5f - h, kRoadLift), P(f, x, y, 0.5f + h, 0.5f - h, kRoadLift), P(f, x, y, 0.5f + h, 0.5f + h, kRoadLift),
                         P(f, x, y, 0.5f - h, 0.5f + h, kRoadLift), n, col);
                    for (int k = 0; k < 4; ++k) {
                        const int nc = S->offset(c, dx[k], dy[k]);
                        if (nc < 0 || !memR_[size_t(nc)]) continue;
                        if (k == 0) quad(rb, P(f, x, y, 0.5f + h, 0.5f - h, kRoadLift), P(f, x, y, 1, 0.5f - h, kRoadLift), P(f, x, y, 1, 0.5f + h, kRoadLift), P(f, x, y, 0.5f + h, 0.5f + h, kRoadLift), n, col);
                        if (k == 1) quad(rb, P(f, x, y, 0, 0.5f - h, kRoadLift), P(f, x, y, 0.5f - h, 0.5f - h, kRoadLift), P(f, x, y, 0.5f - h, 0.5f + h, kRoadLift), P(f, x, y, 0, 0.5f + h, kRoadLift), n, col);
                        if (k == 2) quad(rb, P(f, x, y, 0.5f - h, 0.5f + h, kRoadLift), P(f, x, y, 0.5f + h, 0.5f + h, kRoadLift), P(f, x, y, 0.5f + h, 1, kRoadLift), P(f, x, y, 0.5f - h, 1, kRoadLift), n, col);
                        if (k == 3) quad(rb, P(f, x, y, 0.5f - h, 0, kRoadLift), P(f, x, y, 0.5f + h, 0, kRoadLift), P(f, x, y, 0.5f + h, 0.5f - h, kRoadLift), P(f, x, y, 0.5f - h, 0.5f - h, kRoadLift), n, col);
                    }
                }
            }
    setMesh(dev, bm.fill, fb);
    setMesh(dev, bm.border, bb);
    setMesh(dev, bm.road, rb);
}

void RtsPlanetGroundFx::draw(gfx::Device& dev, Renderer3D& r, const PlanetRenderer& planets, const PlanetViewFrame& frame, double seconds,
                             double time, const std::vector<uint8_t>* vision, const MapGrid& g, const RtsGroundFx& looks, const Rules& rules,
                             const std::vector<uint8_t>* demo) {
    const PlanetWorld& W = planets.world();
    const Surface* S = W.sim.get();
    if (!S || g.surf.get() != S) return;
    const std::vector<uint8_t>* terr = demo && demo->size() == g.flags.size() ? demo : &g.territory;
    const bool useT = (demo ? looks.territoryOn() : looks.showsTerritory()) && (demo || rules.territory.enabled || !rules.rooms.empty()) && terr->size() == g.flags.size();
    const size_t n = g.flags.size();
    if (!init_ || bodies_.size() != W.bodies.size() || memR_.size() != n) {
        bodies_.assign(W.bodies.size(), BodyMesh{});
        memT_.assign(n, 0);
        memR_.assign(n, 0);
        init_ = true;
        nextScan_ = -1;
    }
    if (time >= nextScan_) { // fog-of-war memory: territory learned by seeing it, roads known
        nextScan_ = time + 0.25;
        const bool fog = vision && vision->size() == n;
        for (size_t i = 0; i < n; ++i) {
            const uint8_t rd = (g.flags[i] & kCellRoad) ? 1 : 0;
            uint8_t t = memT_[i];
            if (useT && (!fog || (*vision)[i] == 2)) t = (*terr)[i];
            if (rd != memR_[i] || t != memT_[i]) {
                memR_[i] = rd; memT_[i] = t;
                const int b = S->bodyOfCell(int(i));
                if (b >= 0 && size_t(b) < bodies_.size()) bodies_[size_t(b)].dirty = true;
            }
        }
    }
    quads_ = 0;
    const float pulse = r.materialQuality() != MaterialQuality::Low && looks.territoryLook().pulse ? 0.85f + 0.15f * float(std::sin(time * 2.6)) : 1.0f;
    for (size_t bi = 0; bi < W.bodies.size(); ++bi) {
        if (!planets.surface(int(bi))) continue;
        BodyMesh& bm = bodies_[bi];
        if (bm.dirty) build(dev, int(bi), planets, g, looks);
        const mat4 model = frame.matrix() * mat4::translate(W.center(int(bi), seconds));   // body-local -> view world
        if (bm.road.indexCount) r.drawMesh(bm.road, model, {1, 1, 1, 0.98f}, kDrawNoCastShadow, 0.08f);
        if (bm.fill.indexCount) r.drawMesh(bm.fill, model, {1, 1, 1, 0.98f}, kDrawNoCastShadow, 0.05f, 0.05f);
        if (bm.border.indexCount) r.drawMesh(bm.border, model, {1, 1, 1, 0.98f}, kDrawNoCastShadow, 0.1f, looks.territoryLook().glow * pulse);
        quads_ += int((bm.fill.indexCount + bm.border.indexCount + bm.road.indexCount) / 6);
    }
}

} // namespace rts::game
