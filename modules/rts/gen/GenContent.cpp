// Content stage of the generator: start positions, the start kit, resources, forests, creeps and neutral
// structures (all from the game's own types), symmetric placement and fairness validation with the engine's
// pathfinder. Integer / fixed only.
#include "GenInternal.h"
#include "engine/core/FixedMath.h"
#include <map>

namespace rts::gen {

std::string fmtQ16(int64_t raw) {
    const bool neg = raw < 0;
    uint64_t a = uint64_t(neg ? -raw : raw);
    uint64_t ip = a >> 16, fr = ((a & 0xffff) * 100000 + 32768) >> 16;
    if (fr >= 100000) { ++ip; fr -= 100000; }
    std::string s = std::to_string(ip);
    if (fr) {
        std::string f = std::to_string(fr);
        f = std::string(5 - f.size(), '0') + f;
        while (!f.empty() && f.back() == '0') f.pop_back();
        s += "." + f;
    }
    return (neg && (ip || fr)) ? "-" + s : s;
}

namespace {

constexpr int32_t Q(int num, int den) { return int32_t(int64_t(num) * kOne / den); }

int64_t dist(P2 a, P2 b) {
    const int64_t dx = a.x - b.x, dy = a.y - b.y;
    return isqrt(((dx * dx) >> 16) + ((dy * dy) >> 16)) << 8;
}
P2 centre(const Ctx& c) { return {int64_t(c.w) << 15, int64_t(c.h) << 15}; }
P2 rot(P2 v, int64_t turnsQ16) {
    const int64_t co = fxCosTurns(Fixed::fromRaw(turnsQ16)).raw, si = fxSinTurns(Fixed::fromRaw(turnsQ16)).raw;
    return {(v.x * co - v.y * si) >> 16, (v.x * si + v.y * co) >> 16};
}
int64_t angleOf(P2 v) { return fxAtan2Turns(Fixed::fromRaw(v.y), Fixed::fromRaw(v.x)).raw; }

struct Rng {
    uint64_t s;
    uint32_t next() { s = mix64(s); return uint32_t(s >> 32); }
    uint32_t below(uint32_t n) { return n ? next() % n : 0; }
    int64_t q16() { return int64_t(next() >> 16); }
};

enum class Kind : uint8_t { Mine, Tree, Spot, Spawner, Creep, Wanderer, NeutralStruct, Other };

struct ObjSpec {
    int type = -1;
    Kind kind = Kind::Other;
    int owner = 0;                       // 0 neutral, -1 hostile
    int near = 0, far = 0, centreCount = 0;
    std::vector<P2> nearOffsets;         // template offsets (player-1 frame)
    Json extra = Json::object();         // garrison, amount
};

struct KitItem { int type = -1; P2 off; int count = 1; Json extra = Json::object(); };

Kind classify(const Ctx& c, int ti) {
    const game::UnitType& t = c.R.types[size_t(ti)];
    const Json& tj = c.rulesJson["types"][t.id];
    bool isSpot = false;
    for (auto& o : c.R.types) if (o.placeOn == ti) isSpot = true;
    if (t.structure && t.resourceNode >= 0) {
        if (t.footprintW == 1 && t.footprintH == 1 && t.resourceAmount.raw > 0 && t.resourceAmount <= Fixed::fromInt(600)) return Kind::Tree;
        return Kind::Mine;
    }
    if (isSpot) return Kind::Spot;
    if (t.fieldSpawnRes >= 0) return Kind::Spawner;
    if (!t.structure && tj["neutral"].isObject()) return t.wanders ? Kind::Wanderer : Kind::Creep;
    if (t.structure && (t.captureTime.raw > 0 || t.garrisonMax > 0 || t.capturable || t.claimedByTerritory || t.portalInterval.raw > 0)) return Kind::NeutralStruct;
    return Kind::Other;
}


// ---------------------------------------------------------------- placement

struct Placer {
    Ctx& c;
    game::MapGrid g;            // terrain only (levels / slopes)
    std::vector<uint8_t> reach; // ground cells reachable from player 1's base (far objects must be usable)
    explicit Placer(Ctx& cc) : c(cc) {
        buildGrid(c, g, false);
        if (!c.starts.empty()) reach = reachable(c, g, int(c.starts[0].x >> 16), int(c.starts[0].y >> 16), c.digPlain != 0);
    }
    bool reachableAt(P2 p) const {
        const int x = int(p.x >> 16), y = int(p.y >> 16);
        if (reach.empty() || !c.inside(x, y) || c.connect == "none") return true;
        for (int dy = -2; dy <= 2; ++dy)
            for (int dx = -2; dx <= 2; ++dx) if (c.inside(x + dx, y + dy) && reach[size_t(c.ci(x + dx, y + dy))]) return true;
        return false;
    }

