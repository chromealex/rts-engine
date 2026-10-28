#include "RtsGroundFx.h"
#include "RtsGroundFxShaders.h"
#include "RtsView.h"
#include "../RtsModule.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace rts::game {

using namespace render;

namespace {
constexpr int kChunk = 32;
constexpr float kTerrY = 0.045f, kRoadY = 0.06f, kLaneY = 0.075f;
enum { kDirtyT = 1, kDirtyD = 2, kDirtyM = 4, kDirtyR = 8 };

struct GU0 { float color[4]; float params[4]; float params2[4]; float rect[4]; };

uint32_t hash2(int x, int y, uint32_t s = 0) {
    uint32_t h = uint32_t(x) * 73856093u ^ uint32_t(y) * 19349663u ^ s * 83492791u;
    h ^= h >> 13; h *= 0x5bd1e995u; return h ^ (h >> 15);
}
float rnd(int x, int y, uint32_t s) { return float(hash2(x, y, s) & 0xffff) / 65535.0f; }
float num(const Json& j, const char* k, float def) { const Json& v = j[k]; return v.isNumber() ? float(v.fx().toDouble()) : def; }
bool col(const Json& c, float3& out) {
    if (c.isArray() && c.size() >= 3) { out = {float(c[0].fx().toDouble()), float(c[1].fx().toDouble()), float(c[2].fx().toDouble())}; return true; }
    if (c.isString()) {
        std::string s = c.str();
        if (s.size() == 7 && s[0] == '#') {
            unsigned v = unsigned(std::strtoul(s.c_str() + 1, nullptr, 16));
            out = {float((v >> 16) & 255) / 255.0f, float((v >> 8) & 255) / 255.0f, float(v & 255) / 255.0f};
            return true;
        }
    }
    return false;
}
void colKey(const Json& j, const char* k, float3& out) { col(j[k], out); }
uint32_t c4(float3 c, float a) { return rgbaf({c.x, c.y, c.z, a}); }

// triangle with the winding fixed so that its front face points along n
void tri(MeshBuilder& b, float3 p0, float3 p1, float3 p2, float3 n, uint32_t c0, uint32_t c1, uint32_t c2) {
    uint32_t i = b.addVertex(p0, n, c0), j = b.addVertex(p1, n, c1), k = b.addVertex(p2, n, c2);
    if (dot(cross(p1 - p0, p2 - p0), n) >= 0) b.addTriangle(i, j, k); else b.addTriangle(i, k, j);
}
// quad p0 p1 p2 p3 in order around the rim
void quad(MeshBuilder& b, float3 p0, float3 p1, float3 p2, float3 p3, float3 n, uint32_t c0, uint32_t c1, uint32_t c2, uint32_t c3) {
    uint32_t i0 = b.addVertex(p0, n, c0), i1 = b.addVertex(p1, n, c1), i2 = b.addVertex(p2, n, c2), i3 = b.addVertex(p3, n, c3);
    if (dot(cross(p1 - p0, p2 - p0), n) >= 0) { b.addTriangle(i0, i1, i2); b.addTriangle(i0, i2, i3); }
    else { b.addTriangle(i0, i2, i1); b.addTriangle(i0, i3, i2); }
}
void quad(MeshBuilder& b, float3 p0, float3 p1, float3 p2, float3 p3, float3 n, uint32_t c) { quad(b, p0, p1, p2, p3, n, c, c, c, c); }

float3 roomColor(int idx) {
    static const float3 pal[] = {{0.62f, 0.32f, 0.22f}, {0.80f, 0.66f, 0.30f}, {0.30f, 0.42f, 0.70f}, {0.55f, 0.30f, 0.55f},
                                 {0.30f, 0.58f, 0.42f}, {0.70f, 0.42f, 0.30f}, {0.42f, 0.42f, 0.48f}, {0.66f, 0.30f, 0.36f}};
    return pal[size_t(idx) % 8];
}

// small tileable noise textures (default look; games may set their own through TextureCache paths)
std::vector<uint32_t> makeNoise(int n, bool cobble) {
    std::vector<uint32_t> px(size_t(n * n));
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            float v;
            if (cobble) {
                // cells of a jittered 4x4 grid: stones with darker joints
                float best = 9, second = 9;
                for (int gy = -1; gy <= 4; ++gy)
                    for (int gx = -1; gx <= 4; ++gx) {
                        int wx = (gx + 4) % 4, wy = (gy + 4) % 4;
                        float px_ = (float(gx) + 0.2f + 0.6f * rnd(wx, wy, 7)) * float(n) / 4.0f;
                        float py_ = (float(gy) + 0.2f + 0.6f * rnd(wx, wy, 9)) * float(n) / 4.0f;
                        float d = std::hypot(float(x) - px_, float(y) - py_);
                        if (d < best) { second = best; best = d; } else if (d < second) second = d;
                    }
                float edge = std::clamp((second - best) / (float(n) * 0.05f), 0.0f, 1.0f);
                v = 0.62f + 0.38f * edge - 0.08f * rnd(x, y, 3);
            } else {
                v = 0.86f + 0.14f * rnd(x / 2, y / 2, 5) - 0.06f * rnd(x, y, 11);
            }
            uint8_t q = uint8_t(std::clamp(v, 0.0f, 1.0f) * 255.0f);
            px[size_t(y * n + x)] = rgba(q, q, q, 255);
        }
    return px;
}

bool boxVisible(const mat4& vp, float3 mn, float3 mx) {
    int out[6] = {};
    for (int i = 0; i < 8; ++i) {
        float4 p = vp * float4{(i & 1) ? mx.x : mn.x, (i & 2) ? mx.y : mn.y, (i & 4) ? mx.z : mn.z, 1};
        out[0] += p.x < -p.w; out[1] += p.x > p.w; out[2] += p.y < -p.w; out[3] += p.y > p.w; out[4] += p.w < 0.0f;
    }
    for (int k = 0; k < 5; ++k) if (out[k] == 8) return false;
    return true;
}
} // namespace

// ------------------------------------------------------------------ setup

