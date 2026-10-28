// Detail-resolution terrain editing (docs/terrain.md "Detail surface"): the map's visual detail
// heightmap terrain.detail ((w*k+1) x (h*k+1) samples, k per cell) and its splat weights. Sculpt brushes
// work at sample resolution with soft falloff (raise / lower / smooth / flatten / noise / set / erode /
// stamp), material painting writes splat weights per sample, 16-bit PNG import / export at detail
// resolution. On heightfield maps the simulation corners follow the detail (sampled at the corners),
// so units stand on what is drawn; on cliff-level maps the simulation is untouched (map_validate
// reports where the visual surface leaves the simulated one).
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include "TerrainOps.h"
#include "modules/rts/view/RtsTerrainDetail.h"
#include "modules/rts/view/RtsTerrainSurface.h"

namespace rts::editor {

namespace {
Json str(const std::string& s) { return Json::string(s); }
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
    const float a = v(xi, yi) + (v(xi + 1, yi) - v(xi, yi)) * fx, b = v(xi, yi + 1) + (v(xi + 1, yi + 1) - v(xi, yi + 1)) * fx;
    return a + (b - a) * fy;
}
game::MapGrid gridOfDoc(const MapData& d) {
    game::MapGrid g;
    g.init(d.w, d.h);
    for (int i = 0; i < d.w * d.h; ++i) {
        g.flags[size_t(i)] = game::cellFlagsFromChar(d.cells[size_t(i)]);
        g.level[size_t(i)] = d.levels[size_t(i)];
        g.height[size_t(i)] = d.heights[size_t(i)];
    }
    if (d.corners.size() == size_t((d.w + 1) * (d.h + 1))) g.corner = d.corners;
    return g;
}
// visual terrain block of the project (rules then game.json, game.json wins per key)
Json visualTerrain(const Workspace& ws) {
    Json t = ws.project.rulesJson["terrain"].isObject() ? ws.project.rulesJson["terrain"] : Json::object();
    if (ws.project.manifest["terrain"].isObject()) for (auto& [k, v] : ws.project.manifest["terrain"].members()) t.set(k, v);
    return t;
}
int16_t q16(float h) { return int16_t(std::clamp(std::lround(h * 16.0f), -32768l, 32767l)); }
float f16(int16_t v) { return float(v) / 16.0f; }
} // namespace

// Creates the detail layer from the surface the player sees now (classic or smooth style).
bool ensureDetail(Workspace& ws, int scale) {
    MapData& d = ws.map.data;
    const size_t n = size_t(d.w * scale + 1) * size_t(d.h * scale + 1);
    if (d.detailScale > 0 && d.detail.size() == size_t(d.w * d.detailScale + 1) * size_t(d.h * d.detailScale + 1)) return false;
    const Json vt = visualTerrain(ws);
    const bool smooth = vt.getStr("style") == "smooth";
    game::MapGrid g = gridOfDoc(d);
    const float lh = float(ws.project.rules.levelHeight.toDouble());
    game::terrain::DetailSurface ds;
    ds.init(d.w, d.h, scale);
    game::terrain::DetailParams p;
    p.scale = scale;
    if (const Json& dl = vt["detailLook"]; dl.isObject()) {
        p.bevel = float(dl["bevel"].toDouble(p.bevel)); p.wobble = float(dl["wobble"].toDouble(p.wobble)); p.noise = float(dl["roughness"].toDouble(p.noise));
    }
    if (!smooth) { p.bevel = 0.12f; p.wobble = 0; p.noise = 0; }   // classic look: keep the cliffs, only sample them
    if (g.hasCorners()) game::terrain::synthCorners(g, p, ds); else game::terrain::synthLevels(g, lh, false, p, ds);
    d.detailScale = scale;
    d.detail = game::terrain::detailToInt16(ds);
    d.detail.resize(n);
    return true;
}

