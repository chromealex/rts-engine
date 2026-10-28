// Minimap on planet maps: an orthographic globe of the focused body centred on the camera focus (screen up =
// camera forward), lit by the star, with fog of war, units and the view footprint; at system zoom the
// panel shows the star system from above (orbits, bodies; a click focuses a body). CPU raster like the
// flat minimap; the cell lookup per pixel is cached while the camera does not turn.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "GameClient.h"

namespace rts::client {

namespace {
constexpr int kSide = 192;
constexpr uint32_t px(int r, int g, int b, int a = 255) { return uint32_t(r) | uint32_t(g) << 8 | uint32_t(b) << 16 | uint32_t(a) << 24; }
uint32_t scale(uint32_t c, float k) {
    auto ch = [&](int s) { return std::clamp(int(float((c >> s) & 255u) * k), 0, 255); };
    return px(ch(0), ch(8), ch(16));
}
// globe frame: centre direction d, screen up f (tangent), screen right r
struct Globe { float3 d, f, r; };
Globe globeOf(const PlanetCamera& pc) { const float3 d = pc.dir(), f = pc.forward(); return {d, f, cross(f, d)}; }
} // namespace

static void dumpMinimap(const std::vector<uint32_t>& p, int w, int h);

void Minimap::buildPlanet(gfx::Device& dev, const RtsView& view) {
    release(dev);
    w_ = h_ = kSide;
    gridW_ = gridH_ = 0;
    pixels_.assign(size_t(kSide * kSide), px(0, 0, 0, 0));
    base_ = pixels_;
    body_ = -1;
    pixelCell_.clear();
    gfx::TextureDesc td;
    td.width = w_; td.height = h_; td.filter = gfx::Filter::Linear; td.data = pixels_.data(); td.label = "minimap";
    tex_ = dev.createTexture(td);
    (void)view;
}

void Minimap::updatePlanet(gfx::Device& dev, const RtsView& view, const RtsScene& scene, const Rules& rules, bool fog) {
    if (!tex_.valid()) return;
    const PlanetCamera& pc = view.planetCamera();
    const PlanetWorld& W = view.planets().world();
    const double t = view.planetSeconds();
    system_ = pc.systemBlend() > 0.5f;
    std::fill(pixels_.begin(), pixels_.end(), px(6, 8, 14, 230));
    auto put = [&](int x, int y, uint32_t c) { if (x >= 0 && y >= 0 && x < w_ && y < h_) pixels_[size_t(y * w_ + x)] = c; };
    auto disc = [&](float cx, float cy, float r, uint32_t c) {
        for (int y = int(cy - r); y <= int(cy + r + 1); ++y)
            for (int x = int(cx - r); x <= int(cx + r + 1); ++x)
                if ((float(x) + 0.5f - cx) * (float(x) + 0.5f - cx) + (float(y) + 0.5f - cy) * (float(y) + 0.5f - cy) <= r * r) put(x, y, c);
    };
    if (system_) { // star system from above (orbital plane XZ), scaled to the farthest body
        int root = 0;
        for (size_t i = 0; i < W.bodies.size(); ++i) if (W.bodies[i].parent < 0) { root = int(i); break; }
        const float3 c0 = W.center(root, t);
        float ext = 1;
        for (size_t i = 0; i < W.bodies.size(); ++i) ext = std::max(ext, length(W.center(int(i), t) - c0) + W.bodies[i].radius);
        const float k = (kSide * 0.46f) / ext;
        auto toPx = [&](float3 p) { return float2{kSide * 0.5f + (p.x - c0.x) * k, kSide * 0.5f + (p.z - c0.z) * k}; };
        for (size_t i = 0; i < W.bodies.size(); ++i) { // orbits
            const PlanetBody& b = W.bodies[i];
            if (b.parent < 0) continue;
            const float2 pc2 = toPx(W.center(b.parent, t));
            const int seg = std::max(24, int(b.orbitRadius * k * 6));
            for (int s = 0; s < seg; ++s) {
                const float a = 6.2831853f * float(s) / float(seg);
                put(int(pc2.x + std::cos(a) * b.orbitRadius * k), int(pc2.y + std::sin(a) * b.orbitRadius * k), px(70, 90, 130));
            }
        }
        for (size_t i = 0; i < W.bodies.size(); ++i) {
            const PlanetBody& b = W.bodies[i];
            const float2 p = toPx(W.center(int(i), t));
            const uint32_t c = b.star() ? px(255, 220, 140) : b.kind == PlanetBody::Kind::Moon ? px(170, 170, 170) : b.atmosphere ? px(90, 150, 230) : px(190, 140, 110);
            disc(p.x, p.y, std::max(2.0f, b.radius * k), c);
            if (int(i) == pc.body()) { // focused body: ring
                const float r = std::max(2.0f, b.radius * k) + 3;
                for (int s = 0; s < 40; ++s) { const float a = 6.2831853f * float(s) / 40.0f; put(int(p.x + std::cos(a) * r), int(p.y + std::sin(a) * r), px(255, 255, 255)); }
            }
        }
        dev.updateTexture(tex_, 0, 0, w_, h_, pixels_.data(), w_ * 4);
        dumpMinimap(pixels_, w_, h_);
        return;
    }
    const int body = pc.body();
    const PlanetSurface* s = view.planets().surface(body);
    if (!s) { dev.updateTexture(tex_, 0, 0, w_, h_, pixels_.data(), w_ * 4); return; }
    const PlanetBody& pb = W.bodies[size_t(body)];
    const int N = s->faceSize();
    if (body != body_) { // per cell colour: kind + height
        body_ = body;
        cellColor_.assign(size_t(6 * N * N), 0);
        const float range = std::max(s->maxHeight() - std::min(s->minHeight(), pb.seaLevel > -1e8f ? pb.seaLevel : s->minHeight()), 0.5f);
        for (int f = 0; f < 6; ++f)
            for (int y = 0; y < N; ++y)
                for (int x = 0; x < N; ++x) {
                    const float h = 0.25f * (s->cornerHeight(f, x, y) + s->cornerHeight(f, x + 1, y) + s->cornerHeight(f, x, y + 1) + s->cornerHeight(f, x + 1, y + 1));
                    const uint8_t fl = s->cellFlags(f, x, y);
                    uint32_t c = px(70, 110, 55);
                    if (pb.seaLevel > -1e8f && h < pb.seaLevel) c = px(40, 85, 140);
                    else if (!(fl & kCellGround) && (fl & kCellWater)) c = px(50, 100, 150);
                    else if (!(fl & kCellGround)) c = px(120, 114, 106);
                    else if (fl & kCellRock) c = px(140, 126, 110);
                    else if (fl & kCellNoBuild) c = px(160, 140, 100);
                    if (pb.kind == PlanetBody::Kind::Moon) c = px(150, 150, 148);
                    c = scale(c, 0.8f + 0.4f * std::clamp((h - s->minHeight()) / range, 0.0f, 1.0f));
                    cellColor_[size_t((f * N + y) * N + x)] = c;
                }
        pixelCell_.clear();
    }
    const Globe g = globeOf(pc);
    const int star = W.starOf(body);
    const float3 sun = star >= 0 && star != body ? normalize(W.center(star, t) - W.center(body, t)) : float3{0, 1, 0};
    if (pixelCell_.empty() || dot(g.d, lastDir_) < 0.99999f || dot(g.f, lastFwd_) < 0.99999f) {
        lastDir_ = g.d; lastFwd_ = g.f;
        pixelCell_.assign(size_t(w_ * h_), -1);
        pixelShade_.assign(size_t(w_ * h_), 0);
        const float R = kSide * 0.48f;
        for (int y = 0; y < h_; ++y)
            for (int x = 0; x < w_; ++x) {
                const float u = (float(x) + 0.5f - kSide * 0.5f) / R, v = (kSide * 0.5f - float(y) - 0.5f) / R, rr = u * u + v * v;
                if (rr >= 1) continue;
                const float3 d = g.r * u + g.f * v + g.d * std::sqrt(1 - rr);
                int f; float a, b;
                s->faceFromDir(d, f, a, b);
                const int cx = std::min(int(a), N - 1), cy = std::min(int(b), N - 1);
                pixelCell_[size_t(y * w_ + x)] = (f * N + cy) * N + cx;
                pixelShade_[size_t(y * w_ + x)] = 0.25f + 0.75f * std::clamp(dot(d, sun) * 1.5f + 0.35f, 0.0f, 1.0f);
            }
    }
    const std::vector<uint8_t>& vis = view.planetVision(body);
    const bool useFog = fog && vis.size() == size_t(6 * N * N);
    for (size_t i = 0; i < pixelCell_.size(); ++i) {
        const int c = pixelCell_[i];
        if (c < 0) continue;
        uint32_t col = scale(cellColor_[size_t(c)], pixelShade_[i]);
        if (useFog) { const uint8_t v = vis[size_t(c)]; if (v < 64) col = px(12, 14, 18); else if (v < 192) col = scale(col, 0.6f); }
        pixels_[i] = col;
    }
    const float R = kSide * 0.48f;
    auto project = [&](float3 d, float2& out) {
        d = normalize(d);
        if (dot(d, g.d) < 0.02f) return false;
        out = {kSide * 0.5f + dot(d, g.r) * R, kSide * 0.5f - dot(d, g.f) * R};
        return true;
    };
    for (auto& o : scene.objects) {
        if (!o.visible || o.isProjectile) continue;
        int b; float3 local;
        if (!view.objectOnBody(o, b, local) || b != body) continue;
        const UnitType& ut = rules.types[o.type];
        const bool node = o.owner == 0 && ut.resourceNode >= 0;
        if (node && ut.footprintW <= 1) continue;
        float2 p;
        if (!project(local, p)) continue;
        const float4 c = RtsView::playerColor(o.owner);
        disc(p.x, p.y, ut.structure ? 2.0f : 1.2f, node ? px(255, 216, 50) : px(int(c.x * 255), int(c.y * 255), int(c.z * 255)));
    }
    { // view footprint: a square around the focus, sized by the camera distance
        const float half = std::clamp(pc.distance * 0.6f / std::max(pb.radius, 1.0f) * R, 2.0f, R);
        const float cx = kSide * 0.5f, cy = kSide * 0.5f;
        for (int i = -int(half); i <= int(half); ++i) {
            put(int(cx) + i, int(cy - half), px(255, 255, 255)); put(int(cx) + i, int(cy + half), px(255, 255, 255));
            put(int(cx - half), int(cy) + i, px(255, 255, 255)); put(int(cx + half), int(cy) + i, px(255, 255, 255));
        }
    }
    dev.updateTexture(tex_, 0, 0, w_, h_, pixels_.data(), w_ * 4);
    dumpMinimap(pixels_, w_, h_);
}

// RTS_MINIMAP_DUMP=file.ppm: writes the planet minimap image every 60 updates (headless checks: the HUD is not in screenshots)
static void dumpMinimap(const std::vector<uint32_t>& p, int w, int h) {
    static const char* path = std::getenv("RTS_MINIMAP_DUMP");
    static int n = 0;
    if (!path || ++n % 60) return;
    if (FILE* f = std::fopen(path, "wb")) {
        std::fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (uint32_t c : p) { const unsigned char rgb[3] = {uint8_t(c), uint8_t(c >> 8), uint8_t(c >> 16)}; std::fwrite(rgb, 1, 3, f); }
        std::fclose(f);
    }
}

bool Minimap::planetPointer(RtsView& view, float u, float v) const {
    PlanetCamera& pc = view.planetCamera();
    const PlanetWorld& W = view.planets().world();
    if (system_) { // nearest body with a surface
        const double t = view.planetSeconds();
        int root = 0;
        for (size_t i = 0; i < W.bodies.size(); ++i) if (W.bodies[i].parent < 0) { root = int(i); break; }
        const float3 c0 = W.center(root, t);
        float ext = 1;
        for (size_t i = 0; i < W.bodies.size(); ++i) ext = std::max(ext, length(W.center(int(i), t) - c0) + W.bodies[i].radius);
        const float k = (kSide * 0.46f) / ext;
        int best = -1; float bd = 1e9f;
        for (size_t i = 0; i < W.bodies.size(); ++i) {
            if (!W.bodies[i].surface()) continue;
            const float3 p = W.center(int(i), t);
            const float dx = kSide * 0.5f + (p.x - c0.x) * k - u * kSide, dy = kSide * 0.5f + (p.z - c0.z) * k - v * kSide;
            const float d = dx * dx + dy * dy;
            if (d < bd) { bd = d; best = int(i); }
        }
        if (best < 0) return false;
        pc.focus(best);
        pc.distance = std::min(pc.distance, W.bodies[size_t(best)].radius * 3.0f);
        return true;
    }
    const Globe g = globeOf(pc);
    const float R = 0.48f;
    const float x = (u - 0.5f) / R, y = (0.5f - v) / R, rr = x * x + y * y;
    if (rr >= 1) return false;
    pc.setDir(g.r * x + g.f * y + g.d * std::sqrt(1 - rr));
    return true;
}

} // namespace rts::client
