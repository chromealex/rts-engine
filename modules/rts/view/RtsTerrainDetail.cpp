// Visual detail surface (see RtsTerrainDetail.h).
#include "RtsTerrainDetail.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include "RtsTerrainSurface.h"

namespace rts::game::terrain {

namespace {
uint32_t hash3(int x, int y, uint32_t s) {
    uint32_t h = uint32_t(x) * 374761393u + uint32_t(y) * 668265263u + s * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}
// smooth value noise in -1..1
float vnoise(float x, float y, uint32_t s) {
    const int xi = int(std::floor(x)), yi = int(std::floor(y));
    float fx = x - float(xi), fy = y - float(yi);
    fx = fx * fx * (3 - 2 * fx); fy = fy * fy * (3 - 2 * fy);
    auto v = [&](int a, int b) { return float(hash3(a, b, s) & 0xffff) * (2.0f / 65535.0f) - 1.0f; };
    const float a = v(xi, yi) + (v(xi + 1, yi) - v(xi, yi)) * fx;
    const float b = v(xi, yi + 1) + (v(xi + 1, yi + 1) - v(xi, yi + 1)) * fx;
    return a + (b - a) * fy;
}
// fine-noise amplitude per cell kind (rock rough, sand / water calm)
float kindRoughness(const MapGrid& g, size_t i, bool flatDig) {
    const uint8_t f = g.flags[i];
    if (!(f & kCellGround) && (f & kCellWater)) return 0.4f;
    if (flatDig && g.dig[i]) return 0.8f;
    if (!(f & kCellGround)) return 2.6f;
    if (f & kCellRock) return 1.7f;
    if (f & kCellRamp) return 0.5f;
    if (f & kCellWater) return 0.3f;
    if (f & kCellNoBuild) return 0.6f;
    return 1.0f;
}
float fineNoise(float x, float z, uint32_t seed) {
    return vnoise(x * 2.3f, z * 2.3f, seed + 11) * 0.65f + vnoise(x * 5.1f, z * 5.1f, seed + 13) * 0.35f;
}
// one separable box blur pass (radius r samples) over the region rows/cols, edges clamped
void boxBlur(std::vector<float>& a, int W, int H, int r, std::vector<float>& tmp) {
    if (r <= 0) return;
    tmp.resize(a.size());
    const float inv = 1.0f / float(2 * r + 1);
    for (int y = 0; y < H; ++y) {
        const float* row = &a[size_t(y) * size_t(W)];
        float s = 0;
        for (int i = -r; i <= r; ++i) s += row[std::clamp(i, 0, W - 1)];
        for (int x = 0; x < W; ++x) {
            tmp[size_t(y) * size_t(W) + size_t(x)] = s * inv;
            s += row[std::min(x + r + 1, W - 1)] - row[std::max(x - r, 0)];
        }
    }
    for (int x = 0; x < W; ++x) {
        float s = 0;
        for (int i = -r; i <= r; ++i) s += tmp[size_t(std::clamp(i, 0, H - 1)) * size_t(W) + size_t(x)];
        for (int y = 0; y < H; ++y) {
            a[size_t(y) * size_t(W) + size_t(x)] = s * inv;
            s += tmp[size_t(std::min(y + r + 1, H - 1)) * size_t(W) + size_t(x)] - tmp[size_t(std::max(y - r, 0)) * size_t(W) + size_t(x)];
        }
    }
}
} // namespace

void DetailSurface::init(int width, int height, int scale) {
    w = width; h = height; k = std::max(1, scale);
    hgt.assign(size_t(sw()) * size_t(sh()), 0.0f);
}

float DetailSurface::at(int i, int j) const {
    i = std::clamp(i, 0, sw() - 1); j = std::clamp(j, 0, sh() - 1);
    return hgt[size_t(j) * size_t(sw()) + size_t(i)];
}

float DetailSurface::sample(float x, float z) const {
    if (!valid()) return 0;
    const float fx = std::clamp(x, 0.0f, float(w)) * float(k), fz = std::clamp(z, 0.0f, float(h)) * float(k);
    const int i = std::min(int(fx), sw() - 2), j = std::min(int(fz), sh() - 2);
    // the mesh's triangles: diagonal (0,0)-(1,1) in every sample quad
    return quadSurface(at(i, j), at(i + 1, j), at(i, j + 1), at(i + 1, j + 1), fx - float(i), fz - float(j));
}

int detailScaleFor(int w, int h, int want, size_t maxSamples) {
    int k = 1;
    while (k * 2 <= std::max(want, 1) && size_t(w * k * 2 + 1) * size_t(h * k * 2 + 1) <= maxSamples) k *= 2;
    return k;
}

int synthMargin(const DetailParams& p, int k) {
    return int(std::ceil((p.wobble * 1.5f + p.bevel * 3.0f + 1.0f) * float(k))) + 2;
}

// Levels: the classic per-cell surface (plateaus, ramps, water dips, rock bumps) looked up through a
// smooth domain warp (organic outlines), blurred with three box passes (~Gaussian: bevelled cliffs,
// rounded plateau corners, ramps blended into both levels), then fine noise per cell kind.
void synthLevels(const MapGrid& g, float lh, bool flatDig, const DetailParams& p, DetailSurface& out, int i0, int j0, int i1, int j1) {
    const int k = out.k, SW = out.sw(), SH = out.sh();
    if (i1 <= i0 || j1 <= j0) { i0 = 0; j0 = 0; i1 = SW; j1 = SH; }
    const int r = std::max(0, int(std::lround(p.bevel * float(k) * 0.6f)));
    const int pad = 3 * r + 1;
    const int x0 = std::max(0, i0 - pad), y0 = std::max(0, j0 - pad), x1 = std::min(SW, i1 + pad), y1 = std::min(SH, j1 + pad);
    const int RW = x1 - x0, RH = y1 - y0;
    std::vector<float> a(size_t(RW) * size_t(RH)), rough(size_t(RW) * size_t(RH)), tmp;
    const float inv = 1.0f / float(k);
    for (int y = 0; y < RH; ++y)
        for (int x = 0; x < RW; ++x) {
            const float wx = float(x0 + x) * inv, wz = float(y0 + y) * inv;
            float qx = wx, qz = wz;
            if (p.wobble > 0) {
                qx += p.wobble * (vnoise(wx * 0.55f, wz * 0.55f, p.seed + 1) * 0.75f + vnoise(wx * 1.7f, wz * 1.7f, p.seed + 3) * 0.25f);
                qz += p.wobble * (vnoise(wx * 0.55f, wz * 0.55f, p.seed + 2) * 0.75f + vnoise(wx * 1.7f, wz * 1.7f, p.seed + 4) * 0.25f);
            }
            qx = std::clamp(qx, 0.0f, float(g.w) - 1e-4f); qz = std::clamp(qz, 0.0f, float(g.h) - 1e-4f);
            const int cx = int(qx), cz = int(qz);
            const size_t ci = size_t(g.index(cx, cz));
            a[size_t(y) * size_t(RW) + size_t(x)] = levelsSurface(g, lh, qx, qz) - float(g.height[ci]) * kHeightUnit + fineHeight(g, cx, cz, flatDig);
            rough[size_t(y) * size_t(RW) + size_t(x)] = kindRoughness(g, ci, flatDig);
        }
    for (int pass = 0; pass < 3; ++pass) boxBlur(a, RW, RH, r, tmp);
    boxBlur(rough, RW, RH, std::max(1, k / 2), tmp);
    for (int j = j0; j < j1; ++j)
        for (int i = i0; i < i1; ++i) {
            const size_t li = size_t(j - y0) * size_t(RW) + size_t(i - x0);
            const float wx = float(i) * inv, wz = float(j) * inv;
            out.hgt[size_t(j) * size_t(SW) + size_t(i)] = a[li] + p.noise * rough[li] * fineNoise(wx, wz, p.seed);
        }
}

// Heightfield: Catmull-Rom bicubic through the corners (passes through every corner: the simulation's
// surface at the corners is kept) + fine noise.
void synthCorners(const MapGrid& g, const DetailParams& p, DetailSurface& out, int i0, int j0, int i1, int j1) {
    const int k = out.k, SW = out.sw(), SH = out.sh();
    if (i1 <= i0 || j1 <= j0) { i0 = 0; j0 = 0; i1 = SW; j1 = SH; }
    auto C = [&](int X, int Y) { return cornerHeight(g, X, Y); };
    auto cr = [](float a, float b, float c, float d, float t) {
        return b + 0.5f * t * (c - a + t * (2 * a - 5 * b + 4 * c - d + t * (3 * (b - c) + d - a)));
    };
    for (int j = j0; j < j1; ++j)
        for (int i = i0; i < i1; ++i) {
            const int X = std::min(i / k, g.w - 1), Y = std::min(j / k, g.h - 1);
            const float tx = float(i - X * k) / float(k), tz = float(j - Y * k) / float(k);
            float row[4];
            for (int r = 0; r < 4; ++r)
                row[r] = cr(C(X - 1, Y - 1 + r), C(X, Y - 1 + r), C(X + 1, Y - 1 + r), C(X + 2, Y - 1 + r), tx);
            float hv = cr(row[0], row[1], row[2], row[3], tz);
            const int cx = std::min(int(float(i) / float(k)), g.w - 1), cz = std::min(int(float(j) / float(k)), g.h - 1);
            hv += p.noise * kindRoughness(g, size_t(g.index(cx, cz)), false) * fineNoise(float(i) / float(k), float(j) / float(k), p.seed);
            out.hgt[size_t(j) * size_t(SW) + size_t(i)] = hv;
        }
}

bool decodeDetail(const std::string& bytes, int w, int h, int k, DetailSurface& out, std::string* err) {
    if (k < 1 || k > 16 || w <= 0 || h <= 0) { if (err) *err = "terrain.detail: scale must be 1..16"; return false; }
    const size_t n = size_t(w * k + 1) * size_t(h * k + 1);
    if (bytes.size() != n * 2) {
        if (err) *err = "terrain.detail: expected " + std::to_string(n * 2) + " bytes ((w*scale+1) x (h*scale+1) int16), got " + std::to_string(bytes.size());
        return false;
    }
    out.init(w, h, k);
    const auto* b = reinterpret_cast<const unsigned char*>(bytes.data());
    for (size_t i = 0; i < n; ++i) out.hgt[i] = float(int16_t(uint16_t(b[2 * i] | (b[2 * i + 1] << 8)))) * kHeightUnit;
    return true;
}

std::string encodeDetail(const DetailSurface& d) {
    std::string s(d.hgt.size() * 2, '\0');
    for (size_t i = 0; i < d.hgt.size(); ++i) {
        const int v = int(std::clamp(std::lround(d.hgt[i] * 16.0f), -32768l, 32767l));
        s[2 * i] = char(v & 255); s[2 * i + 1] = char((v >> 8) & 255);
    }
    return s;
}

DetailSurface resampleDetail(const DetailSurface& d, int k) {
    DetailSurface o;
    o.init(d.w, d.h, k);
    for (int j = 0; j < o.sh(); ++j)
        for (int i = 0; i < o.sw(); ++i) {
            const float x = float(i) / float(k), z = float(j) / float(k);
            const float fx = x * float(d.k), fz = z * float(d.k);
            const int a = std::min(int(fx), d.sw() - 2), b = std::min(int(fz), d.sh() - 2);
            const float u = fx - float(a), v = fz - float(b);
            o.hgt[size_t(j) * size_t(o.sw()) + size_t(i)] = (d.at(a, b) * (1 - u) + d.at(a + 1, b) * u) * (1 - v) + (d.at(a, b + 1) * (1 - u) + d.at(a + 1, b + 1) * u) * v;
        }
    return o;
}

} // namespace rts::game::terrain
