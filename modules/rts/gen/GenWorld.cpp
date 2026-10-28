// Whole planet maps (docs/planets.md §4): a planet preset used as the map's "generate" builds the `world` block
// (bodies, per-face rows, corners, material), the players' starts and every object, from the template's world
// (bodies, orbits, visuals) or a default sun + planet + moon. Layouts: "shared" (all players on the main body,
// rotational symmetry about the polar axis, exact for 2 and 4 players), "separate" (every player on an identical
// planet of its own). Moons carry extra resources (moonResources). Starts are flattened, connected by graded
// causeways when needed, and validated with the engine's own pathfinder on the generated map. Integer only.
#include "GenInternal.h"
#include "engine/core/FixedMath.h"
#include "engine/sim/Simulation.h"
#include "modules/rts/RtsModule.h"
#include <map>
#include <queue>

namespace rts::gen {

namespace {

constexpr int32_t Q(int num, int den) { return int32_t(int64_t(num) * kOne / den); }

struct Rng {
    uint64_t s;
    uint32_t next() { s = mix64(s); return uint32_t(s >> 32); }
    uint32_t below(uint32_t n) { return n ? next() % n : 0; }
};

// ---------------------------------------------------------------- sphere helpers

int64_t lonSector(Vec3 d, int n) {             // which of n longitude sectors (rotation images) the direction is in
    if (n <= 1) return 0;
    int64_t a = fxAtan2Turns(d.z, d.x).raw;     // (-0.5, 0.5]
    if (a < 0) a += kOne;
    int64_t k = a * n / kOne;
    return std::clamp<int64_t>(k, 0, n - 1);
}

// direction `dx`, `dy` cells east / south of `d` on face `face` (great-circle step, smooth across seams)
Vec3 stepDir(const game::CubeSphere& S, int face, Vec3 d, int64_t dxQ, int64_t dyQ) {
    if (dxQ == 0 && dyQ == 0) return d;
    Vec3 e, s;
    game::CubeSphere::faceTangents(face, d, e, s);
    const int64_t N = S.faceSize();
    Vec3 v = e * Fixed::fromRaw(dxQ) + s * Fixed::fromRaw(dyQ);          // in cells
    const Fixed len = v.length();
    if (len.raw == 0) return d;
    const Fixed turns = Fixed::fromRaw(len.raw / (4 * N));                 // a cell is 1/(4N) turn
    return fxNormalize(d * fxCosTurns(turns) + (v / len) * fxSinTurns(turns));
}

int64_t arcCells(const game::CubeSphere& S, Vec3 a, Vec3 b) {          // Q16 cells along the great circle
    return game::sphereArcTurns(a, b).raw * 4 * S.faceSize();
}

struct Body {
    std::string id;
    Json def = Json::object();
    bool moon = false, star = false, main = false;
    int owner = 0;                       // separate layout: the player living here
    PlanetGen pg;
    std::vector<uint8_t> occ;            // per cell: 1 object, 2 corridor, 3 base core
    int32_t buildRise = INT32_MAX, walkRise = INT32_MAX;
    int64_t cellSizeRaw = kOne;
};

struct World {
    Ctx& c;
    std::vector<Body> bodies;
    int mainB = 0;
    int symN = 1;
    bool exact = false;
    std::string layout = "shared";
    std::vector<std::pair<int, Vec3>> starts;   // body, direction
    Json entities = Json::array();
    std::map<std::string, int> objCount;
    explicit World(Ctx& cc) : c(cc) {}

    // ---- geometry
    Vec3 image(Vec3 d, int k) const { return rotY(d, k, symN); }
    void cornerImage(const PlanetGen& pg, int f, int i, int j, int k, int& f2, int& i2, int& j2) const {
        Fixed a, b;
        pg.S.faceFromDir(rotY(pg.dirs[size_t(pg.cid(f, i, j))], k, symN), f2, a, b);
        i2 = std::clamp(int((a.raw + 32768) >> 16), 0, pg.N);
        j2 = std::clamp(int((b.raw + 32768) >> 16), 0, pg.N);
    }
    // exact rotational symmetry of the heights (n = 2, 4: quarter turns map corners onto corners)
    void symmetrizeHeights(Body& B) {
        if (!exact || symN <= 1) return;
        PlanetGen& pg = B.pg;
        const int N = pg.N;
        std::vector<int32_t> out = pg.hc;
        for (int f = 0; f < 6; ++f)
            for (int j = 0; j <= N; ++j)
                for (int i = 0; i <= N; ++i) {
                    int best = pg.S.canonicalCorner(f, i, j);
                    for (int k = 1; k < symN; ++k) {
                        int f2, i2, j2;
                        cornerImage(pg, f, i, j, k, f2, i2, j2);
                        best = std::min(best, pg.S.canonicalCorner(f2, i2, j2));
                    }
                    out[size_t(pg.cid(f, i, j))] = pg.hc[size_t(best)];
                }
        pg.hc = out;
        pg.evenSeams();
    }
    int32_t cornerRange(const PlanetGen& pg, int id) const {
        const game::SphereCell s = pg.S.cell(id);
        int32_t lo = INT32_MAX, hi = INT32_MIN;
        for (auto [dx, dy] : {std::pair{0, 0}, std::pair{1, 0}, std::pair{0, 1}, std::pair{1, 1}}) {
            const int32_t v = pg.hc[size_t(pg.cid(s.face, s.x + dx, s.y + dy))];
            lo = std::min(lo, v); hi = std::max(hi, v);
        }
        return hi - lo;
    }
    bool walkable(const Body& B, int id) const {
        const char ch = B.pg.cell[size_t(id)];
        return (ch == '.' || ch == '=') && cornerRange(B.pg, id) <= B.walkRise;
    }