void RtsGroundFx::init(gfx::Device& dev, const Rules& rules, const FileReader& read, const std::string& contentBase) {
    if (dev_) shutdown(*dev_);
    dev_ = &dev;
    rules_ = &rules;
    base_ = contentBase;
    textures_.init(dev);
    terr_ = {}; dig_ = {}; lane_ = {}; road_ = {};
    // defaults per diggable kind: no yield = plain rock, a yield = gold vein, infinite yield = gem seam
    for (const DiggableDef& d : rules.diggables) {
        DigKind k;
        k.rock = {0.40f, 0.35f, 0.30f};
        if (d.yields.empty()) k.style = 0;
        else if (d.infinite) { k.style = 2; k.vein = {0.35f, 0.85f, 0.95f}; k.glow = 0.55f; }
        else { k.style = 1; k.vein = {0.98f, 0.76f, 0.22f}; k.glow = 0.25f; }
        dig_.kinds.push_back(k);
    }
    // the visual `terrain` block: rules file first, game.json overrides per key
    if (read) {
        std::string text, rulesPath = "rules.json";
        Json game;
        try { if (read("game.json", text)) { game = Json::parse(text); rulesPath = game.getStr("rules", "rules.json"); } } catch (...) {}
        try { if (read(rulesPath, text)) parseLook(Json::parse(text)["terrain"]); } catch (...) {}
        if (game.isObject()) parseLook(game["terrain"]);
    }
    useT_ = rules.territory.enabled || !rules.rooms.empty();
    useD_ = !rules.diggables.empty();
    useR_ = rules.roadNetwork.enabled; // or a map with '+' cells (resetGrid)
    loadTex(terr_.tex); loadTex(lane_.tex); loadTex(road_.tex);

    gfx::ShaderDesc sd;
    sd.glslVertex = shaders::kGroundFxVS; sd.glslFragment = shaders::kGroundFxFS; sd.msl = shaders::kGroundFxMSL;
    sd.uniformSlots = 3; sd.textureSlots = 3; sd.label = "groundfx";
    shader_ = dev.createShader(sd);
    if (shader_.valid()) {
        gfx::PipelineDesc p;
        p.shader = shader_;
        p.attrs[0] = {0, gfx::VertexFormat::Float3, 0};
        p.attrs[1] = {1, gfx::VertexFormat::Float3, 12};
        p.attrs[2] = {2, gfx::VertexFormat::UByte4N, 24};
        p.attrCount = 3; p.stride = sizeof(MeshVertex);
        p.indexType = gfx::IndexType::UInt32;
        p.cull = gfx::CullMode::None;
        p.blend = gfx::BlendMode::Alpha;
        p.depthWrite = false;
        p.depthBiasConstant = -1.0f; p.depthBiasSlope = -1.0f;   // decals win the depth test against the ground
        p.label = "groundfx";
        pipe_ = dev.createPipeline(p);
    }
    const uint32_t w = 0xffffffffu;
    gfx::TextureDesc td; td.width = 1; td.height = 1; td.data = &w; td.label = "groundfx white";
    white_ = dev.createTexture(td);
    invalid_ = true;
}

void RtsGroundFx::loadTex(Tex& t) {
    t.ref = {};
    if (!t.path.empty()) t.ref = textures_.resolve(t.path, false, base_);
}

void RtsGroundFx::parseLook(const Json& t) {
    if (!t.isObject()) return;
    if (const Json& j = t["territory"]; !j.isNull()) {
        if (!j.isObject()) terr_.on = j.boolean(true);
        else {
            terr_.on = j.getBool("enabled", true);
            terr_.fill = num(j, "fill", terr_.fill);
            terr_.border = num(j, "border", terr_.border);
            terr_.glow = num(j, "glow", terr_.glow);
            terr_.pulse = j.getBool("pulse", terr_.pulse);
            terr_.roomFill = num(j, "roomFill", terr_.roomFill);
            if (const Json& c = j["colors"]; c.isArray()) {
                terr_.colors.clear();
                for (auto& e : c.items()) { float3 v{-1, -1, -1}; col(e, v); terr_.colors.push_back(v); }
            }
            for (auto& [id, c] : j["rooms"].members()) { float3 v; if (col(c, v)) terr_.rooms[id] = v; }
            terr_.tex.path = j.getStr("texture", terr_.tex.path);
            terr_.tex.scale = num(j, "textureScale", terr_.tex.scale);
            terr_.tex.strength = num(j, "textureStrength", terr_.tex.strength);
        }
    }
    if (const Json& j = t["dig"]; !j.isNull()) {
        if (!j.isObject()) dig_.on = j.boolean(true);
        else {
            dig_.on = j.getBool("enabled", true);
            dig_.height = std::clamp(num(j, "height", dig_.height), 0.2f, 1.25f);
            dig_.progress = j.getBool("progress", dig_.progress);
            colKey(j, "mark", dig_.mark);
            for (auto& [key, k] : j["kinds"].members()) {
                int i = key.size() == 1 && rules_ ? rules_->findDiggable(key[0]) : -1;
                if (i < 0 || size_t(i) >= dig_.kinds.size() || !k.isObject()) continue;
                DigKind& d = dig_.kinds[size_t(i)];
                colKey(k, "rock", d.rock);
                colKey(k, "vein", d.vein);
                std::string st = k.getStr("style");
                if (st == "plain") d.style = 0; else if (st == "vein") d.style = 1; else if (st == "gem") d.style = 2;
                d.glow = num(k, "glow", d.glow);
            }
        }
    }
    auto texKeys = [](const Json& j, Tex& x) {
        x.path = j.getStr("texture", x.path);
        x.scale = std::max(0.05f, num(j, "textureScale", x.scale));
        x.strength = num(j, "textureStrength", x.strength);
    };
    if (const Json& j = t["lanes"]; !j.isNull()) {
        if (!j.isObject()) lane_.on = j.boolean(true);
        else {
            lane_.on = j.getBool("enabled", true);
            colKey(j, "color", lane_.color); colKey(j, "edge", lane_.edge);
            lane_.alpha = num(j, "alpha", lane_.alpha);
            lane_.width = num(j, "width", lane_.width);
            lane_.sideLanes = j.getBool("sideLanes", lane_.sideLanes);
            lane_.all = j.getBool("all", lane_.all);
            texKeys(j, lane_.tex);
        }
    }
    if (const Json& j = t["roads"]; !j.isNull()) {
        if (!j.isObject()) road_.on = j.boolean(true);
        else {
            road_.on = j.getBool("enabled", true);
            colKey(j, "color", road_.color); colKey(j, "edge", road_.edge);
            road_.width = std::clamp(num(j, "width", road_.width), 0.2f, 1.0f);
            road_.alpha = num(j, "alpha", road_.alpha);
            road_.cobbles = j.getBool("cobbles", road_.cobbles);
            texKeys(j, road_.tex);
        }
    }
}

void RtsGroundFx::freeMesh(Mesh& m) {
    if (!dev_) return;
    if (m.vb.valid()) dev_->destroyBuffer(m.vb);
    if (m.ib.valid()) dev_->destroyBuffer(m.ib);
    m = Mesh{};
}

void RtsGroundFx::setMesh(Mesh& m, const MeshBuilder& b) {
    freeMesh(m);
    if (!b.empty()) m = b.upload(*dev_);
}

