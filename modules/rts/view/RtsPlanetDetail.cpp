// Sub-cell detail grid of a planet surface (docs/terrain.md "Detail surface", planets). Presentation only.
//
// Grid: M = N * k samples per face side, (M+1)^2 per face, indexed like the corners (face, i, j). Sample
// (i, j) lies at face coordinates (i / k, j / k) in cells. Seam samples are the corner duplicates of a
// CubeSphere(M): they get the canonical sample's direction (bit-identical) and one averaged height, so
// the faces meet without cracks at every LOD (the renderer's morph targets are grid samples too).
//
// Heights: explicit (body terrain.detail, resampled bilinearly when the tier draws another k) or
// synthesised: Catmull-Rom through the corners (rows continued across the seams through the coarse
// surface), plus fine 3D value noise (continuous across the seams) scaled per cell kind, plus a crater
// field on moons. Everything is a function of the direction except the Catmull-Rom basis, and the seam
// average removes that last difference.
#include "RtsPlanet.h"
#include <algorithm>
#include <cmath>
#include "../RtsMap.h"
#include "../RtsSphere.h"

namespace rts::game {

using render::float3;

namespace {

uint32_t hash4(int x, int y, int z, uint32_t s) {
    uint32_t h = s * 0x9E3779B1u ^ uint32_t(x) * 0x85EBCA77u ^ uint32_t(y) * 0xC2B2AE3Du ^ uint32_t(z) * 0x27D4EB2Fu;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
    return h;
}
float hf(uint32_t h) { return float(h & 0xffff) * (1.0f / 65535.0f); }

float vnoise3(float3 p, uint32_t s) {
    const int xi = int(std::floor(p.x)), yi = int(std::floor(p.y)), zi = int(std::floor(p.z));
    float fx = p.x - float(xi), fy = p.y - float(yi), fz = p.z - float(zi);
    fx = fx * fx * (3 - 2 * fx); fy = fy * fy * (3 - 2 * fy); fz = fz * fz * (3 - 2 * fz);
    auto v = [&](int a, int b, int c) { return hf(hash4(xi + a, yi + b, zi + c, s)) * 2.0f - 1.0f; };
    auto lx = [&](int b, int c) { return v(0, b, c) + (v(1, b, c) - v(0, b, c)) * fx; };
    const float y0 = lx(0, 0) + (lx(1, 0) - lx(0, 0)) * fy, y1 = lx(0, 1) + (lx(1, 1) - lx(0, 1)) * fy;
    return y0 + (y1 - y0) * fz;
}

// q: point in cell units (dir * cellsPerRadian). Two octaves like the flat fineNoise.
float fineNoise3(float3 q, uint32_t seed) {
    return vnoise3(q * 2.3f, seed + 11) * 0.65f + vnoise3(q * 5.1f, seed + 13) * 0.35f;
}

// Crater field: lattice of `spacing` cells, at most one crater per lattice cell. Returns a height offset
// in units of the cell size (bowl < 0, rim > 0).
float craterField(float3 q, float density, uint32_t seed, float spacing, float rMin, float rMax) {
    const float3 c = q * (1.0f / spacing);
    const int xi = int(std::floor(c.x)), yi = int(std::floor(c.y)), zi = int(std::floor(c.z));
    float h = 0;
    for (int dz = -1; dz <= 1; ++dz)
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                const uint32_t hh = hash4(xi + dx, yi + dy, zi + dz, seed);
                if (hf(hh) > density) continue;
                const uint32_t h2 = hash4(xi + dx, yi + dy, zi + dz, seed + 7);
                const float3 ctr{float(xi + dx) + hf(h2), float(yi + dy) + hf(h2 >> 8 | h2 << 24), float(zi + dz) + hf(h2 >> 16 | h2 << 16)};
                const float r = (rMin + (rMax - rMin) * hf(hh >> 16)) / spacing;   // lattice units
                const float3 dv = c - ctr;
                const float d = std::sqrt(dot(dv, dv)) / r;
                if (d > 1.6f) continue;
                const float rr = r * spacing;   // cells
                // bowl (parabolic) + raised rim, fading out by 1.6 r
                float v = 0;
                if (d < 1.0f) v = (d * d - 1.0f) * 0.22f * rr;
                const float rim = (d - 1.0f) / 0.28f;
                v += 0.07f * rr * std::exp(-rim * rim);
                h = std::min(h, 0.0f) + v;   // overlapping craters: later ones carve the earlier ones
            }
    return h;
}

float kindRoughness(uint8_t f) {
    if (!(f & kCellGround) && (f & kCellWater)) return 0.4f;
    if (!(f & kCellGround)) return 2.6f;
    if (f & kCellRock) return 1.7f;
    if (f & kCellRamp) return 0.5f;
    if (f & kCellWater) return 0.3f;
    if (f & kCellNoBuild) return 0.6f;
    return 1.0f;
}

float catmull(float a, float b, float c, float d, float t) {
    return b + 0.5f * t * (c - a + t * (2 * a - 5 * b + 4 * c - d + t * (3 * (b - c) + d - a)));
}

} // namespace

