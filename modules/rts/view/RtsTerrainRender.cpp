// Chunked terrain renderer (see RtsTerrainRender.h).
#include "RtsTerrainRender.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include "../../../engine/platform/Platform.h"
#include "RtsTerrainShaders.h"
#include "RtsTerrainSurface.h"

namespace rts::game {

using namespace render;

namespace {
constexpr int kChunk = 32;
constexpr int kApron = 12;
constexpr int kMaxLod = 5;            // steps 1, 2, 4, 8, 16 cells
constexpr float kApronY = -0.05f;

uint32_t hashCell(int x, int y) { uint32_t h = uint32_t(x) * 73856093u ^ uint32_t(y) * 19349663u; h ^= h >> 13; h *= 0x5bd1e995u; return h ^ (h >> 15); }
float noiseOf(int x, int y) { return float(hashCell(x, y) & 255) / 255.0f; }

struct Hasher {
    uint64_t h = 1469598103934665603ull;
    void add(uint64_t v) { for (int i = 0; i < 8; ++i) { h ^= (v >> (i * 8)) & 0xff; h *= 1099511628211ull; } }
};

float3 jsonColor(const Json& j, float3 def) {
    if (!j.isArray() || j.size() < 3) return def;
    return {float(j[0].toDouble()), float(j[1].toDouble()), float(j[2].toDouble())};
}

struct TU0 { float lod[4], mode[4], scale[4], cliff[4], fog[4], tint[4][4], cliffTint[4]; };

const char* kKindNames[] = {"grass", "sand", "ramp", "rock", "blocked", "lakebed", "shallows", "dug"};
} // namespace

const char* cellKindName(int k) { return k >= 0 && k < 8 ? kKindNames[k] : "?"; }

std::vector<uint8_t> parseMaterialRows(const Json& rows, int w, int h) {
    if (!rows.isArray() || w <= 0 || h <= 0) return {};
    std::vector<uint8_t> out(size_t(w * h), 255);
    for (size_t y = 0; y < rows.size() && int(y) < h; ++y) {
        std::string r = rows[y].str();
        for (size_t x = 0; x < r.size() && int(x) < w; ++x)
            if (r[x] >= '0' && r[x] <= '3') out[y * size_t(w) + x] = uint8_t(r[x] - '0');
    }
    return out;
}

// ------------------------------------------------------------------ setup

void TerrainRenderer::init(gfx::Device& dev, const Rules& rules, const FileReader& read, const std::string& contentBase) {
    if (dev_) shutdown(*dev_);
    dev_ = &dev;
    rules_ = &rules;
    base_ = contentBase;
    read_ = read;
    textures_.init(dev);
    look_ = {};
    if (read) { // visual `terrain` block: rules file first, game.json overrides per key (as RtsGroundFx)
        std::string text, rulesPath = "rules.json";
        Json game;
        try { if (read("game.json", text)) { game = Json::parse(text); rulesPath = game.getStr("rules", "rules.json"); } } catch (...) {}
        try { if (read(rulesPath, text)) parseLook(Json::parse(text)["terrain"]); } catch (...) {}
        if (game.isObject()) parseLook(game["terrain"]);
    }
    auto tex1 = [&](uint32_t px, const char* label) {
        gfx::TextureDesc td; td.width = td.height = 1; td.data = &px; td.repeat = true; td.label = label;
        return dev.createTexture(td);
    };
    white_ = tex1(0xffffffffu, "terrain white");
    flatN_ = tex1(0xffff8080u, "terrain flat normal");
    gray_ = tex1(0xff808080u, "terrain gray");
    texturesLoaded_ = false;
    built_ = false;
}

void TerrainRenderer::parseLook(const Json& t) {
    if (!t.isObject()) return;
    if (t["layers"].isArray()) {
        look_.layers.clear();
        for (auto& l : t["layers"].items()) {
            if (look_.layers.size() >= 4) break;
            TerrainLook::Layer L;
            L.id = l.getStr("id", "layer" + std::to_string(look_.layers.size()));
            L.texture = l.getStr("texture");
            L.normal = l.getStr("normal");
            L.scale = std::max(0.05f, float(l["scale"].toDouble(4)));
            L.tint = jsonColor(l["tint"], {1, 1, 1});
            look_.layers.push_back(L);
        }
    }
    auto layerIndex = [&](const std::string& id) {
        for (size_t i = 0; i < look_.layers.size(); ++i) if (look_.layers[i].id == id) return int(i);
        return -1;
    };
    if (t["cells"].isObject())
        for (auto& [k, v] : t["cells"].members()) {
            int kind = -1;
            for (int i = 0; i < 8; ++i) if (k == kKindNames[i]) kind = i;
            if (kind < 0) continue;
            const Json& lj = v.isString() ? v : v["layer"];
            if (lj.isString()) { int li = layerIndex(lj.str()); if (li >= 0) look_.cellLayer[kind] = li; }
            else if (lj.isNumber()) look_.cellLayer[kind] = std::clamp(int(lj.integer()), 0, 3);
            if (v.isObject() && v["color"].isArray()) { look_.hasColor[kind] = true; look_.color[kind] = jsonColor(v["color"], {1, 1, 1}); }
        }
    if (const Json& c = t["cliff"]; c.isObject()) {
        look_.cliffTexture = c.getStr("texture", look_.cliffTexture);
        look_.cliffNormal = c.getStr("normal", look_.cliffNormal);
        look_.cliffScale = std::max(0.05f, float(c["scale"].toDouble(look_.cliffScale)));
        look_.cliffTint = jsonColor(c["tint"], look_.cliffTint);
        if (c["slope"].isArray() && c["slope"].size() >= 2) {
            look_.slopeStart = float(c["slope"][0].toDouble()); look_.slopeEnd = std::max(look_.slopeStart + 0.01f, float(c["slope"][1].toDouble()));
        }
    }
    if (t.has("splatStrength")) look_.splatStrength = std::clamp(float(t["splatStrength"].toDouble(1)), 0.0f, 1.0f);
    if (const Json& w = t["water"]; w.isObject()) {
        std::string m = w.getStr("mode", "auto");
        look_.water = m == "sheet" ? TerrainLook::Water::Sheet : m == "bodies" ? TerrainLook::Water::Bodies : TerrainLook::Water::Auto;
        if (w["color"].isArray() && w["color"].size() >= 3) {
            float3 c = jsonColor(w["color"], {0.22f, 0.45f, 0.62f});
            look_.waterColor = {c.x, c.y, c.z, w["color"].size() >= 4 ? float(w["color"][3].toDouble()) : 0.78f};
        }
        look_.waterOffset = float(w["offset"].toDouble(look_.waterOffset));
    } else if (w.isString()) {
        std::string m = w.str();
        look_.water = m == "sheet" ? TerrainLook::Water::Sheet : m == "bodies" ? TerrainLook::Water::Bodies : TerrainLook::Water::Auto;
    }
    if (const Json& l = t["lod"]; l.type() == Json::Type::Bool) look_.lod = l.boolean() ? 1 : 0;
    else if (l.isString()) look_.lod = l.str() == "on" ? 1 : l.str() == "off" ? 0 : -1;
    if (t.has("lodRange")) look_.lodRange = std::max(16.0f, float(t["lodRange"].toDouble(128)));
}

void TerrainRenderer::loadTextures(gfx::Device&) {
    texturesLoaded_ = true;
    for (int i = 0; i < 4; ++i) { layerTex_[i] = {}; layerNrm_[i] = {}; }
    cliffTex_ = cliffNrm_ = {};
    // tiling textures: repeat + mipmaps, never from an atlas page (TextureCache::resolve(..., tiling))
    auto load = [&](const std::string& p) { return p.empty() ? TextureRef{} : textures_.resolve(p, false, base_, true); };
    for (size_t i = 0; i < look_.layers.size(); ++i) { layerTex_[i] = load(look_.layers[i].texture); layerNrm_[i] = load(look_.layers[i].normal); }
    cliffTex_ = load(look_.cliffTexture);
    cliffNrm_ = load(look_.cliffNormal);
}

void TerrainRenderer::shutdown(gfx::Device& dev) {
    for (auto& c : chunks_) freeChunk(dev, c);
    chunks_.clear();
    for (auto* l : {&apron_}) { dev.destroyBuffer(l->vb); dev.destroyBuffer(l->ib); *l = {}; }
    dev.destroyBuffer(water_.vb); dev.destroyBuffer(water_.ib); water_ = {};
    for (auto t : {cellTex_, dynTex_, splatTex_, visionTex_, white_, flatN_, gray_}) if (t.valid()) dev.destroyTexture(t);
    cellTex_ = dynTex_ = splatTex_ = visionTex_ = white_ = flatN_ = gray_ = {};
    for (int i = 0; i < 3; ++i) {
        if (pipes_[i].valid()) dev.destroyPipeline(pipes_[i]);
        if (shaders_[i].valid()) dev.destroyShader(shaders_[i]);
        pipes_[i] = {}; shaders_[i] = {}; shaderFailed_[i] = false;
    }
    textures_.shutdown();
    texW_ = texH_ = 0;
    built_ = false;
    globalHash_ = 0;
    dev_ = nullptr;
}

void TerrainRenderer::freeChunk(gfx::Device& dev, Chunk& c) {
    for (auto& l : c.lods) { dev.destroyBuffer(l.vb); dev.destroyBuffer(l.ib); }
    c.lods.clear();
    c.valid = false;
}

void TerrainRenderer::upload(gfx::Device& dev, const std::vector<TV>& v, const std::vector<uint32_t>& idx, Lod& out) {
    dev.destroyBuffer(out.vb); dev.destroyBuffer(out.ib);
    out = {};
    if (idx.empty()) return;
    gfx::BufferDesc vb; vb.type = gfx::BufferType::Vertex; vb.size = v.size() * sizeof(TV); vb.data = v.data(); vb.label = "terrain";
    out.vb = dev.createBuffer(vb);
    gfx::BufferDesc ib; ib.type = gfx::BufferType::Index; ib.size = idx.size() * sizeof(uint32_t); ib.data = idx.data(); ib.label = "terrain";
    out.ib = dev.createBuffer(ib);
    out.indexCount = uint32_t(idx.size());
}

// ------------------------------------------------------------------ cell look

int TerrainRenderer::kindOf(const MapGrid& g, size_t i, bool flatDig) const {
    const uint8_t f = g.flags[i];
    if (!(f & kCellGround) && (f & kCellWater)) return kKindLakebed;
    if (flatDig && g.dig[i]) return kKindDug;
    if (!(f & kCellGround)) return kKindBlocked;
    if (f & kCellRock) return kKindRock;
    if (f & kCellRamp) return kKindRamp;
    if (f & kCellWater) return kKindShallows;
    if (f & kCellNoBuild) return kKindSand;
    return kKindGrass;
}

// Classic cell colours without the dynamic part (fields, creep: tinted in the vertex shader).
float3 TerrainRenderer::cellColor(const MapGrid& g, int x, int y, bool flatDig) const {
    const size_t i = terrain::cellIndexClamped(g, x, y);
    const float n = noiseOf(x, y);
    const int kind = kindOf(g, i, flatDig);
    float3 c;
    if (look_.hasColor[kind]) c = look_.color[kind] * (0.96f + n * 0.08f);
    else switch (kind) {
        case kKindLakebed: c = {0.32f, 0.40f, 0.30f}; break;
        case kKindDug: c = {0.46f + n * 0.04f, 0.41f + n * 0.03f, 0.34f}; break;        // under diggable rock: earthen floor once dug
        case kKindBlocked: c = {0.47f + n * 0.05f, 0.45f + n * 0.05f, 0.42f}; break;
        case kKindRock: c = {0.56f + n * 0.04f, 0.50f + n * 0.04f, 0.44f}; break;       // buildable rock plate
        case kKindRamp: c = {0.55f + n * 0.03f, 0.50f, 0.36f}; break;                   // ramp: packed earth
        case kKindShallows: c = {0.42f, 0.55f, 0.50f}; break;
        case kKindSand: c = {0.60f + n * 0.04f, 0.52f, 0.38f}; break;                   // path / sand
        default: c = {0.33f + n * 0.05f, 0.52f + n * 0.06f, 0.24f + n * 0.03f}; break; // grass
    }
    if (!heightfield_) c = c * (1.0f + float(g.level[i]) * 0.05f);   // plateaus a bit brighter
    return c;
}

// ------------------------------------------------------------------ build

uint64_t TerrainRenderer::chunkHash(const MapGrid& g, const Chunk& c, bool flatDig) const {
    Hasher h;
    if (heightfield_) {
        for (int y = std::max(0, c.y0 - 1); y <= std::min(g.h, c.y1 + 1); ++y)
            for (int x = std::max(0, c.x0 - 1); x <= std::min(g.w, c.x1 + 1); ++x) h.add(uint64_t(uint16_t(g.corner[size_t(g.cornerIndex(x, y))])));
    }
    for (int y = std::max(0, c.y0 - 2); y < std::min(g.h, c.y1 + 2); ++y)
        for (int x = std::max(0, c.x0 - 2); x < std::min(g.w, c.x1 + 2); ++x) {
            size_t i = size_t(g.index(x, y));
            // cell colour inputs (vertex colours) + geometry inputs (levels mode)
            h.add(uint64_t(g.flags[i] & (kCellGround | kCellWater | kCellNoBuild | kCellRamp | kCellRock)) |
                  (uint64_t(uint8_t(g.level[i])) << 8) | (uint64_t(uint16_t(g.height[i])) << 16) | (uint64_t(flatDig && g.dig[i] ? 1 : 0) << 32));
        }
    return h.h;
}

bool TerrainRenderer::build(gfx::Device& dev, const MapGrid& g, bool flatDig) {
    auto t0 = std::chrono::steady_clock::now();
    if (g.w <= 0 || g.h <= 0) return false;
    lh_ = rules_ ? float(rules_->levelHeight.toDouble()) : 1.5f;
    heightfield_ = g.hasCorners() && g.corner.size() == size_t((g.w + 1) * (g.h + 1));
    const bool lod = heightfield_ && (look_.lod == 1 || (look_.lod < 0 && std::max(g.w, g.h) > 256));
    lodLevels_ = lod ? kMaxLod : 1;
    Hasher gh;
    gh.add(uint64_t(g.w)); gh.add(uint64_t(g.h)); gh.add(uint64_t(heightfield_)); gh.add(uint64_t(lodLevels_));
    uint32_t lhBits; std::memcpy(&lhBits, &lh_, 4); gh.add(lhBits);
    bool all = gh.h != globalHash_ || !built_;
    if (all) {
        for (auto& c : chunks_) freeChunk(dev, c);
        mapW_ = g.w; mapH_ = g.h;
        cw_ = (g.w + kChunk - 1) / kChunk; ch_ = (g.h + kChunk - 1) / kChunk;
        chunks_.assign(size_t(cw_ * ch_), Chunk{});
        for (int cy = 0; cy < ch_; ++cy)
            for (int cx = 0; cx < cw_; ++cx) {
                Chunk& c = chunks_[size_t(cy * cw_ + cx)];
                c.x0 = cx * kChunk; c.y0 = cy * kChunk; c.x1 = std::min(g.w, c.x0 + kChunk); c.y1 = std::min(g.h, c.y0 + kChunk);
            }
        globalHash_ = gh.h;
        buildApron(dev, g);
    }
    if (!texturesLoaded_) loadTextures(dev);
    stats_.rebuilt = 0;
    for (auto& c : chunks_) {
        uint64_t h = chunkHash(g, c, flatDig);
        if (c.valid && h == c.hash) continue;
        c.hash = h;
        if (heightfield_) buildHeightChunk(dev, g, c); else buildLevelsChunk(dev, g, c, flatDig);
        c.valid = true;
        ++stats_.rebuilt;
    }
    const bool changed = all || stats_.rebuilt > 0;
    updateCellTextures(dev, g, flatDig);
    updateDynamic(dev, g, true);
    if (changed) buildWater(dev, g, flatDig);
    built_ = true;
    stats_.chunks = int(chunks_.size());
    stats_.lodMax = lodLevels_;
    stats_.buildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return changed;
}

// Levels mode: exactly the classic mesh - per-cell top quads (separate vertices: sharp cliff edges),
// corner heights averaged over same-level cells, cliff walls where neighbouring edges don't meet,
// skirts down the map border.
void TerrainRenderer::buildLevelsChunk(gfx::Device& dev, const MapGrid& g, Chunk& c, bool flatDig) {
    const int W = g.w, H = g.h;
    auto level = [&](int x, int y) { return int(g.level[terrain::cellIndexClamped(g, x, y)]); };
    auto corners = [&](int x, int y, float* o) {
        o[0] = terrain::cellCorner(g, lh_, x, y, 0, 0, flatDig); o[1] = terrain::cellCorner(g, lh_, x, y, 1, 0, flatDig);
        o[2] = terrain::cellCorner(g, lh_, x, y, 0, 1, flatDig); o[3] = terrain::cellCorner(g, lh_, x, y, 1, 1, flatDig);
    };
    // corner heights for the chunk plus one cell east/south (walls)
    const int ex = std::min(W, c.x1 + 1), ey = std::min(H, c.y1 + 1);
    const int cwid = ex - c.x0;
    std::vector<float> ch(size_t(cwid * (ey - c.y0)) * 4);
    for (int y = c.y0; y < ey; ++y) for (int x = c.x0; x < ex; ++x) corners(x, y, &ch[size_t((y - c.y0) * cwid + (x - c.x0)) * 4]);
    auto chAt = [&](int x, int y) { return &ch[size_t((y - c.y0) * cwid + (x - c.x0)) * 4]; };
    // cell colours for the chunk plus a one cell border (corner colour averages)
    const int bx0 = std::max(0, c.x0 - 1), by0 = std::max(0, c.y0 - 1), bx1 = std::min(W, c.x1 + 1), by1 = std::min(H, c.y1 + 1);
    const int bw = bx1 - bx0;
    std::vector<float3> col(size_t(bw * (by1 - by0)));
    for (int y = by0; y < by1; ++y) for (int x = bx0; x < bx1; ++x) col[size_t((y - by0) * bw + (x - bx0))] = cellColor(g, x, y, flatDig);
    auto colAt = [&](int x, int y) { return col[size_t((y - by0) * bw + (x - bx0))]; };

    std::vector<TV> v;
    std::vector<uint32_t> idx;
    v.reserve(size_t((c.x1 - c.x0) * (c.y1 - c.y0)) * 5);
    float3 bmin{1e9f, 1e9f, 1e9f}, bmax{-1e9f, -1e9f, -1e9f};
    auto vert = [&](float3 p, float3 n, uint32_t rgba) {
        bmin = min3(bmin, p); bmax = max3(bmax, p);
        v.push_back({p, n, rgba, {p.y, 0}});
        return uint32_t(v.size() - 1);
    };
    auto quad = [&](float3 p0, float3 p1, float3 p2, float3 p3, float3 n, uint32_t c0, uint32_t c1, uint32_t c2, uint32_t c3) {
        uint32_t base = vert(p0, n, c0); vert(p1, n, c1); vert(p2, n, c2); vert(p3, n, c3);
        // front faces satisfy cross(b - a, c - a) . n > 0; fix the winding per triangle
        auto tri = [&](uint32_t i, uint32_t j, uint32_t k, float3 a, float3 bp, float3 cp) {
            if (dot(cross(bp - a, cp - a), n) >= 0) { idx.push_back(i); idx.push_back(j); idx.push_back(k); }
            else { idx.push_back(i); idx.push_back(k); idx.push_back(j); }
        };
        tri(base, base + 2, base + 1, p0, p2, p1);
        tri(base + 1, base + 2, base + 3, p1, p2, p3);
    };
    auto solid = [](float3 cc) { return rgbaf({cc.x, cc.y, cc.z, 1}); };
    auto wallColor = [&](int x, int y) {
        float n = noiseOf(x * 3, y * 7);
        return solid({0.42f + n * 0.06f, 0.38f + n * 0.05f, 0.33f + n * 0.04f});
    };
    // corner colour: average over the cells around the corner on the same level (soft ground, sharp
    // cliffs). Alpha = bit mask of those cells: the vertex shader adds their dynamic tint.
    auto cornerCol = [&](int x, int y, int cx, int cy) {
        float3 sum{};
        float cnt = 0;
        int mask = 0;
        const int lv = level(x, y);
        for (int dy = cy - 1; dy <= cy; ++dy)
            for (int dx = cx - 1; dx <= cx; ++dx) {
                int nx = x + dx, ny = y + dy;
                if (!g.inside(nx, ny) || level(nx, ny) != lv) continue;
                sum += colAt(nx, ny); cnt += 1;
                mask |= 1 << ((dx - cx + 1) + 2 * (dy - cy + 1));
            }
        float3 cc = cnt > 0 ? sum * (1.0f / cnt) : colAt(x, y);
        uint32_t rgba = rgbaf({cc.x, cc.y, cc.z, 1});
        return (rgba & 0x00ffffffu) | (uint32_t(mask) << 24);
    };
    for (int y = c.y0; y < c.y1; ++y)
        for (int x = c.x0; x < c.x1; ++x) {
            const float* k = chAt(x, y);
            float3 n = normalize({(k[0] + k[2]) - (k[1] + k[3]), 2.0f, (k[0] + k[1]) - (k[2] + k[3])});
            quad({float(x), k[0], float(y)}, {float(x + 1), k[1], float(y)}, {float(x), k[2], float(y + 1)}, {float(x + 1), k[3], float(y + 1)}, n,
                 cornerCol(x, y, 0, 0), cornerCol(x, y, 1, 0), cornerCol(x, y, 0, 1), cornerCol(x, y, 1, 1));
        }
    for (int y = c.y0; y < c.y1; ++y)
        for (int x = c.x0; x < c.x1; ++x) {
            const float* a = chAt(x, y);
            if (x + 1 < W) { // east edge: a(1,0),a(1,1) vs b(0,0),b(0,1)
                const float* bb = chAt(x + 1, y);
                float ta = a[1], tb = a[3], ua = bb[0], ub = bb[2];
                if (std::fabs(ta - ua) > 0.05f || std::fabs(tb - ub) > 0.05f) {
                    bool aHigh = ta + tb > ua + ub;
                    float3 n = aHigh ? float3{1, 0, 0} : float3{-1, 0, 0};
                    float X = float(x + 1);
                    if (aHigh) { uint32_t wc = wallColor(x, y); quad({X, ta, float(y)}, {X, tb, float(y + 1)}, {X, ua, float(y)}, {X, ub, float(y + 1)}, n, wc, wc, wc, wc); }
                    else { uint32_t wc = wallColor(x + 1, y); quad({X, ub, float(y + 1)}, {X, ua, float(y)}, {X, tb, float(y + 1)}, {X, ta, float(y)}, n, wc, wc, wc, wc); }
                }
            }
            if (y + 1 < H) { // south edge: a(0,1),a(1,1) vs b(0,0),b(1,0)
                const float* bb = chAt(x, y + 1);
                float ta = a[2], tb = a[3], ua = bb[0], ub = bb[1];
                if (std::fabs(ta - ua) > 0.05f || std::fabs(tb - ub) > 0.05f) {
                    bool aHigh = ta + tb > ua + ub;
                    float3 n = aHigh ? float3{0, 0, 1} : float3{0, 0, -1};
                    float Z = float(y + 1);
                    if (aHigh) { uint32_t wc = wallColor(x, y); quad({float(x + 1), tb, Z}, {float(x), ta, Z}, {float(x + 1), ub, Z}, {float(x), ua, Z}, n, wc, wc, wc, wc); }
                    else { uint32_t wc = wallColor(x, y + 1); quad({float(x), ua, Z}, {float(x + 1), ub, Z}, {float(x), ta, Z}, {float(x + 1), tb, Z}, n, wc, wc, wc, wc); }
                }
            }
        }
    // skirts down the map border
    for (int x = c.x0; x < c.x1; ++x) {
        if (c.y0 == 0) {
            const float* top = chAt(x, 0);
            uint32_t wc = wallColor(x, 0);
            if (top[0] > 0.0f || top[1] > 0.0f) quad({float(x + 1), top[1], 0}, {float(x), top[0], 0}, {float(x + 1), kApronY, 0}, {float(x), kApronY, 0}, {0, 0, -1}, wc, wc, wc, wc);
        }
        if (c.y1 == H) {
            const float* bot = chAt(x, H - 1);
            uint32_t wc = wallColor(x, H - 1);
            if (bot[2] > 0.0f || bot[3] > 0.0f) quad({float(x), bot[2], float(H)}, {float(x + 1), bot[3], float(H)}, {float(x), kApronY, float(H)}, {float(x + 1), kApronY, float(H)}, {0, 0, 1}, wc, wc, wc, wc);
        }
    }
    for (int y = c.y0; y < c.y1; ++y) {
        if (c.x0 == 0) {
            const float* l = chAt(0, y);
            uint32_t wc = wallColor(0, y);
            if (l[0] > 0.0f || l[2] > 0.0f) quad({0, l[0], float(y)}, {0, l[2], float(y + 1)}, {0, kApronY, float(y)}, {0, kApronY, float(y + 1)}, {-1, 0, 0}, wc, wc, wc, wc);
        }
        if (c.x1 == W) {
            const float* r = chAt(W - 1, y);
            uint32_t wc = wallColor(W - 1, y);
            if (r[1] > 0.0f || r[3] > 0.0f) quad({float(W), r[3], float(y + 1)}, {float(W), r[1], float(y)}, {float(W), kApronY, float(y + 1)}, {float(W), kApronY, float(y)}, {1, 0, 0}, wc, wc, wc, wc);
        }
    }
    c.lods.resize(1);
    upload(dev, v, idx, c.lods[0]);
    c.bmin = bmin; c.bmax = bmax;
}

// Heightfield: a smooth grid on the corners (diagonal (0,0)-(1,1), as terrain::cornerSurface). With
// LOD, level k samples every 2^k-th corner; `morph` is the height on level k+1 at the vertex, so the
// vertex shader can blend into the coarser level before the chunk switches (CDLOD geomorphing).
// Chunk edges get skirts when LOD is on (neighbours may be one level apart).
void TerrainRenderer::buildHeightChunk(gfx::Device& dev, const MapGrid& g, Chunk& c) {
    const int W = g.w, H = g.h;
    auto hAt = [&](int X, int Y) { return terrain::cornerHeight(g, X, Y); };
    auto normalAt = [&](int X, int Y) { return normalize({hAt(X - 1, Y) - hAt(X + 1, Y), 2.0f, hAt(X, Y - 1) - hAt(X, Y + 1)}); };
    auto colorAt = [&](int X, int Y) {
        float3 sum{};
        float cnt = 0;
        int mask = 0;
        for (int i = 0; i < 4; ++i) {
            int x = X - 1 + (i & 1), y = Y - 1 + (i >> 1);
            if (!g.inside(x, y)) continue;
            sum += cellColor(g, x, y, false); cnt += 1; mask |= 1 << i;
        }
        float3 cc = cnt > 0 ? sum * (1.0f / cnt) : float3{0.3f, 0.45f, 0.24f};
        return (rgbaf({cc.x, cc.y, cc.z, 1}) & 0x00ffffffu) | (uint32_t(mask) << 24);
    };
    auto wallColor = [&](int x, int y) {
        float n = noiseOf(x * 3, y * 7);
        return rgbaf({0.42f + n * 0.06f, 0.38f + n * 0.05f, 0.33f + n * 0.04f, 1});
    };
    c.lods.resize(size_t(lodLevels_));
    float3 bmin{1e9f, 1e9f, 1e9f}, bmax{-1e9f, -1e9f, -1e9f};
    for (int k = 0; k < lodLevels_; ++k) {
        const int s = 1 << k;
        std::vector<TV> v;
        std::vector<uint32_t> idx;
        // vertex grid: multiples of s inside the chunk, plus the chunk's far edge (may not be a multiple at the map edge)
        std::vector<int> xs, ys;
        for (int x = c.x0; x < c.x1; x += s) xs.push_back(x);
        xs.push_back(c.x1);
        for (int y = c.y0; y < c.y1; y += s) ys.push_back(y);
        ys.push_back(c.y1);
        const int nx = int(xs.size()), ny = int(ys.size());
        auto morphAt = [&](int X, int Y) {
            if (k + 1 >= lodLevels_) return hAt(X, Y);
            const int S = 2 * s;
            int X0 = (X / S) * S, Y0 = (Y / S) * S;
            if (X0 == X && Y0 == Y) return hAt(X, Y);
            int X1 = std::min(X0 + S, W), Y1 = std::min(Y0 + S, H);
            float fx = X1 > X0 ? float(X - X0) / float(X1 - X0) : 0.0f, fz = Y1 > Y0 ? float(Y - Y0) / float(Y1 - Y0) : 0.0f;
            return terrain::quadSurface(hAt(X0, Y0), hAt(X1, Y0), hAt(X0, Y1), hAt(X1, Y1), fx, fz);
        };
        v.reserve(size_t(nx * ny) + size_t(nx + ny) * 4);
        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i) {
                int X = xs[size_t(i)], Y = ys[size_t(j)];
                float3 p{float(X), hAt(X, Y), float(Y)};
                if (k == 0) { bmin = min3(bmin, p); bmax = max3(bmax, p); }
                v.push_back({p, normalAt(X, Y), colorAt(X, Y), {morphAt(X, Y), 0}});
            }
        auto vi = [&](int i, int j) { return uint32_t(j * nx + i); };
        for (int j = 0; j + 1 < ny; ++j)
            for (int i = 0; i + 1 < nx; ++i) {
                // diagonal (0,0)-(1,1), counter-clockwise seen from above (+y): a, d, b and a, c, d
                uint32_t a = vi(i, j), b = vi(i + 1, j), cc = vi(i, j + 1), d = vi(i + 1, j + 1);
                idx.insert(idx.end(), {a, d, b, a, cc, d});
            }
        // skirts: hide cracks between chunks on different LOD levels
        if (lodLevels_ > 1) {
            const float depth = 0.3f + 0.25f * float(s);
            auto skirt = [&](uint32_t ia, uint32_t ib, float3 out) {
                TV a = v[ia], b = v[ib];
                TV a2 = a, b2 = b;
                a2.pos.y -= depth; b2.pos.y -= depth; a2.morph[0] -= depth; b2.morph[0] -= depth;
                uint32_t base = uint32_t(v.size());
                v.push_back(a); v.push_back(b); v.push_back(a2); v.push_back(b2);
                float3 e = b.pos - a.pos, dn{0, -1, 0};
                if (dot(cross(e, dn), out) >= 0) idx.insert(idx.end(), {base, base + 2, base + 1, base + 1, base + 2, base + 3});
                else idx.insert(idx.end(), {base, base + 1, base + 2, base + 1, base + 3, base + 2});
            };
            for (int i = 0; i + 1 < nx; ++i) { skirt(vi(i, 0), vi(i + 1, 0), {0, 0, -1}); skirt(vi(i, ny - 1), vi(i + 1, ny - 1), {0, 0, 1}); }
            for (int j = 0; j + 1 < ny; ++j) { skirt(vi(0, j), vi(0, j + 1), {-1, 0, 0}); skirt(vi(nx - 1, j), vi(nx - 1, j + 1), {1, 0, 0}); }
        }
        // walls down the map border to the apron
        auto border = [&](int X0, int Y0, int X1, int Y1, float3 n, int cx, int cy) {
            float h0 = hAt(X0, Y0), h1 = hAt(X1, Y1);
            if (h0 <= 0.0f && h1 <= 0.0f) return;
            uint32_t wc = wallColor(cx, cy);
            uint32_t base = uint32_t(v.size());
            v.push_back({{float(X0), h0, float(Y0)}, n, wc, {h0, 0}});
            v.push_back({{float(X1), h1, float(Y1)}, n, wc, {h1, 0}});
            v.push_back({{float(X0), kApronY, float(Y0)}, n, wc, {kApronY, 0}});
            v.push_back({{float(X1), kApronY, float(Y1)}, n, wc, {kApronY, 0}});
            float3 e = float3{float(X1 - X0), 0, float(Y1 - Y0)};
            if (dot(cross(e, float3{0, -1, 0}), n) >= 0) idx.insert(idx.end(), {base, base + 2, base + 1, base + 1, base + 2, base + 3});
            else idx.insert(idx.end(), {base, base + 1, base + 2, base + 1, base + 3, base + 2});
        };
        if (k == 0) {
            for (int x = c.x0; x < c.x1; ++x) {
                if (c.y0 == 0) border(x, 0, x + 1, 0, {0, 0, -1}, x, 0);
                if (c.y1 == H) border(x, H, x + 1, H, {0, 0, 1}, x, H - 1);
            }
            for (int y = c.y0; y < c.y1; ++y) {
                if (c.x0 == 0) border(0, y, 0, y + 1, {-1, 0, 0}, 0, y);
                if (c.x1 == W) border(W, y, W, y + 1, {1, 0, 0}, W - 1, y);
            }
        } else {
            for (int i = 0; i + 1 < nx; ++i) {
                if (c.y0 == 0) border(xs[size_t(i)], 0, xs[size_t(i + 1)], 0, {0, 0, -1}, xs[size_t(i)], 0);
                if (c.y1 == H) border(xs[size_t(i)], H, xs[size_t(i + 1)], H, {0, 0, 1}, xs[size_t(i)], H - 1);
            }
            for (int j = 0; j + 1 < ny; ++j) {
                if (c.x0 == 0) border(0, ys[size_t(j)], 0, ys[size_t(j + 1)], {-1, 0, 0}, 0, ys[size_t(j)]);
                if (c.x1 == W) border(W, ys[size_t(j)], W, ys[size_t(j + 1)], {1, 0, 0}, W - 1, ys[size_t(j)]);
            }
        }
        upload(dev, v, idx, c.lods[size_t(k)]);
    }
    bmin.y = std::min(bmin.y, kApronY) - 1.0f;   // skirts
    c.bmin = bmin; c.bmax = bmax;
}