    // ---- placement
    struct Spot { int face = 0, x0 = 0, y0 = 0; Fixed a, b; };
    bool fits(const Body& B, int ti, int face, int x0, int y0, bool allowCore, bool ring) const {
        const game::UnitType& t = c.R.types[size_t(ti)];
        const int N = B.pg.N, fw = t.structure ? t.footprintW : 1, fh = t.structure ? t.footprintH : 1;
        if (x0 < 0 || y0 < 0 || x0 + fw > N || y0 + fh > N) return false;
        int32_t lo = INT32_MAX, hi = INT32_MIN;
        for (int y = y0; y < y0 + fh; ++y)
            for (int x = x0; x < x0 + fw; ++x) {
                const int id = B.pg.S.cellId(face, x, y);
                const uint8_t o = B.occ[size_t(id)];
                if (o == 1 || (o == 3 && !allowCore) || (o == 2 && t.structure)) return false;
                if (B.pg.cell[size_t(id)] != '.') return false;
                if (!t.structure && !walkable(B, id)) return false;
            }
        if (t.structure)
            for (int y = y0; y <= y0 + fh; ++y)
                for (int x = x0; x <= x0 + fw; ++x) { const int32_t v = B.pg.hc[size_t(B.pg.cid(face, x, y))]; lo = std::min(lo, v); hi = std::max(hi, v); }
        if (t.structure && hi - lo > B.buildRise) return false;
        if (ring && t.structure)
            for (int y = y0 - 1; y <= y0 + fh; ++y)
                for (int x = x0 - 1; x <= x0 + fw; ++x)
                    if (x >= 0 && y >= 0 && x < N && y < N && B.occ[size_t(B.pg.S.cellId(face, x, y))] == 1) return false;
        return true;
    }
    // footprint origin of a structure centred on direction d (as the simulation snaps it), nudged up to `nudge` cells
    bool find(const Body& B, int ti, Vec3 d, bool allowCore, bool ring, int nudge, Spot& out) const {
        const game::UnitType& t = c.R.types[size_t(ti)];
        int face; Fixed a, b;
        B.pg.S.faceFromDir(d, face, a, b);
        const int fw = t.structure ? t.footprintW : 1, fh = t.structure ? t.footprintH : 1;
        const int bx = int(((a.raw - (int64_t(fw) << 15)) + 32768) >> 16), by = int(((b.raw - (int64_t(fh) << 15)) + 32768) >> 16);
        for (int r = 0; r <= nudge; ++r)
            for (int dy = -r; dy <= r; ++dy)
                for (int dx = -r; dx <= r; ++dx) {
                    if (std::max(std::abs(dx), std::abs(dy)) != r) continue;
                    const int x0 = t.structure ? bx + dx : int(a.floorInt()) + dx, y0 = t.structure ? by + dy : int(b.floorInt()) + dy;
                    if (!fits(B, ti, face, x0, y0, allowCore, ring)) continue;
                    out.face = face; out.x0 = x0; out.y0 = y0;
                    if (t.structure) { out.a = Fixed::fromInt(x0) + Fixed::fromInt(fw) / 2; out.b = Fixed::fromInt(y0) + Fixed::fromInt(fh) / 2; }
                    else if (r == 0) { out.a = a; out.b = b; }
                    else { out.a = Fixed::fromInt(x0) + Fixed::fromRatio(1, 2); out.b = Fixed::fromInt(y0) + Fixed::fromRatio(1, 2); }
                    return true;
                }
        return false;
    }
    void mark(Body& B, int ti, const Spot& s) {
        const game::UnitType& t = c.R.types[size_t(ti)];
        if (!t.structure) return;
        for (int y = s.y0; y < s.y0 + t.footprintH; ++y)
            for (int x = s.x0; x < s.x0 + t.footprintW; ++x) B.occ[size_t(B.pg.S.cellId(s.face, x, y))] = 1;
    }
    void emit(const Body& B, int ti, int owner, const Spot& s, const Json& extra) {
        Json e = Json::object();
        e.set("type", Json::string(c.R.types[size_t(ti)].id));
        e.set("owner", Json::number(int64_t(owner)));
        Json at = Json::object();
        at.set("body", Json::string(B.id));
        at.set("face", Json::number(int64_t(s.face)));
        at.set("x", Json::numberText(fmtQ16(s.a.raw)));
        at.set("y", Json::numberText(fmtQ16(s.b.raw)));
        e.set("at", at);
        for (auto& [k, v] : extra.members()) e.set(k, v);
        entities.push(e);
        objCount[c.R.types[size_t(ti)].id]++;
    }
    // one object per rotation image (owner -100: image k belongs to player k+1); all or nothing
    bool placeSet(Body& B, int ti, Vec3 d, int owner, const Json& extra, bool allowCore, bool ring, int nudge, int images) {
        std::vector<Spot> spots;
        std::vector<uint8_t> save = B.occ;
        for (int k = 0; k < images; ++k) {
            Spot s;
            if (!find(B, ti, image(d, k), allowCore, ring, k == 0 ? nudge : (exact ? 0 : 2), s)) { B.occ = save; return false; }
            mark(B, ti, s);
            spots.push_back(s);
        }
        for (size_t k = 0; k < spots.size(); ++k) emit(B, ti, owner == -100 ? int(k) + 1 : owner, spots[k], extra);
        return true;
    }
};

// position of a template entity / start: body id ("" = flat), face, (x, y) in cells
struct TPos { bool planet = false; std::string body; int face = 0; int64_t x = 0, y = 0; bool ok = false; };
TPos tpos(const Json& p) {
    TPos t;
    if (p.isObject()) { t.planet = true; t.body = p.getStr("body"); t.face = int(p.getInt("face", 0)); t.x = p.getFx("x").raw; t.y = p.getFx("y").raw; t.ok = true; }
    else if (p.isArray() && p.size() >= 2) { t.x = p[size_t(0)].fx().raw; t.y = p[size_t(p.size() >= 3 ? 2 : 1)].fx().raw; t.ok = true; }
    return t;
}

} // namespace

bool generateWorld(Ctx& c, const GenRequest& req, Json& map, std::string* err) {
    World W(c);
    Rng rng{c.seed64 ^ 0x77306c64ull};
    const Json& tmpl = c.tmpl;
    W.layout = c.pstr("layout", "shared");
    if (W.layout != "shared" && W.layout != "separate") { if (err) *err = "generate.params.layout: shared or separate"; return false; }
    const int np = c.np;
    W.symN = W.layout == "shared" ? std::max(1, np) : 1;
    if (req.symmetry == "none") W.symN = 1;
    W.exact = W.symN == 2 || W.symN == 4;
    const int defN = std::clamp(c.pint("faceSize", np <= 2 ? 24 : np <= 4 ? 32 : 40), 8, 128);

    // ---- bodies: the template's world (orbits, visuals) or sun + planet + moon
    const Json& tw = tmpl["world"];
    std::vector<Json> defs;
    std::string startId = tw.getStr("start");
    for (auto& b : tw["bodies"].items()) defs.push_back(b);
    auto planetDef = [&](const std::string& id, int N, const char* kind) {
        Json b = Json::object();
        b.set("id", Json::string(id)); b.set("kind", Json::string(kind));
        b.set("radius", Json::number(int64_t(std::max(4, N * 2 / 3))));
        b.set("faceSize", Json::number(int64_t(N)));
        b.set("orbitAltitude", Json::number(int64_t(std::max(3, N / 6))));
        return b;
    };
    if (defs.empty()) {
        Json sun = Json::object();
        sun.set("id", Json::string("sun")); sun.set("kind", Json::string("star")); sun.set("radius", Json::number(int64_t(40)));
        Json vis = Json::object(); vis.set("color", Json::parse("[255, 220, 160]")); sun.set("visual", vis);
        defs.push_back(sun);
        Json p = planetDef("terra", defN, "planet");
        p.set("orbit", Json::parse("{\"around\": \"sun\", \"radius\": 400, \"period\": 1200, \"phase\": 0.1}"));
        defs.push_back(p);
        if (c.pbool("moon", true)) {
            Json m = planetDef("luna", std::max(8, defN / 2), "moon");
            m.set("orbit", Json::parse("{\"around\": \"terra\", \"radius\": 60, \"period\": 300}"));
            defs.push_back(m);
        }
        startId = "terra";
    }
    if (W.layout == "separate") {   // one identical planet per player around the star; the template's main body is the prototype
        Json proto;
        for (auto& d : defs) if (d.getStr("kind", "planet") == "planet" && (startId.empty() || d.getStr("id") == startId)) { proto = d; break; }
        if (!proto.isObject()) proto = planetDef("terra", defN, "planet");
        std::vector<Json> nd;
        std::string star;
        for (auto& d : defs) if (d.getStr("kind") == "star") { nd.push_back(d); if (star.empty()) star = d.getStr("id"); }
        for (int p = 0; p < np; ++p) {
            Json b = proto;
            b.set("id", Json::string("world" + std::to_string(p + 1)));
            b.erase("terrain"); b.erase("generate"); b.erase("generated");
            if (!star.empty()) {
                Json o = Json::object();
                o.set("around", Json::string(star)); o.set("radius", Json::number(int64_t(400)));
                o.set("period", Json::number(int64_t(1200))); o.set("phase", Json::numberText(fmtQ16(int64_t(p) * kOne / np)));
                b.set("orbit", o);
            }
            nd.push_back(b);
        }
        for (auto& d : defs) if (d.getStr("kind") == "moon") {   // moons go around the star, between the worlds
            Json m = d;
            Json o = Json::object();
            o.set("around", Json::string(star.empty() ? "world1" : star)); o.set("radius", Json::number(int64_t(250))); o.set("period", Json::number(int64_t(700)));
            if (!star.empty()) m.set("orbit", o);
            nd.push_back(m);
        }
        defs = nd;
        startId = "world1";
    }
    if (defs.size() > 16) { if (err) *err = "generate: at most 16 bodies"; return false; }
    const int baseR = std::clamp(c.pint("baseRadius", std::max(4, defN / 3)), 2, 24);
    const int32_t moonMul = std::clamp(c.pq("moonResources", 2 * kOne), 0, 8 * kOne);
    for (size_t i = 0; i < defs.size(); ++i) {
        Body B;
        B.def = defs[i];
        B.def.erase("terrain"); B.def.erase("generated");
        B.id = B.def.getStr("id", "body" + std::to_string(i));
        const std::string kind = B.def.getStr("kind", "planet");
        B.star = kind == "star";
        B.moon = kind == "moon";
        if (!B.star) {
            const int N = int(B.def.getInt("faceSize", defN));
            if (N < 4 || N > 256) { if (err) *err = "body '" + B.id + "': faceSize must be 4..256 for generation"; return false; }
            B.pg.init(N);
            B.occ.assign(size_t(B.pg.S.cellCount()), 0);
            const Fixed radius = B.def.getFx("radius", Fixed::fromInt(std::max(4, N * 2 / 3)));
            B.cellSizeRaw = (radius * game::kFxTwoPi / (4 * N)).raw;
            if (c.model == game::TerrainModel::Heightfield) {   // rise per cell edge in kHU, with a margin under the rules' limits
                B.buildRise = int32_t((((c.R.terrain.buildSlope.raw * B.cellSizeRaw) >> 16) * kHU >> 16) * 7 / 10);
                B.walkRise = int32_t((((c.R.terrain.maxSlopeGround.raw * B.cellSizeRaw) >> 16) * kHU >> 16) * 7 / 10);
            }
        }
        W.bodies.push_back(std::move(B));
    }
    W.mainB = -1;
    for (size_t i = 0; i < W.bodies.size(); ++i) if (!W.bodies[i].star && (startId.empty() || W.bodies[i].id == startId) && W.mainB < 0) W.mainB = int(i);
    for (size_t i = 0; i < W.bodies.size() && W.mainB < 0; ++i) if (!W.bodies[i].star) W.mainB = int(i);
    if (W.mainB < 0) { if (err) *err = "generate: the world has no body with a surface"; return false; }
    W.bodies[size_t(W.mainB)].main = true;

    // ---- terrain per body (a moon uses the moon preset's params unless the body says otherwise)
    Json moonP;
    presetParams("moon", c.rulesJson, moonP, nullptr);
    uint32_t sharedSeed = c.seed;
    for (size_t i = 0; i < W.bodies.size(); ++i) {
        Body& B = W.bodies[i];
        if (B.star) continue;
        Ctx bc(c.R, c.rulesJson);
        bc.P = c.P; bc.preset = c.preset; bc.model = c.model; bc.np = np;
        if (B.moon && c.preset != "moon") { bc.P = moonP; bc.preset = "moon"; for (auto& [k, v] : c.P["moonParams"].members()) bc.P.set(k, v); }
        const bool proto = W.layout == "separate" && !B.moon;   // every world of a separate layout is the same planet
        bc.seed = proto ? sharedSeed : hash32(c.seed ^ uint32_t(i * 0x9e3779b9u));
        bc.seed64 = mix64(bc.seed);
        planetHeights(bc, B.pg, B.main && W.layout == "shared" ? W.symN : 1);
        if (B.main && W.layout == "shared") W.symmetrizeHeights(B);
        B.pg.cell.assign(size_t(B.pg.S.cellCount()), '.');
        planetClassify(bc, B.pg);
    }

    // ---- start sites: main body, longitude sector 0, near the equator, on land; images by rotation
    auto chooseStart = [&](Body& B) {
        const int cells = B.pg.S.cellCount();
        int64_t bestScore = INT64_MIN; int best = 0;
        for (int id = 0; id < cells; ++id) {
            const Vec3 d = B.pg.S.cellCenterDir(id);
            if (W.symN > 1 && lonSector(d, W.symN) != 0) continue;
            const int64_t lat = d.y.raw < 0 ? -d.y.raw : d.y.raw;
            if (lat > Q(35, 100)) continue;
            int64_t score = -lat * 2 - int64_t(rng.below(kOne / 2));
            if (B.pg.hasSea) {   // land around: count land cells within the base radius
                int land = 0;
                for (int dy = -baseR; dy <= baseR; dy += 2)
                    for (int dx = -baseR; dx <= baseR; dx += 2) { const int o = B.pg.S.offset(id, dx, dy); if (o >= 0 && B.pg.cellH(o) >= 0) ++land; }
                score += int64_t(land) * kOne / 4;
            }
            if (W.symN > 1) {    // middle of the sector: as far from the neighbours as possible
                int64_t a = fxAtan2Turns(d.z, d.x).raw; if (a < 0) a += kOne;
                score -= std::abs(a - kOne / (2 * W.symN)) * 2;
            }
            if (score > bestScore) { bestScore = score; best = id; }
        }
        return B.pg.S.cellCenterDir(best);
    };
    if (W.layout == "shared") {
        Body& M = W.bodies[size_t(W.mainB)];
        Vec3 d0 = chooseStart(M);
        if (W.symN > 1) for (int k = 0; k < np; ++k) W.starts.push_back({W.mainB, W.image(d0, k)});
        else {   // one player (or symmetry none): farthest-point sampling of land cells
            W.starts.push_back({W.mainB, d0});
            while (int(W.starts.size()) < np) {
                int64_t bd = -1; Vec3 bp = d0;
                for (int id = 0; id < M.pg.S.cellCount(); id += 3) {
                    const Vec3 d = M.pg.S.cellCenterDir(id);
                    if (M.pg.hasSea && M.pg.cellH(id) < 0) continue;
                    int64_t mn = INT64_MAX;
                    for (auto& s : W.starts) mn = std::min<int64_t>(mn, game::sphereArcTurns(d, s.second).raw);
                    if (mn > bd) { bd = mn; bp = d; }
                }
                W.starts.push_back({W.mainB, bp});
            }
        }
    } else {
        Body& P1 = W.bodies[size_t(W.mainB)];
        const Vec3 d0 = chooseStart(P1);
        int p = 0;
        for (size_t i = 0; i < W.bodies.size() && p < np; ++i)
            if (!W.bodies[i].star && !W.bodies[i].moon) { W.bodies[i].owner = ++p; W.starts.push_back({int(i), d0}); }
    }
    const int64_t baseArc = int64_t(baseR) * kOne / (4 * W.bodies[size_t(W.mainB)].pg.N);
    for (auto& [b, d] : W.starts) {   // flatten, never under water
        Body& B = W.bodies[size_t(b)];
        planetFlatten(B.pg, d, baseArc, B.pg.hasSea ? kHU : INT32_MIN / 2);
    }
    for (auto& B : W.bodies) if (!B.star) { B.pg.evenSeams(); if (B.main && W.layout == "shared") W.symmetrizeHeights(B); }

    // ---- connectivity (shared layout): graded causeways between starts where the ground does not connect
    auto reclassify = [&](Body& B, size_t bi) {
        Ctx bc(c.R, c.rulesJson);
        bc.P = c.P; bc.preset = c.preset; bc.model = c.model;
        if (B.moon && c.preset != "moon") { bc.P = moonP; bc.preset = "moon"; }
        bc.seed = W.layout == "separate" && !B.moon ? sharedSeed : hash32(c.seed ^ uint32_t(bi * 0x9e3779b9u));
        bc.seed64 = mix64(bc.seed);
        planetClassify(bc, B.pg);
    };
    for (size_t i = 0; i < W.bodies.size(); ++i) if (!W.bodies[i].star) reclassify(W.bodies[i], i);
    int repairs = 0;
    const std::string connect = c.pstr("connect", "ground");
    if (W.layout == "shared" && np >= 2 && connect != "none") {
        Body& M = W.bodies[size_t(W.mainB)];
        const game::CubeSphere& S = M.pg.S;
        const int cells = S.cellCount();
        for (int round = 0; round < 8; ++round) {
            const int s0 = S.cellFromDir(W.starts[0].second);
            std::vector<uint8_t> seen(static_cast<size_t>(cells), 0);
            std::vector<int> q{s0}; seen[size_t(s0)] = 1;
            for (size_t qi = 0; qi < q.size(); ++qi)
                for (auto [dx, dy] : {std::pair{1, 0}, std::pair{-1, 0}, std::pair{0, 1}, std::pair{0, -1}}) {
                    const int n = S.neighbor(q[qi], dx, dy);
                    if (n < 0 || seen[size_t(n)] || !W.walkable(M, n)) continue;
                    seen[size_t(n)] = 1; q.push_back(n);
                }
            int bad = -1;
            for (size_t k = 1; k < W.starts.size() && bad < 0; ++k) if (!seen[size_t(S.cellFromDir(W.starts[k].second))]) bad = int(k);
            if (bad < 0) break;
            ++repairs;
            // cheapest cell route (non-walkable cells cost more), then a road: smoothed heights, limited climb
            const int t = S.cellFromDir(W.starts[size_t(bad)].second);
            std::vector<int32_t> dist(static_cast<size_t>(cells), INT32_MAX), par(static_cast<size_t>(cells), -1);
            using It = std::pair<int32_t, int32_t>;
            std::priority_queue<It, std::vector<It>, std::greater<It>> pq;
            dist[size_t(s0)] = 0; pq.push({0, s0});
            while (!pq.empty()) {
                auto [dd, i] = pq.top(); pq.pop();
                if (dd != dist[size_t(i)]) continue;
                if (i == t) break;
                for (auto [dx, dy] : {std::pair{1, 0}, std::pair{-1, 0}, std::pair{0, 1}, std::pair{0, -1}}) {
                    const int n = S.neighbor(i, dx, dy);
                    if (n < 0) continue;
                    const int cost = W.walkable(M, n) ? 10 : M.pg.cell[size_t(n)] == '~' ? 50 : 70;
                    if (dd + cost < dist[size_t(n)]) { dist[size_t(n)] = dd + cost; par[size_t(n)] = i; pq.push({dist[size_t(n)], n}); }
                }
            }
            std::vector<int> path;
            for (int i = t; i >= 0; i = par[size_t(i)]) path.push_back(i);
            std::reverse(path.begin(), path.end());
            std::vector<int32_t> h(path.size());
            for (size_t k = 0; k < path.size(); ++k) {
                int64_t sum = 0; int cnt = 0;
                for (int d = -3; d <= 3; ++d) {
                    const int64_t j = int64_t(k) + d;
                    if (j < 0 || j >= int64_t(path.size())) continue;
                    sum += std::max(M.pg.cellH(path[size_t(j)]), M.pg.hasSea ? kHU / 4 : INT32_MIN / 4); ++cnt;
                }
                h[k] = int32_t(sum / cnt);
            }
            const int32_t step = M.walkRise == INT32_MAX ? INT32_MAX / 4 : std::max<int32_t>(1, M.walkRise / 3);
            for (size_t k = 1; k < h.size(); ++k) h[k] = std::clamp(h[k], h[k - 1] - step, h[k - 1] + step);
            for (size_t k = h.size() - 1; k-- > 0;) h[k] = std::clamp(h[k], h[k + 1] - step, h[k + 1] + step);
            for (size_t k = 0; k < path.size(); ++k) {
                const game::SphereCell sc = S.cell(path[k]);
                for (int r = 0; r < W.symN; ++r)
                    for (int dj = -1; dj <= 2; ++dj)
                        for (int di = -1; di <= 2; ++di) {
                            const int i = std::clamp(sc.x + di, 0, M.pg.N), j = std::clamp(sc.y + dj, 0, M.pg.N);
                            int f2 = sc.face, i2 = i, j2 = j;
                            if (r) W.cornerImage(M.pg, sc.face, i, j, r, f2, i2, j2);
                            M.pg.setCorner(f2, i2, j2, h[k]);
                        }
            }
            reclassify(M, size_t(W.mainB));
            for (size_t k = 0; k < path.size(); ++k)   // the causeway itself is open ground (peaks / water cut through)
                for (int r = 0; r < W.symN; ++r) {
                    const int id = r ? S.cellFromDir(W.image(S.cellCenterDir(path[k]), r)) : path[k];
                    if (M.pg.cell[size_t(id)] == '#' || M.pg.cell[size_t(id)] == '~') M.pg.cell[size_t(id)] = '.';
                }
            for (int i : path) M.occ[size_t(i)] = 2;
        }
    }

    // ---- what to place: the template's player-1 kit and neutral objects, else the rules
    struct KitItem { int type; int64_t dx, dy; int count; Json extra; };
    struct Spec { int near = 0, far = 0, moon = 0, pole = 0; std::vector<std::pair<int64_t, int64_t>> nearOff; Json extra = Json::object(); };
    std::vector<KitItem> kit;
    std::map<int, Spec> specs;
    const int tnp = std::max<int>(1, int(tmpl["players"].size()));
    // anchor of template player p: start, else its first object
    std::vector<TPos> anchors(static_cast<size_t>(tnp));
    for (int p = 1; p <= tnp; ++p) {
        const Json& pj = tmpl["players"][size_t(p - 1)];
        if (pj.has("start")) anchors[size_t(p - 1)] = tpos(pj["start"]);
    }
    for (auto& e : tmpl["entities"].items()) {
        const int o = int(e.getInt("owner", 0));
        if (o >= 1 && o <= tnp && !anchors[size_t(o - 1)].ok) anchors[size_t(o - 1)] = tpos(e.has("at") ? e["at"] : e["pos"]);
    }
    // cells between a template position and a template anchor (dx, dy in the anchor's face frame), -1: other body
    auto tBodyN = [&](const std::string& id) { for (auto& b : tw["bodies"].items()) if (b.getStr("id") == id) return int(b.getInt("faceSize", 0)); return 0; };
    auto tBodyKind = [&](const std::string& id) { for (auto& b : tw["bodies"].items()) if (b.getStr("id") == id) return b.getStr("kind", "planet"); return std::string("planet"); };
    auto offsetOf = [&](const TPos& a, const TPos& p, int64_t& dx, int64_t& dy) {
        if (!a.ok || !p.ok) return false;
        if (!a.planet) { dx = p.x - a.x; dy = p.y - a.y; return true; }
        if (p.body != a.body) return false;
        const int N = tBodyN(a.body);
        if (N <= 0) return false;
        if (p.face == a.face) { dx = p.x - a.x; dy = p.y - a.y; return true; }
        const game::CubeSphere S(N);
        const Vec3 da = S.dirFromFace(a.face, Fixed::fromRaw(a.x), Fixed::fromRaw(a.y)), dp = S.dirFromFace(p.face, Fixed::fromRaw(p.x), Fixed::fromRaw(p.y));
        Vec3 e, s;
        game::CubeSphere::faceTangents(a.face, da, e, s);
        const Vec3 v = dp - da;
        const int64_t cellRad = (game::kFxTwoPi / (4 * N)).raw;   // radians per cell on the unit sphere
        dx = v.dot(e).raw * kOne / cellRad; dy = v.dot(s).raw * kOne / cellRad;
        return true;
    };
    const int64_t nearR = int64_t(std::max(6, c.pint("nearRadius", std::max(8, defN / 3)))) << 16;
    for (auto& e : tmpl["entities"].items()) {
        const int ti = c.R.findType(e.getStr("type"));
        if (ti < 0) continue;
        const int o = int(e.getInt("owner", 0));
        const TPos p = tpos(e.has("at") ? e["at"] : e["pos"]);
        int64_t dx = 0, dy = 0;
        if (o == 1) {
            if (offsetOf(anchors[0], p, dx, dy)) {
                Json ex = Json::object();
                if (e.has("garrison")) ex.set("garrison", e["garrison"]);
                kit.push_back({ti, dx, dy, 1, ex});
            }
            continue;
        }
        if (o > 1) continue;
        Spec& s = specs[ti];
        if (e.has("amount") && !s.extra.has("amount")) s.extra.set("amount", e["amount"]);
        if (p.planet && !anchors[0].ok) { s.far++; continue; }
        if (p.planet && p.body != anchors[0].body) { if (tBodyKind(p.body) == "moon") s.moon++; else s.far++; continue; }
        int64_t best = INT64_MAX; int an = 0;
        for (int k = 0; k < tnp; ++k) {
            int64_t ax, ay;
            if (!offsetOf(anchors[size_t(k)], p, ax, ay)) continue;
            const int64_t d = isqrt(((ax * ax) >> 16) + ((ay * ay) >> 16)) << 8;
            if (d < best) { best = d; an = k; if (k == 0) { dx = ax; dy = ay; } }
        }
        if (best <= nearR) { s.near++; if (an == 0) s.nearOff.push_back({dx, dy}); }
        else s.far++;
    }
    for (auto& [ti, s] : specs) { s.near = (s.near + tnp - 1) / tnp; s.far = (s.far + tnp / 2) / tnp; }
    bool factionKit = false;
    for (auto& f : c.R.factions) if (!f.start.empty()) factionKit = true;
    if (kit.empty() && !factionKit) {   // rules heuristics, as on flat maps
        int hall = -1, worker = -1, key = -1;
        for (size_t i = 0; i < c.R.types.size(); ++i) {
            const auto& t = c.R.types[i];
            if (t.key && key < 0) key = int(i);
            if (t.structure && hall < 0 && t.resourceNode < 0)
                for (uint16_t pt : t.produces) if (c.R.types[pt].worker && !c.R.types[pt].structure) { hall = int(i); worker = pt; break; }
        }
        if (hall >= 0) { kit.push_back({hall, 0, 0, 1, Json::object()}); kit.push_back({worker, 3 << 16, 3 << 16, 4, Json::object()}); }
        else if (key >= 0) kit.push_back({key, 0, 0, 1, Json::object()});
    }
    if (specs.empty())
        for (size_t i = 0; i < c.R.types.size(); ++i) {
            Spec s;
            const Kind k = classify(c, int(i));
            if (k == Kind::Mine || k == Kind::Spot) { s.near = 4; s.far = 2; s.moon = 4; }
            else if (k == Kind::Tree) { s.near = 2; s.far = 4; s.moon = 4; }
            else if (k == Kind::Creep) { s.far = 1; s.extra.set("owner", Json::number(int64_t(-1))); }
            else continue;
            specs[int(i)] = s;
        }
    for (auto& o : c.P["objects"].items()) {
        const int ti = c.R.findType(o.getStr("type"));
        if (ti < 0) { c.warn("objects: unknown type '" + o.getStr("type") + "'"); continue; }
        Spec& s = specs[ti];
        s.near = int(o.getInt("near", s.near)); s.far = int(o.getInt("far", s.far)); s.moon = int(o.getInt("moon", s.moon)); s.pole = int(o.getInt("pole", s.pole));
        if (o.has("near")) s.nearOff.clear();
        if (o.has("amount")) s.extra.set("amount", o["amount"]);
    }
    const int32_t resMul = std::clamp(c.pq("resources", kOne), 0, 8 * kOne);

    // ---- reserve base cores, place the kit (every player's copy is the rotation image)
    for (auto& [b, d] : W.starts) {
        Body& B = W.bodies[size_t(b)];
        const int r = std::clamp(c.pint("coreRadius", 3), 1, baseR);   // kit only; resources may sit right outside
        for (int id = 0; id < B.pg.S.cellCount(); ++id)
            if (arcCells(B.pg.S, B.pg.S.cellCenterDir(id), d) <= (int64_t(r) << 16) && B.occ[size_t(id)] != 2) B.occ[size_t(id)] = 3;
    }
    auto startFace = [&](int p) { Fixed a, b; int f; W.bodies[size_t(W.starts[size_t(p)].first)].pg.S.faceFromDir(W.starts[size_t(p)].second, f, a, b); return f; };
    auto placeForAll = [&](int ti, int64_t dx, int64_t dy, int owner, const Json& extra, bool core, bool ring, int nudge) {
        // owner -100: the player's own; else neutral/hostile. Shared: rotation images; separate: same face coords on each world.
        Body& B0 = W.bodies[size_t(W.starts[0].first)];
        const Vec3 d1 = stepDir(B0.pg.S, startFace(0), W.starts[0].second, dx, dy);
        if (W.layout == "shared" && W.symN > 1) return W.placeSet(B0, ti, d1, owner, extra, core, ring, nudge, np);
        bool ok = true;
        for (int p = 0; p < np; ++p) {
            Body& B = W.bodies[size_t(W.starts[size_t(p)].first)];
            const Vec3 d = W.layout == "separate" ? d1 : stepDir(B.pg.S, startFace(p), W.starts[size_t(p)].second, dx, dy);
            World::Spot s;
            if (!W.find(B, ti, d, core, ring, nudge, s)) { ok = false; continue; }
            W.mark(B, ti, s);
            W.emit(B, ti, owner == -100 ? p + 1 : owner, s, extra);
        }
        return ok;
    };
    for (auto& it : kit)
        for (int n = 0; n < it.count; ++n) {
            const int64_t dx = it.dx + int64_t(n % 4) * Q(4, 5), dy = it.dy + int64_t(n / 4) * Q(4, 5);
            if (!placeForAll(it.type, dx, dy, -100, it.extra, true, false, 3)) c.warn("kit: no room for '" + c.R.types[size_t(it.type)].id + "'");
        }
    // near objects: template offsets, else a ring around the base
    for (auto& [ti, s] : specs) {
        const int nearN = int((int64_t(s.near) * resMul + kOne / 2) >> 16);
        const int owner = int(s.extra.getInt("owner", 0));
        Json ex = s.extra; ex.erase("owner");
        int got = 0;
        for (int n = 0; n < nearN; ++n) {
            int64_t dx, dy;
            if (n < int(s.nearOff.size())) { dx = s.nearOff[size_t(n)].first; dy = s.nearOff[size_t(n)].second; }
            else {
                const Fixed a = Fixed::fromRaw(int64_t(rng.below(65536)));
                const int64_t r = int64_t(baseR + 1 + int(rng.below(3))) << 16;
                dx = (r * fxCosTurns(a).raw) >> 16; dy = (r * fxSinTurns(a).raw) >> 16;
            }
            bool ok = placeForAll(ti, dx, dy, owner, ex, false, true, 3);
            for (int t = 0; !ok && t < 6; ++t) {   // blocked: try the same distance at other angles
                const Fixed a = Fixed::fromRaw(int64_t(rng.below(65536)));
                const int64_t r = isqrt(((dx * dx) >> 16) + ((dy * dy) >> 16)) << 8;
                ok = placeForAll(ti, (r * fxCosTurns(a).raw) >> 16, (r * fxSinTurns(a).raw) >> 16, owner, ex, false, true, 2);
            }
            if (ok) ++got;
        }
        if (got < nearN) c.warn("only " + std::to_string(got) + " of " + std::to_string(nearN) + " '" + c.R.types[size_t(ti)].id + "' near each base");
    }
    // far objects: random cells away from every start (sector 0 + images on the shared body; per world when separate)
    for (auto& [ti, s] : specs) {
        const int farN = int((int64_t(s.far) * resMul + kOne / 2) >> 16);
        const int owner = int(s.extra.getInt("owner", 0));
        Json ex = s.extra; ex.erase("owner");
        Body& B0 = W.bodies[size_t(W.starts[0].first)];
        int placed = 0;
        for (int tries = 0; placed < farN && tries < farN * 60 + 40; ++tries) {
            const int id = int(rng.below(uint32_t(B0.pg.S.cellCount())));
            const Vec3 d = B0.pg.S.cellCenterDir(id);
            if (W.layout == "shared" && W.symN > 1 && lonSector(d, W.symN) != 0) continue;
            bool farOk = true;
            for (auto& st : W.starts) if (st.first == W.starts[0].first && arcCells(B0.pg.S, d, st.second) < nearR + (2 << 16)) farOk = false;
            if (!farOk) continue;
            bool ok;
            if (W.layout == "shared") ok = W.placeSet(B0, ti, d, owner, ex, false, true, 1, W.symN > 1 ? np : 1);
            else {
                ok = true;
                std::vector<World::Spot> sp;
                for (int p = 0; p < np && ok; ++p) { World::Spot q; ok = W.find(W.bodies[size_t(W.starts[size_t(p)].first)], ti, d, false, true, 0, q); sp.push_back(q); }
                if (ok) for (int p = 0; p < np; ++p) { Body& B = W.bodies[size_t(W.starts[size_t(p)].first)]; W.mark(B, ti, sp[size_t(p)]); W.emit(B, ti, owner, sp[size_t(p)], ex); }
            }
            if (ok) ++placed;
        }
        // poles (fixed points of the rotation): shared objects for everybody
        for (int q = 0; q < s.pole; ++q) {
            Vec3 pole{Fixed{}, Fixed::fromInt(q % 2 ? -1 : 1), Fixed{}};
            W.placeSet(B0, ti, pole, owner, ex, false, true, 2, 1);
        }
    }
    // moons: resource-rich (moonResources x the template's moon objects), scattered
    for (size_t bi = 0; bi < W.bodies.size(); ++bi) {
        Body& B = W.bodies[bi];
        if (!B.moon) continue;
        for (auto& [ti, s] : specs) {
            const int n = int((int64_t(std::max(s.moon, 0)) * moonMul * resMul / kOne + kOne / 2) >> 16);
            Json ex = s.extra; ex.erase("owner");
            for (int k = 0, tries = 0; k < n && tries < n * 40 + 20; ++tries) {
                World::Spot sp;
                if (W.find(B, ti, B.pg.S.cellCenterDir(int(rng.below(uint32_t(B.pg.S.cellCount())))), false, true, 1, sp)) { W.mark(B, ti, sp); W.emit(B, ti, 0, sp, ex); ++k; }
            }
        }
    }

    // ---- write the map
    Json out = Json::object();
    out.set("name", Json::string(map.getStr("name", "Generated world")));
    Json players = Json::array();
    for (int p = 0; p < np; ++p) {
        Json pj = p < int(map["players"].size()) ? map["players"][size_t(p)] : Json::object();
        if (!pj.has("name")) pj.set("name", Json::string("Player " + std::to_string(p + 1)));
        if (!pj.has("team")) pj.set("team", Json::number(int64_t(p + 1)));
        Body& B = W.bodies[size_t(W.starts[size_t(p)].first)];
        int f; Fixed a, b;
        B.pg.S.faceFromDir(W.starts[size_t(p)].second, f, a, b);
        Json at = Json::object();
        at.set("body", Json::string(B.id)); at.set("face", Json::number(int64_t(f)));
        at.set("x", Json::numberText(fmtQ16(a.raw))); at.set("y", Json::numberText(fmtQ16(b.raw)));
        pj.set("start", at);
        players.push(pj);
    }
    for (auto& [k, v] : map.members()) {
        if (k == "generate" || k == "terrain" || k == "fields" || k == "entities" || k == "regions" || k == "roads" || k == "terrainMaterial" ||
            k == "size" || k == "players" || k == "name" || k == "tiles" || k == "territory" || k == "generated" || k == "world") continue;
        out.set(k, v);
    }
    out.set("players", players);
    Json world = Json::object();
    Json bodies = Json::array();
    for (size_t i = 0; i < W.bodies.size(); ++i) {
        Body& B = W.bodies[i];
        Json b = B.def;
        b.erase("generate");
        if (!B.star) {
            Ctx bc(c.R, c.rulesJson);
            bc.layerIds = c.layerIds;
            planetWrite(bc, B.pg, b);
        }
        bodies.push(b);
    }
    world.set("bodies", bodies);
    world.set("start", Json::string(W.bodies[size_t(W.starts.empty() ? W.mainB : W.starts[0].first)].id));
    for (auto& [k, v] : tw.members()) if (k != "bodies" && k != "start") world.set(k, v);
    out.set("world", world);
    out.set("entities", W.entities);

    // ---- fairness: the engine's own pathfinder on the generated map
    Json g = Json::object();
    g.set("preset", Json::string(c.preset));
    g.set("seed", Json::number(int64_t(req.seed & 0x7fffffffffffffffull)));
    g.set("players", Json::number(int64_t(np)));
    g.set("layout", Json::string(W.layout));
    g.set("symmetry", Json::string(W.symN > 1 ? (W.exact ? "polar-exact" : "polar") : "none"));
    g.set("repairs", Json::number(int64_t(repairs)));
    Json counts = Json::object();
    for (auto& [k, v] : W.objCount) counts.set(k, Json::number(int64_t(v)));
    g.set("objects", counts);
    {   // resources within nearRadius of each start (equalised by symmetry; reported for every layout)
        Json nr = Json::array();
        std::vector<int> per(static_cast<size_t>(np), 0);
        for (auto& e : W.entities.items()) {
            const int ti = c.R.findType(e.getStr("type"));
            const Kind k = classify(c, ti);
            if (k != Kind::Mine && k != Kind::Spot && k != Kind::Tree) continue;
            const int b = [&] { for (size_t i = 0; i < W.bodies.size(); ++i) if (W.bodies[i].id == e["at"].getStr("body")) return int(i); return -1; }();
            if (b < 0) continue;
            const Vec3 d = W.bodies[size_t(b)].pg.S.dirFromFace(int(e["at"].getInt("face")), e["at"].getFx("x"), e["at"].getFx("y"));
            for (int p = 0; p < np; ++p)
                if (W.starts[size_t(p)].first == b && arcCells(W.bodies[size_t(b)].pg.S, d, W.starts[size_t(p)].second) <= nearR) per[size_t(p)]++;
        }
        for (int v : per) nr.push(Json::number(int64_t(v)));
        g.set("nearResources", nr);
        if (np > 1 && *std::min_element(per.begin(), per.end()) != *std::max_element(per.begin(), per.end())) c.warn("resources near the starts differ: " + nr.dump(-1));
    }
    if (W.layout == "shared" && np >= 2) {
        Json probe = out;
        probe.erase("generated");
        SimConfig cfg;
        Simulation sim(cfg, std::make_unique<game::RtsModule>());
        try { sim.setup(c.rulesJson, probe); }
        catch (const std::exception& e) { if (err) *err = std::string("generated world does not load: ") + e.what(); return false; }
        auto* mod = dynamic_cast<game::RtsModule*>(&sim.module());
        game::MapGrid& grid = mod->state(sim).grid;
        std::vector<Vec3> pos;
        const game::Surface* surf = mod->surface(sim);
        for (int p = 0; p < np; ++p) {
            const Json& at = players[size_t(p)]["start"];
            const int b = surf->findBody(at.getStr("body"));
            pos.push_back(game::Surface::anchor(b) + surf->facePoint(b, int(at.getInt("face")), at.getFx("x"), at.getFx("y")));
        }
        Json paths = Json::array();
        bool all = true;
        int64_t mn = INT64_MAX, mx = 0;
        for (int a = 0; a < np; ++a)
            for (int b = a + 1; b < np; ++b) {
                int ca = grid.cellOf(pos[size_t(a)]), cb = grid.cellOf(pos[size_t(b)]);
                int ax = ca % grid.w, ay = ca / grid.w, bx = cb % grid.w, by = cb / grid.w;
                grid.nearestPassable(ax, ay, game::kLayerGround, 8); grid.nearestPassable(bx, by, game::kLayerGround, 8);
                const Vec3 pa = grid.cellCenter(ax, ay), pb = grid.cellCenter(bx, by);
                std::vector<Vec3> path;
                bool found = grid.findPath(pa, pb, game::kLayerGround, path, grid.w * grid.h * 2);
                if (found && !path.empty() && grid.cellOf(path.back()) != grid.index(bx, by)) found = false;
                int64_t len = 0;
                Vec3 q = pa;
                for (auto& wp : path) { len += (wp - q).length().raw; q = wp; }
                Json e = Json::array();
                e.push(Json::number(int64_t(a + 1))); e.push(Json::number(int64_t(b + 1)));
                e.push(found ? Json::numberText(fmtQ16(len)) : Json::string("none"));
                paths.push(e);
                all = all && found;
                if (found) { mn = std::min(mn, len); mx = std::max(mx, len); }
            }
        g.set("paths", paths);
        g.set("connected", Json::fromBool(all));
        if (!all) c.warn("some start areas are not connected by ground");
        if (mn != INT64_MAX && mn > 0) {
            // neighbours only for rotations: opposite players are farther by design; compare each player's nearest rival
            std::vector<int64_t> nearest(static_cast<size_t>(np), INT64_MAX);
            for (auto& e : paths.items()) {
                if (!e[size_t(2)].isNumber()) continue;
                const int64_t l = e[size_t(2)].fx().raw;
                const int a = int(e[size_t(0)].integer()) - 1, b = int(e[size_t(1)].integer()) - 1;
                nearest[size_t(a)] = std::min(nearest[size_t(a)], l); nearest[size_t(b)] = std::min(nearest[size_t(b)], l);
            }
            const int64_t lo = *std::min_element(nearest.begin(), nearest.end()), hi = *std::max_element(nearest.begin(), nearest.end());
            const int64_t spread = lo > 0 && hi != INT64_MAX ? (hi - lo) * 1000 / lo : 0;
            g.set("pathSpread", Json::number(spread));
            if (spread > c.pint("maxPathSpread", 150)) c.warn("path distances to the nearest rival differ by " + std::to_string(spread / 10) + "%");
        }
    } else if (W.layout == "separate") {
        bool orbit = false;
        for (auto& t : c.R.types) if (t.layer & game::kLayerOrbit) orbit = true;
        if (!orbit) c.warn("separate worlds but no orbit-layer unit in the rules: players cannot reach each other");
        g.set("connected", Json::fromBool(orbit));
    }
    if (!c.warnings.empty()) { Json wj = Json::array(); for (auto& s : c.warnings) wj.push(Json::string(s)); g.set("warnings", wj); }
    out.set("generated", g);
    map = std::move(out);
    return true;
}

} // namespace rts::gen