float PlanetSurface::gridCavity(int face, int i, int j) const {
    if (!k_) return 0.5f;
    const int G = m_;
    const int il = std::max(i - 1, 0), ir = std::min(i + 1, G), jl = std::max(j - 1, 0), jr = std::min(j + 1, G);
    const float lap = 0.25f * (gridHeight(face, il, j) + gridHeight(face, ir, j) + gridHeight(face, i, jl) + gridHeight(face, i, jr)) - gridHeight(face, i, j);
    const float cs = radius_ * 1.5708f / float(G);   // sample spacing (world units)
    return std::clamp(0.5f + lap / cs * 2.5f, 0.0f, 1.0f);
}

void PlanetSurface::initDetail(const PlanetBody& b, const PlanetDetailParams& p) {
    k_ = 0; m_ = 0; ddirs_.clear(); dn_.clear(); dh_.clear(); dcanon_.clear();
    if (p.k <= 0 || heights_.empty()) return;
    dp_ = p;
    const int k = p.k, M = n_ * k, S = (M + 1) * (M + 1);
    CubeSphere cs(M);
    ddirs_.assign(size_t(6 * S), {});
    dh_.assign(size_t(6 * S), 0.0f);
    dcanon_.assign(size_t(6 * S), 0);
    k_ = k; m_ = M;
    const float invK = 1.0f / float(k);
    for (int f = 0; f < 6; ++f)
        for (int j = 0; j <= M; ++j)
            for (int i = 0; i <= M; ++i) {
                const size_t id = size_t(gridIndex(f, i, j));
                const bool edge = i == 0 || j == 0 || i == M || j == M;
                int c = int(id);
                if (edge) { int dup[3]; cs.cornerDuplicates(f, i, j, dup); c = dup[0]; }
                dcanon_[id] = c;
                // corner samples keep the corner grid's (fixed-point derived) direction
                if (i % k == 0 && j % k == 0) ddirs_[id] = cornerDir(f, i / k, j / k);
                else ddirs_[id] = dirFromFace(f, float(i) * invK, float(j) * invK);
            }
    for (size_t id = 0; id < ddirs_.size(); ++id) ddirs_[id] = ddirs_[size_t(dcanon_[id])];
    dn_.assign(ddirs_.size(), {});
    for (int f = 0; f < 6; ++f) synthRect(b, f, 0, 0, M, M);
    for (int f = 0; f < 6; ++f) seamAverage(f, 0, 0, M, M);
    for (int f = 0; f < 6; ++f) normalsRect(f, 0, 0, M, M);
    detailRange();
}