// Ground around the map at the apron level, so the edge does not look cut off.
void TerrainRenderer::buildApron(gfx::Device& dev, const MapGrid& g) {
    const float W = float(g.w), H = float(g.h), A = float(kApron);
    std::vector<TV> v;
    std::vector<uint32_t> idx;
    const uint32_t col = rgbaf({0.30f, 0.45f, 0.24f, 1}); // flat: per-cell noise would show as a checkerboard
    auto rect = [&](float x0, float z0, float x1, float z1) {
        uint32_t b = uint32_t(v.size());
        for (float3 p : {float3{x0, kApronY, z0}, float3{x1, kApronY, z0}, float3{x0, kApronY, z1}, float3{x1, kApronY, z1}})
            v.push_back({p, {0, 1, 0}, col, {kApronY, 0}});
        idx.insert(idx.end(), {b, b + 2, b + 1, b + 1, b + 2, b + 3});
    };
    rect(-A, -A, W + A, 0);
    rect(-A, H, W + A, H + A);
    rect(-A, 0, 0, H);
    rect(W, 0, W + A, H);
    upload(dev, v, idx, apron_);
}

// Water. Sheet (classic): one translucent plane slightly below ground level, visible where the
// terrain dips. Bodies: every connected water area gets its own surface just below its lowest shore
// corner, so lakes on plateaus and heightfield valleys hold water at their own level.
void TerrainRenderer::buildWater(gfx::Device& dev, const MapGrid& g, bool flatDig) {
    dev.destroyBuffer(water_.vb); dev.destroyBuffer(water_.ib);
    water_ = {};
    hasWater_ = false;
    const int W = g.w, H = g.h;
    for (auto f : g.flags) if ((f & kCellWater) && !(f & kCellGround)) { hasWater_ = true; break; }
    waterSheet_ = look_.water == TerrainLook::Water::Sheet || (look_.water == TerrainLook::Water::Auto && !heightfield_);
    MeshBuilder wb;
    wb.color(rgba(255, 255, 255));
    if (waterSheet_) {
        if (!hasWater_) return;
        const float a = float(kApron), y = look_.waterOffset;
        wb.quad({-a, y, -a}, {-a, y, float(H) + a}, {float(W) + a, y, float(H) + a}, {float(W) + a, y, -a});
        water_ = wb.upload(dev);
        return;
    }
    std::vector<int> body(size_t(W * H), -1);
    std::vector<int> stack;
    int bodies = 0;
    auto isWater = [&](int x, int y) { return (g.flags[size_t(g.index(x, y))] & kCellWater) != 0; };
    for (int y0 = 0; y0 < H; ++y0)
        for (int x0 = 0; x0 < W; ++x0) {
            if (!isWater(x0, y0) || body[size_t(g.index(x0, y0))] >= 0) continue;
            const int id = bodies++;
            std::vector<int> cells;
            bool deep = false;
            float rim = 1e9f, top = -1e9f;
            stack.assign(1, g.index(x0, y0));
            body[size_t(g.index(x0, y0))] = id;
            while (!stack.empty()) {
                int i = stack.back(); stack.pop_back();
                cells.push_back(i);
                int x = i % W, y = i / W;
                if (!(g.flags[size_t(i)] & kCellGround)) deep = true;
                for (auto [dx, dy] : {std::pair{1, 0}, std::pair{-1, 0}, std::pair{0, 1}, std::pair{0, -1}}) {
                    int nx = x + dx, ny = y + dy;
                    if (!g.inside(nx, ny)) continue;
                    size_t ni = size_t(g.index(nx, ny));
                    if (isWater(nx, ny)) { if (body[ni] < 0) { body[ni] = id; stack.push_back(int(ni)); } continue; }
                    // shore: the ground of the land cell next to the water (its centre; corners dip into the lake)
                    rim = std::min(rim, terrain::heightAt(g, lh_, float(nx) + 0.5f, float(ny) + 0.5f));
                }
                for (int cy = 0; cy <= 1; ++cy) for (int cx = 0; cx <= 1; ++cx) top = std::max(top, terrain::gridCorner(g, lh_, x + cx, y + cy, flatDig));
            }
            if (!deep && look_.water == TerrainLook::Water::Auto && !heightfield_) continue; // shallows alone: no surface
            const float y = (rim < 1e8f ? rim : top) + look_.waterOffset;
            // rows of cells merged into runs
            std::sort(cells.begin(), cells.end());
            for (size_t i = 0; i < cells.size();) {
                size_t j = i + 1;
                while (j < cells.size() && cells[j] == cells[j - 1] + 1 && cells[j] / W == cells[i] / W) ++j;
                float x0f = float(cells[i] % W), x1f = float(cells[j - 1] % W + 1), z0 = float(cells[i] / W), z1 = z0 + 1;
                wb.quad({x0f, y, z0}, {x0f, y, z1}, {x1f, y, z1}, {x1f, y, z0});
                i = j;
            }
        }
    hasWater_ = !wb.empty();
    if (hasWater_) water_ = wb.upload(dev);
}