// Heightfield maps: the simulation corners take the detail height at each corner.
static void syncCorners(MapData& d, int x0, int y0, int x1, int y1) {
    if (d.corners.size() != size_t((d.w + 1) * (d.h + 1)) || d.detailScale <= 0) return;
    const int k = d.detailScale, SW = d.w * k + 1;
    for (int Y = std::max(0, y0); Y <= std::min(d.h, y1); ++Y)
        for (int X = std::max(0, x0); X <= std::min(d.w, x1); ++X)
            d.corners[size_t(Y * (d.w + 1) + X)] = d.detail[size_t(Y * k) * size_t(SW) + size_t(X * k)];
}

struct DabResult { int i0 = 0, j0 = 0, i1 = -1, j1 = -1; };

// One dab at sample resolution. x, z in world units (cells), radius in world units.
static DabResult detailDab(MapData& d, Sculpt op, float x, float z, float radius, float strength, float hardness, uint32_t seed,
                           const std::vector<float>* stamp, int stampW, int stampH, float angle) {
    const int k = d.detailScale, SW = d.w * k + 1, SH = d.h * k + 1;
    const float rs = std::max(radius, 0.25f) * float(k);      // radius in samples
    const float cx = x * float(k), cz = z * float(k);
    DabResult r;
    r.i0 = std::max(0, int(std::floor(cx - rs))); r.i1 = std::min(SW - 1, int(std::ceil(cx + rs)));
    r.j0 = std::max(0, int(std::floor(cz - rs))); r.j1 = std::min(SH - 1, int(std::ceil(cz + rs)));
    if (r.i0 > r.i1 || r.j0 > r.j1) return r;
    auto at = [&](int i, int j) -> int16_t& { return d.detail[size_t(std::clamp(j, 0, SH - 1)) * size_t(SW) + size_t(std::clamp(i, 0, SW - 1))]; };
    // falloff: 1 inside hardness * radius, smooth to 0 at the rim
    auto fall = [&](float dd) {
        const float t = std::clamp((1.0f - dd / rs) / std::max(1.0f - hardness, 0.01f), 0.0f, 1.0f);
        return t * t * (3 - 2 * t);
    };
    float target = 0, wsum = 0;
    if (op == Sculpt::Flatten) {
        for (int j = r.j0; j <= r.j1; ++j)
            for (int i = r.i0; i <= r.i1; ++i) {
                const float dd = std::hypot(float(i) - cx, float(j) - cz);
                if (dd > rs) continue;
                const float w = fall(dd);
                target += f16(at(i, j)) * w; wsum += w;
            }
        target = wsum > 0 ? target / wsum : 0;
    }
    // snapshot (with a margin) for smooth / erode
    const int m = op == Sculpt::Erode ? 2 : 1, RW = r.i1 - r.i0 + 1 + 2 * m, RH = r.j1 - r.j0 + 1 + 2 * m;
    std::vector<float> snap;
    if (op == Sculpt::Smooth || op == Sculpt::Erode) {
        snap.resize(size_t(RW) * size_t(RH));
        for (int j = 0; j < RH; ++j) for (int i = 0; i < RW; ++i) snap[size_t(j) * size_t(RW) + size_t(i)] = f16(at(r.i0 - m + i, r.j0 - m + j));
    }
    auto sn = [&](int i, int j) { return snap[size_t(j - r.j0 + m) * size_t(RW) + size_t(i - r.i0 + m)]; };
    const float ca = std::cos(angle), sa = std::sin(angle);
    const float inv = 1.0f / float(k);
    for (int j = r.j0; j <= r.j1; ++j)
        for (int i = r.i0; i <= r.i1; ++i) {
            const float dd = std::hypot(float(i) - cx, float(j) - cz);
            if (dd > rs) continue;
            const float f = fall(dd);
            float h = f16(at(i, j));
            const float wx = float(i) * inv, wz = float(j) * inv;
            switch (op) {
                case Sculpt::Raise: h += strength * f; break;
                case Sculpt::Lower: h -= strength * f; break;
                case Sculpt::Set: h += (strength - h) * f; break;
                case Sculpt::Flatten: h += (target - h) * f * std::clamp(strength, 0.0f, 1.0f); break;
                case Sculpt::Noise: {
                    // ridged + smooth noise at two scales relative to the brush (small forms)
                    const float s1 = 3.0f / std::max(radius, 0.5f);
                    const float n = vnoise(wx * s1, wz * s1, seed) * 0.6f + vnoise(wx * s1 * 3.1f, wz * s1 * 3.1f, seed + 5) * 0.3f +
                                    (1.0f - std::fabs(vnoise(wx * s1 * 7.3f, wz * s1 * 7.3f, seed + 9))) * 0.1f;
                    h += strength * f * n;
                    break;
                }
                case Sculpt::Smooth: {
                    float s = 0;
                    for (int b = -1; b <= 1; ++b) for (int a = -1; a <= 1; ++a) s += sn(std::clamp(i + a, r.i0 - 1, r.i1 + 1), std::clamp(j + b, r.j0 - 1, r.j1 + 1));
                    h += (s / 9.0f - h) * f * std::clamp(strength, 0.0f, 1.0f);
                    break;
                }
                case Sculpt::Erode: {
                    // thermal erosion: material slides to lower neighbours steeper than the talus,
                    // carving gullies on slopes and depositing at their feet
                    const float talus = 0.6f * inv;   // rise per sample
                    float out = 0;
                    for (auto [a, b] : {std::pair{1, 0}, std::pair{-1, 0}, std::pair{0, 1}, std::pair{0, -1}}) {
                        const float dh = h - sn(std::clamp(i + a, r.i0 - m, r.i1 + m), std::clamp(j + b, r.j0 - m, r.j1 + m));
                        if (dh > talus) out -= (dh - talus) * 0.25f;
                        const float uh = sn(std::clamp(i + a, r.i0 - m, r.i1 + m), std::clamp(j + b, r.j0 - m, r.j1 + m)) - h;
                        if (uh > talus) out += (uh - talus) * 0.2f;
                    }
                    // a little noise so the gullies branch
                    out *= 1.0f + 0.5f * vnoise(wx * 4.0f, wz * 4.0f, seed + 17);
                    h += out * f * std::clamp(strength, 0.0f, 1.0f);
                    break;
                }
                case Sculpt::Stamp: {
                    if (!stamp || stampW < 2 || stampH < 2) break;
                    // brush image (0..1) rotated and scaled to the brush diameter, added as height
                    const float u = (float(i) - cx) / rs, v = (float(j) - cz) / rs;
                    const float su = (ca * u - sa * v) * 0.5f + 0.5f, sv = (sa * u + ca * v) * 0.5f + 0.5f;
                    if (su < 0 || sv < 0 || su > 1 || sv > 1) break;
                    const float px = su * float(stampW - 1), py = sv * float(stampH - 1);
                    const int a0 = int(px), b0 = int(py), a1 = std::min(a0 + 1, stampW - 1), b1 = std::min(b0 + 1, stampH - 1);
                    const float fx = px - float(a0), fy = py - float(b0);
                    auto S = [&](int a, int b) { return (*stamp)[size_t(b) * size_t(stampW) + size_t(a)]; };
                    const float sv2 = (S(a0, b0) * (1 - fx) + S(a1, b0) * fx) * (1 - fy) + (S(a0, b1) * (1 - fx) + S(a1, b1) * fx) * fy;
                    h += strength * sv2 * std::min(1.0f, f * 4.0f);
                    break;
                }
            }
            at(i, j) = q16(h);
        }
    return r;
}

