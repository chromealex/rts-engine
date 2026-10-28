#include "PlanetOps.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <stdexcept>
#include "modules/rts/RtsMap.h"
#include "modules/rts/RtsSphere.h"

namespace rts::editor {

using render::float3;

namespace {
constexpr float kQuarterPi = 0.785398163f;
Json okJson(const std::string& s) { Json r = Json::object(); r.set("ok", Json::fromBool(true)); r.set("summary", Json::string(s)); return r; }

// Per face size: float corner directions (RtsSphere, duplicates identical) and duplicate lists.
struct CornerCache {
    std::vector<float3> dirs;
    std::vector<int> canon;
};
const CornerCache& cornersOf(int N) {
    static std::map<int, CornerCache> cache;
    auto it = cache.find(N);
    if (it != cache.end()) return it->second;
    CornerCache c;
    game::CubeSphere cs(N);
    const int S = (N + 1) * (N + 1);
    c.dirs.resize(size_t(6 * S));
    c.canon.resize(size_t(6 * S));
    for (int f = 0; f < 6; ++f)
        for (int j = 0; j <= N; ++j)
            for (int i = 0; i <= N; ++i) {
                const int id = cs.cornerId(f, i, j);
                c.canon[size_t(id)] = cs.canonicalCorner(f, i, j);
                const Vec3 d = cs.cornerDir(f, i, j);
                c.dirs[size_t(id)] = {float(d.x.toDouble()), float(d.y.toDouble()), float(d.z.toDouble())};
            }
    for (size_t k = 0; k < c.dirs.size(); ++k) c.dirs[k] = c.dirs[size_t(c.canon[k])];
    return cache.emplace(N, std::move(c)).first->second;
}

// rows of faces[f][key], padded to N x N with `fill`
float angleOf(float3 a, float3 b) { return std::acos(std::clamp(dot(a, b), -1.0f, 1.0f)); }
float3 readDir(const Json& p, int N) {
    if (p["dir"].isArray() && p["dir"].size() >= 3) return render::normalize({float(p["dir"][0].toDouble()), float(p["dir"][1].toDouble()), float(p["dir"][2].toDouble())});
    if (!p.has("face") || !p.has("x") || !p.has("y")) throw std::runtime_error("planet brush: give face + x + y (cells on that face) or dir [x, y, z]");
    const int f = int(p["face"].integer(0));
    if (f < 0 || f > 5) throw std::runtime_error("face must be 0..5");
    return planetDir(f, float(p["x"].toDouble()), float(p["y"].toDouble()), N);
}
} // namespace

DocBody* planetBody(MapDoc& doc, const std::string& id, std::string* err) {
    if (!doc.data.planet()) { if (err) *err = "the map has no world block (planets)"; return nullptr; }
    for (size_t i = 0; i < doc.data.bodies.size(); ++i) {
        DocBody& b = doc.data.bodies[i];
        if (b.id == id || std::to_string(i) == id) {
            if (!b.surface()) { if (err) *err = "body '" + id + "' has no surface"; return nullptr; }
            return &b;
        }
    }
    if (err) *err = "unknown body '" + id + "'";
    return nullptr;
}

float3 planetDir(int face, float a, float b, int N) {
    const game::SphereFace& F = game::kSphereFaces[std::clamp(face, 0, 5)];
    const float s = std::tan((2.0f * a / float(N) - 1.0f) * kQuarterPi), t = std::tan((2.0f * b / float(N) - 1.0f) * kQuarterPi);
    auto f3 = [](game::IVec3 v) { return float3{float(v.x), float(v.y), float(v.z)}; };
    return render::normalize(f3(F.n) + f3(F.u) * s + f3(F.v) * t);
}

int planetSculpt(MapDoc& doc, const std::string& id, Sculpt op, float3 dir, float radius, float strength, uint32_t seed) {
    DocBody* body = planetBody(doc, id);
    if (!body) return 0;
    const int N = body->faceSize;
    const CornerCache& cc = cornersOf(N);
    std::vector<int16_t>& items = body->corners;
    items.resize(cc.dirs.size(), 0);
    const float cellAngle = 2.0f * kQuarterPi / float(N);   // ~ one cell at the face centre
    const float ang = std::max(radius, 0.5f) * cellAngle;
    dir = render::normalize(dir);
    std::vector<std::pair<int, float>> hit;   // affected canonical corners with weights
    float target = 0, wsum = 0;
    for (size_t k = 0; k < cc.dirs.size(); ++k) {
        if (cc.canon[k] != int(k)) continue;
        const float a = angleOf(cc.dirs[k], dir);
        if (a > ang) continue;
        const float t = 1 - a / ang;
        hit.push_back({int(k), t * t * (3 - 2 * t)});
        target += float(items[k]) * t; wsum += t;
    }
    if (hit.empty()) return 0;
    target = wsum > 0 ? target / wsum : 0;
    std::vector<float> smooth;   // smooth: mean of the corners within ~1.5 cells (3D, seam-free)
    if (op == Sculpt::Smooth) {
        smooth.resize(hit.size());
        const float nb = cellAngle * 1.5f;
        for (size_t h = 0; h < hit.size(); ++h) {
            float sm = 0, n = 0;
            const float3 d0 = cc.dirs[size_t(hit[h].first)];
            for (size_t k = 0; k < cc.dirs.size(); ++k)
                if (cc.canon[k] == int(k) && angleOf(cc.dirs[k], d0) <= nb) { sm += float(items[k]); n += 1; }
            smooth[h] = n > 0 ? sm / n : float(items[size_t(hit[h].first)]);
        }
    }
    std::map<int, int16_t> out;
    for (size_t h = 0; h < hit.size(); ++h) {
        const auto [k, w] = hit[h];
        float v = float(items[size_t(k)]);
        const float3 d = cc.dirs[size_t(k)];
        switch (op) {
            case Sculpt::Raise: v += strength * 16.0f * w; break;
            case Sculpt::Lower: v -= strength * 16.0f * w; break;
            case Sculpt::Noise: {
                const uint32_t hsh = uint32_t(int(d.x * 977.0f) * 73856093 ^ int(d.y * 977.0f) * 19349663 ^ int(d.z * 977.0f) * 83492791) ^ (seed * 2246822519u);
                v += strength * 16.0f * w * (float((hsh ^ (hsh >> 13)) & 0xffff) / 65535.0f * 2.0f - 1.0f);
                break;
            }
            case Sculpt::Set: v += (strength * 16.0f - v) * w; break;
            case Sculpt::Flatten: v += (target - v) * w * std::clamp(strength, 0.0f, 1.0f); break;
            case Sculpt::Smooth: v += (smooth[h] - v) * w * std::clamp(strength, 0.0f, 1.0f); break;
            default: break;   // erode / stamp: detail resolution only (TerrainDetailOps.cpp)
        }
        out[k] = int16_t(std::clamp(std::lround(v), -32768l, 32767l));
    }
    int changed = 0;
    for (size_t k = 0; k < cc.dirs.size(); ++k) { // every duplicate of a changed canonical corner (seams equal)
        auto it = out.find(cc.canon[k]);
        if (it == out.end()) continue;
        if (items[k] != it->second) ++changed;
        items[k] = it->second;
    }
    doc.touched();
    return changed;
}

static int paintCells(MapDoc& doc, const std::string& id, float3 dir, float radius, const std::function<bool(DocBody&, int, int, int)>& set) {
    DocBody* body = planetBody(doc, id);
    if (!body) return 0;
    const int N = body->faceSize;
    const float ang = std::max(radius, 0.5f) * 2.0f * kQuarterPi / float(N);
    dir = render::normalize(dir);
    int changed = 0;
    for (int f = 0; f < 6; ++f)
        for (int y = 0; y < N; ++y)
            for (int x = 0; x < N; ++x)
                if (angleOf(planetDir(f, float(x) + 0.5f, float(y) + 0.5f, N), dir) <= ang && set(*body, f, x, y)) ++changed;
    if (changed) doc.touched();
    return changed;
}

int planetPaint(MapDoc& doc, const std::string& id, float3 dir, float radius, char cell) {
    return paintCells(doc, id, dir, radius, [cell](DocBody& b, int f, int x, int y) {
        char& c = b.cells[b.cellIndex(f, x, y)];
        if (c == cell) return false;
        c = cell;
        return true;
    });
}

// material rows per face: faces[f].material (digits 0..3, '.' = by cell kind), kept in DocBody::faceExtra
int planetMaterial(MapDoc& doc, const std::string& id, float3 dir, float radius, int layer) {
    DocBody* body = planetBody(doc, id);
    if (!body) return 0;
    const int N = body->faceSize;
    std::vector<std::vector<std::string>> rows(6, std::vector<std::string>(size_t(N), std::string(size_t(N), '.')));
    for (int f = 0; f < 6 && f < int(body->faceExtra.size()); ++f) {
        const Json& r = body->faceExtra[size_t(f)]["material"];
        for (size_t y = 0; y < r.size() && int(y) < N; ++y) {
            const std::string t = r[y].str();
            for (size_t x = 0; x < t.size() && int(x) < N; ++x) rows[size_t(f)][y][x] = t[x];
        }
    }
    const char v = layer >= 0 && layer <= 3 ? char('0' + layer) : '.';
    bool touchedFace[6] = {};
    const int n = paintCells(doc, id, dir, radius, [&](DocBody&, int f, int x, int y) {
        char& c = rows[size_t(f)][size_t(y)][size_t(x)];
        if (c == v) return false;
        c = v; touchedFace[f] = true;
        return true;
    });
    body->faceExtra.resize(6, Json::object());
    for (int f = 0; f < 6; ++f) {
        if (!touchedFace[f]) continue;
        Json a = Json::array();
        for (auto& r : rows[size_t(f)]) a.push(Json::string(r));
        body->faceExtra[size_t(f)].set("material", a);
    }
    return n;
}

void registerPlanetOps(Workspace& ws) {
    Workspace* w = &ws;
    auto needMap = [w] { if (w->project.dir.empty()) throw std::runtime_error("no game is open: call open_game first"); };
    // wrap an existing op: with `body` it edits that planet, otherwise the original op runs unchanged
    auto wrap = [&ws](const char* name, const char* extraDesc, const char* extraProps, Workspace::OpFn planetFn) {
        const Workspace::Op* orig = nullptr;
        for (auto& o : ws.ops()) if (o.name == name) orig = &o;
        if (!orig) return;
        Workspace::Op op = *orig;
        Workspace::OpFn flat = op.fn;
        op.description += extraDesc;
        Json props = op.schema["properties"];
        const Json extra = Json::parse(extraProps);   // keep alive: members() refers into it
        for (auto& [k, v] : extra.members()) props.set(k, v);
        op.schema.set("properties", props);
        op.schema.erase("required");   // planet calls take other parameters (kept documented in the description)
        op.fn = [flat, planetFn](const Json& p) { return p.has("body") ? planetFn(p) : flat(p); };
        ws.registerOp(std::move(op));
    };
    const char* where = R"J({"body": {"type": "string", "description": "planet / moon id of the map world block"},
        "face": {"type": "integer", "description": "0..5, docs/planets.md"}, "dir": {"type": "array", "items": {"type": "number"}, "description": "surface direction [x, y, z] instead of face/x/y"}})J";
    wrap("terrain_sculpt", " Planets: pass body + face + x, y (cells on that face) or dir [x,y,z]; radius in cells; the brush is 3D and crosses face seams.",
         where, [w, needMap](const Json& p) {
             needMap();
             std::string err;
             DocBody* b = planetBody(w->map, p.getStr("body"), &err);
             if (!b) throw std::runtime_error(err);
             Sculpt op = Sculpt::Raise;
             if (!parseSculpt(p.getStr("op"), op)) throw std::runtime_error("op must be raise, lower, smooth, flatten, noise or set");
             const int N = b->faceSize;
             w->map.beginEdit();
             int n = 0;
             if (p["points"].isArray()) {
                 const int f = int(p["face"].integer(0));
                 for (auto& q : p["points"].items())
                     n += planetSculpt(w->map, p.getStr("body"), op, planetDir(f, float(q[0].toDouble()), float(q[1].toDouble()), N), float(p["radius"].toDouble(4)),
                                       float(p["strength"].toDouble(op == Sculpt::Smooth || op == Sculpt::Flatten ? 0.5 : 1.0)), uint32_t(p.getInt("seed", 1)));
             } else
                 n = planetSculpt(w->map, p.getStr("body"), op, readDir(p, N), float(p["radius"].toDouble(4)),
                                  float(p["strength"].toDouble(op == Sculpt::Smooth || op == Sculpt::Flatten ? 0.5 : 1.0)), uint32_t(p.getInt("seed", 1)));
             w->note({Change::Map, 0, 0, -1, -1, -1, "Планета " + p.getStr("body") + ": " + sculptName(op)});
             Json r = okJson("Планета " + p.getStr("body") + ": " + sculptName(op) + ", углов изменено: " + std::to_string(n));
             r.set("corners", Json::number(int64_t(n)));
             return r;
         });
    wrap("terrain_paint", " Planets: pass body + face + x, y (or dir) + radius (cells) to paint the cells of a cube-sphere body.",
         R"({"body": {"type": "string"}, "face": {"type": "integer"}, "x": {"type": "number"}, "y": {"type": "number"}, "radius": {"type": "number"},
            "dir": {"type": "array", "items": {"type": "number"}}})", [w, needMap](const Json& p) {
             needMap();
             std::string err;
             DocBody* b = planetBody(w->map, p.getStr("body"), &err);
             if (!b) throw std::runtime_error(err);
             static const std::pair<const char*, char> kinds[] = {{"ground", '.'}, {"water", '~'}, {"blocked", '#'}, {"nobuild", ','}, {"shallow", '='}, {"ramp", '/'}, {"rock", 'R'}};
             const std::string k = p.getStr("kind");
             char c = k.size() == 1 ? k[0] : 0;
             for (auto& [n, v] : kinds) if (k == n) c = v;
             if (!c) throw std::runtime_error("unknown kind '" + k + "'");
             w->map.beginEdit();
             const int n = planetPaint(w->map, p.getStr("body"), readDir(p, b->faceSize), float(p["radius"].toDouble(3)), c);
             w->note({Change::Map, 0, 0, -1, -1, -1, "Планета " + p.getStr("body") + ": " + k});
             return okJson("Планета " + p.getStr("body") + ": «" + k + "», клеток: " + std::to_string(n));
         });
    wrap("terrain_material", " Planets: pass body + face + x, y (or dir) + radius; layer -1 = by cell kind.",
         where, [w, needMap](const Json& p) {
             needMap();
             std::string err;
             DocBody* b = planetBody(w->map, p.getStr("body"), &err);
             if (!b) throw std::runtime_error(err);
             w->map.beginEdit();
             const int n = planetMaterial(w->map, p.getStr("body"), readDir(p, b->faceSize), float(p["radius"].toDouble(3)), int(p["layer"].integer(0)));
             w->note({Change::Map, 0, 0, -1, -1, -1, "Планета " + p.getStr("body") + ": материал"});
             return okJson("Планета " + p.getStr("body") + ": материал, клеток: " + std::to_string(n));
         });
    ws.registerOp({"planet_info", "Bodies of the open map's world block (planets, moons, stars): id, kind, radius, faceSize, seaLevel, orbit, "
                   "corner height range. Planet tools take body + face (0..5) + x, y in cells (docs/planets.md).", Json::parse("{}"),
                   [w, needMap](const Json&) {
                       needMap();
                       Json r = Json::object(), list = Json::array();
                       for (const DocBody& b : w->map.data.bodies) {
                           Json e = Json::object();
                           e.set("id", Json::string(b.id));
                           for (const char* k : {"kind", "radius", "seaLevel", "orbitAltitude", "orbit"}) if (b.meta.has(k)) e.set(k, b.meta[k]);
                           e.set("faceSize", Json::number(int64_t(b.faceSize)));
                           if (!b.corners.empty()) {
                               auto [mn, mx] = std::minmax_element(b.corners.begin(), b.corners.end());
                               e.set("heightMin", Json::number(double(*mn) / 16.0, 2)); e.set("heightMax", Json::number(double(*mx) / 16.0, 2));
                           }
                           list.push(e);
                       }
                       r.set("bodies", list);
                       r.set("start", Json::string(w->map.data.worldExtra.getStr("start")));
                       r.set("summary", Json::string("Тел: " + std::to_string(list.size())));
                       return r;
                   }});
}

} // namespace rts::editor
