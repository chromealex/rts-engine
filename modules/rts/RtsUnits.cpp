// Orders and movement: move/attack-move/patrol, gathering from nodes and fields (with harvesters,
// reclaim, upkeep), construction in walk/warp/morph modes, repair, entering garrisons, casting.
#include "RtsInternal.h"
#include <algorithm>
#include <map>

namespace rts::game {
using namespace detail;

// ================================================================= queries

Entity RtsModule::findNearestDropoff(Simulation& sim, uint8_t owner, Vec3 p, int res) {
    World& w = sim.world();
    Entity best;
    Fixed bestD{};
    w.each<Unit, Owner, Transform>([&](Entity e, Unit& u, Owner& o, Transform& t) {
        if (o.player != owner || (u.flags & kUnitUnderConstruction)) return;
        if (!(rules_.types[u.type].dropoffMask & (1u << res))) return;
        Fixed d = dist2D(p, t.pos);
        if (!best.valid() || d < bestD) { best = e; bestD = d; }
    });
    return best;
}

Entity RtsModule::findNearestResource(Simulation& sim, uint8_t owner, Vec3 p, int res, Fixed maxDist) {
    World& w = sim.world();
    Entity best;
    Fixed bestD{};
    w.each<ResourceStock, Unit, Owner, Transform>([&](Entity e, ResourceStock& rs, Unit& u, Owner& o, Transform& t) {
        if (rs.res != res || (!rs.infinite && rs.amount.raw <= 0) || (u.flags & kUnitUnderConstruction)) return;
        if (rules_.types[u.type].ownerOnly && o.player != owner) return;
        Fixed d = dist2D(p, t.pos);
        if (d > maxDist) return;
        if (!best.valid() || d < bestD) { best = e; bestD = d; }
    });
    return best;
}

// ================================================================= movement helpers

void RtsModule::stopMoving(Simulation& sim, Entity e) {
    if (auto* m = sim.world().tryGet<Mobile>(e)) { m->moving = false; m->path.clear(); m->pathPos = 0; m->stuck = Fixed{}; m->flow = false; }
}

void RtsModule::moveToFlow(Simulation& sim, Entity e, Vec3 goal, Fixed radius) {
    World& w = sim.world();
    auto* m = w.tryGet<Mobile>(e);
    if (!m) return;
    const UnitType& t = rules_.types[w.get<Unit>(e).type];
    MapGrid& g = state(sim).grid;
    if (t.layer & kLayerAir) { moveTo(sim, e, goal, radius); return; }   // fliers go straight anyway
    struct Timed { MapGrid& g; int64_t t0; ~Timed() { if (g.clockNs) g.pathNs += g.clockNs() - t0; } } timed{g, g.clockNs ? g.clockNs() : 0};
    const Fixed cs = g.surf ? g.surf->cellSize(g.surf->bodyAt(goal)) : kOne;
    const int rc = std::clamp(int((radius / cs).floorInt()), 0, 32);
    const int fi = g.flowAcquire(goal, rc, t.pathLayer, MapGrid::clearanceFor(t.radius), sim.tick());
    if (fi < 0) { moveTo(sim, e, goal, radius); return; }
    // the crowd settles from the goal outward: the first followers stop at the goal point, the others when they touch
    // one that arrived (sysFlowFields); radius stays the size of the field's goal area and the contagion limit
    m->goal = goal; m->goalRadius = fxMin(radius, kOne); m->flowArea = radius; m->stuck = Fixed{}; m->pathPos = 0;
    m->flow = true; m->flowRadius = uint8_t(rc); m->flowStuck = 0;
    m->flowGoalCell = g.cellOf(goal); m->flowArrived = -1; m->flowNear = 0;
    bool arrived = false;
    if (!g.flowSteer(fi, w.get<Transform>(e).pos, goal, m->path, arrived)) { m->flow = false; moveTo(sim, e, goal, radius); return; }
    if (arrived || m->path.empty()) m->path = {goal};
    else m->path.push_back(goal);   // [next waypoint, goal]: the goal keeps the unit moving between refreshes
    m->moving = true;
}

// Units following flow fields: new waypoints when the last one was reached, and every 10 ticks (staggered by
// entity index). Field references (LRU) are counted here. Deterministic: ECS order, fields are pure functions.
void RtsModule::sysFlowFields(Simulation& sim) {
    World& w = sim.world();
    GameState& gs = state(sim);
    MapGrid& g = gs.grid;
    if (!rules_.movement.flow.enabled()) return;
    struct Timed { MapGrid& g; int64_t t0; ~Timed() { if (g.clockNs) g.pathNs += g.clockNs() - t0; } } timed{g, g.clockNs ? g.clockNs() : 0};
    g.flowResetRefs();
    const uint32_t tick = sim.tick();
    std::map<int32_t, int> settled;   // goal cell -> units that arrived there (ordered map: deterministic)
    w.each<Mobile>([&](Entity, Mobile& m) { if (m.flowArrived >= 0) ++settled[m.flowArrived]; });
    w.each<Mobile, Unit, Transform>([&](Entity e, Mobile& m, Unit& u, Transform& tr) {
        // followers that fell back to their own path (stuck) still settle with the crowd
        if (m.flowGoalCell < 0 || m.flowArrived >= 0 || (u.flags & (kUnitOffMap | kUnitPossessed))) return;
        if (!m.moving) {   // stopped on the way (end of its path near the goal): settled; far away: no longer part of it
            if (dist2D(tr.pos, m.goal) <= m.flowArea * 2 + Fixed::fromInt(2)) { m.flowArrived = m.flowGoalCell; ++settled[m.flowGoalCell]; }
            else m.flowGoalCell = -1;
            return;
        }
        const UnitType& t = rules_.types[u.type];
        // arrival: in the goal radius, or touching a unit that already arrived at the same goal (within twice the
        // radius): it stops, and the crowd settles from the goal outward instead of pushing into the ones in front
        const Fixed dg = dist2D(tr.pos, m.goal);
        bool arrive = m.goalRadius.raw > 0 && dg <= m.goalRadius;
        // inside the compact disc of the crowd (~ the goal area) touching settles at once; farther out only a unit
        // that can't get on (slowed down: stuck time) settles, so the crowd fills the area around the goal instead
        // of growing a tail along the approach
        // the settled disc grows with the units that settled: radius of k packed bodies ~ 0.53 * diameter * sqrt(k)
        const auto it = settled.find(m.flowGoalCell);
        const int k = it == settled.end() ? 0 : it->second;
        const Fixed disc = (t.radius * 2 + Fixed::fromRatio(1, 10)) * Fixed::fromRatio(53, 100) * fxSqrt(Fixed::fromInt(k + 1));
        // stuck, or circling the crowd for 20 s without finding a gap: settle wherever it touches it
        const Fixed base = disc * 13 / 10 + Fixed::fromInt(2);
        if (dg <= m.flowArea * 2 + Fixed::fromInt(2)) { if (m.flowNear < 65535) ++m.flowNear; } else m.flowNear = 0;
        // nobody settled yet (the goal point itself is taken / blocked): the first unit hovering in the goal area settles
        if (k == 0 && dg <= m.flowArea && m.flowNear > 2 * sim.tickRate()) arrive = true;
        const bool late = m.stuck > Fixed::fromRatio(1, 2) || m.flowNear > 20 * sim.tickRate();
        const Fixed limit = late ? fxMax(base, m.flowArea * 2 + Fixed::fromInt(2)) : base;
        if (!arrive && m.goalRadius.raw > 0 && dg <= limit) {
            gs.hash.query(tr.pos, t.radius + Fixed::fromInt(2), [&](Entity o) {
                if (arrive || o == e) return;
                auto* om = w.tryGet<Mobile>(o);
                if (!om || om->moving || om->flowArrived != m.flowGoalCell) return;
                const Fixed touch = t.radius + rules_.types[w.get<Unit>(o).type].radius + Fixed::fromRatio(3, 10);
                if (dist2D(w.get<Transform>(o).pos, tr.pos) <= touch) arrive = true;
            });
        }
        if (arrive) { m.moving = false; m.path.clear(); m.pathPos = 0; m.flow = false; m.flowArrived = m.flowGoalCell; ++settled[m.flowGoalCell]; return; }
        if (!m.flow) return;
        const int fi = g.flowFind(g.cellOf(m.goal), m.flowRadius, t.pathLayer, g.flowHierFor(g.cellOf(m.goal)) ? 0 : MapGrid::clearanceFor(t.radius));
        if (fi < 0) { m.flow = false; return; }   // evicted: plain path from here
        g.flowRef(fi, tick);
        if (m.path.size() >= 2 && m.pathPos == 0 && (tick + e.index) % 10 != 0) return;
        if ((tick + e.index) % 200 == 0 && m.flowStuck) --m.flowStuck;   // forgive old stuck events slowly
        bool arrived = false;
        std::vector<Vec3> wp;
        if (!g.flowSteer(fi, tr.pos, m.goal, wp, arrived)) { // off the field (pushed into a pocket): A* from here
            m.flow = false;
            g.findPath(tr.pos, m.goal, t.pathLayer, m.path, 40000, MapGrid::clearanceFor(t.radius));
            m.pathPos = 0;
            m.moving = !m.path.empty();
            return;
        }
        m.path = wp;
        if (arrived || wp.empty()) m.path = {m.goal};
        else m.path.push_back(m.goal);
        m.pathPos = 0;
    });
    g.flowHousekeeping();
}

void RtsModule::moveTo(Simulation& sim, Entity e, Vec3 goal, Fixed radius) { moveToVia(sim, e, goal, radius, false); }

void RtsModule::moveToVia(Simulation& sim, Entity e, Vec3 goal, Fixed radius, bool aroundAllies) {
    World& w = sim.world();
    auto* m = w.tryGet<Mobile>(e);
    if (!m) return;
    const UnitType& t = rules_.types[w.get<Unit>(e).type];
    m->goal = goal;
    m->goalRadius = radius;
    m->stuck = Fixed{};
    m->pathPos = 0;
    m->flow = false;
    m->flowArrived = -1;
    m->flowGoalCell = -1;
    state(sim).grid.findPath(w.get<Transform>(e).pos, goal, t.pathLayer, m->path, 40000, MapGrid::clearanceFor(t.radius), aroundAllies);
    m->moving = !m->path.empty();
}

// ================================================================= melee surround

namespace {
// 32 unit directions (cos, sin) in 16.16 fixed point, k * 11.25 degrees.
constexpr int32_t kDir32[32][2] = {{65536, 0}, {64277, 12785}, {60547, 25080}, {54491, 36410}, {46341, 46341}, {36410, 54491}, {25080, 60547}, {12785, 64277},
    {0, 65536}, {-12785, 64277}, {-25080, 60547}, {-36410, 54491}, {-46341, 46341}, {-54491, 36410}, {-60547, 25080}, {-64277, 12785},
    {-65536, 0}, {-64277, -12785}, {-60547, -25080}, {-54491, -36410}, {-46341, -46341}, {-36410, -54491}, {-25080, -60547}, {-12785, -64277},
    {0, -65536}, {12785, -64277}, {25080, -60547}, {36410, -54491}, {46341, -46341}, {54491, -36410}, {60547, -25080}, {64277, -12785}};
Vec3 dir32(int k) { return {Fixed::fromRaw(kDir32[k & 31][0]), Fixed{}, Fixed::fromRaw(kDir32[k & 31][1])}; }
} // namespace

// A free slot on a ring around the target where this melee unit can hit it; slots are spread by angle,
// claimed per target (deterministic: units are processed in entity order) and kept for a second.
bool RtsModule::attackSlot(Simulation& sim, Entity e, Entity target, Fixed range, Vec3& out) {
    World& w = sim.world();
    MapGrid& g = state(sim).grid;
    auto* c = w.tryGet<Combat>(e);
    if (!c || !w.alive(target)) return false;
    const UnitType& t = rules_.types[w.get<Unit>(e).type];
    const UnitType& tt = rules_.types[w.get<Unit>(target).type];
    const Vec3 tp = w.get<Transform>(target).pos, me = w.get<Transform>(e).pos;
    Fixed rT = tt.structure ? Fixed::fromInt(std::max(tt.footprintW, tt.footprintH)) / 2 : tt.radius;
    Fixed ring = rT + t.radius + range / 2 + Fixed::fromRatio(1, 20);
    if (c->slotTarget == target && c->slot >= 0 && sim.tick() < c->slotTick + 20) {
        const Surface* S0 = state(sim).surface.get();
        out = S0 ? S0->project(tp + S0->fromPlane(tp, dir32(c->slot) * ring)) : tp + dir32(c->slot) * ring;
        return true;
    }
    // drop the old claim
    for (auto& cl : slotClaims_) if (cl.claimer == e.packed()) cl.target = 0;
    int n = int((ring * 6283 / 1000 / (t.radius * 2 + Fixed::fromRatio(1, 10))).toInt());
    n = std::clamp(n, 4, 32);
    const int stride = 32 / n;
    n = 32 / stride;
    const Surface* S = state(sim).surface.get();
    Vec3 d = S ? S->toPlane(tp, me - tp) : flat(me - tp);
    if (d.length().raw == 0) d = {kOne, Fixed{}, Fixed{}};
    int k0 = 0;
    Fixed best{};
    for (int k = 0; k < n; ++k) { Fixed dot = d.dot(dir32(k * stride)); if (k == 0 || dot > best) { best = dot; k0 = k; } }
    for (int step = 0; step < n; ++step) {
        int k = ((k0 + ((step + 1) / 2) * ((step & 1) ? 1 : -1)) % n + n) % n;
        int dirIdx = k * stride;
        Vec3 p = S ? S->project(tp + S->fromPlane(tp, dir32(dirIdx) * ring)) : tp + dir32(dirIdx) * ring;
        if (!g.passableAt(p, t.pathLayer)) continue;
        bool taken = false;
        for (auto& cl : slotClaims_) if (cl.target == target.packed() && cl.slot == dirIdx && cl.claimer != e.packed()) taken = true;
        if (taken) continue;
        slotClaims_.push_back({target.packed(), dirIdx, e.packed()});
        c->slotTarget = target; c->slot = int8_t(dirIdx); c->slotTick = sim.tick(); c->slotPos = p;
        out = p;
        return true;
    }
    return false; // ring full: queue behind as before
}

// ================================================================= orders

bool RtsModule::runGather(Simulation& sim, Entity e, Order& o) {
    World& w = sim.world();
    GameState& gs = state(sim);
    const UnitType& t = rules_.types[w.get<Unit>(e).type];
    auto* cargo = w.tryGet<Cargo>(e);
    auto* m = w.tryGet<Mobile>(e);
    if (!cargo) return true;
    const uint8_t owner = w.get<Owner>(e).player;
    auto gatherDef = [&](int res) -> const GatherDef* {
        for (auto& g : t.gathers) if (g.res == res) return &g;
        return nullptr;
    };

    // ---------- resource field (ore / spice): harvest cell by cell until full
    if (!o.target.valid()) {
        Vec3& tp = w.get<Transform>(e).pos;
        int res = o.param;
        if (o.state == 0) {
            Vec3 cell;
            Vec3 from = o.point.x.raw || o.point.z.raw ? o.point : tp;
            // any field resource this unit can carry when res < 0
            bool found = false;
            if (res >= 0) found = findNearestFieldCell(sim, from, res, cell).valid();
            else for (auto& g : t.gathers) if (!found && rules_.resources[g.res].field && findNearestFieldCell(sim, from, g.res, cell).valid()) { found = true; res = g.res; }
            if (!found || !gatherDef(res)) {
                // field exhausted: bring home what we have
                if (cargo->amount.raw > 0) { Order r; r.type = OrderType::Return; o = r; return false; }
                return true;
            }
            o.param = res;
            o.point2 = cell;
            moveTo(sim, e, cell, Fixed::fromRatio(3, 10));
            o.state = 1;
            return false;
        }
        const GatherDef* gd = gatherDef(res);
        if (!gd) return true;
        if (o.state == 1) {
            if (dist2D(tp, o.point2) <= Fixed::fromRatio(5, 10)) { stopMoving(sim, e); o.state = 2; }
            else if (m && !m->moving) { o.point = o.point2; o.state = 0; }
            return false;
        }
        // state 2: harvesting the current cell
        int cx, cy; gs.grid.cellXY(o.point2, cx, cy);
        if (!gs.grid.inside(cx, cy)) return true;
        size_t ci = size_t(gs.grid.index(cx, cy));
        if (cargo->res != res) { cargo->amount = Fixed{}; cargo->res = int16_t(res); }
        if (gs.grid.fieldRes[ci] != res || gs.grid.fieldAmt[ci].raw <= 0 || (gs.grid.fieldAmt[ci] < kOne && cargo->amount < gd->carry / 2)) {
            o.point = o.point2; o.state = 0; // next cell around here
            return false;
        }
        Fixed rate = gd->carry / fxMax(gd->time, Fixed::fromRatio(1, 10)) * stat(sim, e, Stat::GatherRate);
        Fixed take = fxMin(fxMin(rate * sim.dt(), gd->carry - cargo->amount), gs.grid.fieldAmt[ci]);
        gs.grid.fieldAmt[ci] -= take;
        cargo->amount += take;
        cargo->field = true;
        cargo->lastPos = o.point2;
        gs.fieldsDirty = true;
        if (cargo->amount >= gd->carry) { Order r; r.type = OrderType::Return; o = r; }
        return false;
    }

    // ---------- resource node (mine, tree, geyser, farm, wreck)
    if (!w.alive(o.target) || !w.has<ResourceStock>(o.target)) {
        int res = cargo->res;
        Entity n = res >= 0 ? findNearestResource(sim, owner, cargo->lastPos, res, Fixed::fromInt(14)) : kNullEntity;
        if (!n.valid()) return true;
        o.target = n;
        o.state = 0;
    }
    const UnitType& nt = rules_.types[w.get<Unit>(o.target).type];
    auto& rs0 = w.get<ResourceStock>(o.target);
    const GatherDef* gd = gatherDef(rs0.res);
    if (!gd || (nt.ownerOnly && w.get<Owner>(o.target).player != owner) || w.has<Construction>(o.target)) return true;
    if (o.state == 0) {
        moveTo(sim, e, w.get<Transform>(o.target).pos, nt.radius + t.radius + Fixed::fromRatio(4, 10));
        o.state = 1;
    }
    if (o.state == 1) {
        const Fixed cd = contactDistance(sim, e, o.target);
        // limited nodes (one worker per patch): a worker that can't get to the patch because others stand there
        // waits close by instead of asking for a new path every tick
        const bool queued = nt.maxGatherers > 0 && cd <= Fixed::fromInt(2) && m && !m->moving;
        if (cd <= Fixed::fromRatio(6, 10) || queued) {
            const bool full = nt.maxGatherers > 0 && gatherersAt(sim, o.target) >= nt.maxGatherers;
            if (!full && cd > Fixed::fromRatio(6, 10)) {             // free but out of reach: walk up again now and then
                if ((sim.tick() + e.index) % 20 == 0) o.state = 0;
                return false;
            }
            if (full) {
                if ((sim.tick() + e.index) % 8 != 0) return false;   // wait; look for another node a few times a second
                // node full (WC3 mine: 5 inside, SC patch: one at a time): try a free node close by, else wait
                Vec3 at = w.get<Transform>(e).pos;
                Entity alt;
                Fixed bd = Fixed::fromInt(7);
                const int16_t nres = w.get<ResourceStock>(o.target).res;
                w.each<ResourceStock, Unit, Transform>([&](Entity n, ResourceStock& rs, Unit& nu, Transform& ntr) {
                    if (n == o.target || rs.res != nres || nu.hp.raw <= 0) return;
                    const UnitType& at2 = rules_.types[nu.type];
                    if (at2.ownerOnly && w.get<Owner>(n).player != owner) return;
                    if (at2.maxGatherers > 0 && gatherersAt(sim, n) >= at2.maxGatherers) return;
                    Fixed d = dist2D(ntr.pos, at);
                    if (d < bd) { bd = d; alt = n; }
                });
                if (alt.valid()) { o.target = alt; o.state = 0; }
                return false;
            }
            stopMoving(sim, e); o.state = 2; o.timer = gd->time / fxMax(stat(sim, e, Stat::GatherRate), Fixed::fromRatio(1, 10));
            if (nt.maxGatherers > 0) { gatherersAt(sim, o.target); if (gatherCount_.size() <= o.target.index) gatherCount_.resize(o.target.index + 1, 0); gatherCount_[o.target.index]++; }
        }
        else if (m && !m->moving) o.state = 0;
        return false;
    }
    if (o.state == 2) {
        o.timer -= sim.dt();
        if (o.timer.raw > 0) return false;
        auto& rs = w.get<ResourceStock>(o.target);
        Fixed take = rs.infinite ? gd->carry : fxMin(gd->carry, rs.amount);
        if (!rs.infinite) rs.amount -= take;
        const int16_t res = rs.res;
        const Vec3 nodePos = w.get<Transform>(o.target).pos;
        if (!rs.infinite && rs.amount.raw <= 0) {
            Unit& nu = w.get<Unit>(o.target);
            nu.hp = Fixed{};
            nu.flags |= kUnitSilent; // depleted: removed by the death system
        }
        if (rules_.resources[size_t(res)].kind == ResourceKind::Flow || nt.dropoffMask & (1u << res)) {
            // reclaim (TA) or gather-in-place (farm that is its own dropoff): credit directly
            addResource(sim, owner, uint16_t(res), take * gatherMultFor(sim, owner, res));
            sim.emit({kEvGathered, owner, e, o.target, res, nodePos});
            o.state = 2;
            o.timer = gd->time / fxMax(stat(sim, e, Stat::GatherRate), Fixed::fromRatio(1, 10));
            return !w.alive(o.target) || w.get<Unit>(o.target).hp.raw <= 0;
        }
        if (cargo->res != res) cargo->amount = Fixed{};
        cargo->res = res;
        cargo->amount = take;
        cargo->lastNode = o.target;
        cargo->lastPos = nodePos;
        cargo->field = false;
        Order ret;
        ret.type = OrderType::Return;
        o = ret;
        return false;
    }
    return true;
}

bool RtsModule::runBuild(Simulation& sim, Entity e, Order& o) {
    World& w = sim.world();
    const UnitType& t = rules_.types[w.get<Unit>(e).type];
    auto* m = w.tryGet<Mobile>(e);
    if (o.state == 0) {
        if (o.target.valid()) { o.state = 2; return false; } // helping an existing construction
        if (o.param < 0) return true;
        const UnitType& bt = rules_.types[size_t(o.param)];
        moveTo(sim, e, o.point, bt.radius + t.radius + Fixed::fromRatio(5, 10));
        o.state = 1;
        return false;
    }
    if (o.state == 1) {
        const UnitType& bt = rules_.types[size_t(o.param)];
        Fixed d = dist2D(w.get<Transform>(e).pos, o.point) - bt.radius - t.radius;
        if (d > kOne && m && m->moving) return false;
        if (d > Fixed::fromInt(2)) { o.state = 0; return m && !m->moving && d > Fixed::fromInt(4); }
        stopMoving(sim, e);
        const uint8_t owner = w.get<Owner>(e).player;
        if (!canPlace(sim, owner, uint16_t(o.param), o.point, e)) return true;
        if (rules_.costModel == CostModel::Upfront && !pay(sim, owner, upfrontCost(bt.cost), {})) return true; // goods are delivered later
        const BuildMode mode = t.buildMode;
        Entity s = placeStructure(sim, owner, uint16_t(o.param), o.point, e, false); // invalidates references
        if (mode == BuildMode::Morph) {
            Unit& bu = w.get<Unit>(e);
            bu.hp = Fixed{};
            bu.flags |= kUnitSilent;
            return true;
        }
        if (mode == BuildMode::Warp) {
            // step aside and carry on
            auto& f = w.get<Footprint>(s);
            Transform& tr = w.get<Transform>(e);
            int cx, cy; state(sim).grid.cellXY(tr.pos, cx, cy);
            if (cx >= f.x && cx < f.x + f.w && cy >= f.y && cy < f.y + f.h) tr.pos = freeSpotNear(sim, s, t.pathLayer);
            return true;
        }
        o.target = s;
        o.state = 2;
        auto& f = w.get<Footprint>(s);
        Transform& tr = w.get<Transform>(e);
        int cx, cy; state(sim).grid.cellXY(tr.pos, cx, cy);
        if (cx >= f.x && cx < f.x + f.w && cy >= f.y && cy < f.y + f.h) tr.pos = freeSpotNear(sim, s, t.pathLayer);
        return false;
    }
    // state 2: building (progress is applied by sysConstruction)
    if (!w.alive(o.target) || !w.has<Construction>(o.target)) return true;
    if (contactDistance(sim, e, o.target) > Fixed::fromRatio(8, 10)) {
        if (m && !m->moving) moveTo(sim, e, w.get<Transform>(o.target).pos, rules_.types[w.get<Unit>(o.target).type].radius + t.radius + Fixed::fromRatio(3, 10));
    } else if (m && m->moving) stopMoving(sim, e);
    return false;
}

bool RtsModule::runOrder(Simulation& sim, Entity e, Order& o) {
    World& w = sim.world();
    Unit& u = w.get<Unit>(e);
    const UnitType& t = rules_.types[u.type];
    auto* m = w.tryGet<Mobile>(e);
    auto* combat = w.tryGet<Combat>(e);

    auto rangeTo = [&](Entity target) {
        Fixed r = maxWeaponRange(t) + stat(sim, e, Stat::Range);
        Fixed adv = heightAdvantage(sim, w.get<Transform>(e).pos, w.get<Transform>(target).pos);
        if (adv.raw > 0 && !(t.layer & kLayerAir)) r += rules_.heights.rangePerLevel * adv;
        return r;
    };
    auto chase = [&](Entity target) -> bool { // true while the target is valid
        if (!w.alive(target) || !w.has<Unit>(target)) return false;
        Fixed range = rangeTo(target);
        if (contactDistance(sim, e, target) <= range) {
            if (m && m->moving) stopMoving(sim, e);
            return true;
        }
        if (!m || (u.flags & kUnitHold)) return true;
        Vec3 tp = w.get<Transform>(target).pos;
        // melee: head for a free slot around the target, walking around allies already fighting
        Vec3 slot;
        if (range < Fixed::fromInt(2) && (t.layer & kLayerGround) && !(rules_.types[w.get<Unit>(target).type].layer & kLayerAir) &&
            attackSlot(sim, e, target, range, slot)) {
            if (!m->moving || dist2D(m->goal, slot) > kHalf || (sim.tick() + e.index) % 20 == 0) moveToVia(sim, e, slot, Fixed::fromRatio(1, 5), true);
            return true;
        }
        if (!m->moving || dist2D(m->goal, tp) > kOne || (sim.tick() + e.index) % 20 == 0) moveTo(sim, e, tp, range);
        return true;
    };
    const Fixed spreadK = o.flow ? rules_.movement.flow.spread : Fixed::fromRatio(4, 10);
    auto spread = [&](int movers) { return Fixed::fromRatio(3, 10) + fxSqrt(Fixed::fromInt(std::max(1, movers))) * spreadK; };

    switch (o.type) {
    case OrderType::Move:
        if (o.state == 0) { if (o.flow) moveToFlow(sim, e, o.point, spread(o.param)); else moveTo(sim, e, o.point, spread(o.param)); o.state = 1; return !m; }
        return !m || !m->moving;
    case OrderType::Attack:
        if (!combat) return true;
        combat->target = o.target;
        combat->autoTarget = false;
        if (!chase(o.target)) { combat->target = kNullEntity; return true; }
        return false;
    case OrderType::AttackMove:
    case OrderType::Patrol:
        if (combat && w.alive(combat->target)) {
            if (chase(combat->target)) { o.state = 0; return false; }
            combat->target = kNullEntity;
        }
        if (o.state == 0) { if (o.flow) moveToFlow(sim, e, o.point, spread(o.param)); else moveTo(sim, e, o.point, spread(o.param)); o.state = 1; return false; }
        if (m && !m->moving) {
            if (o.type == OrderType::AttackMove) return true;
            std::swap(o.point, o.point2);
            o.state = 0;
        }
        return false;
    case OrderType::Gather:
        return runGather(sim, e, o);
    case OrderType::Board: return runBoard(sim, e, o);     // agent-mech
    case OrderType::Unload: return runUnload(sim, e, o);
    case OrderType::Follow: return runFollow(sim, e, o);
    case OrderType::Rearm: return runRearm(sim, e, o);
    case OrderType::Trade: return runTrade(sim, e, o);
    case OrderType::PickItem: return runPickItem(sim, e, o);
    case OrderType::Merge: return runMerge(sim, e, o);
    case OrderType::Return: {
        auto* cargo = w.tryGet<Cargo>(e);
        if (!cargo || cargo->res < 0 || cargo->amount.raw <= 0) return true;
        const uint8_t owner = w.get<Owner>(e).player;
        if (!w.alive(o.target) || !(rules_.types[w.get<Unit>(o.target).type].dropoffMask & (1u << cargo->res)) ||
            (w.get<Unit>(o.target).flags & kUnitUnderConstruction) || w.get<Owner>(o.target).player != owner) {
            o.target = findNearestDropoff(sim, owner, w.get<Transform>(e).pos, cargo->res);
            o.state = 0;
            if (!o.target.valid()) return true;
        }
        if (o.state == 0) {
            // goal radius from the short side: a 3x2 refinery must not count as reached 1.5 cells away from its long side
            const UnitType& dt = rules_.types[w.get<Unit>(o.target).type];
            Fixed near = dt.structure ? Fixed::fromInt(std::min(dt.footprintW, dt.footprintH)) / 2 : dt.radius;
            moveTo(sim, e, w.get<Transform>(o.target).pos, near + t.radius + Fixed::fromRatio(4, 10));
            o.state = 1;
        }
        if (contactDistance(sim, e, o.target) <= Fixed::fromRatio(6, 10)) {
            stopMoving(sim, e);
            addResource(sim, owner, uint16_t(cargo->res), cargo->amount * gatherMultFor(sim, owner, cargo->res));
            sim.emit({kEvGathered, owner, e, o.target, cargo->res, w.get<Transform>(e).pos});
            cargo->amount = Fixed{};
            Order g;
            g.type = OrderType::Gather;
            if (cargo->field) { g.target = kNullEntity; g.point = cargo->lastPos; g.param = cargo->res; }
            else {
                g.target = w.alive(cargo->lastNode) ? cargo->lastNode : kNullEntity;
                if (!g.target.valid()) g.target = findNearestResource(sim, owner, cargo->lastPos, cargo->res, Fixed::fromInt(14));
                if (!g.target.valid()) return true;
            }
            o = g;
            return false;
        }
        if (m && !m->moving) o.state = 0;
        return false;
    }
    case OrderType::Build:
        return runBuild(sim, e, o);
    case OrderType::Dig:
        return runDig(sim, e, o);
    case OrderType::ClaimCell:
        return runClaimCell(sim, e, o);
    case OrderType::Need:
        return runNeed(sim, e, o);
    case OrderType::Haul:
        return runHaul(sim, e, o);
    case OrderType::Repair: {
        if (!w.alive(o.target) || !w.has<Unit>(o.target)) return true;
        Unit& tu = w.get<Unit>(o.target);
        const UnitType& tt = rules_.types[tu.type];
        Fixed maxHp = stat(sim, o.target, Stat::MaxHp);
        if (tu.hp >= maxHp || w.has<Construction>(o.target)) return true;
        if (contactDistance(sim, e, o.target) > Fixed::fromRatio(8, 10)) {
            if (m && !m->moving) moveTo(sim, e, w.get<Transform>(o.target).pos, tt.radius + t.radius + Fixed::fromRatio(3, 10));
            return false;
        }
        if (m && m->moving) stopMoving(sim, e);
        // repairing costs a share of the build cost (Warcraft/StarCraft style): 30% of the cost for a full repair
        Fixed step = t.buildPower * sim.dt() / fxMax(tt.buildTime, kOne);
        const uint8_t owner = w.get<Owner>(e).player;
        std::vector<Amount> cost = tt.cost;
        for (auto& c : cost) c.value = c.value * step * Fixed::fromRatio(3, 10);
        if (!pay(sim, owner, cost, {}, false)) return true;
        tu.hp = fxMin(maxHp, tu.hp + maxHp * step);
        return false;
    }
    case OrderType::Cast: {
        if (o.param < 0 || size_t(o.param) >= t.abilities.size()) return true;
        const AbilityDef& a = t.abilities[size_t(o.param)];
        if (!canCast(sim, e, o.param)) return true;
        Vec3 at = o.point;
        if (a.target == TargetKind::Unit) {
            if (!w.alive(o.target) || !w.has<Unit>(o.target)) return true;
            at = w.get<Transform>(o.target).pos;
        }
        Fixed d = dist2D(w.get<Transform>(e).pos, at);
        if (d > a.range) {
            if (m && (!m->moving || (sim.tick() + e.index) % 20 == 0)) moveTo(sim, e, at, a.range - Fixed::fromRatio(1, 4));
            return m == nullptr;
        }
        stopMoving(sim, e);
        if (rules_.advancedEffects) {
            const AbilityDef& al = abilityFor(sim, e, o.param);
            if (al.castTime.raw > 0) {
                // windup: interrupted by stun / silence (canCast) or a new order
                if (o.state < 10) { o.state = 10; o.timer = al.castTime; sim.emit({kEvCastStart, w.get<Owner>(e).player, e, o.target, o.param, at}); return false; }
                o.timer -= sim.dt();
                if (o.timer.raw > 0) return false;
            }
        }
        castAbility(sim, e, o.param, o.target, at);
        return true;
    }
    case OrderType::Enter: {
        if (!w.alive(o.target)) return true;
        Transform& tr = w.get<Transform>(e);
        if (o.state == 3) {
            if (!runLaneWalk(sim, e, o)) return false;
            o.state = 1;
            if (m) stopMoving(sim, e);
        }
        if (o.state == 2 && m && !m->roadPath.empty()) {
            GameState& gs = state(sim);
            if (!m->moving) {
                Entity ne = gs.roads.nodes[m->roadPath.front()];
                if (ne == o.target) { m->roadPath.clear(); o.state = 0; }
                else {
                    Vec3 p = w.get<Transform>(ne).pos;
                    m->goal = p; m->goalRadius = Fixed::fromRatio(3, 10);
                    m->path = {p}; m->pathPos = 0; m->moving = true;
                    if (dist2D(tr.pos, p) < Fixed::fromRatio(4, 10)) m->roadPath.erase(m->roadPath.begin());
                }
            }
            if (!m->roadPath.empty()) {
                Vec3 p = w.get<Transform>(gs.roads.nodes[m->roadPath.front()]).pos;
                if (dist2D(tr.pos, p) < Fixed::fromRatio(4, 10)) { m->roadPath.erase(m->roadPath.begin()); m->moving = false; }
                return false;
            }
        }
        if (o.state == 0 && m) {
            Vec3 tp = w.get<Transform>(o.target).pos;
            m->goal = tp; m->goalRadius = Fixed{}; m->path = {tp}; m->pathPos = 0; m->moving = true;
            // close to the target (a crowd at the door) walk straight in instead of asking for a path every tick
            const bool near = rules_.originals && dist2D(tr.pos, tp) < Fixed::fromInt(4);
            if (!state(sim).roadsOnly && !near) moveTo(sim, e, tp, Fixed{});
            o.state = 1;
        }
        const bool crewing = rules_.originals && rules_.types[w.get<Unit>(o.target).type].crewed && !w.has<Garrison>(o.target);
        if (contactDistance(sim, e, o.target) > (crewing ? Fixed::fromRatio(6, 10) : Fixed::fromRatio(2, 10))) {
            if (m && !m->moving) o.state = 0;
            return false;
        }
        // arrived: man an empty vehicle (Z), reinforce, assault or capture
        if (rules_.originals && rules_.types[w.get<Unit>(o.target).type].crewed && !w.has<Garrison>(o.target)) { crewEnter(sim, e, o.target); return true; }
        if (rules_.advancedEffects) { enterGarrison(sim, e, o.target); return true; }
        auto* g = w.tryGet<Garrison>(o.target);
        const uint8_t me = w.get<Owner>(e).player;
        Owner& to = w.get<Owner>(o.target);
        const UnitType& tt = rules_.types[w.get<Unit>(o.target).type];
        if (g) {
            if (to.player == me || (to.player != 0 && to.player != kHostileNeutral && sim.allied(to.player, me))) {
                ++g->count;
            } else if (g->count > 0) {
                Fixed atk = rules_.originals ? stat(sim, e, Stat::Damage) / fxMax(stat(sim, o.target, Stat::Defense), Fixed::fromRatio(1, 10)) : kOne;
                g->damageAcc += atk / fxMax(tt.garrisonDefense, Fixed::fromRatio(1, 10));
                while (g->damageAcc >= kOne && g->count > 0) { --g->count; g->damageAcc -= kOne; }
                if (rules_.originals) assaultOriginals(sim, e, o.target);
            } else if (tt.capturable) {
                uint8_t old = to.player;
                to.player = me;
                g->count = 1; g->damageAcc = Fixed{}; g->growth = Fixed{}; g->pendingOut = 0;
                if (auto* p = w.tryGet<Production>(o.target)) p->queue.clear();
                sim.emit({kEvCaptured, me, o.target, e, old, w.get<Transform>(o.target).pos});
                if (rules_.originals) captureOriginals(sim, o.target, old, me);
            }
        }
        Unit& uu = w.get<Unit>(e);
        uu.hp = Fixed{};
        uu.flags |= kUnitSilent;
        return true;
    }
    }
    return true;
}

void RtsModule::sysOrders(Simulation& sim) {
    World& w = sim.world();
    std::vector<Entity> ents;
    w.each<Orders, Unit>([&](Entity e, Orders& ord, Unit& u) {
        if (!ord.queue.empty() && u.hp.raw > 0 && !(u.flags & (kUnitOffMap | kUnitPossessed))) ents.push_back(e);
    });
    // soft obstacles (standing ground units) and the melee slot claims of this tick
    MapGrid& g = state(sim).grid;
    g.clearSoft();
    slotClaims_.clear();
    w.each<Mobile, Unit, Transform>([&](Entity e, Mobile& m, Unit& u, Transform& tr) {
        if (m.moving || (u.flags & kUnitOffMap) || !(rules_.types[u.type].layer & kLayerGround)) return;
        int x, y; g.cellXY(tr.pos, x, y);
        if (g.inside(x, y)) g.markSoft(x, y);
        (void)e;
    });
    w.each<Combat>([&](Entity e, Combat& c) {
        if (c.slot >= 0 && w.alive(c.slotTarget) && sim.tick() < c.slotTick + 20) slotClaims_.push_back({c.slotTarget.packed(), c.slot, e.packed()});
    });
    // Orders may spawn entities (construction), so they run outside the iteration.
    for (Entity e : ents) {
        if (!w.alive(e)) continue;
        auto* b = w.tryGet<Buffs>(e);
        if (b && b->stunUntil > sim.tick()) continue;
        if (rules_.advancedEffects) {
            if (isStunned(sim, e)) continue;
            if (auto* as = w.tryGet<AbilityState>(e); as && as->busyUntil > sim.tick()) continue; // backswing
        }
        auto& q0 = w.get<Orders>(e).queue;
        if (q0.empty()) continue;
        Order o = q0.front();
        bool done = runOrder(sim, e, o);
        if (!w.alive(e)) continue;
        auto& q = w.get<Orders>(e).queue;
        if (q.empty()) continue;
        q.front() = o;
        if (done) {
            q.erase(q.begin());
            if (q.empty()) stopMoving(sim, e);
        }
    }
}

// ================================================================= movement

void RtsModule::sysMovement(Simulation& sim) {
    World& w = sim.world();
    GameState& gs = state(sim);
    MapGrid& g = gs.grid;
    const Fixed dt = sim.dt();

    gs.hash.reset(g.w, g.h, 4);
    w.each<Unit, Transform>([&](Entity e, Unit& u, Transform& t) { t.prevPos = t.pos; if (!(u.flags & kUnitOffMap)) gs.hash.insert(e, t.pos); });
    sysFlowFields(sim);

    if (gs.surface) { moveSphere(sim); return; }   // planets (RtsPlanets.cpp)

    // 1) desired movement along paths
    if (rules_.movement.rvo) moveAgents(sim);
    else w.each<Mobile, Unit, Transform>([&](Entity e, Mobile& m, Unit& u, Transform& tr) {
        if (!m.moving || (u.flags & (kUnitOffMap | kUnitPossessed))) return;
        auto* b = w.tryGet<Buffs>(e);
        if (b && b->stunUntil > sim.tick()) return;
        if (rules_.advancedEffects && (statusFlags(sim, e) & (kStStun | kStRoot))) return;
        const UnitType& t = rules_.types[u.type];
        if (m.goalRadius.raw > 0 && dist2D(tr.pos, m.goal) <= m.goalRadius) { m.moving = false; m.path.clear(); return; }
        Fixed step = stat(sim, e, Stat::Speed) * dt;
        if (rules_.roadNetwork.enabled && g.cellOf(tr.pos) >= 0 && (g.flags[size_t(g.cellOf(tr.pos))] & kCellRoad))
            step = step * rules_.roadNetwork.speed;
        const bool ignoreGrid = (t.layer & kLayerAir) || (gs.roadsOnly && t.entersGarrison); // road walkers pass through buildings
        if (g.hf && (t.layer & kLayerGround) && m.pathPos < m.path.size()) { // uphill slower, downhill faster
            Vec2 d = (m.path[m.pathPos] - tr.pos).xz();
            Fixed len = d.length();
            if (len.raw > 0) step = step * slopeSpeed(sim, tr.pos, d / len);
        }
        while (m.pathPos < m.path.size() && step.raw > 0) {
            Vec3 wp = m.path[m.pathPos];
            Vec3 d = flat(wp - tr.pos);
            Fixed len = d.length();
            Vec3 next = len <= step ? Vec3{wp.x, tr.pos.y, wp.z} : tr.pos + d * (step / len);
            int cx0, cy0, cx, cy; g.cellXY(tr.pos, cx0, cy0); g.cellXY(next, cx, cy);
            if (!ignoreGrid && (cx != cx0 || cy != cy0) && g.passable(cx0, cy0, t.pathLayer) && !g.canStep(cx0, cy0, cx, cy, t.pathLayer)) {
                m.stuck += dt;
                break;
            }
            if (len.raw > 0) u.facing = d / len;
            if (len <= step) { tr.pos = next; step -= len; ++m.pathPos; }
            else { tr.pos = next; step = Fixed{}; }
        }
        if (m.pathPos >= m.path.size()) { m.moving = false; m.path.clear(); }
        if (m.stuck > kOne) { // blocked by a new structure: re-path
            m.stuck = Fixed{};
            m.flow = false;
            g.findPath(tr.pos, m.goal, t.pathLayer, m.path);
            m.pathPos = 0;
        }
    });

    if (rules_.dungeon) movePossessed(sim);

    // 2) separation between overlapping mobile units on the same layer. Bodies are move.radius; who yields
    //    depends on mass and activity: fighting / holding / working units are anchored (barely pushed),
    //    moving units push idle ones aside. A small dead zone and a per-tick cap prevent jitter.
    struct Push { Entity e; Vec3 d; };
    std::vector<Push> pushes;
    std::vector<Fixed> weight;
    std::vector<uint8_t> anchored;
    w.each<Mobile, Unit, Transform>([&](Entity e, Mobile& m, Unit& u, Transform&) {
        if (weight.size() <= e.index) { weight.resize(e.index + 1, kOne); anchored.resize(e.index + 1, 0); }
        bool anchor = (u.flags & kUnitHold) != 0;
        if (auto* ord = w.tryGet<Orders>(e); ord && !ord->queue.empty()) {
            const Order& o = ord->queue.front();
            if (o.state == 2 && (o.type == OrderType::Gather || o.type == OrderType::Build || o.type == OrderType::Dig ||
                                 o.type == OrderType::ClaimCell || o.type == OrderType::Need)) anchor = true;
        }
        if (auto* c = w.tryGet<Combat>(e); c && !m.moving && w.alive(c->target)) anchor = true; // standing and fighting
        if (rules_.lanes.enabled && w.has<LaneWalker>(e)) anchor = true;
        const UnitType& t = rules_.types[u.type];
        weight[e.index] = t.mass * (anchor ? 8 : m.moving ? 2 : 1);
        anchored[e.index] = anchor;
    });
    const Fixed deadZone = Fixed::fromRatio(3, 100);
    if (!genericSeparation) mechSeparation(sim, weight, anchored, deadZone, pushes.size(), [&](Entity e, Vec3 d) { pushes.push_back({e, d}); });   // agent-mech BA: same pushes as the loop below, faster in crowds
    else w.each<Mobile, Unit, Transform>([&](Entity e, Mobile& m, Unit& u, Transform& tr) {
        if (u.flags & kUnitOffMap) return;
        if (rules_.lanes.enabled && w.has<LaneWalker>(e)) return; // lane walkers keep strictly to their lane
        const UnitType& t = rules_.types[u.type];
        Vec3 acc{};
        bool any = false;
        (void)m;
        gs.hash.query(tr.pos, t.radius * 2 + kOne, [&](Entity o) {
            if (o == e) return;
            auto* ou = w.tryGet<Unit>(o);
            if (!ou || !w.has<Mobile>(o)) return;
            const UnitType& ot = rules_.types[ou->type];
            if (!(ot.layer & t.layer) && !((ot.layer | t.layer) == (kLayerGround | kLayerSandOnly))) return;
            Vec3 d = flat(tr.pos - w.get<Transform>(o).pos);
            Fixed len = d.length();
            if (w.get<Unit>(o).flags & kUnitOffMap) return;
            Fixed overlap = t.radius + ot.radius - len - deadZone;
            if (overlap.raw <= 0) return;
            if (len.raw == 0) { // exact overlap: deterministic direction from entity indices
                d = (e.index < o.index) ? Vec3{kOne, Fixed{}, Fixed{}} : Vec3{-kOne, Fixed{}, Fixed{}};
                len = kOne;
            }
            Fixed wo = o.index < weight.size() ? weight[o.index] : kOne, we = weight[e.index];
            Fixed share = wo / fxMax(we + wo, Fixed::fromRatio(1, 100));   // the lighter / less busy one yields
            acc += d * (overlap * share / len);
            any = true;
        });
        if (any) {
            Fixed len = acc.length();
            Fixed maxPush = anchored[e.index] ? Fixed::fromRatio(1, 100) : t.speed * dt / 2 + Fixed::fromRatio(1, 50);
            if (len > maxPush) acc = acc * (maxPush / len);
            pushes.push_back({e, acc});
        }
    });
    for (auto& p : pushes) {
        Transform& tr = w.get<Transform>(p.e);
        const UnitType& t = rules_.types[w.get<Unit>(p.e).type];
        Vec3 np = tr.pos + p.d;
        int cx0, cy0, cx, cy; g.cellXY(tr.pos, cx0, cy0); g.cellXY(np, cx, cy);
        bool ok = (t.layer & kLayerAir) || (gs.roadsOnly && t.entersGarrison) ||
                  ((cx == cx0 && cy == cy0) ? g.passable(cx, cy, t.pathLayer) || !g.passable(cx0, cy0, t.pathLayer) : g.canStep(cx0, cy0, cx, cy, t.pathLayer));
        if (ok) tr.pos = np;
    }
    // heightfield: everything stands on the surface (air units too: their flight height is presentation)
    if (g.hf)
        w.each<Mobile, Transform>([&](Entity, Mobile&, Transform& tr) {
            Fixed y = g.surfaceHeight(tr.pos.x, tr.pos.z);
            tr.pos.y = y;
        });
}

} // namespace rts::game