    bool cellOk(int x, int y, bool structure, bool allowReserved, int lvl, bool dig) const {
        if (!c.inside(x, y) || x == 0 || y == 0 || x == c.w - 1 || y == c.h - 1) return false;
        const int i = c.ci(x, y);
        // occ: 1 object, 2 corridor between bases (never built over), 3 base core (kit and base mines only)
        if (c.occ[size_t(i)] == 1 || (structure && c.occ[size_t(i)] == 2) || (c.occ[size_t(i)] == 3 && !allowReserved)) return false;
        if (dig) return c.cell[size_t(i)] != '#' || true;
        const uint8_t f = g.flags[size_t(i)];
        if (!(f & game::kCellGround) || (f & game::kCellWater)) return false;
        if (structure) {
            if (f & (game::kCellRamp | game::kCellNoBuild)) return false;
            if (g.hf ? g.slope[size_t(i)] > g.buildLimit : g.level[size_t(i)] != lvl) return false;
        }
        return true;
    }
    // footprint origin for a structure centred near p
    static void origin(const game::UnitType& t, P2 p, int& x0, int& y0) {
        x0 = int((p.x - (int64_t(t.footprintW) << 15) + 32768) >> 16);
        y0 = int((p.y - (int64_t(t.footprintH) << 15) + 32768) >> 16);
    }
    bool fits(int ti, P2 p, bool allowReserved, bool ring, int& x0, int& y0) const {
        const game::UnitType& t = c.R.types[size_t(ti)];
        if (!t.structure) {
            const int x = int(p.x >> 16), y = int(p.y >> 16);
            x0 = x; y0 = y;
            return c.inside(x, y) && (c.digPlain ? c.cell[size_t(c.ci(x, y))] == '.' : g.passable(x, y, game::kLayerGround)) && c.occ[size_t(c.ci(x, y))] != 1;
        }
        origin(t, p, x0, y0);
        return fitsAt(ti, x0, y0, allowReserved, ring);
    }
    bool fitsAt(int ti, int x0, int y0, bool allowReserved, bool ring) const {
        const game::UnitType& t = c.R.types[size_t(ti)];
        const bool dig = c.digPlain != 0;
        const int lvl = c.inside(x0, y0) ? g.level[size_t(c.ci(x0, y0))] : 0;
        for (int y = y0; y < y0 + t.footprintH; ++y)
            for (int x = x0; x < x0 + t.footprintW; ++x)
                if (!cellOk(x, y, true, allowReserved, lvl, dig)) return false;
        if (ring)
            for (int y = y0 - 1; y <= y0 + t.footprintH; ++y)
                for (int x = x0 - 1; x <= x0 + t.footprintW; ++x)
                    if (c.inside(x, y) && c.occ[size_t(c.ci(x, y))] == 1) return false;
        return true;
    }
    void mark(int ti, int x0, int y0) {
        const game::UnitType& t = c.R.types[size_t(ti)];
        if (!t.structure) return;
        for (int y = y0; y < y0 + t.footprintH; ++y)
            for (int x = x0; x < x0 + t.footprintW; ++x) {
                if (!c.inside(x, y)) continue;
                c.occ[size_t(c.ci(x, y))] = 1;
                if (c.digPlain) c.cell[size_t(c.ci(x, y))] = '.';   // caves: structures stand in a dug pocket
            }
        if (c.digPlain && (t.garrisonMax > 0 || t.portalInterval.raw > 0 || t.claimedByTerritory))
            for (int y = y0 - 1; y <= y0 + t.footprintH; ++y)
                for (int x = x0 - 1; x <= x0 + t.footprintW; ++x)
                    if (c.inside(x, y) && x > 0 && y > 0 && x < c.w - 1 && y < c.h - 1 && c.cell[size_t(c.ci(x, y))] != '#') c.cell[size_t(c.ci(x, y))] = '.';
        c.blockRects.push_back({x0, y0, t.footprintW, t.footprintH});
    }
    void emit(int ti, int owner, int x0, int y0, P2 p, const Json& extra) {
        const game::UnitType& t = c.R.types[size_t(ti)];
        Json e = Json::object();
        e.set("type", Json::string(t.id));
        e.set("owner", Json::number(int64_t(owner)));
        int64_t px = p.x, py = p.y;
        if (t.structure) { px = (int64_t(x0) << 16) + (int64_t(t.footprintW) << 15); py = (int64_t(y0) << 16) + (int64_t(t.footprintH) << 15); }
        Json pos = Json::array();
        pos.push(Json::numberText(fmtQ16(px))); pos.push(Json::numberText(fmtQ16(py)));
        e.set("pos", pos);
        for (auto& [k, v] : extra.members()) e.set(k, v);
        c.entities.push(e);
    }
    // nudge: spiral search for the nearest valid spot (deterministic order)
    bool find(int ti, P2& p, bool allowReserved, bool ring, int radius, int& x0, int& y0) const {
        for (int r = 0; r <= radius; ++r)
            for (int dy = -r; dy <= r; ++dy)
                for (int dx = -r; dx <= r; ++dx) {
                    if (std::max(std::abs(dx), std::abs(dy)) != r) continue;
                    P2 q{p.x + (int64_t(dx) << 16), p.y + (int64_t(dy) << 16)};
                    if (fits(ti, q, allowReserved, ring, x0, y0)) { p = q; return true; }
                }
        return false;
    }
    // positions of the k-th player's copy of a domain point
    P2 imageOf(int k, P2 p) const {
        if (c.sym.kind == Sym::None || k == 0) return p;
        P2 o; c.sym.image(k, p.x, p.y, o.x, o.y);
        return o;
    }
    // places a symmetric set: the domain point and all its images (one per player); owner < 0 => per image k+1
    bool placeSet(int ti, P2 p, int ownerRule, const Json& extra, bool allowReserved, bool ring, int nudge, bool dedupeCentre) {
        int x0, y0;
        if (!find(ti, p, allowReserved, ring, nudge, x0, y0)) return false;
        const int n = c.sym.kind == Sym::None ? 1 : c.sym.n;
        std::vector<std::array<int64_t, 4>> pl;   // x0, y0, px, py
        std::vector<uint8_t> save = c.occ;
        const size_t rects = c.blockRects.size();
        std::vector<char> saveCells = c.cell;
        const game::UnitType& t = c.R.types[size_t(ti)];
        if (t.structure) p = {(int64_t(x0) << 16) + (int64_t(t.footprintW) << 15), (int64_t(y0) << 16) + (int64_t(t.footprintH) << 15)};
        for (int k = 0; k < n; ++k) {
            P2 q = imageOf(k, p);     // exact for mirror / point / rot4: the footprint centre maps onto a footprint centre
            if (dedupeCentre && k > 0 && dist(q, p) < (1 << 16)) break;
            int ix, iy;
            bool ok;
            if (t.structure && c.sym.exact()) {
                ix = int((q.x - (int64_t(t.footprintW) << 15)) >> 16); iy = int((q.y - (int64_t(t.footprintH) << 15)) >> 16);
                ok = fitsAt(ti, ix, iy, allowReserved, ring);
                if (!ok && k > 0) ok = find(ti, q, allowReserved, ring, 1, ix, iy);   // rot4 with a non-square footprint
            } else ok = find(ti, q, allowReserved, ring, c.sym.exact() ? 0 : 2, ix, iy);
            if (!ok) { c.occ = save; c.cell = saveCells; c.blockRects.resize(rects); return false; }
            mark(ti, ix, iy);
            pl.push_back({ix, iy, q.x, q.y});
        }
        for (size_t k = 0; k < pl.size(); ++k) {
            const int owner = ownerRule == -100 ? int(k) + 1 : ownerRule;
            emit(ti, owner, int(pl[k][0]), int(pl[k][1]), {pl[k][2], pl[k][3]}, extra);
        }
        return true;
    }
};

// start points ---------------------------------------------------------

std::vector<P2> templateAnchors(const Json& tmpl, int np) {
    std::vector<P2> a(size_t(np), P2{-1, -1});
    for (int p = 1; p <= np; ++p) {
        const Json& pj = tmpl["players"][size_t(p - 1)];
        if (pj["start"].isArray()) a[size_t(p - 1)] = {pj["start"][size_t(0)].fx().raw, pj["start"][size_t(1)].fx().raw};
    }
    for (auto& e : tmpl["entities"].items()) {
        const int o = int(e.getInt("owner", 0));
        if (o >= 1 && o <= np && a[size_t(o - 1)].x < 0) a[size_t(o - 1)] = {e["pos"][size_t(0)].fx().raw, e["pos"][size_t(e["pos"].size() >= 3 ? 2 : 1)].fx().raw};
    }
    return a;
}

} // namespace

void chooseStarts(Ctx& c) {
    c.starts.clear();
    if (c.np <= 0) return;
    Rng rng{c.seed64 ^ 0x5157a12bull};
    const int margin = c.baseRadius + 2;
    const P2 ctr = centre(c);
    if (c.np == 1) { c.starts.push_back(ctr); return; }
    if (c.P["starts"].isArray() && int(c.P["starts"].size()) >= c.np) {     // explicit (cells), e.g. hand-made layouts
        for (int k = 0; k < c.np; ++k) c.starts.push_back({c.P["starts"][size_t(k)][size_t(0)].fx().raw, c.P["starts"][size_t(k)][size_t(1)].fx().raw});
        return;
    }
    const int64_t spread = std::clamp<int64_t>(c.pq("startSpread", Q(85, 100)), Q(2, 10), kOne);
    const int64_t jitter = c.pq("startJitter", Q(3, 1));
    if (c.sym.kind != Sym::None) {
        // candidate cells in the domain; score: closeness of the distance to the nearest image to `spread` of
        // the best possible, plus seeded jitter
        struct Cand { P2 p; int64_t d; };
        std::vector<Cand> cs;
        int64_t best = 0;
        for (int y = margin; y < c.h - margin; ++y)
            for (int x = margin; x < c.w - margin; ++x) {
                P2 p{(int64_t(x) << 16) + 32768, (int64_t(y) << 16) + 32768};
                if (!c.sym.inDomain(p.x, p.y)) continue;
                int64_t dmin = INT64_MAX;
                for (int k = 1; k < c.sym.n; ++k) { P2 q; c.sym.image(k, p.x, p.y, q.x, q.y); dmin = std::min(dmin, dist(p, q)); }
                cs.push_back({p, dmin});
                best = std::max(best, dmin);
            }
        if (cs.empty()) { c.warn("map too small for the base radius"); cs.push_back({{int64_t(margin) << 16, int64_t(margin) << 16}, 0}); }
        const int64_t target = (best * spread) >> 16;
        int64_t bestScore = INT64_MIN; P2 pick = cs[0].p;
        for (auto& cd : cs) {
            int64_t score = -std::abs(cd.d - target) - ((rng.q16() * jitter) >> 16);
            if (c.shape != "lanes" && c.shape != "caves" && c.hg.v.size()) {     // prefer land over water
                const int x = int(cd.p.x >> 16), y = int(cd.p.y >> 16);
                if (c.hg.cell(x, y) < c.sea) score -= int64_t(4) << 16;
            }
            if (score > bestScore) { bestScore = score; pick = cd.p; }
        }
        for (int k = 0; k < c.sym.n; ++k) { P2 q; c.sym.image(k, pick.x, pick.y, q.x, q.y); c.starts.push_back(q); }
        return;
    }
    // no symmetry: farthest-point sampling over a coarse candidate lattice
    std::vector<P2> cs;
    for (int y = margin; y < c.h - margin; y += 2)
        for (int x = margin; x < c.w - margin; x += 2) cs.push_back({(int64_t(x) << 16) + 32768, (int64_t(y) << 16) + 32768});
    if (cs.empty()) cs.push_back(ctr);
    c.starts.push_back(cs[rng.below(uint32_t(cs.size()))]);
    while (int(c.starts.size()) < c.np) {
        int64_t bd = -1; P2 bp = cs[0];
        for (auto& p : cs) {
            int64_t dmin = INT64_MAX;
            for (auto& s : c.starts) dmin = std::min(dmin, dist(p, s));
            dmin -= (rng.q16() * jitter) >> 16;
            if (dmin > bd) { bd = dmin; bp = p; }
        }
        c.starts.push_back(bp);
    }
}

// ---------------------------------------------------------------- content

void placeContent(Ctx& c) {
    const int N = c.w * c.h;
    c.occ.assign(size_t(N), 0);
    Rng rng{c.seed64 ^ 0xc0ffee11ull};
    const Json& tmpl = c.tmpl;
    const P2 ctr = centre(c);
    // reserve the base cores and the corridors between bases
    for (auto& s : c.starts) {
        const int sx = int(s.x >> 16), sy = int(s.y >> 16), r = std::max(3, c.baseRadius - 3);
        for (int y = sy - r; y <= sy + r; ++y)
            for (int x = sx - r; x <= sx + r; ++x) if (c.inside(x, y)) c.occ[size_t(c.ci(x, y))] = 3;
    }
    for (auto& path : c.corridors)
        for (int i : path) {
            const int x = i % c.w, y = i / c.w;
            c.forImages(x, y, [&](int ix, int iy, int) {
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) if (c.inside(ix + dx, iy + dy) && c.occ[size_t(c.ci(ix + dx, iy + dy))] != 3) c.occ[size_t(c.ci(ix + dx, iy + dy))] = 2;
            });
        }
    Placer pl(c);