void RtsGroundFx::shutdown(gfx::Device& dev) {
    dev_ = &dev;
    for (Chunk& c : chunks_)
        for (Mesh* m : {&c.fill, &c.border, &c.rock, &c.veins, &c.gems, &c.marks, &c.road}) freeMesh(*m);
    chunks_.clear();
    freeMesh(lanes_);
    if (pipe_.valid()) dev.destroyPipeline(pipe_);
    if (shader_.valid()) dev.destroyShader(shader_);
    if (white_.valid()) dev.destroyTexture(white_);
    pipe_ = {}; shader_ = {}; white_ = {};
    textures_.shutdown();
    dev_ = nullptr;
    invalid_ = true;
}

float3 RtsGroundFx::ownerColor(int slot) const {
    if (slot >= 1 && size_t(slot - 1) < terr_.colors.size() && terr_.colors[size_t(slot - 1)].x >= 0) return terr_.colors[size_t(slot - 1)];
    float4 c = RtsView::playerColor(slot);
    return {c.x, c.y, c.z};
}

// ------------------------------------------------------------------ change tracking

void RtsGroundFx::dirty(int x, int y, bool neighbours, int what) {
    auto mark = [&](int cx, int cy) {
        if (cx < 0 || cy < 0 || cx >= gridW_ || cy >= gridH_) return;
        Chunk& c = chunks_[size_t((cy / kChunk) * chunksX_ + cx / kChunk)];
        if (what & kDirtyT) c.dT = true;
        if (what & kDirtyD) c.dD = true;
        if (what & kDirtyM) c.dM = true;
        if (what & kDirtyR) c.dR = true;
    };
    mark(x, y);
    if (neighbours) { mark(x - 1, y); mark(x + 1, y); mark(x, y - 1); mark(x, y + 1); }
}

void RtsGroundFx::resetGrid(const MapGrid& g) {
    for (Chunk& c : chunks_)
        for (Mesh* m : {&c.fill, &c.border, &c.rock, &c.veins, &c.gems, &c.marks, &c.road}) freeMesh(*m);
    gridW_ = g.w; gridH_ = g.h;
    chunksX_ = (g.w + kChunk - 1) / kChunk; chunksY_ = (g.h + kChunk - 1) / kChunk;
    chunks_.assign(size_t(chunksX_ * chunksY_), Chunk{});
    const size_t n = size_t(g.w * g.h);
    // terrain (rock, roads) is known from the start like the map itself; territory and rooms are learned by seeing them
    memT_.assign(n, 0); memRoom_.assign(n, -1); memM_.assign(n, 0);
    memD_.assign(n, 0); memR_.assign(n, 0);
    bool anyRoad = false;
    for (size_t i = 0; i < n; ++i) {
        memD_[i] = i < g.dig.size() ? g.dig[i] : 0;
        memR_[i] = (g.flags[i] & kCellRoad) ? 1 : 0;
        anyRoad |= memR_[i] != 0;
    }
    useR_ = (rules_ && rules_->roadNetwork.enabled) || anyRoad;
    const float lh = rules_ ? float(rules_->levelHeight.toDouble()) : 1.5f;
    int maxLevel = 0;
    for (auto l : g.level) maxLevel = std::max<int>(maxLevel, l);
    for (Chunk& c : chunks_) { c.yMin = -1.0f; c.yMax = float(maxLevel) * lh + 3.0f; }
    marked_.clear();
    laneCount_ = ~size_t(0);
    invalid_ = false;
    nextScan_ = -1;
}

void RtsGroundFx::scan(const MapGrid& g, const std::vector<uint8_t>* vision, uint8_t viewer) {
    const bool fog = vision && vision->size() == g.flags.size();
    const uint8_t bit = viewer >= 1 && viewer <= 8 ? uint8_t(1u << (viewer - 1)) : 0xff;
    const bool hasDig = g.dig.size() == g.flags.size(), hasTerr = g.territory.size() == g.flags.size() && g.room.size() == g.flags.size();
    const bool hasMark = g.digMark.size() == g.flags.size();
    marked_.clear();
    for (int y = 0; y < g.h; ++y)
        for (int x = 0; x < g.w; ++x) {
            const size_t i = size_t(y * g.w + x);
            const uint8_t m = hasMark && hasDig && g.dig[i] ? uint8_t(g.digMark[i] & bit) : 0;
            if (m != memM_[i]) { memM_[i] = m; dirty(x, y, false, kDirtyM); }
            if (m) marked_.push_back(uint32_t(i));
            if (fog && (*vision)[i] != 2) continue;
            if (useT_ && hasTerr) {
                if (g.territory[i] != memT_[i]) { memT_[i] = g.territory[i]; dirty(x, y, true, kDirtyT); }
                if (g.room[i] != memRoom_[i]) { memRoom_[i] = g.room[i]; dirty(x, y, false, kDirtyT); }
            }
            const uint8_t d = hasDig ? g.dig[i] : 0;
            if (d != memD_[i]) { memD_[i] = d; dirty(x, y, true, kDirtyD | kDirtyT | kDirtyM); }
            const uint8_t r = (g.flags[i] & kCellRoad) ? 1 : 0;
            if (r != memR_[i]) { memR_[i] = r; dirty(x, y, true, kDirtyR); }
        }
}

// ------------------------------------------------------------------ territory