// Built-in stamp brushes (0..1, 64x64): crater, mesa, dunes, rocks.
static std::vector<float> builtinStamp(const std::string& name, uint32_t seed) {
    const int N = 64;
    std::vector<float> s(size_t(N * N));
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x) {
            const float u = (float(x) + 0.5f) / float(N) * 2 - 1, v = (float(y) + 0.5f) / float(N) * 2 - 1, r = std::sqrt(u * u + v * v);
            float h = 0;
            if (name == "crater") h = r < 1 ? (r < 0.7f ? -0.6f * (1 - (r / 0.7f) * (r / 0.7f)) : 0.35f * std::sin((r - 0.7f) / 0.3f * 3.14159f)) : 0;
            else if (name == "mesa") h = r < 1 ? std::clamp((0.85f - r) * 6.0f, 0.0f, 1.0f) : 0;
            else if (name == "dunes") h = r < 1 ? (0.5f + 0.5f * std::sin(u * 9.0f + vnoise(u * 2, v * 2, seed) * 2.0f)) * (1 - r) : 0;
            else h = r < 1 ? std::max(0.0f, vnoise(u * 5, v * 5, seed) * 0.7f + vnoise(u * 11, v * 11, seed + 3) * 0.3f) * (1 - r * r) : 0;   // rocks
            s[size_t(y * N + x)] = h;
        }
    return s;
}