    // ---- the start kit
    std::vector<KitItem> kit;
    const int tnp = std::max<int>(1, int(tmpl["players"].size()));
    std::vector<P2> anchors = templateAnchors(tmpl, tnp);
    if (c.P["kit"].isArray()) {
        for (auto& k : c.P["kit"].items()) {
            const int ti = c.R.findType(k.getStr("type"));
            if (ti < 0) { c.warn("kit: unknown type '" + k.getStr("type") + "'"); continue; }
            KitItem it; it.type = ti; it.off = {k["offset"][size_t(0)].fx().raw, k["offset"][size_t(1)].fx().raw}; it.count = int(k.getInt("count", 1));
            if (k.has("garrison")) it.extra.set("garrison", k["garrison"]);
            kit.push_back(it);
        }
    } else if (anchors[0].x >= 0) {
        for (auto& e : tmpl["entities"].items()) {
            if (e.getInt("owner", 0) != 1) continue;
            const int ti = c.R.findType(e.getStr("type"));
            if (ti < 0) continue;
            KitItem it; it.type = ti;
            it.off = {e["pos"][size_t(0)].fx().raw - anchors[0].x, e["pos"][size_t(e["pos"].size() >= 3 ? 2 : 1)].fx().raw - anchors[0].y};
            if (e.has("garrison")) it.extra.set("garrison", e["garrison"]);
            kit.push_back(it);
        }
    }
    const bool factionStart = [&] { for (auto& f : c.R.factions) if (!f.start.empty()) return true; return false; }();
    if (kit.empty() && !factionStart) {   // heuristics: a producer of workers + workers, else a key unit, else the biggest garrison
        int hall = -1, worker = -1, key = -1, garr = -1;
        for (size_t i = 0; i < c.R.types.size(); ++i) {
            const auto& t = c.R.types[i];
            if (t.key && key < 0) key = int(i);
            if (t.structure && t.garrisonMax > 0 && (garr < 0 || t.garrisonMax > c.R.types[size_t(garr)].garrisonMax)) garr = int(i);
            if (t.structure && hall < 0 && t.resourceNode < 0)
                for (uint16_t pt : t.produces) if (c.R.types[pt].worker && !c.R.types[pt].structure) { hall = int(i); worker = pt; break; }
        }
        if (hall >= 0) {
            kit.push_back({hall, {0, 0}, 1, Json::object()});
            kit.push_back({worker, {int64_t(3) << 16, int64_t(3) << 16}, 4, Json::object()});
        } else if (key >= 0) kit.push_back({key, {0, 0}, 1, Json::object()});
        else if (garr >= 0) { KitItem it{garr, {0, 0}, 1, Json::object()}; it.extra.set("garrison", Json::number(int64_t(15))); kit.push_back(it); }
        if (kit.empty()) c.warn("no start kit: give params.kit or a template map with player 1's objects");
    }
    // the kit's frame: template offsets are rotated from the template's start->centre direction to ours
    int64_t frameTurn = 0;
    if (anchors[0].x >= 0 && tmpl["size"].isArray() && !c.P["kit"].isArray()) {
        P2 tc{tmpl["size"][size_t(0)].fx().raw / 2, tmpl["size"][size_t(1)].fx().raw / 2};
        P2 a{tc.x - anchors[0].x, tc.y - anchors[0].y}, b{ctr.x - c.starts[0].x, ctr.y - c.starts[0].y};
        frameTurn = angleOf(b) - angleOf(a);
        if (c.pbool("snapKit", true)) frameTurn = ((frameTurn + (frameTurn >= 0 ? kOne / 8 : -kOne / 8)) / (kOne / 4)) * (kOne / 4);  // quarter turns: footprints stay aligned
    }
    int kitExtent = 0;
    for (auto& it : kit) kitExtent = std::max<int>(kitExtent, int(std::max(std::abs(it.off.x), std::abs(it.off.y)) >> 16));
    auto kitPos = [&](int k, P2 off) {
        P2 o = rot(off, frameTurn);
        if (c.sym.kind == Sym::None) {     // per-start frames (no symmetry): rotate toward the centre
            P2 d{ctr.x - c.starts[size_t(k)].x, ctr.y - c.starts[size_t(k)].y}, d0{ctr.x - c.starts[0].x, ctr.y - c.starts[0].y};
            P2 r = rot(o, angleOf(d) - angleOf(d0));
            return P2{c.starts[size_t(k)].x + r.x, c.starts[size_t(k)].y + r.y};
        }
        return pl.imageOf(k, P2{c.starts[0].x + o.x, c.starts[0].y + o.y});
    };
    const bool anySymKit = c.sym.kind != Sym::None;
    for (auto& it : kit) {
        const game::UnitType& t = c.R.types[size_t(it.type)];
        for (int n = 0; n < it.count; ++n) {
            P2 off = it.off;
            if (n) off = {off.x + int64_t(n % 4) * Q(4, 5), off.y + int64_t(n / 4) * Q(4, 5)};
            if (anySymKit) {
                if (!pl.placeSet(it.type, kitPos(0, off), -100, it.extra, true, false, 3, false)) c.warn("kit: no room for '" + t.id + "'");
                continue;
            }
            for (int k = 0; k < c.np; ++k) {
                P2 p = kitPos(k, off);
                int x0, y0;
                if (!pl.find(it.type, p, true, false, 3, x0, y0)) { c.warn("kit: no room for '" + t.id + "'"); continue; }
                pl.mark(it.type, x0, y0);
                pl.emit(it.type, k + 1, x0, y0, p, it.extra);
            }
        }
    }