void RtsGroundFx::buildTerritory(Chunk& c, int cx, int cy, const MapGrid& g, const RtsView& v) {
    MeshBuilder fb, bb;
    const float3 up{0, 1, 0};
    const float e = 0.002f;
    auto H = [&](float x, float z, int ix, int iz) {
        return v.heightAt(g, std::clamp(x, float(ix) + e, float(ix + 1) - e), std::clamp(z, float(iz) + e, float(iz + 1) - e)) + kTerrY;
    };
    auto P = [&](float x, float z, int ix, int iz) { return float3{x, H(x, z, ix, iz), z}; };
    auto owner = [&](int x, int y) -> int { return g.inside(x, y) ? memT_[size_t(y * g.w + x)] : -1; };
    std::vector<std::string> roomIds;
    if (rules_) for (auto& r : rules_->rooms) roomIds.push_back(r.id);
    const float bw = std::clamp(terr_.border, 0.02f, 0.5f);
    for (int y = cy * kChunk; y < std::min(g.h, (cy + 1) * kChunk); ++y)
        for (int x = cx * kChunk; x < std::min(g.w, (cx + 1) * kChunk); ++x) {
            const size_t i = size_t(y * g.w + x);
            const int o = memT_[i];
            if (o == 0 || memD_[i]) continue;
            const float3 oc = ownerColor(o);
            const float X = float(x), Z = float(y);
            if (terr_.fill > 0.001f) {
                // a touch of per-cell variation reads as claimed floor tiles
                float k = 0.92f + 0.08f * rnd(x, y, 21);
                quad(fb, P(X, Z, x, y), P(X + 1, Z, x, y), P(X + 1, Z + 1, x, y), P(X, Z + 1, x, y), up, c4(oc * k, terr_.fill));
            }
            if (int16_t rm = memRoom_[i]; rm >= 0) {
                float3 rc = roomColor(rm);
                if (size_t(rm) < roomIds.size()) if (auto it = terr_.rooms.find(roomIds[size_t(rm)]); it != terr_.rooms.end()) rc = it->second;
                const float m = 0.06f;
                quad(fb, P(X + m, Z + m, x, y), P(X + 1 - m, Z + m, x, y), P(X + 1 - m, Z + 1 - m, x, y), P(X + m, Z + 1 - m, x, y), up,
                     c4(rc * (0.9f + 0.1f * rnd(x, y, 5)), terr_.roomFill));
            }
            // border strips inside the cell along edges facing another owner
            static const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
            for (int d = 0; d < 4; ++d) {
                int n = owner(x + dx[d], y + dy[d]);
                if (n < 0 || n == o) continue;
                // outer edge a-b (on the cell boundary), inner edge shifted by bw towards the centre
                float3 a, b, ia, ib;
                if (d == 0) { a = P(X + 1, Z, x, y); b = P(X + 1, Z + 1, x, y); ia = P(X + 1 - bw, Z, x, y); ib = P(X + 1 - bw, Z + 1, x, y); }
                else if (d == 1) { a = P(X, Z, x, y); b = P(X, Z + 1, x, y); ia = P(X + bw, Z, x, y); ib = P(X + bw, Z + 1, x, y); }
                else if (d == 2) { a = P(X, Z + 1, x, y); b = P(X + 1, Z + 1, x, y); ia = P(X, Z + 1 - bw, x, y); ib = P(X + 1, Z + 1 - bw, x, y); }
                else { a = P(X, Z, x, y); b = P(X + 1, Z, x, y); ia = P(X, Z + bw, x, y); ib = P(X + 1, Z + bw, x, y); }
                const float3 bright = oc * 0.55f + float3{0.45f, 0.45f, 0.45f};
                quad(bb, a, b, ib, ia, up, c4(bright, 0.95f), c4(bright, 0.95f), c4(oc, 0.0f), c4(oc, 0.0f));
                if (hi_) { // crisp core line on the edge
                    float3 ca = lerp(a, ia, 0.18f), cb = lerp(b, ib, 0.18f);
                    quad(bb, a, b, cb, ca, up, c4({1, 1, 1}, 0.55f));
                }
            }
        }
    setMesh(c.fill, fb);
    setMesh(c.border, bb);
    c.dT = false;
}

// ------------------------------------------------------------------ diggable rock

float RtsGroundFx::digTop(const MapGrid& g, const RtsView& v, int x, int y) const {
    return v.heightAt(g, float(x) + 0.5f, float(y) + 0.5f) + dig_.height;
}