// splat weights per sample: initialised from the cell kinds (visual terrain.cells) the first time
static void ensureSplat(Workspace& ws) {
    MapData& d = ws.map.data;
    const int k = d.detailScale, SW = d.w * k + 1, SH = d.h * k + 1;
    if (d.detailSplat.size() == size_t(SW) * size_t(SH) * 4) return;
    const Json vt = visualTerrain(ws);
    std::vector<std::string> ids;
    for (auto& l : vt["layers"].items()) ids.push_back(l.getStr("id"));
    auto layerOf = [&](const char* kind) {
        const Json& c = vt["cells"][kind];
        const std::string id = c.isString() ? c.str() : c.getStr("layer");
        for (size_t i = 0; i < ids.size(); ++i) if (ids[i] == id) return int(i);
        return 0;
    };
    const std::vector<uint8_t> painted = materialLayer(d);
    d.detailSplat.assign(size_t(SW) * size_t(SH) * 4, 0);
    for (int j = 0; j < SH; ++j)
        for (int i = 0; i < SW; ++i) {
            const int cx = std::min(i / k, d.w - 1), cy = std::min(j / k, d.h - 1);
            const size_t ci = d.idx(cx, cy);
            const uint8_t f = game::cellFlagsFromChar(d.cells[ci]);
            const char* kind = !(f & game::kCellGround) && (f & game::kCellWater) ? "lakebed" : !(f & game::kCellGround) ? "blocked"
                             : (f & game::kCellRock) ? "rock" : (f & game::kCellRamp) ? "ramp" : (f & game::kCellWater) ? "shallows"
                             : (f & game::kCellNoBuild) ? "sand" : "grass";
            int l = ci < painted.size() && painted[ci] < 4 ? painted[ci] : layerOf(kind);
            d.detailSplat[(size_t(j) * size_t(SW) + size_t(i)) * 4 + size_t(std::clamp(l, 0, 3))] = 255;
        }
}

static void paintSplat(MapData& d, float x, float z, float radius, int layer, float strength, float hardness) {
    const int k = d.detailScale, SW = d.w * k + 1, SH = d.h * k + 1;
    const float rs = std::max(radius, 0.25f) * float(k), cx = x * float(k), cz = z * float(k);
    for (int j = std::max(0, int(cz - rs)); j <= std::min(SH - 1, int(cz + rs) + 1); ++j)
        for (int i = std::max(0, int(cx - rs)); i <= std::min(SW - 1, int(cx + rs) + 1); ++i) {
            const float dd = std::hypot(float(i) - cx, float(j) - cz);
            if (dd > rs) continue;
            float t = std::clamp((1.0f - dd / rs) / std::max(1.0f - hardness, 0.01f), 0.0f, 1.0f);
            t = t * t * (3 - 2 * t) * std::clamp(strength, 0.0f, 1.0f);
            uint8_t* p = &d.detailSplat[(size_t(j) * size_t(SW) + size_t(i)) * 4];
            for (int l = 0; l < 4; ++l) {
                const float v = float(p[l]) / 255.0f, target = l == layer ? 1.0f : 0.0f;
                p[l] = uint8_t(std::clamp((v + (target - v) * t) * 255.0f + 0.5f, 0.0f, 255.0f));
            }
        }
}

