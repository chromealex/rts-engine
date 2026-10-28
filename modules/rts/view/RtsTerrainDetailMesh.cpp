// Detail mode of the terrain renderer: the surface comes from a DetailSurface (k samples per cell;
// map file or synthesised, RtsTerrainDetail.h) instead of the cell grid. Per chunk, CDLOD levels with
// steps of 2^L detail samples are built lazily when first drawn (LRU-evicted over a memory budget),
// geomorphing to the next coarser level in the vertex shader as in the heightfield path. Lighting uses
// a per-pixel normal texture at detail resolution (all tiers; b = cavity, a = macro variation) and the
// splat weights are stored at detail resolution too (soft, warped borders between cell kinds).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "../../../engine/platform/Platform.h"
#include "RtsTerrainRender.h"
#include "RtsTerrainSurface.h"

namespace rts::game {

using namespace render;

namespace {
constexpr int kChunk = 32;
constexpr float kApronY = -0.05f;
// texture side cap: desktop 4096 (2 RGBA8 + mips ~170 MB at 1024^2 k=4), mobile 2048 (~45 MB; normals cover 1-2 samples per texel)
int maxTexSide() { return platform::isTouchDevice() ? 2048 : 4096; }
uint32_t hashI(int x, int y, uint32_t s) {
    uint32_t h = uint32_t(x) * 374761393u + uint32_t(y) * 668265263u + s * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}
float vnoise(float x, float y, uint32_t s) {
    const int xi = int(std::floor(x)), yi = int(std::floor(y));
    float fx = x - float(xi), fy = y - float(yi);
    fx = fx * fx * (3 - 2 * fx); fy = fy * fy * (3 - 2 * fy);
    auto v = [&](int a, int b) { return float(hashI(a, b, s) & 0xffff) * (2.0f / 65535.0f) - 1.0f; };
    const float a = v(xi, yi) + (v(xi + 1, yi) - v(xi, yi)) * fx;
    const float b = v(xi, yi + 1) + (v(xi + 1, yi + 1) - v(xi, yi + 1)) * fx;
    return a + (b - a) * fy;
}
} // namespace

void TerrainRenderer::setDetail(const terrain::DetailSurface* d, const std::vector<uint8_t>* splat) {
    const bool has = d && d->valid();
    std::vector<uint8_t> sp = splat ? *splat : std::vector<uint8_t>{};
    // same size as the drawn surface: only the changed samples are rebuilt (editor sculpting)
    if (has && hasFileDetail_ && !fileDetailChanged_ && d->w == fileDetail_.w && d->h == fileDetail_.h && d->k == fileDetail_.k && sp.size() == fileSplat_.size()) {
        const int SW = d->sw(), SH = d->sh();
        int i0 = SW, j0 = SH, i1 = -1, j1 = -1;
        for (int j = 0; j < SH; ++j)
            for (int i = 0; i < SW; ++i) {
                const size_t n = size_t(j) * size_t(SW) + size_t(i);
                if (d->hgt[n] == fileDetail_.hgt[n] && (sp.empty() || std::memcmp(&sp[n * 4], &fileSplat_[n * 4], 4) == 0)) continue;
                i0 = std::min(i0, i); j0 = std::min(j0, j); i1 = std::max(i1, i); j1 = std::max(j1, j);
            }
        if (i1 >= 0) {
            fileDetail_.hgt = d->hgt; fileSplat_ = std::move(sp);
            dirtyI0_ = std::min(dirtyI0_, i0); dirtyJ0_ = std::min(dirtyJ0_, j0); dirtyI1_ = std::max(dirtyI1_, i1 + 1); dirtyJ1_ = std::max(dirtyJ1_, j1 + 1);
        }
        return;
    }
    if (!has && !hasFileDetail_) return;
    hasFileDetail_ = has;
    if (has) fileDetail_ = *d; else fileDetail_ = {};
    fileSplat_ = std::move(sp);
    fileDetailChanged_ = true;
}

float TerrainRenderer::surfaceAt(const MapGrid& g, float x, float z) const {
    if (detailOn_ && detail_.w == g.w && detail_.h == g.h) return detail_.sample(x, z);
    return terrain::heightAt(g, lh_, x, z);
}

size_t TerrainRenderer::detailSampleCap() const {
    // CPU floats + one RGBA8 normal and one splat texel per sample (capped at kMaxTex per side)
    return platform::isTouchDevice() ? size_t(2049) * 2049 : size_t(4097) * 4097;
}

uint64_t TerrainRenderer::detailHash(const Chunk& c) const {
    uint64_t h = 1469598103934665603ull;
    const int k = detail_.k;
    for (int j = c.y0 * k; j <= c.y1 * k; ++j)
        for (int i = c.x0 * k; i <= c.x1 * k; ++i) {
            uint32_t b; float v = detail_.at(i, j); std::memcpy(&b, &v, 4);
            h = (h ^ b) * 1099511628211ull;
        }
    return h;
}

// (Re)creates the detail surface where needed, the per-pixel textures and marks chunks whose samples
// changed. LOD meshes are built lazily in draw().
bool TerrainRenderer::buildDetail(gfx::Device& dev, const MapGrid& g, bool flatDig, bool all) {
    const int W = g.w, H = g.h;
    int want = hasFileDetail_ ? fileDetail_.k : look_.detailScale;
    if (tierFor0() == 0) want = std::min(want, 2);  // weak GPUs: fewer samples
    const int k = terrain::detailScaleFor(W, H, want, detailSampleCap());
    terrain::DetailParams p;
    p.scale = k; p.bevel = look_.bevel; p.wobble = look_.wobble; p.noise = look_.roughness;
    bool resized = detail_.w != W || detail_.h != H || detail_.k != k || all || fileDetailChanged_;
    int ri0 = 0, rj0 = 0, ri1 = 0, rj1 = 0;   // resynthesised sample rect (empty = none)
    const auto tp0 = std::chrono::steady_clock::now();
    if (resized) {
        if (hasFileDetail_) detail_ = fileDetail_.k == k && fileDetail_.w == W && fileDetail_.h == H ? fileDetail_ : terrain::resampleDetail(fileDetail_, k);
        else {
            detail_.init(W, H, k);
            if (heightfield_) terrain::synthCorners(g, p, detail_); else terrain::synthLevels(g, lh_, flatDig, p, detail_);
        }
        ri1 = detail_.sw(); rj1 = detail_.sh();
        for (auto& c : chunks_) { freeChunk(dev, c); c.hash = chunkHash(g, c, flatDig); c.geoHash = 0; }
    } else if (hasFileDetail_ && dirtyI1_ > dirtyI0_) {
        // explicit detail edited in place (same scale as drawn): copy the changed samples
        if (detail_.k == fileDetail_.k) {
            ri0 = dirtyI0_; rj0 = dirtyJ0_; ri1 = dirtyI1_; rj1 = dirtyJ1_;
            for (int j = rj0; j < rj1; ++j)
                for (int i = ri0; i < ri1; ++i) detail_.hgt[size_t(j) * size_t(detail_.sw()) + size_t(i)] = fileDetail_.hgt[size_t(j) * size_t(fileDetail_.sw()) + size_t(i)];
        } else {
            detail_ = terrain::resampleDetail(fileDetail_, k);
            ri0 = 0; rj0 = 0; ri1 = detail_.sw(); rj1 = detail_.sh();
        }
    } else if (!hasFileDetail_) {
        // editor edits: resynthesise around chunks whose cells changed
        int x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1;
        for (auto& c : chunks_) {
            uint64_t h = chunkHash(g, c, flatDig);
            if (h == c.hash) continue;
            c.hash = h;
            x0 = std::min(x0, c.x0); y0 = std::min(y0, c.y0); x1 = std::max(x1, c.x1); y1 = std::max(y1, c.y1);
        }
        if (x1 >= 0) {
            const int m = terrain::synthMargin(p, k);
            ri0 = std::max(0, x0 * k - m); rj0 = std::max(0, y0 * k - m);
            ri1 = std::min(detail_.sw(), x1 * k + m + 1); rj1 = std::min(detail_.sh(), y1 * k + m + 1);
            if (heightfield_) terrain::synthCorners(g, p, detail_, ri0, rj0, ri1, rj1);
            else terrain::synthLevels(g, lh_, flatDig, p, detail_, ri0, rj0, ri1, rj1);
        }
    }
    fileDetailChanged_ = false;
    dirtyI0_ = dirtyJ0_ = 1 << 30; dirtyI1_ = dirtyJ1_ = -1;
    detailK_ = k;
    if (hasFileDetail_ && !resized) // explicit detail: cell edits change colours / splat only
        for (auto& c : chunks_) {
            const uint64_t h = chunkHash(g, c, flatDig);
            if (h == c.hash) continue;
            c.hash = h; c.geoHash = 0;
            const int a0 = c.x0 * k, b0 = c.y0 * k, a1 = c.x1 * k + 1, b1 = c.y1 * k + 1;
            if (ri1 <= ri0) { ri0 = a0; rj0 = b0; ri1 = a1; rj1 = b1; }
            else { ri0 = std::min(ri0, a0); rj0 = std::min(rj0, b0); ri1 = std::max(ri1, a1); rj1 = std::max(rj1, b1); }
        }
    // cell colours (vertex colours are the bilinear mix of the classic cell colours)
    cellCol_.resize(size_t(W * H));
    for (int y = 0; y < H; ++y) for (int x = 0; x < W; ++x) cellCol_[size_t(y * W + x)] = cellColor(g, x, y, flatDig);
    bool changed = false;
    for (auto& c : chunks_) {
        // bounds + change detection on the samples themselves (covers synthesis margins and files)
        if (ri1 > ri0 && rj1 > rj0 && (c.x1 * k < ri0 || c.x0 * k >= ri1 || c.y1 * k < rj0 || c.y0 * k >= rj1)) { continue; }
        if (ri1 <= ri0 && c.valid) continue;
        const uint64_t gh = detailHash(c);
        float mn = 1e9f, mx = -1e9f;
        for (int j = c.y0 * k; j <= c.y1 * k; ++j)
            for (int i = c.x0 * k; i <= c.x1 * k; ++i) { float v = detail_.at(i, j); mn = std::min(mn, v); mx = std::max(mx, v); }
        c.bmin = {float(c.x0), std::min(mn, kApronY) - 1.0f, float(c.y0)};
        c.bmax = {float(c.x1), mx + 0.1f, float(c.y1)};
        if (c.valid && gh == c.geoHash) continue;
        for (auto& l : c.lods) { geoBytes_ -= std::min<size_t>(geoBytes_, size_t(l.indexCount) * 4 + size_t(l.vertexCount) * sizeof(TV)); dev.destroyBuffer(l.vb); dev.destroyBuffer(l.ib); l = {}; }
        c.geoHash = gh;
        c.valid = true;
        changed = true;
        ++stats_.rebuilt;
    }
    int lv = 1;
    while ((1 << lv) <= kChunk * k / 4) ++lv;           // coarsest level: 4x4 quads per chunk
    lodLevels_ = lv;
    for (auto& c : chunks_) { c.lods.resize(size_t(lodLevels_)); c.lastUse.resize(size_t(lodLevels_), 0); }
    const auto tp1 = std::chrono::steady_clock::now();
    if (changed || resized) updateDetailTextures(dev, ri1 > ri0 ? ri0 : 0, rj1 > rj0 ? rj0 : 0, ri1 > ri0 ? ri1 : detail_.sw(), rj1 > rj0 ? rj1 : detail_.sh());
    if (std::getenv("RTS_TERRAIN_STATS"))
        std::fprintf(stderr, "[terrain] detail build: surface+chunks %.1f ms, textures %.1f ms\n",
                     std::chrono::duration<double, std::milli>(tp1 - tp0).count(), std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tp1).count());
    detailOn_ = true;
    stats_.detailK = k;
    return changed || resized;
}