    // ---- neutral objects: from the template (counts near / far / centre) or from the rules
    std::map<int, ObjSpec> specs;
    const int64_t nearR = int64_t(std::max(10, c.pint("nearRadius", 14))) << 16;
    if (!tmpl["entities"].items().empty()) {
        P2 tc{tmpl["size"][size_t(0)].fx().raw / 2, tmpl["size"][size_t(1)].fx().raw / 2};
        std::map<int, std::array<int, 3>> cnt;
        for (auto& e : tmpl["entities"].items()) {
            const int o = int(e.getInt("owner", 0));
            if (o > 0) continue;
            const int ti = c.R.findType(e.getStr("type"));
            if (ti < 0) continue;
            P2 p{e["pos"][size_t(0)].fx().raw, e["pos"][size_t(e["pos"].size() >= 3 ? 2 : 1)].fx().raw};
            ObjSpec& s = specs[ti];
            s.type = ti; s.kind = classify(c, ti); s.owner = o < 0 ? -1 : 0;
            if (e.has("garrison") && !s.extra.has("garrison")) s.extra.set("garrison", e["garrison"]);
            if (e.has("amount") && !s.extra.has("amount")) s.extra.set("amount", e["amount"]);
            int64_t dn = INT64_MAX; int an = 0;
            for (int k = 0; k < tnp; ++k) if (anchors[size_t(k)].x >= 0 && dist(p, anchors[size_t(k)]) < dn) { dn = dist(p, anchors[size_t(k)]); an = k; }
            if (dist(p, tc) < (int64_t(3) << 16)) cnt[ti][2]++;
            else if (dn <= nearR) { cnt[ti][0]++; if (an == 0) s.nearOffsets.push_back({p.x - anchors[0].x, p.y - anchors[0].y}); }
            else cnt[ti][1]++;
        }
        for (auto& [ti, ct] : cnt) {
            specs[ti].near = (ct[0] + tnp - 1) / tnp;
            specs[ti].far = (ct[1] + tnp / 2) / tnp;
            specs[ti].centreCount = ct[2] ? 1 : 0;
        }
    } else {
        for (size_t i = 0; i < c.R.types.size(); ++i) {
            ObjSpec s; s.type = int(i); s.kind = classify(c, int(i));
            const auto& t = c.R.types[i];
            switch (s.kind) {
                case Kind::Mine: s.near = t.footprintW == 1 ? 6 : 1; s.far = t.footprintW == 1 ? 4 : 1; break;
                case Kind::Tree: s.near = 24; s.far = std::max(8, c.w * c.h / 40 / std::max(1, c.np)); break;
                case Kind::Spot: s.near = 3; s.far = 2; break;
                case Kind::Spawner: s.near = 1; s.far = 1; break;
                case Kind::Creep: s.owner = -1; s.far = 2; break;
                case Kind::Wanderer: s.owner = -1; s.centreCount = 1; break;
                case Kind::NeutralStruct: s.far = 2; if (t.garrisonMax > 0) s.extra.set("garrison", Json::number(int64_t(8))); break;
                default: continue;
            }
            specs[int(i)] = s;
        }
    }
    for (auto& o : c.P["objects"].items()) {          // explicit overrides
        const int ti = c.R.findType(o.getStr("type"));
        if (ti < 0) { c.warn("objects: unknown type '" + o.getStr("type") + "'"); continue; }
        ObjSpec& s = specs[ti];
        if (s.type < 0) { s.type = ti; s.kind = classify(c, ti); }
        s.near = int(o.getInt("near", s.near)); s.far = int(o.getInt("far", s.far)); s.centreCount = int(o.getInt("centre", s.centreCount));
        s.owner = int(o.getInt("owner", s.owner));
        if (o.has("garrison")) s.extra.set("garrison", o["garrison"]);
        if (o.has("amount")) s.extra.set("amount", o["amount"]);
        if (o.has("near")) s.nearOffsets.clear();
    }
    const int32_t treeMul = std::clamp(c.pq("trees", kOne), 0, 8 * kOne), resMul = std::clamp(c.pq("resources", kOne), 0, 8 * kOne);
    // far objects are counted per template player: scale by map area per player relative to the template's
    int32_t areaMul = kOne;
    if (tmpl["size"].isArray()) {
        const int64_t ta = std::max<int64_t>(1, tmpl["size"][size_t(0)].integer(64) * tmpl["size"][size_t(1)].integer(64)) / tnp;
        const int64_t ga = int64_t(c.w) * c.h / std::max(1, c.np);
        areaMul = int32_t(std::clamp<int64_t>(ga * kOne / ta, kOne / 8, 4 * kOne));
    }
    const bool anySym = c.sym.kind != Sym::None;
    const int domains = anySym ? 1 : c.np;            // without symmetry every player gets its own copy

