// Minimap: rasterized on the CPU (terrain, fog, units, camera frustum) into a small texture that
// the HUD shows as <img src="#minimap"/>. Cheap enough to redo every frame on weak devices.
#include <algorithm>
#include <cmath>

#include "GameClient.h"

namespace rts::client {

namespace {

constexpr int kTargetSide = 192;  // image pixels on the longer map side

constexpr uint32_t rgba(int r, int g, int b, int a = 255) {
    return uint32_t(r) | uint32_t(g) << 8 | uint32_t(b) << 16 | uint32_t(a) << 24; // bytes in memory: R G B A
}
uint32_t darken(uint32_t c, int num, int den) {
    auto ch = [&](int s) { return int((c >> s) & 255u) * num / den; };
    return rgba(ch(0), ch(8), ch(16));
}

} // namespace

void Minimap::build(gfx::Device& dev, const GameState& gs, const Rules&) {
    release(dev);
    gridW_ = gs.grid.w; gridH_ = gs.grid.h;
    const int side = std::max(1, std::max(gridW_, gridH_));
    const float pxPerCell = side <= kTargetSide ? float(std::max(1, kTargetSide / side)) : float(kTargetSide) / float(side);
    cellsPerPx_ = 1.0f / pxPerCell;
    w_ = std::max(1, int(std::ceil(float(gridW_) * pxPerCell)));
    h_ = std::max(1, int(std::ceil(float(gridH_) * pxPerCell)));
    base_.assign(size_t(w_) * size_t(h_), 0);
    for (int y = 0; y < h_; ++y)
        for (int x = 0; x < w_; ++x) {
            const int cx = std::min(gridW_ - 1, int(float(x) * cellsPerPx_)), cy = std::min(gridH_ - 1, int(float(y) * cellsPerPx_));
            const uint8_t f = gs.grid.flags[size_t(gs.grid.index(cx, cy))];
            uint32_t c = rgba(60, 92, 50);
            if (!(f & kCellGround) && (f & kCellWater)) c = rgba(50, 100, 150);
            else if (!(f & kCellGround)) c = rgba(110, 106, 100);
            base_[size_t(y) * size_t(w_) + size_t(x)] = c;
        }
    pixels_ = base_;
    gfx::TextureDesc td;
    td.width = w_; td.height = h_; td.filter = gfx::Filter::Nearest; td.data = pixels_.data(); td.label = "minimap";
    tex_ = dev.createTexture(td);
}

void Minimap::release(gfx::Device& dev) {
    if (tex_.valid()) dev.destroyTexture(tex_);
    tex_ = {};
}

void Minimap::update(gfx::Device& dev, const GameState& gs, const Rules& rules, const RtsScene& scene, const std::vector<uint8_t>* vision,
                     const float3 (&q)[4], bool haveQuad, const std::vector<OverlayMinimapMark>* marks) {
    if (!tex_.valid() || gs.grid.w != gridW_ || gs.grid.h != gridH_) return;
    const float ppc = 1.0f / cellsPerPx_;
    for (int y = 0; y < h_; ++y)
        for (int x = 0; x < w_; ++x) {
            const size_t i = size_t(y) * size_t(w_) + size_t(x);
            uint32_t c = base_[i];
            if (vision) {
                const int cx = std::min(gridW_ - 1, int(float(x) * cellsPerPx_)), cy = std::min(gridH_ - 1, int(float(y) * cellsPerPx_));
                const uint8_t v = (*vision)[size_t(gs.grid.index(cx, cy))];
                if (v == 0) c = rgba(12, 14, 18);
                else if (v == 1) c = darken(c, 3, 5);
            }
            pixels_[i] = c;
        }
    auto fill = [&](float wx, float wz, float r, uint32_t c) {
        auto span = [&](float lo, float hi, int n, int& a, int& b) {
            a = int(std::floor(lo * ppc));
            b = std::max(a, int(std::ceil(hi * ppc)) - 1);
            a = std::clamp(a, 0, n - 1); b = std::clamp(b, 0, n - 1);
        };
        int x0, x1, y0, y1;
        span(wx - r, wx + r, w_, x0, x1);
        span(wz - r, wz + r, h_, y0, y1);
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) pixels_[size_t(y) * size_t(w_) + size_t(x)] = c;
    };
    for (auto& o : scene.objects) {
        if (!o.visible || o.isProjectile) continue;
        const UnitType& t = rules.types[o.type];
        const bool node = o.owner == 0 && t.resourceNode >= 0;
        if (node && t.footprintW <= 1) continue; // trees: too many dots
        const float4 pc = RtsView::playerColor(o.owner);
        const uint32_t c = node ? rgba(255, 216, 50) : rgba(int(pc.x * 255), int(pc.y * 255), int(pc.z * 255));
        const float r = t.structure ? std::max(1.0f, float(std::max(t.footprintW, t.footprintH)) * 0.5f) : 0.6f;
        fill(o.pos.x, o.pos.z, std::max(r, cellsPerPx_ * 0.75f), c);
    }
    if (marks) // overlays with style.space "minimap" (pings, alerts): rings / discs blended over the image
        for (auto& m : *marks) {
            const float a = float(m.rgba >> 24) / 255.0f;
            const float cx = m.x * ppc, cy = m.z * ppc, r = std::max(1.0f, m.radius * ppc);
            const int x0 = std::max(0, int(cx - r - 1)), x1 = std::min(w_ - 1, int(cx + r + 1));
            const int y0 = std::max(0, int(cy - r - 1)), y1 = std::min(h_ - 1, int(cy + r + 1));
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x) {
                    const float d = std::hypot(x + 0.5f - cx, y + 0.5f - cy);
                    if (m.ring ? std::abs(d - r) > 0.75f : d > r) continue;
                    uint32_t& px = pixels_[size_t(y) * size_t(w_) + size_t(x)];
                    auto mix = [&](int s) { return int(float((px >> s) & 255u) * (1 - a) + float((m.rgba >> s) & 255u) * a); };
                    px = rgba(mix(0), mix(8), mix(16));
                }
        }
    if (haveQuad) { // camera frustum on the ground
        for (int k = 0; k < 4; ++k) {
            const float3 a = q[k], b = q[(k + 1) % 4];
            const int steps = int(std::max(std::abs(b.x - a.x), std::abs(b.z - a.z)) * ppc) + 1;
            for (int s = 0; s <= steps; ++s) {
                const float t = float(s) / float(steps);
                const int x = int((a.x + (b.x - a.x) * t) * ppc), y = int((a.z + (b.z - a.z) * t) * ppc);
                if (x >= 0 && y >= 0 && x < w_ && y < h_) pixels_[size_t(y) * size_t(w_) + size_t(x)] = rgba(255, 255, 255);
            }
        }
    }
    dev.updateTexture(tex_, 0, 0, w_, h_, pixels_.data(), w_ * 4);
}

} // namespace rts::client