// Normal (rg = n.xz, b = cavity, a = macro noise) and splat textures at detail resolution (at most
// kMaxTex per side; larger maps get one texel per few samples). Only the rect around [i0,i1)x[j0,j1)
// (detail samples) is recomputed and uploaded.
void TerrainRenderer::updateDetailTextures(gfx::Device& dev, int i0, int j0, int i1, int j1) {
    const int SW = detail_.sw(), SH = detail_.sh(), W = detail_.w, H = detail_.h, k = detail_.k;
    const int TW = std::min(SW, maxTexSide()), TH = std::min(SH, maxTexSide());
    if (TW != nrmW_ || TH != nrmH_ || !detailNrmTex_.valid()) {
        if (detailNrmTex_.valid()) dev.destroyTexture(detailNrmTex_);
        if (detailSplatTex_.valid()) dev.destroyTexture(detailSplatTex_);
        nrmW_ = TW; nrmH_ = TH;
        nrmPx_.assign(size_t(TW) * size_t(TH) * 4, 128);
        dsplatPx_.assign(size_t(TW) * size_t(TH) * 4, 0);
        gfx::TextureDesc td; td.width = TW; td.height = TH; td.filter = gfx::Filter::Linear; td.mipmaps = true;
        td.data = nrmPx_.data(); td.label = "terrain detail normals"; detailNrmTex_ = dev.createTexture(td);
        td.data = dsplatPx_.data(); td.label = "terrain detail splat"; detailSplatTex_ = dev.createTexture(td);
        i0 = 0; j0 = 0; i1 = SW; j1 = SH;
    }
    // texel t <-> sample t * (S - 1) / (T - 1)
    const float sx = float(SW - 1) / float(std::max(TW - 1, 1)), sz = float(SH - 1) / float(std::max(TH - 1, 1));
    const int t0 = std::max(0, int(std::floor(float(i0 - 2) / sx))), t1 = std::min(TW, int(std::ceil(float(i1 + 2) / sx)) + 1);
    const int u0 = std::max(0, int(std::floor(float(j0 - 2) / sz))), u1 = std::min(TH, int(std::ceil(float(j1 + 2) / sz)) + 1);
    if (t1 <= t0 || u1 <= u0) return;
    const float e = 1.0f / float(k);             // one sample in world units
    const int layers = std::max(1, int(look_.layers.size()));
    auto cellLayer = [&](int cx, int cy) {
        cx = std::clamp(cx, 0, W - 1); cy = std::clamp(cy, 0, H - 1);
        const size_t i = size_t(cy * W + cx);
        int l = i < materialLayer_.size() && materialLayer_[i] < 4 ? materialLayer_[i] : cellLayerOf_[i];
        return std::clamp(l, 0, layers - 1);
    };
    const bool fileSplat = fileSplat_.size() == size_t(SW) * size_t(SH) * 4;
    for (int v = u0; v < u1; ++v)
        for (int t = t0; t < t1; ++t) {
            const float x = float(t) * sx * e, z = float(v) * sz * e;
            // nearest sample (texel ~ sample): central differences on the grid
            const int si = int(float(t) * sx + 0.5f), sj = int(float(v) * sz + 0.5f);
            const float hc = detail_.at(si, sj);
            const float hx0 = detail_.at(si - 1, sj), hx1 = detail_.at(si + 1, sj), hz0 = detail_.at(si, sj - 1), hz1 = detail_.at(si, sj + 1);
            float3 n = normalize({hx0 - hx1, 2.0f * e, hz0 - hz1});
            // cavity: how far the point lies below its neighbours at two radii (0.5 = flat)
            const float ring = (detail_.at(si - 3, sj) + detail_.at(si + 3, sj) + detail_.at(si, sj - 3) + detail_.at(si, sj + 3)) * 0.25f;
            const float cav = std::clamp(0.5f + ((hx0 + hx1 + hz0 + hz1) * 0.25f - hc) * 3.0f + (ring - hc) * 0.8f, 0.0f, 1.0f);
            const float macro = 0.5f + 0.5f * (vnoise(x * 0.07f, z * 0.07f, 91) * 0.6f + vnoise(x * 0.23f, z * 0.23f, 93) * 0.4f);
            uint8_t* px = &nrmPx_[(size_t(v) * size_t(TW) + size_t(t)) * 4];
            px[0] = uint8_t(std::clamp(n.x * 127.5f + 127.5f, 0.0f, 255.0f));
            px[1] = uint8_t(std::clamp(n.z * 127.5f + 127.5f, 0.0f, 255.0f));
            px[2] = uint8_t(cav * 255.0f + 0.5f);
            px[3] = uint8_t(std::clamp(macro, 0.0f, 1.0f) * 255.0f + 0.5f);
            uint8_t* sp = &dsplatPx_[(size_t(v) * size_t(TW) + size_t(t)) * 4];
            if (fileSplat) {
                const int si = std::min(int(float(t) * sx + 0.5f), SW - 1), sj = std::min(int(float(v) * sz + 0.5f), SH - 1);
                std::memcpy(sp, &fileSplat_[(size_t(sj) * size_t(SW) + size_t(si)) * 4], 4);
                continue;
            }
            // cell layers mixed bilinearly between cell centres at a warped position: soft, organic borders
            const float wx = x - 0.5f + 0.35f * vnoise(x * 1.3f, z * 1.3f, 31), wz = z - 0.5f + 0.35f * vnoise(x * 1.3f, z * 1.3f, 37);
            const int cx = int(std::floor(wx)), cz = int(std::floor(wz));
            const float fx = std::clamp(wx - float(cx), 0.0f, 1.0f), fz = std::clamp(wz - float(cz), 0.0f, 1.0f);
            float wl[4] = {0, 0, 0, 0};
            wl[cellLayer(cx, cz)] += (1 - fx) * (1 - fz); wl[cellLayer(cx + 1, cz)] += fx * (1 - fz);
            wl[cellLayer(cx, cz + 1)] += (1 - fx) * fz; wl[cellLayer(cx + 1, cz + 1)] += fx * fz;
            for (int l = 0; l < 4; ++l) {
                const float s = std::clamp((wl[l] - 0.5f) * 2.2f + 0.5f, 0.0f, 1.0f);  // sharpen the blend band
                sp[l] = uint8_t(s * 255.0f + 0.5f);
            }
            if (sp[0] + sp[1] + sp[2] + sp[3] == 0) sp[cellLayer(int(x), int(z))] = 255;
        }
    dev.updateTexture(detailNrmTex_, 0, u0, TW, u1 - u0, &nrmPx_[size_t(u0) * size_t(TW) * 4], TW * 4);
    dev.updateTexture(detailSplatTex_, 0, u0, TW, u1 - u0, &dsplatPx_[size_t(u0) * size_t(TW) * 4], TW * 4);
}