void PlanetSurface::synthRect(const PlanetBody& b, int face, int i0, int j0, int i1, int j1) {
    const int k = k_, M = m_, N = n_;
    i0 = std::max(i0, 0); j0 = std::max(j0, 0); i1 = std::min(i1, M); j1 = std::min(j1, M);
    if (i1 < i0 || j1 < j0) return;
    // explicit heights: copy / resample
    if (b.detailK > 0) {
        const int K = b.detailK, ME = N * K, SE = (ME + 1) * (ME + 1);
        if (b.detail.size() == size_t(6 * SE)) {
            auto E = [&](int x, int y) { return float(b.detail[size_t((face * (ME + 1) + y) * (ME + 1) + x)]) / 16.0f; };
            for (int j = j0; j <= j1; ++j)
                for (int i = i0; i <= i1; ++i) {
                    float h;
                    if (K == k) h = E(i, j);
                    else {
                        const float a = float(i) * float(K) / float(k), c = float(j) * float(K) / float(k);
                        const int x = std::min(int(a), ME - 1), y = std::min(int(c), ME - 1);
                        const float fx = a - float(x), fy = c - float(y);
                        h = (E(x, y) * (1 - fx) + E(x + 1, y) * fx) * (1 - fy) + (E(x, y + 1) * (1 - fx) + E(x + 1, y + 1) * fx) * fy;
                    }
                    dh_[size_t(gridIndex(face, i, j))] = h;
                }
            return;
        }
    }
    // corner rows continued across the seams through the coarse surface
    const int P = N + 3;   // corners -1 .. N+1
    std::vector<float> C(size_t(P) * size_t(P));
    const int cx0 = std::max(-1, i0 / k - 1), cy0 = std::max(-1, j0 / k - 1);
    const int cx1 = std::min(N + 1, i1 / k + 2), cy1 = std::min(N + 1, j1 / k + 2);
    for (int y = cy0; y <= cy1; ++y)
        for (int x = cx0; x <= cx1; ++x) {
            float h;
            if (x >= 0 && y >= 0 && x <= N && y <= N) h = cornerHeight(face, x, y);
            else h = coarseHeightAt(dirFromFace(face, float(x), float(y)));
            C[size_t(y + 1) * size_t(P) + size_t(x + 1)] = h;
        }
    auto CH = [&](int x, int y) { return C[size_t(std::clamp(y, -1, N + 1) + 1) * size_t(P) + size_t(std::clamp(x, -1, N + 1) + 1)]; };
    const float cellsPerRad = float(N) / 1.5708f;
    const float cellSize = radius_ / cellsPerRad;
    const float crater = dp_.craters;
    for (int j = j0; j <= j1; ++j)
        for (int i = i0; i <= i1; ++i) {
            const int X = std::min(i / k, N - 1), Y = std::min(j / k, N - 1);
            const float tx = float(i - X * k) / float(k), tz = float(j - Y * k) / float(k);
            float row[4];
            for (int r = 0; r < 4; ++r)
                row[r] = catmull(CH(X - 1, Y - 1 + r), CH(X, Y - 1 + r), CH(X + 1, Y - 1 + r), CH(X + 2, Y - 1 + r), tx);
            float h = catmull(row[0], row[1], row[2], row[3], tz);
            const size_t id = size_t(gridIndex(face, i, j));
            const float3 q = ddirs_[id] * cellsPerRad;
            h += dp_.roughness * kindRoughness(cellFlags(face, std::min(i / k, N - 1), std::min(j / k, N - 1))) * fineNoise3(q, dp_.seed);
            if (crater > 0)
                h += cellSize * (craterField(q, crater * 0.55f, dp_.seed + 101, 3.0f, 0.5f, 1.4f) +
                                 craterField(q, crater * 0.7f, dp_.seed + 202, 1.1f, 0.18f, 0.45f));
            dh_[id] = h;
        }
}

void PlanetSurface::seamAverage(int face, int i0, int j0, int i1, int j1) {
    const int M = m_;
    const CubeSphere cs(M);
    auto fix = [&](int i, int j) {
        int dup[3];
        const int cnt = cs.cornerDuplicates(face, i, j, dup);
        if (cnt < 2) return;
        float h = 0;
        for (int d = 0; d < cnt; ++d) h += dh_[size_t(dup[d])];
        h /= float(cnt);
        for (int d = 0; d < cnt; ++d) dh_[size_t(dup[d])] = h;
    };
    i0 = std::max(i0, 0); j0 = std::max(j0, 0); i1 = std::min(i1, M); j1 = std::min(j1, M);
    for (int i = i0; i <= i1; ++i) { if (j0 == 0) fix(i, 0); if (j1 == M) fix(i, M); }
    for (int j = j0; j <= j1; ++j) { if (i0 == 0) fix(0, j); if (i1 == M) fix(M, j); }
}

float3 PlanetSurface::sampleNormal(int f, int i, int j) const {
    const int M = m_;
    auto P = [&](int x, int y) -> float3 {
        if (x >= 0 && y >= 0 && x <= M && y <= M) return gridPos(f, x, y);
        const float3 d = dirFromFace(f, float(x) / float(k_), float(y) / float(k_));
        return d * (radius_ + heightAt(d));
    };
    float3 n = normalize(cross(P(i, j + 1) - P(i, j - 1), P(i + 1, j) - P(i - 1, j)));
    const float3& d = ddirs_[size_t(gridIndex(f, i, j))];
    return dot(n, d) < 0 ? -n : n;
}