    // near-base objects: template offsets (rotated into our frame), else rings around the start
    auto placeNear = [&](ObjSpec& s, int count) {
        const game::UnitType& t = c.R.types[size_t(s.type)];
        const bool tree = s.kind == Kind::Tree;
        int placed = 0;
        for (int n = 0; n < count; ++n) {
            P2 off;
            if (n < int(s.nearOffsets.size()) && !tree) off = rot(s.nearOffsets[size_t(n)], frameTurn);
            else {
                const int64_t r = (int64_t(c.baseRadius + 1) << 16) + rng.q16() * 4;
                const int64_t a = tree ? int64_t(rng.below(65536)) : int64_t(rng.below(65536));
                off = rot(P2{r, 0}, a);
            }
            if (s.kind == Kind::Mine && t.footprintW == 1 && n > 0 && placed > 0) {     // mineral line: next to the last one
                off = rot(s.nearOffsets.empty() ? P2{int64_t(c.baseRadius + 1) << 16, 0} : s.nearOffsets[0], frameTurn);
                off = {off.x + rot(P2{int64_t(n) << 16, 0}, kOne / 4 + angleOf(off)).x / 2, off.y + rot(P2{int64_t(n) << 16, 0}, kOne / 4 + angleOf(off)).y / 2};
            }
            bool ok = false;
            if (anySym && c.sym.exact()) ok = pl.placeSet(s.type, P2{c.starts[0].x + off.x, c.starts[0].y + off.y}, s.owner, s.extra, !tree, !tree, 4, false);
            else if (anySym) {   // rotations by 1/n turn: try the whole set, else rotate the offset per base and search
                ok = pl.placeSet(s.type, P2{c.starts[0].x + off.x, c.starts[0].y + off.y}, s.owner, s.extra, !tree, !tree, 4, false);
                for (int a = 1; !ok && a < 8; ++a) {
                    const P2 o2 = rot(off, int64_t(a) * kOne / 8);
                    ok = pl.placeSet(s.type, P2{c.starts[0].x + o2.x, c.starts[0].y + o2.y}, s.owner, s.extra, !tree, !tree, 3, false);
                }
            } else {
                ok = true;
                for (int k = 0; k < c.np; ++k) {
                    P2 p = kitPos(k, rot(off, -frameTurn));
                    int x0, y0;
                    if (!pl.find(s.type, p, !tree, !tree, 4, x0, y0)) { ok = false; continue; }
                    pl.mark(s.type, x0, y0); pl.emit(s.type, s.owner, x0, y0, p, s.extra);
                }
            }
            if (ok) ++placed;
        }
        return placed;
    };
    // far objects: random domain cells away from every start (min distance), creeps in camps of 2..3
    auto placeFar = [&](ObjSpec& s, int count) {
        const game::UnitType& t = c.R.types[size_t(s.type)];
        int placed = 0, tries = 0;
        const int64_t minD = nearR + (int64_t(2) << 16);
        while (placed < count && tries < count * 60 + 60) {
            ++tries;
            P2 p{int64_t(1 + rng.below(uint32_t(c.w - 2))) << 16, int64_t(1 + rng.below(uint32_t(c.h - 2))) << 16};
            p.x += 32768; p.y += 32768;
            if (anySym && !c.sym.inDomain(p.x, p.y)) continue;
            bool farOk = true;
            for (auto& st : c.starts) if (dist(p, st) < minD) farOk = false;
            if (!farOk) continue;
            if (s.kind != Kind::Tree && !pl.reachableAt(p)) continue;
            if (s.kind == Kind::Tree) {   // forests: only where the biome is forest-ish (grass with low probability)
                const uint8_t b = c.biome[size_t(c.ci(int(p.x >> 16), int(p.y >> 16)))];
                if (b != BForest && b != BSwamp && !(b == BGrass && rng.below(4) == 0) && !(b == BSnow && rng.below(3) == 0)) continue;
            }
            if (anySym) { if (pl.placeSet(s.type, p, s.owner, s.extra, false, s.kind != Kind::Tree, 1, false)) ++placed; }
            else {
                int x0, y0;
                if (pl.find(s.type, p, false, s.kind != Kind::Tree, 1, x0, y0)) { pl.mark(s.type, x0, y0); pl.emit(s.type, s.owner, x0, y0, p, s.extra); ++placed; }
            }
            // a forest grows around the seed tree
            if (s.kind == Kind::Tree && placed < count) {
                const int clump = 2 + int(rng.below(6));
                for (int q = 0; q < clump && placed < count; ++q) {
                    P2 r{p.x + (int64_t(int(rng.below(5)) - 2) << 16), p.y + (int64_t(int(rng.below(5)) - 2) << 16)};
                    if (anySym && !c.sym.inDomain(r.x, r.y)) continue;
                    if (anySym) { if (pl.placeSet(s.type, r, s.owner, s.extra, false, false, 0, false)) ++placed; }
                    else { int x0, y0; if (pl.fits(s.type, r, false, false, x0, y0)) { pl.mark(s.type, x0, y0); pl.emit(s.type, s.owner, x0, y0, r, s.extra); ++placed; } }
                }
            }
            if (s.kind == Kind::Creep) {  // camp mates
                for (int m = 0; m < 1 + int(rng.below(2)); ++m) {
                    P2 r{p.x + (int64_t(m + 1) << 16) * 4 / 5, p.y};
                    if (anySym) pl.placeSet(s.type, r, s.owner, s.extra, false, false, 1, false);
                }
            }
            (void)t;
        }
        return placed;
    };
    // order: mines, spots, spawners, structures, creeps, trees (trees fill what is left)
    const Kind order[] = {Kind::Mine, Kind::Spot, Kind::Spawner, Kind::NeutralStruct, Kind::Other, Kind::Creep, Kind::Wanderer, Kind::Tree};
    Json counts = Json::object();
    for (Kind k : order)
        for (auto& [ti, s] : specs) {
            if (s.kind != k) continue;
            const int32_t mul = k == Kind::Tree ? treeMul : (k == Kind::Mine || k == Kind::Spot || k == Kind::Spawner) ? resMul : kOne;
            const int nearN = int((int64_t(s.near) * mul + kOne / 2) >> 16);
            const int farN = int((((int64_t(s.far) * mul) >> 16) * (k == Kind::Tree ? areaMul : kOne) + kOne / 2) >> 16);
            int got = 0;
            if (nearN > 0) {
                const int n = placeNear(s, nearN);
                if (n < nearN && k != Kind::Tree) c.warn("only " + std::to_string(n) + " of " + std::to_string(nearN) + " '" + c.R.types[size_t(ti)].id + "' near each base");
                got += n * (anySym ? c.np : 1);
            }
            if (farN > 0) got += placeFar(s, farN * domains) * (anySym ? c.np : 1);
            for (int q = 0; q < s.centreCount; ++q) {
                P2 p = ctr;
                if (pl.placeSet(s.type, p, s.owner, s.extra, true, true, 3, true)) got += 1;
            }
            if (got) counts.set(c.R.types[size_t(ti)].id, Json::number(int64_t(got)));
        }
    c.info.set("objects", counts);