// ------------------------------------------------------------------ per-cell textures

void TerrainRenderer::updateCellTextures(gfx::Device& dev, const MapGrid& g, bool flatDig) {
    const int W = g.w, H = g.h;
    if (texW_ != W || texH_ != H) {
        for (auto* t : {&cellTex_, &dynTex_, &splatTex_, &visionTex_}) if (t->valid()) { dev.destroyTexture(*t); *t = {}; }
        texW_ = W; texH_ = H;
        cellPx_.assign(size_t(W * H) * 4, 0);
        dynPx_.assign(size_t(W * H) * 4, 0);
        splatPx_.assign(size_t(W * H) * 4, 0);
        visionPx_.assign(size_t(W * H), 255);
        gfx::TextureDesc td; td.width = W; td.height = H; td.filter = gfx::Filter::Nearest;
        td.data = cellPx_.data(); td.label = "terrain cells"; cellTex_ = dev.createTexture(td);
        td.data = dynPx_.data(); td.label = "terrain dynamic"; dynTex_ = dev.createTexture(td);
        td.filter = gfx::Filter::Linear;
        td.data = splatPx_.data(); td.label = "terrain splat"; splatTex_ = dev.createTexture(td);
        td.format = gfx::PixelFormat::R8; td.filter = gfx::Filter::Nearest;
        td.data = visionPx_.data(); td.label = "terrain vision"; visionTex_ = dev.createTexture(td);
        visionInit_ = false;
    }
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            size_t i = size_t(y * W + x);
            float3 c = cellColor(g, x, y, flatDig);
            uint32_t px = rgbaf({c.x, c.y, c.z, 1});
            std::memcpy(&cellPx_[i * 4], &px, 4);
            int layer = i < materialLayer_.size() && materialLayer_[i] < 4 ? materialLayer_[i] : look_.cellLayer[kindOf(g, i, flatDig)];
            layer = std::clamp(layer, 0, std::max(0, int(look_.layers.size()) - 1));
            for (int k = 0; k < 4; ++k) splatPx_[i * 4 + size_t(k)] = k == layer ? 255 : 0;
        }
    dev.updateTexture(cellTex_, 0, 0, W, H, cellPx_.data(), W * 4);
    dev.updateTexture(splatTex_, 0, 0, W, H, splatPx_.data(), W * 4);
}