void RtsGroundFx::buildDig(Chunk& c, int cx, int cy, const MapGrid& g, const RtsView& v) {
    MeshBuilder rb, vb, gb;
    auto kindAt = [&](int x, int y) -> int { return g.inside(x, y) ? memD_[size_t(y * g.w + x)] : -1; };
    const float bevel = hi_ ? 0.09f : 0.0f;
    for (int y = cy * kChunk; y < std::min(g.h, (cy + 1) * kChunk); ++y)
        for (int x = cx * kChunk; x < std::min(g.w, (cx + 1) * kChunk); ++x) {
            const int k = memD_[size_t(y * g.w + x)];
            if (!k) continue;
            const DigKind& K = dig_.kinds[std::min(size_t(k - 1), dig_.kinds.size() - 1)];
            const float X = float(x), Z = float(y), top = digTop(g, v, x, y);
            const float n0 = rnd(x, y, 1);
            float3 rock = K.rock * (0.97f + 0.06f * n0);
            if (K.style != 0 && !hi_) rock = lerp(rock, K.vein, 0.35f);   // low quality: vein shown as a tint only
            // walls on edges towards dug / open cells; map edge: no wall
            static const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
            bool wall[4];
            for (int d = 0; d < 4; ++d) wall[d] = kindAt(x + dx[d], y + dy[d]) == 0;
            const float x0 = X + (wall[1] ? bevel : 0), x1 = X + 1 - (wall[0] ? bevel : 0);
            const float z0 = Z + (wall[3] ? bevel : 0), z1 = Z + 1 - (wall[2] ? bevel : 0);
            // continuous per-corner noise (per-cell values read as a checkerboard); vein cells tinted
            auto cornerC = [&](int ix, int iz) {
                float3 c = rock * (0.9f + 0.16f * rnd(ix, iz, 3));
                if (K.style != 0) c = lerp(c, K.vein * 0.8f, 0.18f + 0.22f * rnd(ix, iz, 13));
                return c4(c, 1);
            };
            quad(rb, {x0, top, z0}, {x1, top, z0}, {x1, top, z1}, {x0, top, z1}, {0, 1, 0},
                 cornerC(x, y), cornerC(x + 1, y), cornerC(x + 1, y + 1), cornerC(x, y + 1));
            const float wallTop = top - bevel * 0.9f;
            for (int d = 0; d < 4; ++d) {
                if (!wall[d]) continue;
                // outer edge endpoints (on the cell boundary) and inner (top) endpoints
                float3 oa, ob, ia, ib, nrm;
                if (d == 0) { oa = {X + 1, wallTop, Z}; ob = {X + 1, wallTop, Z + 1}; ia = {x1, top, z0}; ib = {x1, top, z1}; nrm = {1, 0, 0}; }
                else if (d == 1) { oa = {X, wallTop, Z}; ob = {X, wallTop, Z + 1}; ia = {x0, top, z0}; ib = {x0, top, z1}; nrm = {-1, 0, 0}; }
                else if (d == 2) { oa = {X, wallTop, Z + 1}; ob = {X + 1, wallTop, Z + 1}; ia = {x0, top, z1}; ib = {x1, top, z1}; nrm = {0, 0, 1}; }
                else { oa = {X, wallTop, Z}; ob = {X + 1, wallTop, Z}; ia = {x0, top, z0}; ib = {x1, top, z0}; nrm = {0, 0, -1}; }
                if (bevel > 0) quad(rb, oa, ob, ib, ia, normalize(nrm + float3{0, 1, 0}), c4(rock * 0.92f, 1));
                // the wall reaches down to the floor next to it (slightly below to hide seams)
                const float gx = (oa.x + ob.x) * 0.5f + nrm.x * 0.3f, gz = (oa.z + ob.z) * 0.5f + nrm.z * 0.3f;
                const float floorY = v.heightAt(g, gx, gz) - 0.05f;
                float3 fa{oa.x, floorY, oa.z}, fb2{ob.x, floorY, ob.z};
                const float3 wc = rock * 0.72f, wd = rock * 0.5f;
                quad(rb, oa, ob, fb2, fa, nrm, c4(wc, 1), c4(wc, 1), c4(wd, 1), c4(wd, 1));
                if (K.style == 1 && hi_) { // vein streaks on the wall face
                    for (int s = 0; s < 2; ++s) {
                        float t = 0.2f + 0.6f * rnd(x * 4 + d, y, 31 + uint32_t(s)), hgt = 0.12f + 0.15f * rnd(x, y * 4 + d, 41 + uint32_t(s));
                        float yy = floorY + 0.1f + (wallTop - floorY - 0.3f) * rnd(x + d, y, 51 + uint32_t(s));
                        float3 along = ob - oa; along.y = 0;
                        float3 p = oa + along * t + nrm * 0.012f;
                        float3 u = normalize(along) * 0.09f;
                        quad(vb, float3{p.x - u.x, yy, p.z - u.z}, float3{p.x + u.x, yy + hgt * 0.4f, p.z + u.z},
                             float3{p.x + u.x, yy + hgt, p.z + u.z}, float3{p.x - u.x, yy + hgt * 0.6f, p.z - u.z}, nrm, c4(K.vein, 1));
                    }
                }
            }
            if (!hi_ || K.style == 0) continue;
            if (K.style == 1) { // gold nuggets on the top
                for (int s = 0; s < 4; ++s) {
                    float px = x0 + 0.12f + (x1 - x0 - 0.24f) * rnd(x, y, 61 + uint32_t(s)), pz = z0 + 0.12f + (z1 - z0 - 0.24f) * rnd(x, y, 71 + uint32_t(s));
                    float r = 0.08f + 0.08f * rnd(x, y, 81 + uint32_t(s)), a = 6.283f * rnd(x, y, 91 + uint32_t(s));
                    float3 ctr{px, top + 0.05f, pz};
                    float3 p[4];
                    for (int q = 0; q < 4; ++q) p[q] = {px + std::cos(a + q * 1.5708f) * r, top + 0.006f, pz + std::sin(a + q * 1.5708f) * r};
                    for (int q = 0; q < 4; ++q) {
                        float3 nn = normalize(cross(p[(q + 1) % 4] - p[q], ctr - p[q]));
                        if (nn.y < 0) nn = nn * -1.0f;
                        tri(vb, p[q], p[(q + 1) % 4], ctr, nn, c4(K.vein, 1), c4(K.vein, 1), c4(K.vein * 1.15f, 1));
                    }
                }
            } else { // gem crystals: tilted pyramids
                for (int s = 0; s < 3; ++s) {
                    float px = x0 + 0.18f + (x1 - x0 - 0.36f) * rnd(x, y, 101 + uint32_t(s)), pz = z0 + 0.18f + (z1 - z0 - 0.36f) * rnd(x, y, 111 + uint32_t(s));
                    float r = 0.07f + 0.05f * rnd(x, y, 121 + uint32_t(s)), h = 0.22f + 0.25f * rnd(x, y, 131 + uint32_t(s));
                    float a = 6.283f * rnd(x, y, 141 + uint32_t(s));
                    float3 tip{px + 0.08f * std::cos(a * 1.7f), top + h, pz + 0.08f * std::sin(a * 1.7f)};
                    float3 p[4];
                    for (int q = 0; q < 4; ++q) p[q] = {px + std::cos(a + q * 1.5708f) * r, top - 0.01f, pz + std::sin(a + q * 1.5708f) * r};
                    const float3 gc = K.vein * (0.85f + 0.3f * rnd(x, y, 151 + uint32_t(s)));
                    for (int q = 0; q < 4; ++q) {
                        float3 mid = (p[q] + p[(q + 1) % 4]) * 0.5f;
                        float3 out{mid.x - px, 0, mid.z - pz};
                        float3 nn = normalize(cross(p[(q + 1) % 4] - p[q], tip - p[q]));
                        if (dot(nn, out) < 0) nn = nn * -1.0f;
                        tri(gb, p[q], p[(q + 1) % 4], tip, nn, c4(gc * 0.7f, 1), c4(gc * 0.7f, 1), c4(gc * 1.2f, 1));
                    }
                }
            }
        }
    setMesh(c.rock, rb);
    setMesh(c.veins, vb);
    setMesh(c.gems, gb);
    c.dD = false;
}

void RtsGroundFx::buildMarks(Chunk& c, int cx, int cy, const MapGrid& g, const RtsView& v) {
    MeshBuilder mb;
    const float3 up{0, 1, 0};
    for (int y = cy * kChunk; y < std::min(g.h, (cy + 1) * kChunk); ++y)
        for (int x = cx * kChunk; x < std::min(g.w, (cx + 1) * kChunk); ++x) {
            const size_t i = size_t(y * g.w + x);
            if (!memM_[i] || !memD_[i]) continue;
            const float X = float(x), Z = float(y), t = digTop(g, v, x, y) + 0.012f;
            const float m = 0.1f, w = 0.07f;
            quad(mb, {X + m, t, Z + m}, {X + 1 - m, t, Z + m}, {X + 1 - m, t, Z + 1 - m}, {X + m, t, Z + 1 - m}, up, c4(dig_.mark, 0.38f));
            // frame
            const float a0 = m, a1 = 1 - m;
            quad(mb, {X + a0, t, Z + a0}, {X + a1, t, Z + a0}, {X + a1, t, Z + a0 + w}, {X + a0, t, Z + a0 + w}, up, c4(dig_.mark, 0.9f));
            quad(mb, {X + a0, t, Z + a1 - w}, {X + a1, t, Z + a1 - w}, {X + a1, t, Z + a1}, {X + a0, t, Z + a1}, up, c4(dig_.mark, 0.9f));
            quad(mb, {X + a0, t, Z + a0 + w}, {X + a0 + w, t, Z + a0 + w}, {X + a0 + w, t, Z + a1 - w}, {X + a0, t, Z + a1 - w}, up, c4(dig_.mark, 0.9f));
            quad(mb, {X + a1 - w, t, Z + a0 + w}, {X + a1, t, Z + a0 + w}, {X + a1, t, Z + a1 - w}, {X + a1 - w, t, Z + a1 - w}, up, c4(dig_.mark, 0.9f));
        }
    setMesh(c.marks, mb);
    c.dM = false;
}

