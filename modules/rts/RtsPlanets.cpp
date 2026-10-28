// Planets (docs/planets.md §3): the map `world` block, placement on cube-sphere bodies, movement on surfaces
// and orbit shells, vision over the sphere (horizon included), interplanetary transit. Flat maps never get
// here: every entry point is guarded by GameState::surface.
#include "RtsInternal.h"
#include "engine/core/FixedMath.h"
#include <algorithm>
#include <stdexcept>

namespace rts::game {
using namespace detail;

namespace {
BodyKind parseKind(const std::string& k, const std::string& where) {
    if (k == "planet") return BodyKind::Planet;
    if (k == "moon") return BodyKind::Moon;
    if (k == "star") return BodyKind::Star;
    if (k == "plane") throw std::runtime_error(where + ": kind 'plane' is implicit (a map without a world block)");
    throw std::runtime_error(where + ": unknown kind '" + k + "' (planet, moon, star)");
}
} // namespace

const Surface* RtsModule::surface(Simulation& sim) const {
    auto* gs = sim.world().resource<GameState>();
    return gs ? gs->surface.get() : nullptr;
}

int RtsModule::bodyOf(Simulation& sim, Vec3 p) const {
    const Surface* s = surface(sim);
    return s ? s->bodyAt(p) : 0;
}

Vec3 RtsModule::bodyWorldPosition(Simulation& sim, int body, uint32_t tick) const {
    const Surface* s = surface(sim);
    if (!s) return {};
    return s->bodyPosition(body, Fixed::fromRatio(int64_t(tick), int64_t(sim.tickRate())));
}

Vec3 RtsModule::worldPosition(Simulation& sim, Entity e, uint32_t tick) const {
    World& w = sim.world();
    auto* tr = w.tryGet<Transform>(e);
    if (!tr) return {};
    const Surface* s = surface(sim);
    if (!s) return tr->pos;
    if (auto* t = w.tryGet<Transit>(e)) {
        const uint32_t span = std::max<uint32_t>(1, t->arrive - t->depart);
        const uint32_t k = std::min(span, tick > t->depart ? tick - t->depart : 0);
        return t->fromWorld + (t->toWorld - t->fromWorld) * Fixed::fromRatio(int64_t(k), int64_t(span));
    }
    const int b = s->bodyAt(tr->pos);
    return bodyWorldPosition(sim, b, tick) + (tr->pos - Surface::anchor(b));
}

uint8_t RtsModule::cellVision(Simulation& sim, uint8_t player, int body, int face, int x, int y) const {
    auto* gs = sim.world().resource<GameState>();
    const Surface* s = surface(sim);
    if (!s || body < 0 || size_t(body) >= s->bodies.size() || player >= gs->vision.size()) return 0;
    const BodyDef& b = s->bodies[size_t(body)];
    if (!b.hasSurface() || face < 0 || face > 5 || x < 0 || y < 0 || x >= b.faceSize || y >= b.faceSize) return 0;
    if (player == 0) return 2;
    return gs->vision[player][size_t(s->gridCell(body, face, x, y))];
}

Vec3 RtsModule::lift(Simulation& sim, Vec3 p, Fixed h) const {
    const Surface* s = surface(sim);
    if (!s) { p.y = groundHeight(sim, p) + h; return p; }
    if (!Surface::onSphere(p)) return p;
    // airborne / orbit positions keep their altitude; ground positions go to the surface + h
    if (s->altitude(p) > s->heightAt(p) + Fixed::fromInt(1)) return p;
    return s->project(p, h);
}

Vec3 RtsModule::mapPos(Simulation& sim, const Json& p) const {
    const Surface* s = surface(sim);
    if (!p.isObject()) {
        Vec3 v{p[0].fx(), Fixed{}, p[p.size() >= 3 ? 2 : 1].fx()};
        if (s) throw std::runtime_error("map position: planet maps place things with {\"body\", \"face\", \"x\", \"y\"}");
        return v;
    }
    if (!s) throw std::runtime_error("map position {body, face, x, y}: the map has no world block");
    int b = p["body"].isString() ? s->findBody(p.getStr("body")) : int(p["body"].integer(s->startBody));
    if (b < 0 || size_t(b) >= s->bodies.size()) throw std::runtime_error("map position: unknown body '" + p.getStr("body") + "'");
    const BodyDef& bd = s->bodies[size_t(b)];
    if (!bd.hasSurface()) throw std::runtime_error("map position: body '" + bd.id + "' has no surface");
    const int face = int(p.getInt("face", 0));
    if (face < 0 || face > 5) throw std::runtime_error("map position: face must be 0..5");
    const Fixed n = Fixed::fromInt(bd.faceSize);
    const Fixed a = fxClamp(p.getFx("x"), Fixed{}, n), c = fxClamp(p.getFx("y"), Fixed{}, n);
    return Surface::anchor(b) + s->facePoint(b, face, a, c);
}

void RtsModule::setupWorld(Simulation& sim, const Json& map) {
    GameState& gs = state(sim);
    MapGrid& g = gs.grid;
    const Json& wj = map["world"];
    auto surf = std::make_shared<Surface>();
    std::vector<std::string> parents;
    const auto& items = wj["bodies"].items();
    if (items.empty()) throw std::runtime_error("map world: no bodies");
    if (int(items.size()) > Surface::kMaxBodies) throw std::runtime_error("map world: at most " + std::to_string(Surface::kMaxBodies) + " bodies");
    for (const Json& bj : items) {
        BodyDef b;
        b.id = bj.getStr("id", "body" + std::to_string(surf->bodies.size()));
        const std::string where = "map world body '" + b.id + "'";
        if (surf->findBody(b.id) >= 0) throw std::runtime_error(where + ": defined twice");
        b.kind = parseKind(bj.getStr("kind", "planet"), where);
        b.radius = bj.getFx("radius", Fixed::fromInt(32));
        if (b.radius < Fixed::fromInt(2) || b.radius > Fixed::fromInt(1200)) throw std::runtime_error(where + ": radius must be 2..1200");
        b.faceSize = b.kind == BodyKind::Star ? 0 : int(bj.getInt("faceSize", 0));
        if (b.kind != BodyKind::Star && (b.faceSize < 2 || b.faceSize > 512)) throw std::runtime_error(where + ": faceSize must be 2..512");
        b.seaLevel = bj.getFx("seaLevel");
        b.orbitAltitude = bj.getFx("orbitAltitude", b.radius * Fixed::fromRatio(15, 100));
        if (b.orbitAltitude.raw <= 0 || b.radius + b.orbitAltitude > Fixed::fromInt(1500)) throw std::runtime_error(where + ": orbitAltitude must be > 0 (radius + altitude <= 1500)");
        const Json& o = bj["orbit"];
        parents.push_back(o.isObject() ? o.getStr("around") : "");
        if (o.isObject()) {
            b.orbitRadius = o.getFx("radius");
            b.orbitPeriod = o.getFx("period");
            b.orbitPhase = o.getFx("phase");
            if (b.orbitPeriod.raw <= 0) throw std::runtime_error(where + ": orbit.period must be > 0 (seconds)");
        }
        if (bj["pos"].isArray()) b.pos = {bj["pos"][0].fx(), bj["pos"][1].fx(), bj["pos"][2].fx()};
        surf->bodies.push_back(b);
    }
    for (size_t i = 0; i < parents.size(); ++i) {
        if (parents[i].empty()) continue;
        const int p = surf->findBody(parents[i]);
        if (p < 0 || p >= int(i)) throw std::runtime_error("map world body '" + surf->bodies[i].id + "': orbit.around '" + parents[i] + "' must name an earlier body");
        surf->bodies[i].parent = p;
    }
    surf->startBody = -1;
    if (wj.has("start")) {
        surf->startBody = surf->findBody(wj.getStr("start"));
        if (surf->startBody < 0) throw std::runtime_error("map world: unknown start body '" + wj.getStr("start") + "'");
    }
    for (size_t i = 0; i < surf->bodies.size() && surf->startBody < 0; ++i) if (surf->bodies[i].hasSurface()) surf->startBody = int(i);
    if (surf->startBody < 0) throw std::runtime_error("map world: no body with a surface");
    surf->build();
    if (surf->W * surf->H > 6 * 512 * 512 * 2) throw std::runtime_error("map world: too many cells (at most 3.1M)");
    // corners per body (terrain.corners or terrain.cornersFile -> cornersBlob)
    for (size_t bi = 0; bi < surf->bodies.size(); ++bi) {
        const BodyDef& b = surf->bodies[bi];
        if (!b.hasSurface()) continue;
        const Json& t = items[bi]["terrain"];
        const size_t n = size_t(6) * size_t(b.faceSize + 1) * size_t(b.faceSize + 1);
        std::vector<int16_t> c(n, 0);
        if (t["corners"].size() > 0) {
            if (t["corners"].size() != n) throw std::runtime_error("map world body '" + b.id + "': terrain.corners needs 6*(faceSize+1)^2 = " + std::to_string(n) + " values");
            for (size_t i = 0; i < n; ++i) c[i] = int16_t(t["corners"][i].integer());
        } else if (t.has("cornersFile")) {
            std::string err;
            if (!decodeCorners(t, 6 * (b.faceSize + 1) - 1, b.faceSize, c, &err)) throw std::runtime_error("map world body '" + b.id + "': " + err);
        }
        // seam corners are stored on 2-3 faces: average the duplicates (tools write them equal)
        const CubeSphere& cs = surf->cubeOf(int(bi));
        const int N = b.faceSize, S = (N + 1) * (N + 1);
        for (int f = 0; f < 6; ++f)
            for (int j = 0; j <= N; ++j)
                for (int i = 0; i <= N; ++i) {
                    if (i != 0 && j != 0 && i != N && j != N) continue;
                    int ids[3];
                    const int k = cs.cornerDuplicates(f, i, j, ids);
                    if (ids[0] != cs.cornerId(f, i, j)) continue;
                    int64_t sum = 0;
                    for (int m = 0; m < k; ++m) sum += c[size_t(ids[m])];
                    const int16_t v = int16_t(sum >= 0 ? (sum + k / 2) / k : -((-sum + k / 2) / k));
                    for (int m = 0; m < k; ++m) c[size_t(ids[m])] = v;
                }
        std::copy(c.begin(), c.end(), surf->corners.begin() + long(b.cornerBase));
        (void)S;
    }
    g.init(surf->W, surf->H);
    std::fill(g.flags.begin(), g.flags.end(), uint8_t(0));   // void cells of smaller bodies: never passable
    for (size_t bi = 0; bi < surf->bodies.size(); ++bi) {
        const BodyDef& b = surf->bodies[bi];
        if (!b.hasSurface()) continue;
        const int N = b.faceSize;
        const Json& faces = items[bi]["terrain"]["faces"];
        if (faces.size() != 0 && faces.size() != 6) throw std::runtime_error("map world body '" + b.id + "': terrain.faces needs 6 faces");
        for (int f = 0; f < 6; ++f) {
            const Json& fj = faces.size() ? faces[size_t(f)] : Json();
            const Json& rows = fj["rows"];
            const Json& levels = fj["levels"];
            for (int y = 0; y < N; ++y) {
                const std::string row = y < int(rows.size()) ? rows[size_t(y)].str() : std::string();
                const std::string lv = y < int(levels.size()) ? levels[size_t(y)].str() : std::string();
                for (int x = 0; x < N; ++x) {
                    const size_t i = size_t(surf->gridCell(int(bi), f, x, y));
                    const char ch = x < int(row.size()) ? row[size_t(x)] : '.';
                    const int dk = rules_.findDiggable(ch);
                    if (dk >= 0) { g.flags[i] = 0; g.dig[i] = uint8_t(dk + 1); g.digHp[i] = rules_.diggables[size_t(dk)].hardness; }
                    else g.flags[i] = cellFlagsFromChar(ch);
                    if (x < int(lv.size()) && lv[size_t(x)] >= '0' && lv[size_t(x)] <= '9') g.level[i] = int8_t(lv[size_t(x)] - '0');
                }
            }
            // resource fields per face: "fields": {"metal": ["..19.."]} digits 1..9 of fieldCellMax
            for (auto& [rid, fr] : fj["fields"].members()) {
                const int ri = rules_.findResource(rid);
                if (ri < 0) throw std::runtime_error("map world body '" + b.id + "' fields: unknown resource '" + rid + "'");
                const ResourceDef& rd = rules_.resources[size_t(ri)];
                for (int y = 0; y < N && y < int(fr.size()); ++y) {
                    const std::string row = fr[size_t(y)].str();
                    for (int x = 0; x < N && x < int(row.size()); ++x) {
                        if (row[size_t(x)] < '1' || row[size_t(x)] > '9') continue;
                        const size_t i = size_t(surf->gridCell(int(bi), f, x, y));
                        if (!(g.flags[i] & kCellGround)) continue;
                        g.fieldRes[i] = int16_t(ri);
                        g.fieldAmt[i] = rd.fieldCellMax * (row[size_t(x)] - '0') / 9;
                    }
                }
            }
        }
    }
    if (rules_.terrain.model == TerrainModel::Flat) std::fill(g.level.begin(), g.level.end(), int8_t(0));
    std::shared_ptr<const Surface> cs = surf;
    gs.surface = cs;
    g.surf = cs;
    gs.hash.surf = cs;
    if (rules_.terrain.heightfield()) {
        const TerrainRules& tr = rules_.terrain;
        g.slopeLimit[0] = MapGrid::slopeUnits(tr.maxSlopeGround);
        g.sandLimit = MapGrid::slopeUnits(tr.maxSlopeSand.raw < 0 ? tr.maxSlopeGround : tr.maxSlopeSand);
        g.buildLimit = MapGrid::slopeUnits(tr.buildSlope);
        for (size_t k = 0; k < tr.slopeClasses.size() && k < 15; ++k) g.slopeLimit[k + 1] = MapGrid::slopeUnits(tr.slopeClasses[k]);
        g.enableSphereHeights(rules_.levelHeight);
    }
}

// ================================================================= movement on spheres

void RtsModule::moveSphere(Simulation& sim) {
    World& w = sim.world();
    GameState& gs = state(sim);
    MapGrid& g = gs.grid;
    const Surface& S = *gs.surface;
    const Fixed dt = sim.dt();
    auto place = [&](const UnitType& t, Vec3 p) {   // keep a position on its layer
        if (t.layer & kLayerOrbit) return S.toShell(p, S.bodies[size_t(S.bodyAt(p))].shellRadius());
        return S.project(p);
    };
    // 1) path following on the tangent plane, re-projected onto the surface / shell every step
    w.each<Mobile, Unit, Transform>([&](Entity e, Mobile& m, Unit& u, Transform& tr) {
        if (!m.moving || (u.flags & (kUnitInHand | kUnitPossessed))) return;
        auto* b = w.tryGet<Buffs>(e);
        if (b && b->stunUntil > sim.tick()) return;
        if (rules_.advancedEffects && (statusFlags(sim, e) & (kStStun | kStRoot))) return;
        const UnitType& t = rules_.types[u.type];
        if (m.goalRadius.raw > 0 && dist2D(tr.pos, m.goal) <= m.goalRadius) { m.moving = false; m.path.clear(); return; }
        Fixed step = stat(sim, e, Stat::Speed) * dt;
        const bool free = (t.layer & kLayerAir) != 0;
        while (m.pathPos < m.path.size() && step.raw > 0) {
            const Vec3 wp = m.path[m.pathPos];
            Vec3 d = S.tangent(tr.pos, wp - tr.pos);
            Fixed len = dist2D(tr.pos, wp);
            const Fixed dl = d.length();
            if (dl.raw > 0 && g.hf && !free) {          // uphill slower, downhill faster
                const Vec3 ahead = S.project(tr.pos + d * (kHalf / dl));
                const Fixed climb = (S.heightAt(ahead) - S.heightAt(tr.pos)) * 2;
                const Fixed f = climb.raw > 0 ? kOne - rules_.terrain.uphillSpeed * climb : kOne - rules_.terrain.downhillSpeed * climb;
                step = step * fxClamp(f, Fixed::fromRatio(1, 4), Fixed::fromInt(2));
            }
            Vec3 next = len <= step || dl.raw == 0 ? place(t, wp) : place(t, tr.pos + d * (step / dl));
            if (!free) {
                const int c0 = g.cellOf(tr.pos), c1 = g.cellOf(next);
                if (c0 != c1 && c0 >= 0 && g.passable(c0 % g.w, c0 / g.w, t.pathLayer) && !g.canMoveCells(c0, c1, t.pathLayer)) {
                    // flow-field followers slide along the wall (the move split on the face axes); others stop as before
                    bool slid = false;
                    if (m.flow && dl.raw > 0) {
                        Vec3 east, south;
                        S.frame(tr.pos, east, south);
                        const Vec3 mv = d * (fxMin(step, len) / dl);
                        for (const Vec3 axis : {east, south}) {
                            const Vec3 np = place(t, tr.pos + axis * mv.dot(axis));
                            const int cn = g.cellOf(np);
                            if (cn == c0 || g.canMoveCells(c0, cn, t.pathLayer)) { tr.pos = np; slid = true; break; }
                        }
                    }
                    m.stuck += dt;
                    if (slid) m.stuck -= dt / 2;
                    break;
                }
            }
            if (dl.raw > 0) u.facing = d / dl;
            if (len <= step) { tr.pos = next; step -= len; ++m.pathPos; }
            else { tr.pos = next; step = Fixed{}; }
        }
        if (m.pathPos >= m.path.size()) { m.moving = false; m.path.clear(); }
        if (m.stuck > kOne) {
            m.stuck = Fixed{};
            // on a flow field: re-sample it (crowds clear by themselves; the field already holds the way around the
            // obstacles). Only a unit stuck over and over (a new wall the field can't see yet) falls back to A*.
            if (m.flow && ++m.flowStuck < 6) { m.path.clear(); m.pathPos = 0; m.path.push_back(m.goal); }
            else {
            m.flow = false;
            g.findPath(tr.pos, m.goal, t.pathLayer, m.path, 40000, MapGrid::clearanceFor(t.radius), true);
            m.pathPos = 0;
            m.moving = !m.path.empty();
            }
        }
    });
    // 2) separation of overlapping units on the same layer (tangent plane)
    struct Push { Entity e; Vec3 d; };
    std::vector<Push> pushes;
    w.each<Mobile, Unit, Transform>([&](Entity e, Mobile& m, Unit& u, Transform& tr) {
        if (u.flags & kUnitInHand) return;
        const UnitType& t = rules_.types[u.type];
        Vec3 acc{};
        bool any = false;
        gs.hash.query(tr.pos, t.radius * 2 + kOne, [&](Entity o) {
            if (o == e) return;
            auto* ou = w.tryGet<Unit>(o);
            if (!ou || !w.has<Mobile>(o) || (ou->flags & kUnitInHand)) return;
            const UnitType& ot = rules_.types[ou->type];
            if (((ot.layer ^ t.layer) & kLayerOrbit) || !(ot.layer & t.layer)) return;
            Vec3 d = S.tangent(tr.pos, tr.pos - w.get<Transform>(o).pos);
            Fixed len = d.length();
            const Fixed overlap = t.radius + ot.radius - len - Fixed::fromRatio(3, 100);
            if (overlap.raw <= 0) return;
            if (len.raw == 0) {
                Vec3 east, south;
                S.frame(tr.pos, east, south);
                d = e.index < o.index ? east : east * Fixed::fromInt(-1);
                len = kOne;
            }
            const Fixed share = (m.moving ? Fixed::fromRatio(2, 3) : Fixed::fromRatio(1, 3));
            acc += d * (overlap * share / len);
            any = true;
        });
        if (!any) return;
        const Fixed maxPush = t.speed * dt / 2 + Fixed::fromRatio(1, 50);
        const Fixed len = acc.length();
        if (len > maxPush) acc = acc * (maxPush / len);
        pushes.push_back({e, acc});
    });
    for (auto& p : pushes) {
        Transform& tr = w.get<Transform>(p.e);
        const UnitType& t = rules_.types[w.get<Unit>(p.e).type];
        const Vec3 np = place(t, tr.pos + p.d);
        if (!(t.layer & kLayerAir)) {
            const int c0 = g.cellOf(tr.pos), c1 = g.cellOf(np);
            if (c0 != c1 && !g.canMoveCells(c0, c1, t.pathLayer) && c0 >= 0 && g.passable(c0 % g.w, c0 / g.w, t.pathLayer)) continue;
        }
        tr.pos = np;
    }
}

// ================================================================= vision on spheres

void RtsModule::visionSphere(Simulation& sim) {
    World& w = sim.world();
    GameState& gs = state(sim);
    const MapGrid& g = gs.grid;
    const Surface& S = *gs.surface;
    const size_t np = gs.vision.size();
    std::vector<std::vector<uint8_t>> sharers(np);
    for (size_t p = 1; p < np; ++p) for (size_t q = 1; q < np; ++q) if (q == p || (rules_.fog.shareAllies && sim.allied(uint8_t(p), uint8_t(q)))) sharers[p].push_back(uint8_t(q));
    auto see = [&](uint8_t owner, size_t i) {
        for (uint8_t q : sharers[owner]) {
            uint8_t& v = gs.vision[q][i];
            if (v != 2) { v = 2; gs.visibleCells[q].push_back(int32_t(i)); }
        }
    };
    const bool los = (g.hf && rules_.heights.blockVision) || rules_.fog.lineOfSight;
    std::vector<int32_t> cells;
    // ground radius per cell (lazily filled; 0 = not yet): terrain + vision blockers (structures with
    // vision.blocks over their footprint, undug rock)
    std::vector<Fixed> ground(los ? g.flags.size() : 0);
    std::vector<Fixed> blockers(los ? g.flags.size() : 0);
    if (los) {
        const Fixed rock = rules_.fog.rockHeight;
        if (rock.raw > 0) for (size_t i = 0; i < g.dig.size(); ++i) if (g.dig[i]) blockers[i] = rock;
        w.each<Unit, Transform>([&](Entity e, Unit& u, Transform& tr) {
            const UnitType& t = rules_.types[u.type];
            if (t.visionBlock.raw <= 0 || u.hp.raw <= 0) return;
            if (auto* f = w.tryGet<Footprint>(e); f && f->x >= 0) {
                for (int y = 0; y < f->h; ++y) for (int x = 0; x < f->w; ++x) { Fixed& b = blockers[size_t((f->y + y) * g.w + f->x + x)]; b = fxMax(b, t.visionBlock); }
            } else if (const int c = g.cellOf(tr.pos); c >= 0) blockers[size_t(c)] = fxMax(blockers[size_t(c)], t.visionBlock);
        });
    }
    auto occHeight = [&](int c) { return S.cellHeight(c) + blockers[size_t(c)]; };
    w.each<Unit, Owner, Transform>([&](Entity e, Unit& u, Owner& o, Transform& tr) {
        if (o.player == 0 || o.player >= np || (u.flags & kUnitInHand)) return;
        const UnitType& t = rules_.types[u.type];
        const Fixed sight = stat(sim, e, Stat::Sight);
        const int body = S.bodyAt(tr.pos);
        if (!S.bodies[size_t(body)].hasSurface()) return;
        const int c = S.cellAt(tr.pos);
        // cells are ~0.8 of the mean size near face corners: widen the square, then test the distance
        const Fixed cs = S.cellSize(body);
        const int k = int((sight / (cs * Fixed::fromRatio(8, 10))).floorInt()) + 1;
        const bool air = (t.layer & kLayerAir) != 0;
        const Vec3 o3 = Surface::anchor(body);
        const Vec3 eye = air || t.seesOver ? tr.pos : S.project(tr.pos, rules_.terrain.eyeHeight + t.eyeHeight);
        const Fixed R = S.bodies[size_t(body)].radius;
        cells.clear();
        const int M = S.cubeOf(body).faceSize();
        if (k >= M) { for (int i = 0; i < 6 * M * M; ++i) { const SphereCell sc = S.cubeOf(body).cell(i); cells.push_back(S.gridCell(body, sc.face, sc.x, sc.y)); } }
        else
            for (int dy = -k; dy <= k; ++dy)
                for (int dx = -k; dx <= k; ++dx) {
                    int n = S.offset(c, dx, dy);
                    if (n < 0) { const int mid = S.offset(c, 0, dy); n = mid < 0 ? -1 : S.offset(mid, dx, 0); }
                    if (n >= 0) cells.push_back(n);
                }
        const std::vector<uint8_t>& mine = gs.vision[o.player];
        for (int32_t n : cells) {
            if (mine[size_t(n)] == 2) continue;   // already lit for this owner (and its sharers) this pass
            const Vec3 cc = S.cellCenter(n);
            if (dist2D(cc, tr.pos) > sight) continue;
            if (los && !air && !t.seesOver) {
                // march the 3D sight line in radial terms: at sample k the line's distance from the centre
                // must stay above the ground radius of the cell under it (hills and the horizon). The cell
                // under a sample is found with cellFromDir (table lookup, no trigonometry); ground radii are
                // cached per cell for this pass.
                const Vec3 tgt = cc + S.cellDir(n) * rules_.terrain.targetHeight;
                const int steps = int((dist2D(cc, tr.pos) / cs).floorInt()) + 1;   // about one sample per cell
                bool visible = true;
                for (int st = 1; st < steps && visible; ++st) {
                    const Vec3 p = eye + (tgt - eye) * Fixed::fromRatio(st, steps);
                    const Vec3 l = p - o3;
                    const SphereCell sc = S.cubeOf(body).cell(S.cubeOf(body).cellFromDir(l));
                    const int gc = S.gridCell(body, sc.face, sc.x, sc.y);
                    Fixed& gr = ground[size_t(gc)];
                    if (gr.raw == 0) gr = R + occHeight(gc) + Fixed::fromRaw(1);
                    if (l.lengthSq() < gr * gr) visible = false;
                }
                if (!visible) continue;
            }
            see(o.player, size_t(n));
        }
    });
}

// ================================================================= interplanetary transit

bool RtsModule::canTransit(Simulation& sim, Entity e, int body) const {
    World& w = sim.world();
    const Surface* S = surface(sim);
    if (!S || body < 0 || size_t(body) >= S->bodies.size() || !S->bodies[size_t(body)].hasSurface()) return false;
    auto* u = w.tryGet<Unit>(e);
    if (!u || u->hp.raw <= 0 || (u->flags & (kUnitInHand | kUnitUnderConstruction)) || !w.has<Mobile>(e)) return false;
    if (!(rules_.types[u->type].layer & kLayerOrbit)) return false;
    return S->bodyAt(w.get<Transform>(e).pos) != body;
}

void RtsModule::applyTransit(Simulation& sim, const Command& cmd) {
    World& w = sim.world();
    const Surface* S = surface(sim);
    if (!S || cmd.param < 0 || cmd.param >= int64_t(S->bodies.size())) return;
    const int to = int(cmd.param);
    for (Entity e : cmd.units) {
        if (!w.alive(e) || !w.has<Owner>(e) || w.get<Owner>(e).player != cmd.player || !canTransit(sim, e, to)) continue;
        Transform& tr = w.get<Transform>(e);
        const UnitType& t = rules_.types[w.get<Unit>(e).type];
        const int from = S->bodyAt(tr.pos);
        const uint32_t now = sim.tick();
        Transit tx;
        tx.from = int16_t(from); tx.to = int16_t(to); tx.depart = now;
        tx.fromWorld = bodyWorldPosition(sim, from, now) + (tr.pos - Surface::anchor(from));
        // arrive on the side of the target shell that faces the departure point (positions at departure)
        const Vec3 tc = bodyWorldPosition(sim, to, now);
        Vec3 dir = fxNormalize(tx.fromWorld - tc);
        if (dir.lengthSq().raw == 0) dir = {kOne, Fixed{}, Fixed{}};
        tx.arrival = dir * S->bodies[size_t(to)].shellRadius();
        tx.toWorld = tc + tx.arrival;
        const Fixed speed = fxMax(t.transitSpeed, Fixed::fromRatio(1, 10));
        const Fixed secs = (tx.toWorld - tx.fromWorld).length() / speed;
        tx.arrive = now + std::max<uint32_t>(1, uint32_t((secs * int64_t(sim.tickRate())).toInt()));
        stopMoving(sim, e);
        if (auto* ord = w.tryGet<Orders>(e)) ord->queue.clear();
        if (auto* c = w.tryGet<Combat>(e)) c->target = kNullEntity;
        w.get<Unit>(e).flags |= uint8_t(kUnitInHand | kUnitInTransit);
        if (w.has<Transit>(e)) w.get<Transit>(e) = tx; else w.add<Transit>(e, tx);
        sim.emit({kEvTransitStart, cmd.player, e, kNullEntity, to, tr.pos});
    }
}

void RtsModule::sysTransit(Simulation& sim) {
    World& w = sim.world();
    std::vector<Entity> arrived;
    w.each<Transit>([&](Entity e, Transit& t) { if (sim.tick() >= t.arrive) arrived.push_back(e); });
    for (Entity e : arrived) {
        const Transit t = w.get<Transit>(e);
        w.remove<Transit>(e);
        if (!w.has<Unit>(e)) continue;
        Unit& u = w.get<Unit>(e);
        u.flags &= uint8_t(~(kUnitInHand | kUnitInTransit));
        Transform& tr = w.get<Transform>(e);
        tr.pos = Surface::anchor(t.to) + t.arrival;
        tr.prevPos = tr.pos;
        sim.emit({kEvTransitEnd, w.get<Owner>(e).player, e, kNullEntity, t.to, tr.pos});
    }
}

void RtsModule::hashWorld(Simulation& sim, Hasher& h) {
    World& w = sim.world();
    const Surface& S = *state(sim).surface;
    h.u32(uint32_t(S.bodies.size()));
    for (auto& b : S.bodies) { h.u32(uint32_t(b.faceSize)); h.fx(b.radius); h.fx(b.orbitPeriod); }
    for (size_t i = 0; i < S.corners.size(); i += 17) h.i32(S.corners[i]);
    w.each<Transit>([&](Entity e, Transit& t) { h.u64(e.packed()); h.i32(t.from); h.i32(t.to); h.u32(t.depart); h.u32(t.arrive); h.v3(t.arrival); });
}

} // namespace rts::game
