// Visual terrain surface: the one definition of "where the ground is" for the view (terrain mesh,
// RtsView::heightAt) and the editor core (sculpt tools start from the surface the player sees).
// Header-only and depends on MapGrid only, so apps/editor/core can use it without the renderer.
// Presentation math in float: never used by the simulation (see docs/terrain.md).
//
//   levels mode (no corners):  per cell  heights/16 + level * levelHeight, ramps slope towards the
//                              higher neighbour; the mesh averages corners over same-level cells so
//                              plateaus keep sharp cliff walls.
//   heightfield (corners):     corner heights (w+1)*(h+1), two triangles per cell with the diagonal
//                              from corner (0,0) to (1,1) - exactly the triangles of the mesh (also of
//                              the coarser LODs: CDLOD geomorphing collapses onto that diagonal).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include "../RtsMap.h"

namespace rts::game::terrain {

constexpr float kHeightUnit = 1.0f / 16.0f;

inline size_t cellIndexClamped(const MapGrid& g, int x, int y) {
    return size_t(std::clamp(y, 0, g.h - 1) * g.w + std::clamp(x, 0, g.w - 1));
}

// Surface of a cell without its level: water dips, blocked rock rises (not diggable rock when the
// ground overlays draw it as blocks: flatDig).
inline float fineHeight(const MapGrid& g, int x, int y, bool flatDig = false) {
    size_t i = cellIndexClamped(g, x, y);
    float h = float(g.height[i]) * kHeightUnit;
    if (!(g.flags[i] & kCellGround) && !(flatDig && g.dig[i])) h += (g.flags[i] & kCellWater) ? -0.45f : 0.9f;
    return h;
}

// Levels mode: height of cell (x,y) at its corner (cx,cy) in {0,1}. Averages the cells touching the
// corner that are on the same level (sharp plateau edges); ramps blend into a slope.
inline float cellCorner(const MapGrid& g, float lh, int x, int y, int cx, int cy, bool flatDig = false) {
    auto level = [&](int ax, int ay) { return int(g.level[cellIndexClamped(g, ax, ay)]); };
    auto ramp = [&](int ax, int ay) { return (g.flags[cellIndexClamped(g, ax, ay)] & kCellRamp) != 0; };
    const int myLevel = level(x, y);
    const bool myRamp = ramp(x, y);
    float sum = 0, cnt = 0;
    int maxNeighbourLevel = myLevel;
    bool touchesHigher = false;
    for (int dy = cy - 1; dy <= cy; ++dy)
        for (int dx = cx - 1; dx <= cx; ++dx) {
            int nx = x + dx, ny = y + dy;
            if (!g.inside(nx, ny)) continue;
            int l = level(nx, ny);
            bool r = ramp(nx, ny);
            if (l == myLevel || (myRamp && r && std::abs(l - myLevel) <= 1)) { sum += fineHeight(g, nx, ny, flatDig); cnt += 1; }
            if (!r) maxNeighbourLevel = std::max(maxNeighbourLevel, l);
            if (!r && l > myLevel) touchesHigher = true;
        }
    float h = (cnt > 0 ? sum / cnt : fineHeight(g, x, y, flatDig)) + float(myLevel) * lh;
    if (myRamp && touchesHigher) h += float(maxNeighbourLevel - myLevel) * lh; // a ramp corner touching higher ground rises to it
    return h;
}

// Heightfield corner (X,Y), X in 0..w, Y in 0..h (clamped).
inline float cornerHeight(const MapGrid& g, int X, int Y) {
    X = std::clamp(X, 0, g.w); Y = std::clamp(Y, 0, g.h);
    return float(g.corner[size_t(g.cornerIndex(X, Y))]) * kHeightUnit;
}

// One height per grid corner: the heightfield corner, or (levels mode) the average of the corners
// of the cells touching it. Coarse LODs, water rims and "sculpt from the current surface" use it.
inline float gridCorner(const MapGrid& g, float lh, int X, int Y, bool flatDig = false) {
    if (g.hasCorners()) return cornerHeight(g, X, Y);
    float s = 0, n = 0;
    for (int y = Y - 1; y <= Y; ++y)
        for (int x = X - 1; x <= X; ++x)
            if (g.inside(x, y)) { s += cellCorner(g, lh, x, y, X - x, Y - y, flatDig); n += 1; }
    return n > 0 ? s / n : 0.0f;
}

// Triangle interpolation inside one quad with corners h00 (x0,z0), h10, h01, h11; fx, fz in 0..1.
// Diagonal (0,0)-(1,1).
inline float quadSurface(float h00, float h10, float h01, float h11, float fx, float fz) {
    if (fx >= fz) return h00 + (h10 - h00) * fx + (h11 - h10) * fz;
    return h00 + (h11 - h01) * fx + (h01 - h00) * fz;
}

inline float cornerSurface(const MapGrid& g, float x, float z) {
    x = std::clamp(x, 0.0f, float(g.w)); z = std::clamp(z, 0.0f, float(g.h));
    int cx = std::min(int(std::floor(x)), g.w - 1), cz = std::min(int(std::floor(z)), g.h - 1);
    return quadSurface(cornerHeight(g, cx, cz), cornerHeight(g, cx + 1, cz), cornerHeight(g, cx, cz + 1), cornerHeight(g, cx + 1, cz + 1),
                       x - float(cx), z - float(cz));
}

// Levels mode: flat per cell (plus fine height), ramps interpolate towards the higher neighbour.
inline float levelsSurface(const MapGrid& g, float lh, float x, float z) {
    int cx = std::clamp(int(std::floor(x)), 0, g.w - 1), cz = std::clamp(int(std::floor(z)), 0, g.h - 1);
    size_t i = size_t(g.index(cx, cz));
    float h = float(g.height[i]) * kHeightUnit + float(g.level[i]) * lh;
    if (g.flags[i] & kCellRamp) {
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

inline float heightAt(const MapGrid& g, float lh, float x, float z) {
    if (g.w == 0) return 0;
    return g.hasCorners() ? cornerSurface(g, x, z) : levelsSurface(g, lh, x, z);
}

} // namespace rts::game::terrain