void registerTerrainDetailOps(Workspace& ws) {
    Workspace* w = &ws;
    auto needMap = [w] { if (w->project.dir.empty()) throw std::runtime_error("no game is open: call open_game first"); };
    auto add = [&ws](const char* name, const char* desc, const char* sch, Workspace::OpFn fn) { ws.registerOp({name, desc, Json::parse(sch), std::move(fn)}); };

    add("terrain_detail",
        "The map's visual detail heightmap (terrain.detail: k samples per cell, sub-cell relief; docs/terrain.md). action: "
        "'info' (scale, size, min/max, deviation from the simulation surface), 'create' (scale 1/2/4/8, from the surface drawn now: "
        "smooth style = organic cliffs / bicubic relief), 'remove'. Sculpting with terrain_sculpt {detail:true} creates it on demand. "
        "Visual only; on heightfield maps the corners follow it. Undoable.",
        R"({"properties": {"action": {"type": "string", "enum": ["info", "create", "remove"]}, "scale": {"type": "integer"}}, "required": ["action"]})",
        [w, needMap](const Json& p) {
            needMap();
            MapData& d = w->map.data;
            const std::string a = p.getStr("action");
            Json r = Json::object();
            if (a == "create") {
                const int k = std::clamp(int(p.getInt("scale", 4)), 1, 8);
                w->map.beginEdit();
                d.detail.clear(); d.detailSplat.clear(); d.detailScale = 0;
                ensureDetail(*w, k);
                w->map.touched();
                w->note({Change::Map, 0, 0, d.w - 1, d.h - 1, -1, "Детальный рельеф"});
                r.set("summary", str("Детальный рельеф x" + std::to_string(k)));
            } else if (a == "remove") {
                w->map.beginEdit();
                d.detail.clear(); d.detailSplat.clear(); d.detailScale = 0; d.detailFile.clear();
                w->map.touched();
                r.set("summary", str("Детальный рельеф удалён"));
            }
            r.set("ok", Json::fromBool(true));
            r.set("scale", Json::number(int64_t(d.detailScale)));
            if (d.detailScale > 0 && !d.detail.empty()) {
                const auto [mn, mx] = std::minmax_element(d.detail.begin(), d.detail.end());
                r.set("samples", Json::number(int64_t(d.detail.size())));
                r.set("min", Json::number(double(f16(*mn)), 2)); r.set("max", Json::number(double(f16(*mx)), 2));
                r.set("splat", Json::fromBool(!d.detailSplat.empty()));
                const DetailDeviation dv = detailDeviation(*w, 0.5f);
                r.set("maxDeviation", Json::number(double(dv.maxDev), 2)); r.set("cellsOver", Json::number(int64_t(dv.cellsOver)));
            }
            return r;
        });

    add("terrain_paint_material",
        "Paint a terrain material layer (index 0..3 of the visual terrain.layers) at detail resolution: soft brush at x, y (world units, "
        "fractions allowed), radius in world units, strength 0..1, hardness 0..1; 'points' for a stroke. Creates the detail layer (scale 4) "
        "when missing. Visual only. Undoable.",
        R"({"properties": {"layer": {"type": "integer"}, "x": {"type": "number"}, "y": {"type": "number"}, "radius": {"type": "number"},
            "strength": {"type": "number"}, "hardness": {"type": "number"}, "points": {"type": "array", "items": {"type": "array", "items": {"type": "number"}}}},
            "required": ["layer"]})",
        [w, needMap](const Json& p) {
            needMap();
            const int layer = std::clamp(int(p.getInt("layer", 0)), 0, 3);
            std::vector<std::pair<float, float>> pts;
            if (p["points"].isArray()) for (auto& q : p["points"].items()) pts.push_back({float(q[0].toDouble()), float(q[1].toDouble())});
            else { if (!p.has("x") || !p.has("y")) throw std::runtime_error("expected x, y (or points)"); pts.push_back({float(p["x"].toDouble()), float(p["y"].toDouble())}); }
            w->map.beginEdit();
            ensureDetail(*w, 4);
            ensureSplat(*w);
            const float rad = float(p["radius"].toDouble(2));
            for (auto [x, z] : pts) paintSplat(w->map.data, x, z, rad, layer, float(p["strength"].toDouble(1)), float(p["hardness"].toDouble(0.3)));
            w->map.touched();
            const auto [x0, z0] = pts.front();
            w->note({Change::Cells, int(x0 - rad), int(z0 - rad), int(x0 + rad), int(z0 + rad), -1, "Материал (детально)"});
            Json r = Json::object();
            r.set("ok", Json::fromBool(true));
            r.set("summary", str("Материал земли (детально): слой " + std::to_string(layer)));
            return r;
        });
}