// Resource fields and creep: compared against the last upload, a band of rows per call (the whole
// map when `all`), only changed rows are uploaded.
void TerrainRenderer::updateDynamic(gfx::Device& dev, const MapGrid& g, bool all) {
    const int W = g.w, H = g.h;
    if (!dynTex_.valid() || texW_ != W || texH_ != H) return;
    const int rows = all ? H : std::clamp(65536 / std::max(W, 1), 1, H);
    int lo = H, hi = -1;
    for (int r = 0; r < rows; ++r) {
        int y = all ? r : (dynRow_ + r) % H;
        bool changed = false;
        for (int x = 0; x < W; ++x) {
            size_t i = size_t(y * W + x);
            uint8_t px[4] = {0, 0, 0, 255};
            if (g.fieldRes[i] >= 0 && g.fieldAmt[i].raw > 0) {
                float k = std::min(1.0f, float(g.fieldAmt[i].toDouble()) / 60.0f) * (0.55f + noiseOf(x, y) * 0.25f);
                px[0] = uint8_t(std::clamp(k * 255.0f + 0.5f, 1.0f, 255.0f));
                px[1] = uint8_t((g.fieldRes[i] % 3) * 127);
            }
            if (g.creep[i]) px[2] = 255;
            if (std::memcmp(&dynPx_[i * 4], px, 4) != 0) { std::memcpy(&dynPx_[i * 4], px, 4); changed = true; }
        }
        if (changed) { lo = std::min(lo, y); hi = std::max(hi, y); }
    }
    if (!all) dynRow_ = (dynRow_ + rows) % H;
    if (hi >= lo) dev.updateTexture(dynTex_, 0, lo, W, hi - lo + 1, &dynPx_[size_t(lo * W) * 4], W * 4);
}