void RtsGroundFx::drawProgress(Renderer3D& r, const MapGrid& g, const RtsView& v, const std::vector<uint8_t>* vision) {
    if (!dig_.progress || !rules_) return;
    const bool fog = vision && vision->size() == g.flags.size();
    for (uint32_t i : marked_) {
        if (i >= g.dig.size() || !g.dig[i] || !memD_[i] || (fog && (*vision)[i] != 2)) continue;
        const DiggableDef& d = rules_->diggables[std::min(size_t(g.dig[i] - 1), rules_->diggables.size() - 1)];
        if (d.hardness.raw <= 0 || g.digHp[i] >= d.hardness) continue;
        const float p = std::clamp(1.0f - float((g.digHp[i] / d.hardness).toDouble()), 0.0f, 1.0f);
        const int x = int(i) % g.w, y = int(i) / g.w;
        const float X = float(x), Z = float(y), t = digTop(g, v, x, y) + 0.02f;
        // cracks darken the top as the rock gives way, a bar shows the remaining hardness
        uint32_t dark = rgba(20, 14, 8, uint8_t(40 + 110 * p));
        r.triangle({X + 0.1f, t, Z + 0.1f}, {X + 0.9f, t, Z + 0.1f}, {X + 0.9f, t, Z + 0.9f}, dark);
        r.triangle({X + 0.1f, t, Z + 0.1f}, {X + 0.9f, t, Z + 0.9f}, {X + 0.1f, t, Z + 0.9f}, dark);
        const float bx0 = X + 0.15f, bx1 = X + 0.85f, bz0 = Z + 0.45f, bz1 = Z + 0.57f, tt = t + 0.004f;
        uint32_t bg = rgba(0, 0, 0, 170), fg = rgbaf({dig_.mark.x, dig_.mark.y, dig_.mark.z, 1});
        r.triangle({bx0, tt, bz0}, {bx1, tt, bz0}, {bx1, tt, bz1}, bg);
        r.triangle({bx0, tt, bz0}, {bx1, tt, bz1}, {bx0, tt, bz1}, bg);
        const float fx = bx0 + (bx1 - bx0) * p;
        r.triangle({bx0, tt + 0.002f, bz0}, {fx, tt + 0.002f, bz0}, {fx, tt + 0.002f, bz1}, fg);
        r.triangle({bx0, tt + 0.002f, bz0}, {fx, tt + 0.002f, bz1}, {bx0, tt + 0.002f, bz1}, fg);
    }
}

// ------------------------------------------------------------------ roads (cells)

void RtsGroundFx::buildRoads(Chunk& c, int cx, int cy, const MapGrid& g, const RtsView& v) {
    MeshBuilder eb, tb; // edge (wider, darker) below the surface
    const float3 up{0, 1, 0};
    auto road = [&](int x, int y) { return g.inside(x, y) && memR_[size_t(y * g.w + x)] != 0; };
    auto emit = [&](float w, float yo, uint32_t col, MeshBuilder& b, int x, int y) {
        const float X = float(x), Z = float(y), h = w * 0.5f;
        auto P = [&](float px, float pz) {
            const float e = 0.002f;
            return float3{px, v.heightAt(g, std::clamp(px, X + e, X + 1 - e), std::clamp(pz, Z + e, Z + 1 - e)) + yo, pz};
        };
        const bool E = road(x + 1, y), W = road(x - 1, y), S = road(x, y + 1), N = road(x, y - 1);
        const int cnt = int(E) + int(W) + int(S) + int(N);
        const float mx = X + 0.5f, mz = Z + 0.5f;
        // L corner: a quarter ring around the cell corner between the two arms
        if (cnt == 2 && (E != W)) {
            const float ccx = E ? X + 1 : X, ccz = S ? Z + 1 : Z;  // pivot = shared corner
            const float r0 = 0.5f - h, r1 = 0.5f + h;
            const float a0 = std::atan2(mz - ccz, 0.0f);
            float a1 = std::atan2(0.0f, mx - ccx);
            if (a1 - a0 > 3.1415927f) a1 -= 6.2831853f; else if (a0 - a1 > 3.1415927f) a1 += 6.2831853f; // the short way round
            const int seg = hi_ ? 6 : 3;
            for (int s = 0; s < seg; ++s) {
                float t0 = a0 + (a1 - a0) * float(s) / float(seg), t1 = a0 + (a1 - a0) * float(s + 1) / float(seg);
                quad(b, P(ccx + std::cos(t0) * r0, ccz + std::sin(t0) * r0), P(ccx + std::cos(t1) * r0, ccz + std::sin(t1) * r0),
                     P(ccx + std::cos(t1) * r1, ccz + std::sin(t1) * r1), P(ccx + std::cos(t0) * r1, ccz + std::sin(t0) * r1), up, col);
            }
            return;
        }
        // centre piece (rounded when it is a dead end or alone) + arms to the connected edges
        if (cnt <= 1) {
            const int seg = hi_ ? 10 : 6;
            for (int s = 0; s < seg; ++s) {
                float t0 = 6.2831853f * float(s) / float(seg), t1 = 6.2831853f * float(s + 1) / float(seg);
                tri(b, P(mx, mz), P(mx + std::cos(t0) * h, mz + std::sin(t0) * h), P(mx + std::cos(t1) * h, mz + std::sin(t1) * h), up, col, col, col);
            }
        } else {
            quad(b, P(mx - h, mz - h), P(mx + h, mz - h), P(mx + h, mz + h), P(mx - h, mz + h), up, col);
        }
        // arms; a dead end's arm runs into the centre so it merges with the round cap
        const float k = cnt <= 1 ? 0.0f : h;
        if (E) quad(b, P(mx + k, mz - h), P(X + 1, mz - h), P(X + 1, mz + h), P(mx + k, mz + h), up, col);
        if (W) quad(b, P(X, mz - h), P(mx - k, mz - h), P(mx - k, mz + h), P(X, mz + h), up, col);
        if (S) quad(b, P(mx - h, mz + k), P(mx + h, mz + k), P(mx + h, Z + 1), P(mx - h, Z + 1), up, col);
        if (N) quad(b, P(mx - h, Z), P(mx + h, Z), P(mx + h, mz - k), P(mx - h, mz - k), up, col);
    };
    for (int y = cy * kChunk; y < std::min(g.h, (cy + 1) * kChunk); ++y)
        for (int x = cx * kChunk; x < std::min(g.w, (cx + 1) * kChunk); ++x) {
            if (!memR_[size_t(y * g.w + x)]) continue;
            if (hi_) emit(std::min(1.0f, road_.width + 0.16f), kRoadY - 0.006f, c4(road_.edge, road_.alpha * 0.9f), eb, x, y);
            const float n = 0.94f + 0.06f * rnd(x, y, 17);
            emit(road_.width, kRoadY, c4(road_.color * n, road_.alpha), tb, x, y);
        }
    // one mesh: edges first so the surface blends over them
    MeshBuilder all;
    for (const MeshBuilder* b : {&eb, &tb}) {
        const uint32_t base = uint32_t(all.vertexCount());
        for (auto& vtx : b->vertices()) all.addVertex(vtx.pos, vtx.nrm, vtx.rgba);
        const auto& id = b->indices();
        for (size_t k = 0; k + 2 < id.size(); k += 3) all.addTriangle(base + id[k], base + id[k + 1], base + id[k + 2]);
    }
    setMesh(c.road, all);
    c.dR = false;
}

