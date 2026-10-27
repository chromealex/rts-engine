// Terrain mesh: cliff levels as plateaus with vertical rock walls, ramps as slopes between levels,
// fine heights inside a level, cell colours (grass, sand/no-build, rock, lake bed, shallows),
// resource fields and creep tinted into the ground. Rebuilt when the grid changes (editor) or when
// fields/creep change (game, throttled by the caller).
#include <algorithm>
#include <cmath>
#include "../RtsModule.h"
#include "RtsView.h"

namespace rts::game {

using namespace render;

namespace {
constexpr float kHeightUnit = 1.0f / 16.0f;
uint32_t hashCell(int x, int y) { uint32_t h = uint32_t(x) * 73856093u ^ uint32_t(y) * 19349663u; h ^= h >> 13; h *= 0x5bd1e995u; return h ^ (h >> 15); }
} // namespace

float RtsView::heightAt(const MapGrid& g, float x, float z) const {
    if (g.w == 0) return 0;
    int cx = std::clamp(int(std::floor(x)), 0, g.w - 1), cz = std::clamp(int(std::floor(z)), 0, g.h - 1);
    size_t i = size_t(g.index(cx, cz));
    const float lh = rules_ ? float(rules_->levelHeight.toDouble()) : 1.5f;
    float h = float(g.height[i]) * kHeightUnit + float(g.level[i]) * lh;
    if (g.flags[i] & kCellRamp) {
        // ramps: interpolate towards the higher neighbour along the slope
        float fx = x - float(cx), fz = z - float(cz);
        int best = g.level[i];
        float t = 0.5f;
        for (auto [dx, dy] : {std::pair{1, 0}, std::pair{-1, 0}, std::pair{0, 1}, std::pair{0, -1}}) {
            if (!g.inside(cx + dx, cz + dy)) continue;
            int l = g.level[size_t(g.index(cx + dx, cz + dy))];
            if (l > best) { best = l; t = dx > 0 ? fx : dx < 0 ? 1 - fx : dy > 0 ? fz : 1 - fz; }
        }
        h += float(best - g.level[i]) * lh * t;
    }
    return h;
}

void RtsView::buildTerrain(gfx::Device& dev, const MapGrid& g) {
    if (hasTerrain_) {
        dev.destroyBuffer(terrain_.vb); dev.destroyBuffer(terrain_.ib);
        dev.destroyBuffer(water_.vb); dev.destroyBuffer(water_.ib);
    }
    const int W = g.w, H = g.h;
    const float lh = rules_ ? float(rules_->levelHeight.toDouble()) : 1.5f;
    auto inside = [&](int x, int y) { return x >= 0 && y >= 0 && x < W && y < H; };
    auto idx = [&](int x, int y) { return size_t(std::clamp(y, 0, H - 1) * W + std::clamp(x, 0, W - 1)); };
    auto level = [&](int x, int y) { return int(g.level[idx(x, y)]); };
    auto ramp = [&](int x, int y) { return (g.flags[idx(x, y)] & kCellRamp) != 0; };
    // surface height of a cell (without the level): water dips, blocked rock rises
    auto fine = [&](int x, int y) {
        size_t i = idx(x, y);
        float h = float(g.height[i]) * kHeightUnit;
        if (!(g.flags[i] & kCellGround)) h += (g.flags[i] & kCellWater) ? -0.45f : 0.9f;
        return h;
    };
    auto cellColor = [&](int x, int y) -> float3 {
        size_t i = idx(x, y);
        float n = float(hashCell(x, y) & 255) / 255.0f;
        uint8_t f = g.flags[i];
        float3 c;
        if (!(f & kCellGround) && (f & kCellWater)) c = {0.32f, 0.40f, 0.30f};                  // lake bed
        else if (!(f & kCellGround)) c = {0.47f + n * 0.05f, 0.45f + n * 0.05f, 0.42f};          // blocked rock
        else if (f & kCellRock) c = {0.56f + n * 0.04f, 0.50f + n * 0.04f, 0.44f};               // buildable rock plate
        else if (f & kCellRamp) c = {0.55f + n * 0.03f, 0.50f, 0.36f};                           // ramp: packed earth
        else if ((f & kCellWater)) c = {0.42f, 0.55f, 0.50f};                                    // shallows
        else if (f & kCellNoBuild) c = {0.60f + n * 0.04f, 0.52f, 0.38f};                        // path / sand
        else c = {0.33f + n * 0.05f, 0.52f + n * 0.06f, 0.24f + n * 0.03f};                     // grass
        c = c * (1.0f + float(level(x, y)) * 0.05f);                                            // plateaus a bit brighter
        if (g.fieldRes[i] >= 0 && g.fieldAmt[i].raw > 0) {
            float k = std::min(1.0f, float(g.fieldAmt[i].toDouble()) / 60.0f) * (0.55f + n * 0.25f);
            float3 ore = g.fieldRes[i] % 3 == 0 ? float3{0.86f, 0.62f, 0.20f} : g.fieldRes[i] % 3 == 1 ? float3{0.30f, 0.78f, 0.86f} : float3{0.80f, 0.36f, 0.76f};
            c = c * (1 - k) + ore * k;
        }
        if (g.creep[i]) c = c * 0.45f + float3{0.36f, 0.16f, 0.34f} * 0.55f;
        return c;
    };
    // corner height of cell (x,y) at corner (cx,cy) in {0,1}: average over the cells touching that corner
    // which are on the same level (plateaus keep sharp edges); ramps blend both levels into a slope.
    auto corner = [&](int x, int y, int cx, int cy) {
        const int myLevel = level(x, y);
        const bool myRamp = ramp(x, y);
        float sum = 0, cnt = 0;
        int maxNeighbourLevel = myLevel;
        for (int dy = cy - 1; dy <= cy; ++dy)
            for (int dx = cx - 1; dx <= cx; ++dx) {
                int nx = x + dx, ny = y + dy;
                if (!inside(nx, ny)) continue;
                int l = level(nx, ny);
                bool r = ramp(nx, ny);
                if (l == myLevel || (myRamp && r && std::abs(l - myLevel) <= 1)) { sum += fine(nx, ny); cnt += 1; }
                if (!ramp(nx, ny)) maxNeighbourLevel = std::max(maxNeighbourLevel, l);
                (void)r;
            }
        float h = (cnt > 0 ? sum / cnt : fine(x, y)) + float(myLevel) * lh;
        if (myRamp) {
            // a ramp corner touching higher non-ramp ground rises to it
            bool touchesHigher = false;
            for (int dy = cy - 1; dy <= cy; ++dy)
                for (int dx = cx - 1; dx <= cx; ++dx) {
                    int nx = x + dx, ny = y + dy;
                    if (inside(nx, ny) && !ramp(nx, ny) && level(nx, ny) > myLevel) touchesHigher = true;
                }
            if (touchesHigher) h += float(maxNeighbourLevel - myLevel) * lh;
        }
        return h;
    };

    MeshBuilder b;
    std::vector<float> ch(size_t(W * H) * 4); // corner heights per cell: (0,0) (1,0) (0,1) (1,1)
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            float* c = &ch[size_t(y * W + x) * 4];
            c[0] = corner(x, y, 0, 0); c[1] = corner(x, y, 1, 0); c[2] = corner(x, y, 0, 1); c[3] = corner(x, y, 1, 1);
        }
    auto addQuad4 = [&](float3 p0, float3 p1, float3 p2, float3 p3, float3 n, float3 c0, float3 c1, float3 c2, float3 c3) {
        uint32_t base = uint32_t(b.vertexCount());
        b.addVertex(p0, n, rgbaf({c0.x, c0.y, c0.z, 1})); b.addVertex(p1, n, rgbaf({c1.x, c1.y, c1.z, 1}));
        b.addVertex(p2, n, rgbaf({c2.x, c2.y, c2.z, 1})); b.addVertex(p3, n, rgbaf({c3.x, c3.y, c3.z, 1}));
        // front faces satisfy cross(b - a, c - a) . n > 0 (same convention as the ground); fix the winding per triangle
        auto tri = [&](uint32_t i, uint32_t j, uint32_t k, float3 a, float3 bp, float3 cp) {
            if (dot(cross(bp - a, cp - a), n) >= 0) b.addTriangle(i, j, k); else b.addTriangle(i, k, j);
        };
        tri(base, base + 2, base + 1, p0, p2, p1);
        tri(base + 1, base + 2, base + 3, p1, p2, p3);
    };
    auto addQuad = [&](float3 p0, float3 p1, float3 p2, float3 p3, float3 n, float3 col) { addQuad4(p0, p1, p2, p3, n, col, col, col, col); };
    // corner colours blend the cells around the corner that share the level (soft ground, sharp cliffs)
    std::vector<float3> cellCol(size_t(W * H));
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) cellCol[size_t(y * W + x)] = cellColor(x, y);
    auto cornerCol = [&](int x, int y, int cx, int cy) {
        float3 sum{};
        float cnt = 0;
        const int lv = level(x, y);
        for (int dy = cy - 1; dy <= cy; ++dy)
            for (int dx = cx - 1; dx <= cx; ++dx) {
                int nx = x + dx, ny = y + dy;
                if (!inside(nx, ny) || level(nx, ny) != lv) continue;
                sum += cellCol[size_t(ny * W + nx)]; cnt += 1;
            }
        return cnt > 0 ? sum * (1.0f / cnt) : cellCol[size_t(y * W + x)];
    };
    // top surfaces: per-cell quads (separate vertices so cliff edges stay sharp); smooth-ish
    // normals from the corner heights of the cell
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const float* c = &ch[size_t(y * W + x) * 4];
            float3 n = normalize({(c[0] + c[2]) - (c[1] + c[3]), 2.0f, (c[0] + c[1]) - (c[2] + c[3])});
            addQuad4({float(x), c[0], float(y)}, {float(x + 1), c[1], float(y)}, {float(x), c[2], float(y + 1)}, {float(x + 1), c[3], float(y + 1)}, n,
                     cornerCol(x, y, 0, 0), cornerCol(x, y, 1, 0), cornerCol(x, y, 0, 1), cornerCol(x, y, 1, 1));
        }
    // cliff walls where neighbouring edges don't meet
    auto wallColor = [&](int x, int y) {
        float n = float(hashCell(x * 3, y * 7) & 255) / 255.0f;
        return float3{0.42f + n * 0.06f, 0.38f + n * 0.05f, 0.33f + n * 0.04f};
    };
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const float* a = &ch[size_t(y * W + x) * 4];
            if (x + 1 < W) { // east edge: a(1,0),a(1,1) vs b(0,0),b(0,1)
                const float* bb = &ch[size_t(y * W + x + 1) * 4];
                float ta = a[1], tb = a[3], ua = bb[0], ub = bb[2];
                if (std::fabs(ta - ua) > 0.05f || std::fabs(tb - ub) > 0.05f) {
                    bool aHigh = ta + tb > ua + ub;
                    float3 n = aHigh ? float3{1, 0, 0} : float3{-1, 0, 0};
                    float X = float(x + 1);
                    if (aHigh) addQuad({X, ta, float(y)}, {X, tb, float(y + 1)}, {X, ua, float(y)}, {X, ub, float(y + 1)}, n, wallColor(x, y));
                    else addQuad({X, ub, float(y + 1)}, {X, ua, float(y)}, {X, tb, float(y + 1)}, {X, ta, float(y)}, n, wallColor(x + 1, y));
                }
            }
            if (y + 1 < H) { // south edge: a(0,1),a(1,1) vs b(0,0),b(1,0)
                const float* bb = &ch[size_t((y + 1) * W + x) * 4];
                float ta = a[2], tb = a[3], ua = bb[0], ub = bb[1];
                if (std::fabs(ta - ua) > 0.05f || std::fabs(tb - ub) > 0.05f) {
                    bool aHigh = ta + tb > ua + ub;
                    float3 n = aHigh ? float3{0, 0, 1} : float3{0, 0, -1};
                    float Z = float(y + 1);
                    if (aHigh) addQuad({float(x + 1), tb, Z}, {float(x), ta, Z}, {float(x + 1), ub, Z}, {float(x), ua, Z}, n, wallColor(x, y));
                    else addQuad({float(x), ua, Z}, {float(x + 1), ub, Z}, {float(x), ta, Z}, {float(x + 1), tb, Z}, n, wallColor(x, y + 1));
                }
            }
        }
    // apron around the map (ground level) so the edge does not look cut off; skirt down the map border
    const int apron = 12;
    for (int y = -apron; y < H + apron; ++y)
        for (int x = -apron; x < W + apron; ++x) {
            if (x >= 0 && y >= 0 && x < W && y < H) continue;
            float3 c = float3{0.30f, 0.45f, 0.24f}; // flat apron: per-cell noise would show as a checkerboard
            float h = -0.05f;
            addQuad({float(x), h, float(y)}, {float(x + 1), h, float(y)}, {float(x), h, float(y + 1)}, {float(x + 1), h, float(y + 1)}, {0, 1, 0}, c);
        }
    for (int x = 0; x < W; ++x) {
        const float* top = &ch[size_t(x) * 4];
        const float* bot = &ch[size_t((H - 1) * W + x) * 4];
        if (top[0] > 0.0f || top[1] > 0.0f) addQuad({float(x + 1), top[1], 0}, {float(x), top[0], 0}, {float(x + 1), -0.05f, 0}, {float(x), -0.05f, 0}, {0, 0, -1}, wallColor(x, 0));
        if (bot[2] > 0.0f || bot[3] > 0.0f) addQuad({float(x), bot[2], float(H)}, {float(x + 1), bot[3], float(H)}, {float(x), -0.05f, float(H)}, {float(x + 1), -0.05f, float(H)}, {0, 0, 1}, wallColor(x, H - 1));
    }
    for (int y = 0; y < H; ++y) {
        const float* l = &ch[size_t(y * W) * 4];
        const float* r = &ch[size_t(y * W + W - 1) * 4];
        if (l[0] > 0.0f || l[2] > 0.0f) addQuad({0, l[0], float(y)}, {0, l[2], float(y + 1)}, {0, -0.05f, float(y)}, {0, -0.05f, float(y + 1)}, {-1, 0, 0}, wallColor(0, y));
        if (r[1] > 0.0f || r[3] > 0.0f) addQuad({float(W), r[3], float(y + 1)}, {float(W), r[1], float(y)}, {float(W), -0.05f, float(y + 1)}, {float(W), -0.05f, float(y)}, {1, 0, 0}, wallColor(W - 1, y));
    }
    terrain_ = b.upload(dev);
    // water: one translucent sheet slightly below ground level (visible only where the terrain dips)
    MeshBuilder wb;
    wb.color(rgba(255, 255, 255));
    wb.quad({-float(apron), -0.12f, -float(apron)}, {-float(apron), -0.12f, float(H + apron)}, {float(W + apron), -0.12f, float(H + apron)}, {float(W + apron), -0.12f, -float(apron)});
    water_ = wb.upload(dev);
    hasTerrain_ = true;
}

} // namespace rts::game