    // ---- resource fields (Red Alert ore, Dune spice): around spawners, plus the template's fields (near ones keep
    // their offset from the base, far ones go to reachable random spots); without a template: patches near + far
    auto addField = [&](const std::string& rid, P2 p, int radius, const Json& amount) {
        Json f = Json::object();
        f.set("resource", Json::string(rid));
        Json cc = Json::array(); cc.push(Json::number(int64_t(p.x >> 16))); cc.push(Json::number(int64_t(p.y >> 16)));
        f.set("center", cc);
        f.set("radius", Json::number(int64_t(radius)));
        if (amount.isNumber()) f.set("amount", amount);
        c.fields.push(f);
    };
    auto fieldSpot = [&](P2& p, int radius) {        // nudge onto open, reachable ground away from structures
        for (int r = 0; r <= 6; ++r)
            for (int dy = -r; dy <= r; ++dy)
                for (int dx = -r; dx <= r; ++dx) {
                    if (std::max(std::abs(dx), std::abs(dy)) != r) continue;
                    const int x = int(p.x >> 16) + dx, y = int(p.y >> 16) + dy;
                    if (!c.inside(x, y) || !pl.reachableAt({int64_t(x) << 16, int64_t(y) << 16})) continue;
                    int good = 0, all = 0;
                    for (int yy = y - radius; yy <= y + radius; ++yy)
                        for (int xx = x - radius; xx <= x + radius; ++xx) {
                            if (!c.inside(xx, yy) || (xx - x) * (xx - x) + (yy - y) * (yy - y) > radius * radius) continue;
                            ++all;
                            if ((pl.g.flags[size_t(c.ci(xx, yy))] & game::kCellGround) && c.occ[size_t(c.ci(xx, yy))] != 1) ++good;
                        }
                    if (all && good * 4 >= all * 3) { p = {int64_t(x) << 16, int64_t(y) << 16}; return true; }
                }
        return false;
    };
    for (size_t r = 0; r < c.R.resources.size(); ++r) {
        if (!c.R.resources[r].field) continue;
        const std::string rid = c.R.resources[r].id;
        const int radius = std::clamp(c.pint("fieldRadius", 4), 1, 12);
        for (size_t i = 0; i < c.R.types.size(); ++i)
            if (c.R.types[i].fieldSpawnRes == int(r))
                for (auto& e : c.entities.items())
                    if (e.getStr("type") == c.R.types[i].id) addField(rid, {e["pos"][size_t(0)].fx().raw, e["pos"][size_t(1)].fx().raw}, radius, Json());
        std::vector<std::pair<P2, int>> nearF;   // template fields: offset from player 1's anchor
        int farN = 0, farR = radius;
        bool tmplFields = false;
        for (auto& f : tmpl["fields"].items()) {
            if (f.getStr("resource") != rid || !f["center"].isArray()) continue;
            tmplFields = true;
            const P2 p{f["center"][size_t(0)].fx().raw, f["center"][size_t(1)].fx().raw};
            const int fr = int(f.getInt("radius", 3));
            int64_t dn = INT64_MAX; int an = 0;
            for (int k = 0; k < tnp; ++k) if (anchors[size_t(k)].x >= 0 && dist(p, anchors[size_t(k)]) < dn) { dn = dist(p, anchors[size_t(k)]); an = k; }
            if (dn <= nearR + (int64_t(4) << 16)) { if (an == 0) nearF.push_back({{p.x - anchors[0].x, p.y - anchors[0].y}, fr}); }
            else { ++farN; farR = fr; }
        }
        if (!tmplFields && c.P["kit"].isNull() && tmpl["entities"].items().empty()) {   // no template at all
            nearF.push_back({rot(P2{int64_t(c.baseRadius + radius + 2) << 16, 0}, int64_t(rng.below(65536))), radius});
            farN = 2;
        }
        farN = (farN + tnp / 2) / std::max(1, tnp);
        farN = std::max(0, c.pint("fieldsFar", farN));
        for (auto& [off0, fr] : nearF) {
            P2 off = rot(off0, frameTurn), p0{c.starts[0].x + off.x, c.starts[0].y + off.y};
            if (!fieldSpot(p0, fr)) continue;
            for (int k = 0; k < c.np; ++k) addField(rid, anySym ? pl.imageOf(k, p0) : kitPos(k, rot(off, -frameTurn)), fr, Json());
        }
        for (int n = 0, tries = 0; n < farN && tries < 200; ++tries) {
            P2 p{int64_t(farR + 1 + rng.below(uint32_t(std::max(1, c.w - 2 * farR - 2)))) << 16, int64_t(farR + 1 + rng.below(uint32_t(std::max(1, c.h - 2 * farR - 2)))) << 16};
            if (anySym && !c.sym.inDomain(p.x, p.y)) continue;
            bool farOk = true;
            for (auto& st : c.starts) if (dist(p, st) < nearR) farOk = false;
            if (!farOk || !fieldSpot(p, farR)) continue;
            for (int k = 0; k < (anySym ? c.np : 1); ++k) addField(rid, pl.imageOf(k, p), farR, Json());
            ++n;
        }
    }