// ------------------------------------------------------------------ lanes (MW2) and map roads between buildings

void RtsGroundFx::buildLanes(const RtsScene& s, const MapGrid& g, const RtsView& v) {
    MeshBuilder b;
    const float3 up{0, 1, 0};
    auto ribbon = [&](const std::vector<float3>& pts, float width, float alpha, float3 colr, float3 edge, float yo, bool fadeEnds) {
        if (pts.size() < 2) return;
        // cumulative length for end fades
        std::vector<float> len(pts.size(), 0);
        for (size_t k = 1; k < pts.size(); ++k) { float3 d = pts[k] - pts[k - 1]; d.y = 0; len[k] = len[k - 1] + length(d); }
        const float total = len.back();
        uint32_t prev[4] = {};
        for (size_t k = 0; k < pts.size(); ++k) {
            float3 t = pts[std::min(k + 1, pts.size() - 1)] - pts[k ? k - 1 : 0];
            t.y = 0;
            float tl = length(t);
            float3 side = tl > 1e-5f ? float3{-t.z / tl, 0, t.x / tl} : float3{1, 0, 0};
            float fade = 1;
            if (fadeEnds) fade = std::clamp(std::min(len[k], total - len[k]) / 1.2f, 0.0f, 1.0f);
            const float off[4] = {-0.5f, -0.32f, 0.32f, 0.5f};
            const float al[4] = {0.0f, 1.0f, 1.0f, 0.0f};
            uint32_t cur[4];
            for (int q = 0; q < 4; ++q) {
                float3 p = pts[k] + side * (off[q] * width);
                p.y = v.heightAt(g, p.x, p.z) + yo;
                float3 cc = (q == 0 || q == 3) ? edge : colr;
                cur[q] = b.addVertex(p, up, c4(cc, alpha * fade * (q == 0 || q == 3 ? 0.0f : al[q])));
            }
            if (k > 0)
                for (int q = 0; q < 3; ++q) { b.addTriangle(prev[q], prev[q + 1], cur[q + 1]); b.addTriangle(prev[q], cur[q + 1], cur[q]); }
            std::memcpy(prev, cur, sizeof prev);
        }
    };
    // buildings a lane must not cross (garrisons other than its own two ends)
    struct Obst { float x, z, r; Entity e; };
    std::vector<Obst> obst;
    for (const SceneObject& o : s.objects)
        if (o.garrison >= 0 && rules_ && o.type < rules_->types.size()) {
            const UnitType& t = rules_->types[o.type];
            obst.push_back({o.pos.x, o.pos.z, std::max(0.8f, float(std::max(t.footprintW, t.footprintH)) * 0.5f), o.entity});
        }
    // Mushroom Wars 2 shows paths between neighbouring buildings only: keep a pair a-b unless some
    // building c is closer to both ends than they are to each other (relative neighbourhood graph)
    auto neighbours = [&](const Lane& l, const std::vector<float3>& pts) {
        if (lane_.all || pts.size() < 2) return true;
        const float3 a = pts.front(), b = pts.back();
        const float dab = std::hypot(a.x - b.x, a.z - b.z);
        for (const Obst& ob : obst) {
            if (ob.e == l.a || ob.e == l.b) continue;
            const float da = std::hypot(a.x - ob.x, a.z - ob.z), db = std::hypot(b.x - ob.x, b.z - ob.z);
            if (std::max(da, db) < dab * 0.97f) return false;
        }
        return true;
    };
    if (s.lanes) {
        const LaneRules& LR = rules_->lanes;
        const float full = lane_.width > 0 ? lane_.width : float(LR.left + LR.right) * float(LR.spacing.toDouble()) * 0.6f + 0.6f;
        for (const Lane& l : *s.lanes) {
            std::vector<float3> pts;
            pts.reserve(l.points.size());
            for (const Vec3& p : l.points) pts.push_back({float(p.x.toDouble()), 0, float(p.z.toDouble())});
            if (!lane_.all && l.offset == 0) {
                bool blocked = false;
                for (const Obst& ob : obst) {
                    if (ob.e == l.a || ob.e == l.b) continue;
                    for (const float3& p : pts) if (std::hypot(p.x - ob.x, p.z - ob.z) < ob.r) { blocked = true; break; }
                    if (blocked) break;
                }
                if (blocked || !neighbours(l, pts)) continue;
            }
            if (l.offset == 0) {
                // centre of the band between the outermost lanes
                std::vector<float3> c = pts;
                const float shift = (float(LR.right) - float(LR.left)) * 0.5f * float(LR.spacing.toDouble());
                for (size_t k = 0; k < c.size() && std::fabs(shift) > 1e-4f; ++k) {
                    float3 t = pts[std::min(k + 1, pts.size() - 1)] - pts[k ? k - 1 : 0];
                    float tl = std::hypot(t.x, t.z);
                    if (tl > 1e-5f) { c[k].x += -t.z / tl * shift; c[k].z += t.x / tl * shift; }
                }
                ribbon(c, full, lane_.alpha, lane_.color, lane_.edge, kLaneY, true);
                if (hi_ && lane_.sideLanes) ribbon(pts, 0.14f, lane_.alpha * 0.8f, lane_.edge, lane_.edge, kLaneY + 0.004f, true);
            }
        }
        // offset lanes as faint tracks (only for pairs that were drawn)
        if (hi_ && lane_.sideLanes) {
            for (const Lane& l : *s.lanes) {
                if (l.offset == 0) continue;
                std::vector<float3> pts;
                for (const Vec3& p : l.points) pts.push_back({float(p.x.toDouble()), 0, float(p.z.toDouble())});
                bool blocked = false;
                if (!lane_.all)
                    for (const Obst& ob : obst) {
                        if (ob.e == l.a || ob.e == l.b) continue;
                        for (const float3& p : pts) if (std::hypot(p.x - ob.x, p.z - ob.z) < ob.r) { blocked = true; break; }
                        if (blocked) break;
                    }
                if (!blocked && neighbours(l, pts)) ribbon(pts, 0.1f, lane_.alpha * 0.45f, lane_.edge, lane_.edge, kLaneY + 0.004f, true);
            }
        }
    }
    // hand-made map roads between buildings (Mushroom Wars): straight ribbons
    for (auto& [a, bb] : s.roads) {
        std::vector<float3> pts;
        float3 d = bb - a; d.y = 0;
        const int n = std::max(2, int(length(d) * 2.0f) + 1);
        for (int k = 0; k <= n; ++k) pts.push_back(lerp(a, bb, float(k) / float(n)));
        ribbon(pts, lane_.width > 0 ? lane_.width : 0.9f, lane_.alpha, lane_.color, lane_.edge, kLaneY, true);
    }
    setMesh(lanes_, b);
}

