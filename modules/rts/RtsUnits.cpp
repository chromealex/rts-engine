// Orders and movement: move/attack-move/patrol, gathering from nodes and fields (with harvesters,
// reclaim, upkeep), construction in walk/warp/morph modes, repair, entering garrisons, casting.
#include "RtsInternal.h"
#include <algorithm>

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
    if (auto* m = sim.world().tryGet<Mobile>(e)) { m->moving = false; m->path.clear(); m->pathPos = 0; m->stuck = Fixed{}; }
}

void RtsModule::moveTo(Simulation& sim, Entity e, Vec3 goal, Fixed radius) {
    World& w = sim.world();
    auto* m = w.tryGet<Mobile>(e);
    if (!m) return;
    const UnitType& t = rules_.types[w.get<Unit>(e).type];
    m->goal = goal;
    m->goalRadius = radius;
    m->stuck = Fixed{};
    m->pathPos = 0;
    state(sim).grid.findPath(w.get<Transform>(e).pos, goal, t.layer, m->path);
    m->moving = !m->path.empty();
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
        int cx = gs.grid.cellX(o.point2.x), cy = gs.grid.cellY(o.point2.z);
        if (!gs.grid.inside(cx, cy)) return true;
        size_t ci = size_t(gs.grid.index(cx, cy));
        if (cargo->res != res) { cargo->amount = Fixed{}; cargo->res = int16_t(res); }
        if (gs.grid.fieldRes[ci] != res || gs.grid.fieldAmt[ci].raw <= 0) {
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
        if (contactDistance(sim, e, o.target) <= Fixed::fromRatio(6, 10)) { stopMoving(sim, e); o.state = 2; o.timer = gd->time / fxMax(stat(sim, e, Stat::GatherRate), Fixed::fromRatio(1, 10)); }
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
            addResource(sim, owner, uint16_t(res), take * gs.players[owner].gatherMult);
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
        if (rules_.costModel == CostModel::Upfront && !pay(sim, owner, bt.cost, {})) return true;
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
            int cx = int(tr.pos.x.floorInt()), cy = int(tr.pos.z.floorInt());
            if (cx >= f.x && cx < f.x + f.w && cy >= f.y && cy < f.y + f.h) tr.pos = freeSpotNear(sim, s, t.layer);
            return true;
        }
        o.target = s;
        o.state = 2;
        auto& f = w.get<Footprint>(s);
        Transform& tr = w.get<Transform>(e);
        int cx = int(tr.pos.x.floorInt()), cy = int(tr.pos.z.floorInt());
        if (cx >= f.x && cx < f.x + f.w && cy >= f.y && cy < f.y + f.h) tr.pos = freeSpotNear(sim, s, t.layer);
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
        int adv = levelAt(sim, w.get<Transform>(e).pos) - levelAt(sim, w.get<Transform>(target).pos);
        if (adv > 0 && !(t.layer & kLayerAir)) r += rules_.heights.rangePerLevel * adv;
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
        if (!m->moving || dist2D(m->goal, tp) > kOne || (sim.tick() + e.index) % 20 == 0) moveTo(sim, e, tp, range);
        return true;
    };
    auto spread = [&](int movers) { return Fixed::fromRatio(3, 10) + fxSqrt(Fixed::fromInt(std::max(1, movers))) * Fixed::fromRatio(4, 10); };

    switch (o.type) {
    case OrderType::Move:
        if (o.state == 0) { moveTo(sim, e, o.point, spread(o.param)); o.state = 1; return !m; }
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
        if (o.state == 0) { moveTo(sim, e, o.point, spread(o.param)); o.state = 1; return false; }
        if (m && !m->moving) {
            if (o.type == OrderType::AttackMove) return true;
            std::swap(o.point, o.point2);
            o.state = 0;
        }
        return false;
    case OrderType::Gather:
        return runGather(sim, e, o);
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
            moveTo(sim, e, w.get<Transform>(o.target).pos, rules_.types[w.get<Unit>(o.target).type].radius + t.radius + Fixed::fromRatio(4, 10));
            o.state = 1;
        }
        if (contactDistance(sim, e, o.target) <= Fixed::fromRatio(6, 10)) {
            stopMoving(sim, e);
            addResource(sim, owner, uint16_t(cargo->res), cargo->amount * state(sim).players[owner].gatherMult);
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
        castAbility(sim, e, o.param, o.target, at);
        return true;
    }
    case OrderType::Enter: {
        if (!w.alive(o.target)) return true;
        Transform& tr = w.get<Transform>(e);
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
            if (!state(sim).roadsOnly) moveTo(sim, e, tp, Fixed{});
            o.state = 1;
        }
        if (contactDistance(sim, e, o.target) > Fixed::fromRatio(2, 10)) {
            if (m && !m->moving) o.state = 0;
            return false;
        }
        // arrived: reinforce, assault or capture
        auto* g = w.tryGet<Garrison>(o.target);
        const uint8_t me = w.get<Owner>(e).player;
        Owner& to = w.get<Owner>(o.target);
        const UnitType& tt = rules_.types[w.get<Unit>(o.target).type];
        if (g) {
            if (to.player == me || (to.player != 0 && to.player != kHostileNeutral && sim.allied(to.player, me))) {
                ++g->count;
            } else if (g->count > 0) {
                g->damageAcc += kOne / fxMax(tt.garrisonDefense, Fixed::fromRatio(1, 10));
                while (g->damageAcc >= kOne && g->count > 0) { --g->count; g->damageAcc -= kOne; }
            } else if (tt.capturable) {
                uint8_t old = to.player;
                to.player = me;
                g->count = 1; g->damageAcc = Fixed{}; g->growth = Fixed{}; g->pendingOut = 0;
                if (auto* p = w.tryGet<Production>(o.target)) p->queue.clear();
                sim.emit({kEvCaptured, me, o.target, e, old, w.get<Transform>(o.target).pos});
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
        if (!ord.queue.empty() && u.hp.raw > 0 && !(u.flags & (kUnitInHand | kUnitPossessed))) ents.push_back(e);
    });
    // Orders may spawn entities (construction), so they run outside the iteration.
    for (Entity e : ents) {
        if (!w.alive(e)) continue;
        auto* b = w.tryGet<Buffs>(e);
        if (b && b->stunUntil > sim.tick()) continue;
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
    w.each<Unit, Transform>([&](Entity e, Unit& u, Transform& t) { t.prevPos = t.pos; if (!(u.flags & kUnitInHand)) gs.hash.insert(e, t.pos); });

    // 1) desired movement along paths
    w.each<Mobile, Unit, Transform>([&](Entity e, Mobile& m, Unit& u, Transform& tr) {
        if (!m.moving || (u.flags & (kUnitInHand | kUnitPossessed))) return;
        auto* b = w.tryGet<Buffs>(e);
        if (b && b->stunUntil > sim.tick()) return;
        const UnitType& t = rules_.types[u.type];
        if (m.goalRadius.raw > 0 && dist2D(tr.pos, m.goal) <= m.goalRadius) { m.moving = false; m.path.clear(); return; }
        Fixed step = stat(sim, e, Stat::Speed) * dt;
        const bool ignoreGrid = (t.layer & kLayerAir) || (gs.roadsOnly && t.entersGarrison); // road walkers pass through buildings
        while (m.pathPos < m.path.size() && step.raw > 0) {
            Vec3 wp = m.path[m.pathPos];
            Vec3 d = flat(wp - tr.pos);
            Fixed len = d.length();
            Vec3 next = len <= step ? Vec3{wp.x, tr.pos.y, wp.z} : tr.pos + d * (step / len);
            int cx0 = g.cellX(tr.pos.x), cy0 = g.cellY(tr.pos.z), cx = g.cellX(next.x), cy = g.cellY(next.z);
            if (!ignoreGrid && (cx != cx0 || cy != cy0) && g.passable(cx0, cy0, t.layer) && !g.canStep(cx0, cy0, cx, cy, t.layer)) {
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
            g.findPath(tr.pos, m.goal, t.layer, m.path);
            m.pathPos = 0;
        }
    });

    if (rules_.dungeon) movePossessed(sim);

    // 2) separation between overlapping mobile units on the same layer
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
            if (!ou || !w.has<Mobile>(o)) return;
            const UnitType& ot = rules_.types[ou->type];
            if (!(ot.layer & t.layer) && !((ot.layer | t.layer) == (kLayerGround | kLayerSandOnly))) return;
            Vec3 d = flat(tr.pos - w.get<Transform>(o).pos);
            Fixed len = d.length();
            Fixed overlap = t.radius + ot.radius - len;
            if (overlap.raw <= 0) return;
            if (len.raw == 0) { // exact overlap: deterministic direction from entity indices
                d = (e.index < o.index) ? Vec3{kOne, Fixed{}, Fixed{}} : Vec3{-kOne, Fixed{}, Fixed{}};
                len = kOne;
            }
            Fixed share = m.moving ? kHalf : (w.get<Mobile>(o).moving ? Fixed::fromRatio(3, 4) : kHalf);
            acc += d * (overlap * share / len);
            any = true;
        });
        if (any) {
            Fixed len = acc.length(), maxPush = t.speed * dt + Fixed::fromRatio(1, 20);
            if (len > maxPush) acc = acc * (maxPush / len);
            pushes.push_back({e, acc});
        }
    });
    for (auto& p : pushes) {
        Transform& tr = w.get<Transform>(p.e);
        const UnitType& t = rules_.types[w.get<Unit>(p.e).type];
        Vec3 np = tr.pos + p.d;
        int cx0 = g.cellX(tr.pos.x), cy0 = g.cellY(tr.pos.z), cx = g.cellX(np.x), cy = g.cellY(np.z);
        bool ok = (t.layer & kLayerAir) || (gs.roadsOnly && t.entersGarrison) ||
                  ((cx == cx0 && cy == cy0) ? g.passable(cx, cy, t.layer) || !g.passable(cx0, cy0, t.layer) : g.canStep(cx0, cy0, cx, cy, t.layer));
        if (ok) tr.pos = np;
    }
}

} // namespace rts::game