void TerrainRenderer::updateVision(gfx::Device& dev, const std::vector<uint8_t>& vision) {
    if (!visionTex_.valid() || vision.size() != visionPx_.size()) return;
    const int W = texW_, H = texH_;
    int lo = H, hi = -1;
    for (int y = 0; y < H; ++y) {
        uint8_t* dst = &visionPx_[size_t(y * W)];
        const uint8_t* src = &vision[size_t(y * W)];
        bool changed = !visionInit_;
        for (int x = 0; x < W; ++x) {
            uint8_t v = src[x] >= 2 ? 255 : src[x] == 1 ? 128 : 0;
            if (dst[x] != v) { dst[x] = v; changed = true; }
        }
        if (changed) { lo = std::min(lo, y); hi = std::max(hi, y); }
    }
    visionInit_ = true;
    if (hi >= lo) dev.updateTexture(visionTex_, 0, lo, W, hi - lo + 1, &visionPx_[size_t(lo * W)], W);
}

// ------------------------------------------------------------------ draw

int TerrainRenderer::tierFor(const Renderer3D& r) const {
    if (!look_.materials()) return 0;   // no materials: vertex colours on every tier
    if (const char* e = std::getenv("RTS_TERRAIN")) {
        std::string v = e;
        return v == "low" ? 0 : v == "medium" ? 1 : 2;
    }
    // the renderer's model material tier, capped by the device default (weak Mali / Adreno / PowerVR
    // get vertex colours, other phones splat without normal maps) - one quality switch for both
    auto tier = [](MaterialQuality q) { return q == MaterialQuality::Low ? 0 : q == MaterialQuality::Medium ? 1 : 2; };
    static const int deviceTier = dev_ ? tier(Renderer3D::defaultMaterialQualityFor(*dev_, platform::isTouchDevice())) : 2;
    return std::min(tier(r.materialQuality()), deviceTier);
}