// ------------------------------------------------------------------ helpers used by TerrainOps / validate

bool detailSculpt(Workspace& ws, Sculpt op, const std::vector<std::pair<float, float>>& pts, float radius, float strength, float hardness,
                  uint32_t seed, const std::string& stampName, float angle, int& x0, int& y0, int& x1, int& y1, std::string* err) {
    MapData& d = ws.map.data;
    ensureDetail(ws, 4);
    std::vector<float> stamp;
    int sw = 0, shh = 0;
    if (op == Sculpt::Stamp) {
        if (stampName.empty() || stampName == "crater" || stampName == "mesa" || stampName == "dunes" || stampName == "rocks") {
            stamp = builtinStamp(stampName.empty() ? "rocks" : stampName, seed); sw = shh = 64;
        } else { // grayscale PNG in the game folder
            std::vector<int16_t> v;
            if (!readHeightPng(ws.project.dir + "/" + stampName, 64, 64, 65535.0f / 16.0f, 0, v, err)) return false;
            stamp.resize(v.size());
            for (size_t i = 0; i < v.size(); ++i) stamp[i] = float(uint16_t(v[i])) / 65535.0f;
            sw = shh = 64;
        }
    }
    const int k = d.detailScale;
    int i0 = 1 << 30, j0 = 1 << 30, i1 = -1, j1 = -1;
    for (auto [x, z] : pts) {
        DabResult r = detailDab(d, op, x, z, radius, strength, hardness, seed, stamp.empty() ? nullptr : &stamp, sw, shh, angle);
        i0 = std::min(i0, r.i0); j0 = std::min(j0, r.j0); i1 = std::max(i1, r.i1); j1 = std::max(j1, r.j1);
    }
    if (i1 < 0) { x0 = y0 = 0; x1 = y1 = -1; return true; }
    x0 = i0 / k; y0 = j0 / k; x1 = (i1 + k - 1) / k; y1 = (j1 + k - 1) / k;
    syncCorners(d, x0, y0, x1, y1);
    ws.map.touched();
    return true;
}

DetailDeviation detailDeviation(const Workspace& ws, float threshold) { return detailDeviation(ws.map.data, ws.project.rules, threshold); }

DetailDeviation detailDeviation(const MapData& d, const game::Rules& rules, float threshold) {
    DetailDeviation out;
    if (d.detailScale <= 0 || d.detail.empty()) return out;
    game::MapGrid g = gridOfDoc(d);
    const float lh = float(rules.levelHeight.toDouble());
    game::terrain::DetailSurface ds;
    game::terrain::detailFromInt16(d.w, d.h, d.detailScale, d.detail, ds);
    const bool hf = rules.terrain.heightfield() && g.hasCorners();
    for (int y = 0; y < d.h; ++y)
        for (int x = 0; x < d.w; ++x) {
            const uint8_t f = g.flags[d.idx(x, y)];
            if (!(f & game::kCellGround)) continue;    // units never stand on water / blocked cells
            // simulation ground at the cell centre: bilinear corners (heightfield) or the cliff level
            const float cx = float(x) + 0.5f, cz = float(y) + 0.5f;
            const float sim = hf ? float(g.surfaceHeight(Fixed::fromRatio(2 * x + 1, 2), Fixed::fromRatio(2 * y + 1, 2)).toDouble())
                                 : game::terrain::levelsSurface(g, lh, cx, cz);
            const float dev = std::fabs(ds.sample(cx, cz) - sim);
            out.maxDev = std::max(out.maxDev, dev);
            if (dev > threshold) { if (out.cellsOver++ == 0) { out.x = x; out.y = y; } }
        }
    return out;
}

} // namespace rts::editor