// ------------------------------------------------------------------ frame

void RtsGroundFx::draw(Renderer3D& r, const RtsScene& s, double time, const std::vector<uint8_t>* vision, const MapGrid& g, const RtsView& v) {
    if (!dev_ || !rules_ || g.w <= 0 || g.h <= 0) return;
    time_ = float(std::fmod(time, 1000.0));
    bool hi = r.materialQuality() != MaterialQuality::Low;
    if (const char* q = std::getenv("RTS_GROUNDFX")) hi = std::string(q) != "low";
    if (hi != hi_) { hi_ = hi; invalid_ = true; }
    if (invalid_ || g.w != gridW_ || g.h != gridH_ || s.viewer != viewer_) { viewer_ = s.viewer; resetGrid(g); }
    if (geomDirty_) {
        geomDirty_ = false;
        for (Chunk& c : chunks_) c.dT = c.dD = c.dM = c.dR = true;
        laneCount_ = ~size_t(0);
    }
    // the layers change only through the sim (GameState::territoryDirty is a sticky hint the view may
    // not reset), so they are compared a few times per second against the fog-of-war memory
    if ((useT_ || useD_ || useR_) && (time >= nextScan_ || nextScan_ < 0)) { scan(g, vision, viewer_); nextScan_ = time + 0.2; }

    // lanes: rebuilt when the lane set or the map roads change
    if (lane_.on) {
        uint32_t sig = s.lanes ? s.laneSignature : 0u;
        size_t count = (s.lanes ? s.lanes->size() : 0) * 131 + s.roads.size();
        for (auto& [a, b] : s.roads) sig = sig * 31u + uint32_t(int(a.x * 4) * 7 + int(a.z * 4) * 13 + int(b.x * 4) * 17 + int(b.z * 4) * 19);
        if (sig != laneSig_ || count != laneCount_) { laneSig_ = sig; laneCount_ = count; buildLanes(s, g, v); }
    }

    // visible chunks, rebuilt lazily
    visible_.clear();
    const mat4& vp = r.viewProj();
    for (int cy = 0; cy < chunksY_; ++cy)
        for (int cx = 0; cx < chunksX_; ++cx) {
            Chunk& c = chunks_[size_t(cy * chunksX_ + cx)];
            float3 mn{float(cx * kChunk), c.yMin, float(cy * kChunk)}, mx{float((cx + 1) * kChunk), c.yMax, float((cy + 1) * kChunk)};
            if (!boxVisible(vp, mn, mx)) continue;
            if (useT_ && terr_.on && c.dT) buildTerritory(c, cx, cy, g, v);
            if (useD_ && dig_.on && c.dD) buildDig(c, cx, cy, g, v);
            if (useD_ && dig_.on && c.dM) buildMarks(c, cx, cy, g, v);
            if (useR_ && road_.on && c.dR) buildRoads(c, cx, cy, g, v);
            visible_.push_back(cy * chunksX_ + cx);
        }
    // rock blocks: ordinary lit opaque meshes (cast and receive sun shadows, point lights)
    if (useD_ && dig_.on)
        for (int ci : visible_) {
            Chunk& c = chunks_[size_t(ci)];
            if (c.rock.indexCount) r.drawMesh(c.rock, mat4{}, {1, 1, 1, 1}, 0, 0.12f);
            if (c.veins.indexCount) r.drawMesh(c.veins, mat4{}, {1, 1, 1, 1}, kDrawNoCastShadow, 0.9f, 0.25f);
            if (c.gems.indexCount) {
                float glow = 0.0f;
                for (auto& k : dig_.kinds) glow = std::max(glow, k.glow);
                r.drawMesh(c.gems, mat4{}, {1, 1, 1, 1}, kDrawNoCastShadow, 1.0f, glow * (hi_ ? 0.85f + 0.15f * std::sin(time_ * 2.0f) : 1.0f));
            }
        }
    if (useD_ && dig_.on) drawProgress(r, g, v, vision);

    if (!pipe_.valid()) return;
    r.drawCustom([this, &r](gfx::Device& dev) {
        dev.setPipeline(pipe_);
        r.bindLitFrame();
        auto drawM = [&](const Mesh& m, const Tex* tex, TextureRef builtin, float emissive, float pulse, float spec) {
            if (!m.indexCount) return;
            GU0 u{};
            u.color[0] = u.color[1] = u.color[2] = u.color[3] = 1;
            TextureRef t = tex && tex->ref.valid() ? tex->ref : builtin;
            const bool textured = t.valid();
            u.params[0] = textured ? 1.0f : 0.0f;
            u.params[1] = tex ? tex->scale : 2.0f;
            u.params[2] = time_;
            u.params[3] = pulse;
            u.params2[0] = emissive; u.params2[1] = spec; u.params2[2] = tex ? tex->strength : 1.0f; u.params2[3] = 1;
            if (textured) { u.rect[0] = t.u0; u.rect[1] = t.v0; u.rect[2] = t.u1 - t.u0; u.rect[3] = t.v1 - t.v0; }
            else { u.rect[2] = u.rect[3] = 1; }
            if (textured && tex && !tex->ref.valid()) { u.params[1] = 1.6f; u.params2[2] = 0.55f; } // built-in noise: subtle
            dev.setTexture(2, textured ? t.tex : white_);
            dev.setUniforms(0, &u, sizeof u);
            dev.setVertexBuffer(m.vb);
            dev.setIndexBuffer(m.ib);
            dev.drawIndexed(m.indexCount);
        };
        const TextureRef cobble = hi_ && road_.cobbles ? textures_.addGenerated("builtin:groundfx-cobble", 64, 64, makeNoise(64, true), false) : TextureRef{};
        const TextureRef dirt = hi_ ? textures_.addGenerated("builtin:groundfx-dirt", 32, 32, makeNoise(32, false), false) : TextureRef{};
        const float pulse = hi_ && terr_.pulse ? 0.35f : 0.0f;
        for (int ci : visible_) {
            const Chunk& c = chunks_[size_t(ci)];
            if (useT_ && terr_.on) { drawM(c.fill, &terr_.tex, {}, 0.05f, 0, 0.05f); drawM(c.border, nullptr, {}, terr_.glow, pulse, 0.1f); }
        }
        for (int ci : visible_) if (useR_ && road_.on) drawM(chunks_[size_t(ci)].road, &road_.tex, cobble, 0.0f, 0, 0.08f);
        if (lane_.on) drawM(lanes_, &lane_.tex, dirt, 0.0f, 0, 0.05f);
        for (int ci : visible_) if (useD_ && dig_.on) drawM(chunks_[size_t(ci)].marks, nullptr, {}, 0.6f, hi_ ? 0.4f : 0.0f, 0.1f);
    });
}

} // namespace rts::game
