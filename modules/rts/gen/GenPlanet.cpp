// Planet generation (docs/planets.md §2/§4): six cube-sphere faces sampled with 3D noise on the surface
// direction (RtsSphere.h), so the duplicated seam corners get bit-identical heights and faces join without
// cracks. Latitude-based biomes and ice caps, craters for moons, sea level by quantile, start sites
// maximally separated on the sphere (great-circle distance), flattened base areas. Integer / fixed only.
#include "GenInternal.h"
#include "modules/rts/RtsSphere.h"
#include "engine/core/FixedMath.h"

namespace rts::gen {

using game::sphereArcTurns;
namespace {
constexpr int32_t Q(int num, int den) { return int32_t(int64_t(num) * kOne / den); }
}

bool generatePlanet(Ctx& c, Json& body, std::string* err) {
    const std::string shape = c.pstr("shape", "planet");
    if (shape != "planet") { if (err) *err = "body '" + body.getStr("id") + "' generate: preset '" + c.preset + "' is for flat maps (use rocky, ocean or moon)"; return false; }
    const int N = int(body.getInt("faceSize", c.pint("faceSize", 32)));
    if (N < 4 || N > 512) { if (err) *err = "body '" + body.getStr("id") + "': faceSize must be 4..512"; return false; }
    const game::CubeSphere S(N);
    const int CN = N + 1;
    const int64_t radius = body["radius"].isNumber() ? body["radius"].fx().raw : (int64_t(N) << 16);
    // feature frequency: `scale` cells per noise lattice cell at the equator (a face spans ~ N cells = 1/4 turn)
    const int64_t freq = std::max<int64_t>(1, (int64_t(N) * 4 * kOne) / std::max<int32_t>(c.pq("scale", Q(24, 1)), Q(2, 1)) / 6); // lattice units per unit dir
    Fbm f;
    f.octaves = std::clamp(c.pint("octaves", 5), 1, 8);
    f.gain = std::clamp(c.pq("roughness", Q(1, 2)), Q(1, 10), Q(9, 10));
    f.ridged = std::clamp(c.pq("ridged", Q(3, 10)), 0, kOne);
    f.warp = std::clamp(c.pq("warp", Q(1, 2)), 0, 4 * kOne);
    const int32_t relief = std::clamp(c.pq("relief", Q(8, 1)), Q(1, 2), Q(60, 1)) >> 4;   // kHU
    std::vector<int32_t> hc(size_t(6 * CN * CN));
    std::vector<Vec3> dirs(hc.size());
    // craters (moons): seeded directions, bowl + rim
    std::vector<std::pair<Vec3, int64_t>> craters;
    for (int k = 0; k < std::clamp(c.pint("craters", 0), 0, 64); ++k) {
        const uint32_t a = hashCoords(k, 3, 9, c.seed), b = hashCoords(k, 5, 11, c.seed);
        Vec3 d{Fixed::fromRaw(int64_t(a & 0x1ffff) - kOne), Fixed::fromRaw(int64_t(b & 0x1ffff) - kOne), Fixed::fromRaw(int64_t((a >> 15) & 0x1ffff) - kOne)};
        if (d.lengthSq().raw == 0) continue;
        craters.push_back({fxNormalize(d), int64_t(Q(2, 100)) + int64_t(b >> 26) * 300});   // radius in turns
    }
    for (int face = 0; face < 6; ++face)
        for (int j = 0; j <= N; ++j)
            for (int i = 0; i <= N; ++i) {
                const Vec3 d = S.cornerDir(face, i, j);
                const size_t k = size_t(face * CN * CN + j * CN + i);
                dirs[k] = d;
                int64_t v = fbm3(d.x.raw * freq, d.y.raw * freq, d.z.raw * freq, c.sub(31), f);
                for (auto& [cd, cr] : craters) {
                    const int64_t arc = sphereArcTurns(d, cd).raw;
                    if (arc >= cr * 3 / 2) continue;
                    const int64_t t = arc * kOne / cr;               // 0 centre, 1 rim
                    v += t < kOne ? -(((kOne - ((t * t) >> 16)) * 3) / 4) : ((kOne * 3 / 2 - t) * 2 / 3);
                }
                hc[k] = int32_t((v * relief) >> 16);
            }
    // seam corners: equal by construction (dirFromFace is bit-identical); average duplicates defensively
    for (int face = 0; face < 6; ++face)
        for (int j = 0; j <= N; ++j)
            for (int i = 0; i <= N; ++i) {
                if (i != 0 && j != 0 && i != N && j != N) continue;
                int ids[3]; const int n = S.cornerDuplicates(face, i, j, ids);
                int64_t s = 0; for (int q = 0; q < n; ++q) s += hc[size_t(ids[q])];
                const int32_t avg = int32_t(s / n);
                for (int q = 0; q < n; ++q) hc[size_t(ids[q])] = avg;
            }
    const int32_t water = std::clamp(c.pq("water", Q(3, 10)), 0, Q(95, 100));
    const int32_t sea = water > 0 ? quantile(hc, water) : INT32_MIN / 2;
    if (sea > INT32_MIN / 2) for (auto& v : hc) v -= sea;               // sea level = radius
    const int32_t sea0 = water > 0 ? 0 : INT32_MIN / 2;

    // start sites: farthest-point sampling on land cell centres (great-circle distance)
    const int cells = S.cellCount();
    auto cellH = [&](int id) {
        const game::SphereCell sc = S.cell(id);
        const int b = sc.face * CN * CN;
        return int32_t((int64_t(hc[size_t(b + sc.y * CN + sc.x)]) + hc[size_t(b + sc.y * CN + sc.x + 1)] + hc[size_t(b + (sc.y + 1) * CN + sc.x)] + hc[size_t(b + (sc.y + 1) * CN + sc.x + 1)]) / 4);
    };
    const int32_t ice = std::clamp(c.pq("ice", Q(1, 10)), 0, kOne);   // polar cap size (fraction of |y|)
    std::vector<int> starts;
    std::vector<Vec3> sdirs;
    if (c.np > 0) {
        int first = -1;
        const uint32_t pick = hashCoords(1, 2, 3, c.seed);
        std::vector<int> cand;
        for (int id = 0; id < cells; ++id) {
            const Vec3 d = S.cellCenterDir(id);
            const int64_t lat = d.y.raw < 0 ? -d.y.raw : d.y.raw;
            if ((sea0 > INT32_MIN / 2 && cellH(id) < kHU / 2) || lat > kOne - ice - Q(15, 100)) continue;
            cand.push_back(id);
        }
        if (cand.empty()) for (int id = 0; id < cells; ++id) cand.push_back(id);
        first = cand[pick % cand.size()];
        starts.push_back(first); sdirs.push_back(S.cellCenterDir(first));
        while (int(starts.size()) < c.np) {
            int64_t bd = -1; int bi = cand[0];
            for (int id : cand) {
                const Vec3 d = S.cellCenterDir(id);
                int64_t mn = INT64_MAX;
                for (auto& s : sdirs) mn = std::min<int64_t>(mn, sphereArcTurns(d, s).raw);
                if (mn > bd) { bd = mn; bi = id; }
            }
            starts.push_back(bi); sdirs.push_back(S.cellCenterDir(bi));
        }
        // flatten base areas (corners within baseArc of a start), never under water
        const int64_t baseArc = std::max<int64_t>(Q(1, 200), int64_t(c.pint("baseRadius", std::max(3, N / 8))) * kOne / (4 * N));
        for (size_t s = 0; s < starts.size(); ++s) {
            const int32_t target = std::max(cellH(starts[s]), kHU);
            for (size_t k = 0; k < hc.size(); ++k) {
                const int64_t arc = sphereArcTurns(dirs[k], sdirs[s]).raw;
                if (arc >= baseArc * 2) continue;
                const int32_t t = smoothstepQ(int32_t(baseArc * 2), int32_t(baseArc), int32_t(arc));
                hc[k] = int32_t(hc[k] + ((int64_t(target - hc[k]) * t) >> 16));
            }
        }
    }

    // classify cells per face
    Json faces = Json::array();
    std::vector<int32_t> land;
    for (int id = 0; id < cells; ++id) if (cellH(id) >= 0 || sea0 == INT32_MIN / 2) land.push_back(cellH(id));
    const int32_t peak = land.empty() ? INT32_MAX : quantile(land, c.pq("peaks", Q(97, 100)));
    Json matFaces = Json::array();
    for (int face = 0; face < 6; ++face) {
        Json rows = Json::array(), mrows = Json::array();
        for (int y = 0; y < N; ++y) {
            std::string r(size_t(N), '.'), m(size_t(N), '.');
            for (int x = 0; x < N; ++x) {
                const int id = S.cellId(face, x, y);
                const Vec3 d = S.cellCenterDir(id);
                const int64_t lat = d.y.raw < 0 ? -d.y.raw : d.y.raw;
                const int32_t hv = cellH(id);
                uint8_t b = BGrass;
                char ch = '.';
                if (sea0 > INT32_MIN / 2 && hv < 0) { ch = '~'; b = BOcean; }
                else if (hv >= peak && c.model != game::TerrainModel::Heightfield) { ch = '#'; b = BRock; }
                if (lat > kOne - ice) b = BSnow;                                 // ice caps (over water too: frozen sea)
                else if (b == BGrass) {
                    const int32_t moist = fbm3(d.x.raw * 3, d.y.raw * 3, d.z.raw * 3, c.sub(32), Fbm{}) / 2 + kOne / 2;
                    if (lat < Q(3, 10) && moist < Q(45, 100)) b = BDesert;
                    else if (moist > Q(58, 100)) b = BForest;
                    else if (lat > Q(6, 10)) b = BSteppe;
                }
                if (b == BSnow && ch == '~') ch = '.';   // frozen sea is walkable ice
                if (c.preset == "moon" || c.pbool("barren", false)) b = hv > 0 ? BRock : BSteppe;
                r[size_t(x)] = ch;
                static const char* pref[BCount][3] = {{"lakebed", "earth", "sand"}, {"sand", "path", "earth"}, {"grass", "earth", "snow"}, {"forest", "grass", "earth"},
                                                      {"sand", "desert", "path"}, {"dirt", "earth", "path"}, {"rock", "stone", "earth"}, {"snow", "ice", "rock"}, {"mud", "earth", "dirt"}};
                for (const char* idp : pref[b]) {
                    auto it = std::find(c.layerIds.begin(), c.layerIds.end(), idp);
                    if (it != c.layerIds.end() && it - c.layerIds.begin() < 4) { m[size_t(x)] = char('0' + (it - c.layerIds.begin())); break; }
                }
            }
            rows.push(Json::string(r)); mrows.push(Json::string(m));
        }
        Json fj = Json::object();
        fj.set("rows", rows);
        faces.push(fj);
        matFaces.push(mrows);
    }
    Json corners = Json::array();
    for (int32_t v : hc) corners.push(Json::number(int64_t(std::clamp<int32_t>(v / (kHU / 16), -32000, 32000))));
    Json terrain = Json::object();
    terrain.set("faces", faces);
    terrain.set("corners", corners);
    if (!c.layerIds.empty()) terrain.set("material", matFaces);
    body.set("terrain", terrain);
    if (!body.has("seaLevel")) body.set("seaLevel", Json::number(int64_t(0)));
    Json g = Json::object();
    g.set("preset", Json::string(c.preset));
    g.set("faceSize", Json::number(int64_t(N)));
    Json st = Json::array();
    for (int id : starts) {
        const game::SphereCell sc = S.cell(id);
        Json a = Json::object();
        a.set("body", Json::string(body.getStr("id")));
        a.set("face", Json::number(int64_t(sc.face)));
        a.set("x", Json::numberText(fmtQ16((int64_t(sc.x) << 16) + 32768)));
        a.set("y", Json::numberText(fmtQ16((int64_t(sc.y) << 16) + 32768)));
        st.push(a);
    }
    g.set("starts", st);
    if (sdirs.size() >= 2) {
        int64_t mn = INT64_MAX;
        for (size_t a = 0; a < sdirs.size(); ++a)
            for (size_t b = a + 1; b < sdirs.size(); ++b) mn = std::min<int64_t>(mn, sphereArcTurns(sdirs[a], sdirs[b]).raw);
        g.set("minStartArc", Json::numberText(fmtQ16(mn)));
    }
    (void)radius;
    body.set("generated", g);
    body.erase("generate");
    return true;
}

} // namespace rts::gen