    // ---- roads between garrison buildings (Mushroom Wars): each building to its 2 nearest others
    if (tmpl["roads"].isArray() || c.pbool("roads", false)) {
        std::vector<std::pair<P2, size_t>> nodes;
        for (size_t i = 0; i < c.entities.size(); ++i) {
            Json& e = c.entities.at(i);
            const int ti = c.R.findType(e.getStr("type"));
            if (ti < 0 || c.R.types[size_t(ti)].garrisonMax <= 0) continue;
            e.set("id", Json::string("g" + std::to_string(nodes.size())));
            nodes.push_back({{e["pos"][size_t(0)].fx().raw, e["pos"][size_t(1)].fx().raw}, i});
        }
        std::vector<std::pair<int, int>> edges;
        for (size_t a = 0; a < nodes.size(); ++a) {
            std::vector<std::pair<int64_t, size_t>> d;
            for (size_t b = 0; b < nodes.size(); ++b) if (b != a) d.push_back({dist(nodes[a].first, nodes[b].first), b});
            std::sort(d.begin(), d.end());
            for (size_t k = 0; k < d.size() && k < 2; ++k) {
                std::pair<int, int> e{int(std::min(a, d[k].second)), int(std::max(a, d[k].second))};
                if (std::find(edges.begin(), edges.end(), e) == edges.end()) edges.push_back(e);
            }
        }
        std::sort(edges.begin(), edges.end());
        for (auto& [a, b] : edges) {
            Json r = Json::array();
            r.push(Json::string("g" + std::to_string(a))); r.push(Json::string("g" + std::to_string(b)));
            c.roads.push(r);
        }
    }
}

// ---------------------------------------------------------------- unblocking

namespace {
void entityRect(const Ctx& c, const Json& e, int& x0, int& y0, int& fw, int& fh) {
    const int ti = c.R.findType(e.getStr("type"));
    const game::UnitType& t = c.R.types[size_t(ti)];
    fw = t.structure ? t.footprintW : 0; fh = t.structure ? t.footprintH : 0;
    const int64_t px = e["pos"][size_t(0)].fx().raw, py = e["pos"][size_t(1)].fx().raw;
    x0 = int((px - (int64_t(fw) << 15) + 32768) >> 16);
    y0 = int((py - (int64_t(fh) << 15) + 32768) >> 16);
}
} // namespace

// Objects must never cut a start off (trees in a 1-cell gap): while some start is unreachable with objects,
// remove the objects on the cheapest route (and their symmetric images). Kit objects are never removed.
void unblockStarts(Ctx& c) {
    if (c.connect == "none" || c.starts.size() < 2) return;
    const bool dig = c.digPlain != 0;
    for (int round = 0; round < 24; ++round) {
        game::MapGrid g;
        c.blockRects.clear();
        for (auto& e : c.entities.items()) {
            int x0, y0, fw, fh; entityRect(c, e, x0, y0, fw, fh);
            if (fw > 0) c.blockRects.push_back({x0, y0, fw, fh});
        }
        buildGrid(c, g, true);
        const int sx = int(c.starts[0].x >> 16), sy = int(c.starts[0].y >> 16);
        int ax = sx, ay = sy;
        g.nearestPassable(ax, ay, game::kLayerGround, 8);
        std::vector<uint8_t> seen = reachable(c, g, ax, ay, dig);
        int bad = -1, bx = 0, by = 0;
        for (size_t k = 1; k < c.starts.size() && bad < 0; ++k) {
            bx = int(c.starts[k].x >> 16); by = int(c.starts[k].y >> 16);
            g.nearestPassable(bx, by, game::kLayerGround, 8);
            if (!seen[size_t(c.ci(bx, by))]) bad = int(k);
        }
        if (bad < 0) return;
        // cheapest route through the objects (blocked cells cost extra); drop objects on it (all symmetric images)
        std::vector<uint8_t> onPath(static_cast<size_t>(c.w * c.h), 0);
        game::MapGrid t;
        buildGrid(c, t, false);
        std::vector<uint8_t> objMask(static_cast<size_t>(c.w * c.h), 0);
        for (auto& e : c.entities.items()) {   // 1 removable neutral object, 2 a player's kit (never removed)
            int x0, y0, fw, fh; entityRect(c, e, x0, y0, fw, fh);
            const uint8_t v = e.getInt("owner", 0) > 0 ? 2 : 1;
            for (int y = y0; y < y0 + fh; ++y)
                for (int x = x0; x < x0 + fw; ++x) if (c.inside(x, y)) objMask[size_t(c.ci(x, y))] = std::max(objMask[size_t(c.ci(x, y))], v);
        }
        for (int i : repairPath(c, t, ax, ay, bx, by, dig, &objMask))
            c.forImages(i % c.w, i / c.w, [&](int ix, int iy, int) { onPath[size_t(c.ci(ix, iy))] = 1; });
        Json kept = Json::array();
        int removed = 0;
        for (auto& e : c.entities.items()) {
            int x0, y0, fw, fh; entityRect(c, e, x0, y0, fw, fh);
            bool hit = false;
            if (e.getInt("owner", 0) == 0 || e.getInt("owner", 0) < 0)
                for (int y = y0; y < y0 + fh && !hit; ++y)
                    for (int x = x0; x < x0 + fw; ++x) if (c.inside(x, y) && onPath[size_t(c.ci(x, y))]) { hit = true; break; }
            if (hit) ++removed; else kept.push(e);
        }
        if (!removed) {
            c.warn("objects could not be cleared from the route between starts");
            return;
        }
        c.entities = kept;
    }
}