gfx::PipelineHandle TerrainRenderer::pipeline(gfx::Device& dev, int tier) {
    for (; tier >= 0; --tier) {
        if (pipes_[tier].valid()) return pipes_[tier];
        if (shaderFailed_[tier]) continue;
        const std::string def = "#define TERRAIN_TIER " + std::to_string(tier) + "\n";
        const std::string vs = def + shaders::kTerrainVS, fs = def + shaders::kTerrainFS, msl = def + shaders::kTerrainMSL;
        gfx::ShaderDesc sd;
        sd.glslVertex = vs.c_str(); sd.glslFragment = fs.c_str(); sd.msl = msl.c_str();
        sd.uniformSlots = 3; sd.textureSlots = 16; sd.label = "terrain";
        shaders_[tier] = dev.createShader(sd);
        if (!shaders_[tier].valid()) {
            std::fprintf(stderr, "[terrain] tier %d shader: %s\n", tier, dev.lastError().c_str());
            shaderFailed_[tier] = true;
            continue;
        }
        gfx::PipelineDesc p;
        p.shader = shaders_[tier];
        p.attrs[0] = {0, gfx::VertexFormat::Float3, 0};
        p.attrs[1] = {1, gfx::VertexFormat::Float3, 12};
        p.attrs[2] = {2, gfx::VertexFormat::UByte4N, 24};
        p.attrs[3] = {3, gfx::VertexFormat::Float2, 28};
        p.attrCount = 4; p.stride = sizeof(TV);
        p.indexType = gfx::IndexType::UInt32;
        p.label = "terrain";
        pipes_[tier] = dev.createPipeline(p);
        if (pipes_[tier].valid()) return pipes_[tier];
        shaderFailed_[tier] = true;
    }
    return {};
}