void PlanetSurface::normalsRect(int face, int i0, int j0, int i1, int j1) {
    const int M = m_;
    i0 = std::max(i0, 0); j0 = std::max(j0, 0); i1 = std::min(i1, M); j1 = std::min(j1, M);
    for (int j = j0; j <= j1; ++j)
        for (int i = i0; i <= i1; ++i) {
            const size_t id = size_t(gridIndex(face, i, j));
            if (size_t(dcanon_[id]) != id) continue;   // duplicates copy their canonical sample below
            dn_[id] = sampleNormal(face, i, j);
        }
    for (int j = j0; j <= j1; ++j)
        for (int i = i0; i <= i1; ++i) {
            if (i != 0 && j != 0 && i != M && j != M) continue;
            const size_t id = size_t(gridIndex(face, i, j));
            const size_t c = size_t(dcanon_[id]);
            if (c != id) {   // the canonical one may lie on a face outside the rect: compute it there
                const int S = (M + 1) * (M + 1), cf = int(c) / S, cj = (int(c) % S) / (M + 1), ci = int(c) % (M + 1);
                dn_[c] = sampleNormal(cf, ci, cj);
                dn_[id] = dn_[c];
            }
        }
}

void PlanetSurface::detailRange() {
    for (float h : dh_) { minH_ = std::min(minH_, h); maxH_ = std::max(maxH_, h); }
}

void PlanetSurface::resynthDetail(const PlanetBody& b, const Rect (&r)[6]) {
    if (!k_) return;
    const int k = k_, g = 2 * k + 1;   // Catmull-Rom reaches two corners
    Rect sr[6];
    for (int f = 0; f < 6; ++f) {
        if (r[f].x1 < r[f].x0) continue;
        sr[f] = {r[f].x0 * k - g, r[f].y0 * k - g, r[f].x1 * k + g, r[f].y1 * k + g};
        synthRect(b, f, sr[f].x0, sr[f].y0, sr[f].x1, sr[f].y1);
    }
    for (int f = 0; f < 6; ++f) if (sr[f].x1 >= sr[f].x0) seamAverage(f, sr[f].x0, sr[f].y0, sr[f].x1, sr[f].y1);
    for (int f = 0; f < 6; ++f) if (sr[f].x1 >= sr[f].x0) normalsRect(f, sr[f].x0 - 1, sr[f].y0 - 1, sr[f].x1 + 1, sr[f].y1 + 1);
    detailRange();
}

bool PlanetSurface::updateDetail(const std::vector<int16_t>& detail, Rect (&sr)[6]) {
    for (auto& r : sr) r = {};
    if (!k_ || detail.size() != dh_.size()) return false;
    const int M = m_;
    bool any = false;
    for (int f = 0; f < 6; ++f)
        for (int j = 0; j <= M; ++j)
            for (int i = 0; i <= M; ++i) {
                const size_t id = size_t(gridIndex(f, i, j));
                const float h = float(detail[id]) / 16.0f;
                if (h == dh_[id]) continue;
                dh_[id] = h;
                Rect& r = sr[f];
                if (r.x1 < r.x0) r = {i, j, i, j};
                else { r.x0 = std::min(r.x0, i); r.y0 = std::min(r.y0, j); r.x1 = std::max(r.x1, i); r.y1 = std::max(r.y1, j); }
                any = true;
            }
    if (!any) return false;
    for (int f = 0; f < 6; ++f) if (sr[f].x1 >= sr[f].x0) seamAverage(f, sr[f].x0, sr[f].y0, sr[f].x1, sr[f].y1);
    for (int f = 0; f < 6; ++f) if (sr[f].x1 >= sr[f].x0) normalsRect(f, sr[f].x0 - 1, sr[f].y0 - 1, sr[f].x1 + 1, sr[f].y1 + 1);
    for (auto& r : sr) if (r.x1 >= r.x0) { r.x0 -= 1; r.y0 -= 1; r.x1 += 1; r.y1 += 1; }
    detailRange();
    return true;
}

} // namespace rts::game