// ---------------------------------------------------------------- regions (Northgard / Z / State.io)

void genRegions(Ctx& c) {
    const bool want = c.tmpl["regions"].isObject() || c.pbool("regions", false);
    if (!want || c.starts.empty()) return;
    Rng rng{c.seed64 ^ 0x4e6e0123ull};
    const int per = std::clamp(c.pint("regionsPerPlayer", 3), 1, 8);
    std::vector<P2> seeds;
    for (auto& s : c.starts) seeds.push_back(s);        // region k (k < np) = player k+1's home
    const bool anySym = c.sym.kind != Sym::None;
    const int64_t minD = int64_t(std::max(6, std::min(c.w, c.h) / 6)) << 16;
    for (int n = 0, tries = 0; n < per - 1 && tries < 400; ++tries) {
        P2 p{(int64_t(2 + rng.below(uint32_t(std::max(1, c.w - 4)))) << 16) + 32768, (int64_t(2 + rng.below(uint32_t(std::max(1, c.h - 4)))) << 16) + 32768};
        if (anySym && !c.sym.inDomain(p.x, p.y)) continue;
        bool ok = true;
        for (auto& q : seeds) if (dist(p, q) < minD) ok = false;
        if (!ok) continue;
        for (int k = 0; k < (anySym ? c.sym.n : 1); ++k) { P2 q = p; if (anySym) c.sym.image(k, p.x, p.y, q.x, q.y); seeds.push_back(q); }
        ++n;
    }
    seeds.push_back(centre(c));
    if (seeds.size() > 52) seeds.resize(52);
    auto letter = [](size_t i) { return char(i < 26 ? 'A' + i : 'a' + (i - 26)); };
    Json rows = Json::array();
    for (int y = 0; y < c.h; ++y) {
        std::string r(size_t(c.w), '.');
        for (int x = 0; x < c.w; ++x) {
            if (c.cell[size_t(c.ci(x, y))] == '~' || c.cell[size_t(c.ci(x, y))] == '#') continue;
            P2 p{(int64_t(x) << 16) + 32768, (int64_t(y) << 16) + 32768};
            int64_t bd = INT64_MAX; size_t bi = 0;
            for (size_t i = 0; i < seeds.size(); ++i) { const int64_t d = dist(p, seeds[i]); if (d < bd) { bd = d; bi = i; } }
            r[size_t(x)] = letter(bi);
        }
        rows.push(Json::string(r));
    }
    Json names = Json::object();
    for (size_t i = 0; i < seeds.size(); ++i)
        names.set(std::string(1, letter(i)), Json::string(i < c.starts.size() ? "Home " + std::to_string(i + 1) : i + 1 == seeds.size() ? "Centre" : "Land " + std::to_string(i + 1 - c.starts.size())));
    Json rg = Json::object();
    rg.set("rows", rows);
    rg.set("names", names);
    c.regions = rg;
}

// ---------------------------------------------------------------- fairness

void validateFairness(Ctx& c) {
    game::MapGrid g;
    buildGrid(c, g, true);
    Json starts = Json::array(), paths = Json::array();
    for (auto& s : c.starts) { Json p = Json::array(); p.push(Json::numberText(fmtQ16(s.x))); p.push(Json::numberText(fmtQ16(s.y))); starts.push(p); }
    c.info.set("starts", starts);
    if (c.connect == "none" || c.starts.size() < 2) return;
    // path from just outside each base core toward the other's (engine A*), lengths in cells
    int64_t mn = INT64_MAX, mx = 0;
    bool allFound = true;
    const bool dig = c.digPlain != 0;
    for (size_t a = 0; a < c.starts.size(); ++a)
        for (size_t b = a + 1; b < c.starts.size(); ++b) {
            int ax = int(c.starts[a].x >> 16), ay = int(c.starts[a].y >> 16), bx = int(c.starts[b].x >> 16), by = int(c.starts[b].y >> 16);
            int64_t len = 0;
            bool found;
            if (dig) {   // caves: BFS through diggable rock (imps dig the way)
                g.nearestPassable(ax, ay, game::kLayerGround, 8);
                std::vector<uint8_t> seen = reachable(c, g, ax, ay, true);
                g.nearestPassable(bx, by, game::kLayerGround, 6);
                found = seen[size_t(c.ci(bx, by))] != 0;
                len = dist(c.starts[a], c.starts[b]);
            } else {
                g.nearestPassable(ax, ay, game::kLayerGround, 8); g.nearestPassable(bx, by, game::kLayerGround, 8);
                std::vector<Vec3> path;
                found = g.findPath(g.cellCenter(ax, ay), g.cellCenter(bx, by), game::kLayerGround, path, c.w * c.h * 2);
                Vec3 p = g.cellCenter(ax, ay);
                for (auto& wp : path) { len += (wp - p).xz().length().raw; p = wp; }
                if (found && !path.empty()) found = g.cellX(path.back().x) == bx && g.cellY(path.back().z) == by;
            }
            allFound = allFound && found;
            Json e = Json::array();
            e.push(Json::number(int64_t(a + 1))); e.push(Json::number(int64_t(b + 1)));
            e.push(found ? Json::numberText(fmtQ16(len)) : Json::string("none"));
            paths.push(e);
            if (found) { mn = std::min(mn, len); mx = std::max(mx, len); }
        }
    c.info.set("paths", paths);
    c.info.set("connected", Json::fromBool(allFound));
    if (!allFound) c.warn("some start areas are not connected by ground");
    if (mn != INT64_MAX && mn > 0) {
        const int64_t spread = (mx - mn) * 1000 / mn;     // per mille
        c.info.set("pathSpread", Json::number(spread));
        if (c.sym.kind == Sym::None && spread > c.pint("maxPathSpread", 150)) c.warn("path distances between starts differ by " + std::to_string(spread / 10) + "%");
    }
}

} // namespace rts::gen