void TerrainRenderer::draw(Renderer3D& r, const MapGrid& g, const std::vector<uint8_t>* vision, bool) {
    if (!built_ || !dev_ || g.w != mapW_ || g.h != mapH_) return;
    gfx::Device& dev = *dev_;
    updateDynamic(dev, g, false);
    const bool fog = heightfield_ && vision && !vision->empty();
    if (fog) updateVision(dev, *vision);
    // frustum: left/right/bottom/top planes of the view-projection (independent of the clip depth range)
    const mat4& vp = r.viewProj();
    float4 planes[4];
    for (int i = 0; i < 4; ++i) {
        int row = i / 2;
        float s = (i % 2) ? -1.0f : 1.0f;
        planes[i] = {vp.at(3, 0) + s * vp.at(row, 0), vp.at(3, 1) + s * vp.at(row, 1), vp.at(3, 2) + s * vp.at(row, 2), vp.at(3, 3) + s * vp.at(row, 3)};
    }
    auto visible = [&](float3 mn, float3 mx) {
        for (auto& p : planes) {
            float3 q{p.x >= 0 ? mx.x : mn.x, p.y >= 0 ? mx.y : mn.y, p.z >= 0 ? mx.z : mn.z};
            if (p.x * q.x + p.y * q.y + p.z * q.z + p.w < 0) return false;
        }
        return true;
    };
    const float3 eye = r.camera().eye();
    struct Item { const Lod* lod; float morphStart, morphEnd; };
    auto items = std::make_shared<std::vector<Item>>();
    stats_.drawn = 0;
    stats_.triangles = apron_.indexCount / 3;
    for (auto& c : chunks_) {
        if (!c.valid || c.lods.empty() || !visible(c.bmin, c.bmax)) continue;
        int k = 0;
        float start = 1e9f, end = 2e9f;
        if (lodLevels_ > 1) {
            float3 q = max3(c.bmin, min3(eye, c.bmax));
            float d = length(q - eye);
            float range = look_.lodRange;
            while (k + 1 < lodLevels_ && d >= range) { ++k; range *= 2; }
            if (k + 1 < lodLevels_) { start = range * 0.75f; end = range; }
        }
        const Lod& l = c.lods[size_t(k)];
        if (!l.indexCount) continue;
        items->push_back({&l, start, end});
        ++stats_.drawn;
        stats_.triangles += l.indexCount / 3;
    }
    const int tier = tierFor(r);
    gfx::PipelineHandle pipe = pipeline(dev, tier);
    if (!pipe.valid()) return;
    TU0 u{};
    u.lod[2] = float(mapW_); u.lod[3] = float(mapH_);
    u.mode[0] = 0.05f;
    u.mode[1] = tier >= 1 && look_.materials() ? 1.0f : 0.0f;
    u.mode[2] = tier >= 2 ? 1.0f : 0.0f;
    u.mode[3] = fog ? 1.0f : 0.0f;
    for (int i = 0; i < 4; ++i) {
        const TerrainLook::Layer* L = i < int(look_.layers.size()) ? &look_.layers[size_t(i)] : nullptr;
        u.scale[i] = L ? L->scale : 4.0f;
        u.tint[i][0] = L ? L->tint.x : 1; u.tint[i][1] = L ? L->tint.y : 1; u.tint[i][2] = L ? L->tint.z : 1;
        u.tint[i][3] = layerNrm_[i].valid() ? 1.0f : 0.0f;
    }
    u.cliff[0] = look_.cliffScale; u.cliff[1] = look_.splatStrength;
    u.cliff[2] = cliffTex_.valid() ? look_.slopeStart : 2.0f; u.cliff[3] = cliffTex_.valid() ? look_.slopeEnd : 3.0f; // no cliff texture: never
    u.cliffTint[0] = look_.cliffTint.x; u.cliffTint[1] = look_.cliffTint.y; u.cliffTint[2] = look_.cliffTint.z;
    u.cliffTint[3] = cliffNrm_.valid() ? 1.0f : 0.0f;
    u.fog[0] = 120.0f / 255.0f; u.fog[1] = 235.0f / 255.0f;
    const Lod* apron = apron_.indexCount ? &apron_ : nullptr;
    r.drawCustomOpaque([this, &r, items, u, pipe, apron](gfx::Device& d) {
        d.setPipeline(pipe);
        r.bindLitFrame();
        d.setTexture(2, cellTex_); d.setTexture(3, dynTex_); d.setTexture(4, splatTex_);
        for (int i = 0; i < 4; ++i) {
            d.setTexture(5 + i, layerTex_[i].valid() ? layerTex_[i].tex : white_);
            d.setTexture(9 + i, layerNrm_[i].valid() ? layerNrm_[i].tex : flatN_);
        }
        d.setTexture(13, cliffTex_.valid() ? cliffTex_.tex : gray_);
        d.setTexture(14, cliffNrm_.valid() ? cliffNrm_.tex : flatN_);
        d.setTexture(15, visionTex_);
        TU0 uu = u;
        auto drawLod = [&](const Lod& l, float s, float e) {
            uu.lod[0] = s; uu.lod[1] = e;
            d.setUniforms(0, &uu, sizeof uu);
            d.setVertexBuffer(l.vb);
            d.setIndexBuffer(l.ib);
            d.drawIndexed(l.indexCount);
        };
        if (apron) drawLod(*apron, 1e9f, 2e9f);
        for (auto& it : *items) drawLod(*it.lod, it.morphStart, it.morphEnd);
    });
    if (hasWater_ && water_.indexCount) r.drawMesh(water_, mat4{}, look_.waterColor, kDrawNoCastShadow, 0.9f);
    // RTS_TERRAIN_STATS=1: chunks / triangles / frame time every 60 frames (stderr; perf checks on big maps)
    static const bool printStats = std::getenv("RTS_TERRAIN_STATS") != nullptr;
    if (printStats) {
        static auto last = std::chrono::steady_clock::now();
        static int frames = 0;
        if (++frames == 60) {
            auto now = std::chrono::steady_clock::now();
            double ms = std::chrono::duration<double, std::milli>(now - last).count() / frames;
            std::fprintf(stderr, "[terrain] map %dx%d chunks %d drawn %d lods %d tris %llu frame %.2f ms build %.1f ms\n", mapW_, mapH_,
                         stats_.chunks, stats_.drawn, lodLevels_, (unsigned long long)stats_.triangles, ms, stats_.buildMs);
            last = now; frames = 0;
        }
    }
}

} // namespace rts::game
