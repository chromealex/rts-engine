// Planet detail grids in the editor core (docs/terrain.md "Detail surface", planets): create a body's detail
// grid from the surface the view draws (same synthesis: RtsPlanetDetail.cpp), sculpt it with 3D brushes that
// cross the face seams (duplicated seam samples written equal), keep the corners on it, and the MCP side
// (terrain_detail / terrain_sculpt with body).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <stdexcept>
#include "PlanetOps.h"
#include "modules/rts/RtsMap.h"
#include "modules/rts/RtsSphere.h"
#include "modules/rts/view/RtsPlanet.h"

namespace rts::editor {

using render::float3;

namespace {
constexpr float kQuarterPi = 0.785398163f;

Json visualTerrainOf(const Workspace& ws) {
    Json t = ws.project.rulesJson["terrain"].isObject() ? ws.project.rulesJson["terrain"] : Json::object();
    if (ws.project.manifest["terrain"].isObject()) for (auto& [k, v] : ws.project.manifest["terrain"].members()) t.set(k, v);
    return t;
}

// Sample directions of a detail grid (float, like the view: duplicates share the canonical one).
struct SampleCache {
    std::vector<float3> dirs;
    std::vector<int> canon;
};
const SampleCache& samplesOf(int N, int k) {
    static std::map<std::pair<int, int>, SampleCache> cache;
    auto it = cache.find({N, k});
    if (it != cache.end()) return it->second;
    if (cache.size() > 4) cache.clear();
    SampleCache c;
    const int M = N * k, S = (M + 1) * (M + 1);
    game::CubeSphere cs(M);
    c.dirs.resize(size_t(6 * S));
    c.canon.resize(size_t(6 * S));
    for (int f = 0; f < 6; ++f)
        for (int j = 0; j <= M; ++j)
            for (int i = 0; i <= M; ++i) {
                const int id = (f * (M + 1) + j) * (M + 1) + i;
                c.canon[size_t(id)] = (i == 0 || j == 0 || i == M || j == M) ? cs.canonicalCorner(f, i, j) : id;
                c.dirs[size_t(id)] = planetDir(f, float(i) / float(k), float(j) / float(k), N);
            }
    for (size_t s = 0; s < c.dirs.size(); ++s) c.dirs[s] = c.dirs[size_t(c.canon[s])];
    return cache.emplace(std::pair{N, k}, std::move(c)).first->second;
}

constexpr float kU = game::kPlanetDetailUnit;   // detail values per world unit (corners: 16)
int16_t qd(float h) { return int16_t(std::clamp(std::lround(h * kU), -32768l, 32767l)); }
int16_t detailToCorner(int16_t v) { return int16_t(std::clamp(std::lround(float(v) * 16.0f / kU), -32768l, 32767l)); }
} // namespace

game::PlanetBody planetBodyOf(const DocBody& b) {
    game::PlanetBody pb;
    pb.id = b.id;
    const std::string kind = b.meta.getStr("kind", "planet");
    pb.kind = kind == "moon" ? game::PlanetBody::Kind::Moon : kind == "star" ? game::PlanetBody::Kind::Star : game::PlanetBody::Kind::Planet;
    pb.radius = std::max(1.0f, float(b.meta["radius"].toDouble(32)));
    pb.faceSize = b.faceSize;
    pb.corners = b.corners;
    pb.flags.resize(b.cells.size());
    for (size_t i = 0; i < b.cells.size(); ++i) pb.flags[i] = game::cellFlagsFromChar(b.cells[i]);
    const std::string st = b.terrainExtra.getStr("style");
    pb.style = st == "smooth" ? 1 : st == "classic" ? 0 : -1;
    if (const Json& dl = b.terrainExtra["detailLook"]; dl.isObject()) {
        pb.detailScale = int(dl["scale"].integer(0));
        pb.roughness = float(dl["roughness"].toDouble(-1));
        pb.craters = float(dl["craters"].toDouble(-1));
    }
    return pb;
}

int planetDetailScaleFor(int N, int want) {
    want = std::clamp(want, 1, 16);
    auto fits = [&](int k) { const size_t M = size_t(N) * size_t(k); return 6 * (M + 1) * (M + 1) <= size_t(2400000) && 3 * (M + 1) <= 4096; };
    while (want > 1 && !fits(want)) want /= 2;
    return want;
}

bool ensurePlanetDetail(Workspace& ws, DocBody& b, int k) {
    if (!b.surface()) return false;
    if (b.detailScale > 0 && b.detail.size() == size_t(6) * size_t(b.detailSide() + 1) * size_t(b.detailSide() + 1)) return false;
    k = planetDetailScaleFor(b.faceSize, k);
    const game::PlanetBody pb = planetBodyOf(b);
    const Json vt = visualTerrainOf(ws);
    const bool smooth = pb.style == 1 || (pb.style < 0 && vt.getStr("style") == "smooth");
    game::PlanetDetailParams p;
    p.k = k;
    // same defaults as the view (PlanetRenderer::setupDetail): the game's detailLook.roughness x 1.5, craters on moons
    const float gameRough = std::clamp(float(vt["detailLook"]["roughness"].toDouble(0.04)), 0.0f, 1.0f);
    p.roughness = pb.roughness >= 0 ? pb.roughness : gameRough * 1.5f;
    p.craters = pb.craters >= 0 ? pb.craters : pb.kind == game::PlanetBody::Kind::Moon ? 0.6f : 0.0f;
    uint32_t h = 2166136261u;
    for (char ch : pb.id) h = (h ^ uint8_t(ch)) * 16777619u;
    p.seed = h | 1u;
    if (!smooth) { p.roughness = 0; p.craters = 0; }   // classic look: only the bicubic surface through the corners
    game::PlanetSurface s;
    s.init(pb);
    s.initDetail(pb, p);
    if (!s.detailScale()) return false;
    const int M = s.gridSize();
    b.detailScale = k;
    b.detail.assign(size_t(6) * size_t(M + 1) * size_t(M + 1), 0);
    for (int f = 0; f < 6; ++f)
        for (int j = 0; j <= M; ++j)
            for (int i = 0; i <= M; ++i) b.detail[b.detailIndex(f, i, j)] = qd(s.gridHeight(f, i, j));
    return true;
}

void planetDetailFollowCorners(DocBody& b, const std::vector<int16_t>& oldCorners) {
    if (b.detailScale <= 0 || oldCorners.size() != b.corners.size()) return;
    const int N = b.faceSize, k = b.detailScale, M = N * k;
    if (b.detail.size() != size_t(6) * size_t(M + 1) * size_t(M + 1)) return;
    for (int f = 0; f < 6; ++f)
        for (int Y = 0; Y < N; ++Y)
            for (int X = 0; X < N; ++X) {
                float d[4];
                bool any = false;
                for (int q = 0; q < 4; ++q) {
                    const size_t c = b.cornerIndex(f, X + (q & 1), Y + (q >> 1));
                    d[q] = (float(b.corners[c]) - float(oldCorners[c])) * (kU / 16.0f);
                    any |= d[q] != 0;
                }
                if (!any) continue;
                // the cell's samples (its right / bottom edge belongs to the next cell unless at the face edge)
                const int i1 = X == N - 1 ? k : k - 1, j1 = Y == N - 1 ? k : k - 1;
                for (int jj = 0; jj <= j1; ++jj)
                    for (int ii = 0; ii <= i1; ++ii) {
                        const float fx = float(ii) / float(k), fy = float(jj) / float(k);
                        const float dv = (d[0] * (1 - fx) + d[1] * fx) * (1 - fy) + (d[2] * (1 - fx) + d[3] * fx) * fy;
                        int16_t& v = b.detail[b.detailIndex(f, X * k + ii, Y * k + jj)];
                        v = int16_t(std::clamp(std::lround(float(v) + dv), -32768l, 32767l));
                    }
            }
    // seam samples of different faces got the same bilinear delta (the corners along the edge are shared)
}

int planetDetailSculpt(Workspace& ws, const std::string& id, Sculpt op, float3 dir, float radius, float strength, float hardness, uint32_t seed, std::string* err) {
    DocBody* body = planetBody(ws.map, id, err);
    if (!body) return 0;
    if (op == Sculpt::Erode || op == Sculpt::Stamp) { if (err) *err = "erode / stamp are not available on planets (raise, lower, smooth, flatten, noise, set)"; return -1; }
    ensurePlanetDetail(ws, *body, 4);
    if (body->detailScale <= 0) { if (err) *err = "body has no detail grid"; return -1; }
    const int N = body->faceSize, k = body->detailScale, M = N * k;
    const SampleCache& sc = samplesOf(N, k);
    std::vector<int16_t>& v = body->detail;
    const float cellAngle = 2.0f * kQuarterPi / float(N);
    const float ang = std::max(radius, 0.5f / float(k)) * cellAngle, cosAng = std::cos(ang);
    dir = render::normalize(dir);
    const float soft = std::max(1e-3f, 1.0f - std::clamp(hardness, 0.0f, 0.95f));
    std::vector<std::pair<int, float>> hit;
    float target = 0, wsum = 0;
    for (size_t s = 0; s < sc.dirs.size(); ++s) {
        if (sc.canon[s] != int(s)) continue;
        const float c = dot(sc.dirs[s], dir);
        if (c < cosAng) continue;
        const float a = std::acos(std::clamp(c, -1.0f, 1.0f));
        float t = std::clamp((1 - a / ang) / soft, 0.0f, 1.0f);
        t = t * t * (3 - 2 * t);
        hit.push_back({int(s), t});
        target += float(v[s]) * t; wsum += t;
    }
    if (hit.empty()) return 0;
    target = wsum > 0 ? target / wsum : 0;
    auto at = [&](int f, int i, int j) { return float(v[body->detailIndex(f, std::clamp(i, 0, M), std::clamp(j, 0, M))]); };
    std::vector<std::pair<int, int16_t>> out;
    out.reserve(hit.size());
    const int S = (M + 1) * (M + 1);
    for (auto [s, w] : hit) {
        float h = float(v[size_t(s)]);
        const float3 d = sc.dirs[size_t(s)];
        switch (op) {
            case Sculpt::Raise: h += strength * kU * w; break;
            case Sculpt::Lower: h -= strength * kU * w; break;
            case Sculpt::Noise: {
                const float q = 977.0f * float(k);
                const uint32_t hs = uint32_t(int(d.x * q) * 73856093 ^ int(d.y * q) * 19349663 ^ int(d.z * q) * 83492791) ^ (seed * 2246822519u);
                h += strength * kU * w * (float((hs ^ (hs >> 13)) & 0xffff) / 65535.0f * 2.0f - 1.0f);
                break;
            }
            case Sculpt::Set: h += (strength * kU - h) * w; break;
            case Sculpt::Flatten: h += (target - h) * w * std::clamp(strength, 0.0f, 1.0f); break;
            case Sculpt::Smooth: {
                const int f = s / S, j = (s % S) / (M + 1), i = s % (M + 1);
                float m = 0;
                for (int dj = -1; dj <= 1; ++dj) for (int di = -1; di <= 1; ++di) m += at(f, i + di, j + dj);
                h += (m / 9.0f - h) * w * std::clamp(strength, 0.0f, 1.0f);
                break;
            }
            default: break;
        }
        out.push_back({s, int16_t(std::clamp(std::lround(h), -32768l, 32767l))});
    }
    std::vector<int32_t> nv(v.size(), INT32_MIN);
    for (auto [s, h] : out) nv[size_t(s)] = h;
    int changed = 0;
    for (size_t s = 0; s < v.size(); ++s) { // every duplicate of a changed canonical sample (seams equal)
        const int32_t h = nv[size_t(sc.canon[s])];
        if (h == INT32_MIN || v[s] == int16_t(h)) continue;
        v[s] = int16_t(h);
        ++changed;
    }
    // the corners (simulation surface) stay on the detail
    for (int f = 0; f < 6; ++f)
        for (int J = 0; J <= N; ++J)
            for (int I = 0; I <= N; ++I) body->corners[body->cornerIndex(f, I, J)] = detailToCorner(v[body->detailIndex(f, I * k, J * k)]);
    ws.map.touched();
    return changed;
}

// terrain_detail with `body`
Json planetDetailOp(Workspace& ws, const Json& p) {
    std::string err;
    DocBody* b = planetBody(ws.map, p.getStr("body"), &err);
    if (!b) throw std::runtime_error(err);
    const std::string a = p.getStr("action", "info");
    Json r = Json::object();
    if (a == "create") {
        ws.map.beginEdit();
        b->detail.clear(); b->detailScale = 0;
        ensurePlanetDetail(ws, *b, int(p.getInt("scale", 4)));
        ws.map.touched();
        ws.note({Change::Map, 0, 0, -1, -1, -1, "Планета " + b->id + ": детальный рельеф"});
        r.set("summary", Json::string("Планета " + b->id + ": детальный рельеф x" + std::to_string(b->detailScale)));
    } else if (a == "remove") {
        ws.map.beginEdit();
        b->detail.clear(); b->detailScale = 0; b->detailFile.clear();
        ws.map.touched();
        ws.note({Change::Map, 0, 0, -1, -1, -1, "Планета " + b->id + ": детальный рельеф удалён"});
        r.set("summary", Json::string("Планета " + b->id + ": детальный рельеф удалён"));
    } else if (a != "info") throw std::runtime_error("action must be info, create or remove");
    r.set("ok", Json::fromBool(true));
    r.set("body", Json::string(b->id));
    r.set("scale", Json::number(int64_t(b->detailScale)));
    if (b->detailScale > 0 && !b->detail.empty()) {
        const auto [mn, mx] = std::minmax_element(b->detail.begin(), b->detail.end());
        r.set("samples", Json::number(int64_t(b->detail.size())));
        r.set("min", Json::number(double(*mn) / kU, 2)); r.set("max", Json::number(double(*mx) / kU, 2));
    }
    if (!r.has("summary")) r.set("summary", Json::string(b->detailScale > 0 ? "Детальный рельеф x" + std::to_string(b->detailScale) : "Нет детального рельефа (вид синтезирует при style smooth)"));
    return r;
}

} // namespace rts::editor