void TerrainRenderer::buildDetailLod(gfx::Device& dev, const MapGrid& g, Chunk& c, int L) {
    const int k = detail_.k, s = 1 << L, W = g.w, H = g.h;
    const int si0 = c.x0 * k, sj0 = c.y0 * k, si1 = c.x1 * k, sj1 = c.y1 * k;
    std::vector<int> is, js;
    for (int i = si0; i < si1; i += s) is.push_back(i);
    is.push_back(si1);
    for (int j = sj0; j < sj1; j += s) js.push_back(j);
    js.push_back(sj1);
    const int nx = int(is.size()), ny = int(js.size());
    const float inv = 1.0f / float(k);
    auto morphAt = [&](int i, int j) {
        if (L + 1 >= lodLevels_) return detail_.at(i, j);
        const int S = 2 * s;
        const int i0 = (i / S) * S, j0 = (j / S) * S;
        if (i0 == i && j0 == j) return detail_.at(i, j);
        const int i1 = std::min(i0 + S, detail_.sw() - 1), j1 = std::min(j0 + S, detail_.sh() - 1);
        const float fx = i1 > i0 ? float(i - i0) / float(i1 - i0) : 0.0f, fz = j1 > j0 ? float(j - j0) / float(j1 - j0) : 0.0f;
        return terrain::quadSurface(detail_.at(i0, j0), detail_.at(i1, j0), detail_.at(i0, j1), detail_.at(i1, j1), fx, fz);
    };
    auto colorAt = [&](float x, float z) {
        const float fx = x - 0.5f, fz = z - 0.5f;
        const int cx = int(std::floor(fx)), cz = int(std::floor(fz));
        const float u = fx - float(cx), v = fz - float(cz);
        auto cc = [&](int a, int b) { return cellCol_[size_t(std::clamp(b, 0, H - 1) * W + std::clamp(a, 0, W - 1))]; };
        float3 col = (cc(cx, cz) * (1 - u) + cc(cx + 1, cz) * u) * (1 - v) + (cc(cx, cz + 1) * (1 - u) + cc(cx + 1, cz + 1) * u) * v;
        return (rgbaf({col.x, col.y, col.z, 1}) & 0x00ffffffu) | (15u << 24);   // all 4 cells around: dynamic tint
    };
    std::vector<TV> v;
    std::vector<uint32_t> idx;
    v.reserve(size_t(nx * ny) + size_t(nx + ny) * 8);
    for (int b = 0; b < ny; ++b)
        for (int a = 0; a < nx; ++a) {
            const int i = is[size_t(a)], j = js[size_t(b)];
            const float x = float(i) * inv, z = float(j) * inv, h = detail_.at(i, j);
            float3 n = normalize({detail_.at(i - 1, j) - detail_.at(i + 1, j), 2.0f * inv, detail_.at(i, j - 1) - detail_.at(i, j + 1)});
            v.push_back({{x, h, z}, n, colorAt(x, z), {morphAt(i, j), 0}});
        }
    auto vi = [&](int a, int b) { return uint32_t(b * nx + a); };
    for (int b = 0; b + 1 < ny; ++b)
        for (int a = 0; a + 1 < nx; ++a) {
            uint32_t p0 = vi(a, b), p1 = vi(a + 1, b), p2 = vi(a, b + 1), p3 = vi(a + 1, b + 1);
            idx.insert(idx.end(), {p0, p3, p1, p0, p2, p3});
        }
    // skirts against cracks between levels, and walls down to the apron at the map border
    const float depth = 0.15f + 0.2f * float(s) * inv * 4.0f;
    auto skirt = [&](uint32_t ia, uint32_t ib, float3 out, float bottom, bool wall) {
        TV a = v[ia], b2 = v[ib];
        TV a2 = a, b3 = b2;
        if (wall) {
            a2.pos.y = b3.pos.y = a2.morph[0] = b3.morph[0] = bottom;
            const uint32_t wc = rgbaf({0.42f, 0.38f, 0.33f, 1});
            a.rgba = b2.rgba = a2.rgba = b3.rgba = wc;
            a.nrm = b2.nrm = a2.nrm = b3.nrm = out;
            if (a.pos.y <= bottom + 1e-3f && b2.pos.y <= bottom + 1e-3f) return;
        } else { a2.pos.y -= depth; b3.pos.y -= depth; a2.morph[0] -= depth; b3.morph[0] -= depth; }
        const uint32_t base = uint32_t(v.size());
        v.push_back(a); v.push_back(b2); v.push_back(a2); v.push_back(b3);
        const float3 e = b2.pos - a.pos;
        if (dot(cross(e, float3{0, -1, 0}), out) >= 0) idx.insert(idx.end(), {base, base + 2, base + 1, base + 1, base + 2, base + 3});
        else idx.insert(idx.end(), {base, base + 1, base + 2, base + 1, base + 3, base + 2});
    };
    for (int a = 0; a + 1 < nx; ++a) {
        skirt(vi(a, 0), vi(a + 1, 0), {0, 0, -1}, kApronY, c.y0 == 0);
        skirt(vi(a, ny - 1), vi(a + 1, ny - 1), {0, 0, 1}, kApronY, c.y1 == H);
    }
    for (int b = 0; b + 1 < ny; ++b) {
        skirt(vi(0, b), vi(0, b + 1), {-1, 0, 0}, kApronY, c.x0 == 0);
        skirt(vi(nx - 1, b), vi(nx - 1, b + 1), {1, 0, 0}, kApronY, c.x1 == W);
    }
    Lod& l = c.lods[size_t(L)];
    upload(dev, v, idx, l);
    l.vertexCount = uint32_t(v.size());
    geoBytes_ += size_t(l.indexCount) * 4 + size_t(l.vertexCount) * sizeof(TV);
}

} // namespace rts::game
