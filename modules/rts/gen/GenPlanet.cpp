// Planet terrain (docs/planets.md §2/§4): six cube-sphere faces sampled with 3D noise on the surface direction
// (RtsSphere.h), so the duplicated seam corners get bit-identical heights and faces join without cracks.
// Optional rotational symmetry about the polar (Y) axis for fair starts: heights are the average of the noise
// over the n rotations, exact for n = 2 and 4 (quarter turns map the cube-sphere onto itself). Latitude
// biomes and ice caps, craters for moons, sea level by quantile, flattened base areas. Integer / fixed only.
#include "GenInternal.h"
#include "modules/rts/RtsSphere.h"
#include "engine/core/FixedMath.h"

namespace rts::gen {

using game::sphereArcTurns;
namespace {
constexpr int32_t Q(int num, int den) { return int32_t(int64_t(num) * kOne / den); }
}

Vec3 rotY(Vec3 d, int k, int n) {
    if (n <= 1 || k % n == 0) return d;
    const Fixed a = Fixed::fromRatio(k % n, n);
    const Fixed co = fxCosTurns(a), si = fxSinTurns(a);
    return {d.x * co - d.z * si, d.y, d.x * si + d.z * co};
}

void PlanetGen::init(int n) {
    N = n; S = game::CubeSphere(n);
    const int CN = N + 1;
    hc.assign(size_t(6 * CN * CN), 0);
    dirs.resize(hc.size());
    for (int f = 0; f < 6; ++f)
        for (int j = 0; j <= N; ++j)
            for (int i = 0; i <= N; ++i) dirs[size_t(cid(f, i, j))] = S.cornerDir(f, i, j);
}

int32_t PlanetGen::cellH(int id) const {
    const game::SphereCell sc = S.cell(id);
    return int32_t((int64_t(hc[size_t(cid(sc.face, sc.x, sc.y))]) + hc[size_t(cid(sc.face, sc.x + 1, sc.y))] +
                    hc[size_t(cid(sc.face, sc.x, sc.y + 1))] + hc[size_t(cid(sc.face, sc.x + 1, sc.y + 1))]) / 4);
}

void PlanetGen::setCorner(int f, int i, int j, int32_t v) {
    int ids[3];
    const int n = S.cornerDuplicates(f, i, j, ids);
    for (int q = 0; q < n; ++q) hc[size_t(ids[q])] = v;
}

void PlanetGen::evenSeams() {
    for (int face = 0; face < 6; ++face)
        for (int j = 0; j <= N; ++j)
            for (int i = 0; i <= N; ++i) {
                if (i != 0 && j != 0 && i != N && j != N) continue;
                int ids[3]; const int n = S.cornerDuplicates(face, i, j, ids);
                int64_t s = 0; for (int q = 0; q < n; ++q) s += hc[size_t(ids[q])];
                const int32_t avg = int32_t(s / n);
                for (int q = 0; q < n; ++q) hc[size_t(ids[q])] = avg;
            }
}

void planetHeights(Ctx& c, PlanetGen& pg, int symN) {
    const int N = pg.N;
    const int64_t freq = std::max<int64_t>(1, (int64_t(N) * 4 * kOne) / std::max<int32_t>(c.pq("scale", Q(24, 1)), Q(2, 1)) / 6);
    Fbm f;
    f.octaves = std::clamp(c.pint("octaves", 5), 1, 8);
    f.gain = std::clamp(c.pq("roughness", Q(1, 2)), Q(1, 10), Q(9, 10));
    f.ridged = std::clamp(c.pq("ridged", Q(3, 10)), 0, kOne);
    f.warp = std::clamp(c.pq("warp", Q(1, 2)), 0, 4 * kOne);
    const int32_t relief = std::clamp(c.pq("relief", Q(8, 1)), Q(1, 2), Q(60, 1)) >> 4;   // kHU
    std::vector<std::pair<Vec3, int64_t>> craters;   // moons: seeded directions, bowl + rim
    for (int k = 0; k < std::clamp(c.pint("craters", 0), 0, 64); ++k) {
        const uint32_t a = hashCoords(k, 3, 9, c.seed), b = hashCoords(k, 5, 11, c.seed);
        Vec3 d{Fixed::fromRaw(int64_t(a & 0x1ffff) - kOne), Fixed::fromRaw(int64_t(b & 0x1ffff) - kOne), Fixed::fromRaw(int64_t((a >> 15) & 0x1ffff) - kOne)};
        if (d.lengthSq().raw == 0) continue;
        craters.push_back({fxNormalize(d), int64_t(Q(2, 100)) + int64_t(b >> 26) * 300});   // radius in turns
    }
    symN = std::max(1, symN);
    for (size_t k = 0; k < pg.hc.size(); ++k) {
        int64_t sum = 0;
        for (int r = 0; r < symN; ++r) {    // average over the rotations: the same set for every rotated point
            const Vec3 d = rotY(pg.dirs[k], r, symN);
            int64_t v = fbm3(d.x.raw * freq, d.y.raw * freq, d.z.raw * freq, c.sub(31), f);
            for (auto& [cd, cr] : craters) {
                const int64_t arc = sphereArcTurns(d, cd).raw;
                if (arc >= cr * 3 / 2) continue;
                const int64_t t = arc * kOne / cr;               // 0 centre, 1 rim
                v += t < kOne ? -(((kOne - ((t * t) >> 16)) * 3) / 8) : ((kOne * 3 / 2 - t) / 3);   // shallow bowl + low rim
            }
            sum += std::clamp<int64_t>(v, -kOne, kOne);   // overlapping craters never dig deeper than the relief
        }
        pg.hc[k] = int32_t(((sum / symN) * relief) >> 16);
    }
    pg.evenSeams();   // equal by construction (dirFromFace is bit-identical); averaged defensively
    const int32_t water = std::clamp(c.pq("water", Q(3, 10)), 0, Q(95, 100));
    pg.hasSea = water > 0;
    const int32_t sea = pg.hasSea ? quantile(pg.hc, water) : 0;
    if (pg.hasSea) for (auto& v : pg.hc) v -= sea;               // sea level = radius
}

void planetFlatten(PlanetGen& pg, Vec3 dir, int64_t arcTurns, int32_t minH) {
    const int32_t target = std::max(pg.cellH(pg.S.cellFromDir(dir)), minH);
    for (size_t k = 0; k < pg.hc.size(); ++k) {
        const int64_t arc = sphereArcTurns(pg.dirs[k], dir).raw;
        if (arc >= arcTurns * 2) continue;
        const int32_t t = smoothstepQ(int32_t(arcTurns * 2), int32_t(arcTurns), int32_t(arc));
        pg.hc[k] = int32_t(pg.hc[k] + ((int64_t(target - pg.hc[k]) * t) >> 16));
    }
}

void planetClassify(Ctx& c, PlanetGen& pg) {
    const int cells = pg.S.cellCount();
    pg.cell.assign(size_t(cells), '.');
    pg.biome.assign(size_t(cells), BGrass);
    std::vector<int32_t> land;
    for (int id = 0; id < cells; ++id) if (pg.cellH(id) >= 0 || !pg.hasSea) land.push_back(pg.cellH(id));
    const int32_t peak = land.empty() ? INT32_MAX : quantile(land, c.pq("peaks", Q(97, 100)));
    const int32_t ice = std::clamp(c.pq("ice", Q(1, 10)), 0, kOne);
    for (int id = 0; id < cells; ++id) {
        const Vec3 d = pg.S.cellCenterDir(id);
        const int64_t lat = d.y.raw < 0 ? -d.y.raw : d.y.raw;
        const int32_t hv = pg.cellH(id);
        uint8_t b = BGrass;
        char ch = '.';
        if (pg.hasSea && hv < 0) { ch = '~'; b = BOcean; }
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
        pg.cell[size_t(id)] = ch;
        pg.biome[size_t(id)] = b;
    }
}

void planetWrite(Ctx& c, PlanetGen& pg, Json& body) {
    const int N = pg.N;
    static const char* pref[BCount][3] = {{"lakebed", "earth", "sand"}, {"sand", "path", "earth"}, {"grass", "earth", "snow"}, {"forest", "grass", "earth"},
                                          {"sand", "desert", "path"}, {"dirt", "earth", "path"}, {"rock", "stone", "earth"}, {"snow", "ice", "rock"}, {"mud", "earth", "dirt"}};
    Json faces = Json::array(), matFaces = Json::array();
    for (int face = 0; face < 6; ++face) {
        Json rows = Json::array(), mrows = Json::array();
        for (int y = 0; y < N; ++y) {
            std::string r(size_t(N), '.'), m(size_t(N), '.');
            for (int x = 0; x < N; ++x) {
                const int id = pg.S.cellId(face, x, y);
                r[size_t(x)] = pg.cell[size_t(id)];
                for (const char* idp : pref[pg.biome[size_t(id)]]) {
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
    for (int32_t v : pg.hc) corners.push(Json::number(int64_t(std::clamp<int32_t>(v / (kHU / 16), -32000, 32000))));
    Json terrain = Json::object();
    terrain.set("faces", faces);
    terrain.set("corners", corners);
    if (!c.layerIds.empty()) terrain.set("material", matFaces);
    // visual detail hints (docs/terrain.md "Detail surface"; used when the game's terrain.style is "smooth"):
    // small craters on moons / barren bodies, calmer fine relief on ocean worlds
    Json look = Json::object();
    if (c.preset == "moon" || c.pbool("barren", false)) { look.set("craters", Json::number(0.7, 2)); look.set("roughness", Json::number(0.08, 2)); }
    else if (c.preset == "ocean") look.set("roughness", Json::number(0.04, 2));
    if (look.size() > 0) terrain.set("detailLook", look);
    body.set("terrain", terrain);
    body.set("seaLevel", Json::number(int64_t(0)));
}

// Terrain only (a body with "generate" inside a hand-made world): start sites are reported, nothing is placed.
bool generatePlanet(Ctx& c, Json& body, std::string* err) {
    const std::string shape = c.pstr("shape", "planet");
    if (shape != "planet") { if (err) *err = "body '" + body.getStr("id") + "' generate: preset '" + c.preset + "' is for flat maps (use rocky, ocean or moon)"; return false; }
    const int N = int(body.getInt("faceSize", c.pint("faceSize", 32)));
    if (N < 4 || N > 512) { if (err) *err = "body '" + body.getStr("id") + "': faceSize must be 4..512"; return false; }
    PlanetGen pg;
    pg.init(N);
    planetHeights(c, pg, 1);
    const int cells = pg.S.cellCount();
    const int32_t ice = std::clamp(c.pq("ice", Q(1, 10)), 0, kOne);
    std::vector<int> starts;
    std::vector<Vec3> sdirs;
    if (c.np > 0) {   // farthest-point sampling on land cell centres (great-circle distance)
        const uint32_t pick = hashCoords(1, 2, 3, c.seed);
        std::vector<int> cand;
        for (int id = 0; id < cells; ++id) {
            const Vec3 d = pg.S.cellCenterDir(id);
            const int64_t lat = d.y.raw < 0 ? -d.y.raw : d.y.raw;
            if ((pg.hasSea && pg.cellH(id) < kHU / 2) || lat > kOne - ice - Q(15, 100)) continue;
            cand.push_back(id);
        }
        if (cand.empty()) for (int id = 0; id < cells; ++id) cand.push_back(id);
        const int first = cand[pick % cand.size()];
        starts.push_back(first); sdirs.push_back(pg.S.cellCenterDir(first));
        while (int(starts.size()) < c.np) {
            int64_t bd = -1; int bi = cand[0];
            for (int id : cand) {
                const Vec3 d = pg.S.cellCenterDir(id);
                int64_t mn = INT64_MAX;
                for (auto& s : sdirs) mn = std::min<int64_t>(mn, sphereArcTurns(d, s).raw);
                if (mn > bd) { bd = mn; bi = id; }
            }
            starts.push_back(bi); sdirs.push_back(pg.S.cellCenterDir(bi));
        }
        const int64_t baseArc = std::max<int64_t>(Q(1, 200), int64_t(c.pint("baseRadius", std::max(3, N / 8))) * kOne / (4 * N));
        for (auto& d : sdirs) planetFlatten(pg, d, baseArc, pg.hasSea ? kHU : INT32_MIN / 2);
    }
    planetClassify(c, pg);
    planetWrite(c, pg, body);
    Json g = Json::object();
    g.set("preset", Json::string(c.preset));
    g.set("faceSize", Json::number(int64_t(N)));
    Json st = Json::array();
    for (int id : starts) {
        const game::SphereCell sc = pg.S.cell(id);
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
    body.set("generated", g);
    body.erase("generate");
    return true;
}

} // namespace rts::gen
