// agent-mech mechanics, part 1: stances, retreat, transports and garrisoned infantry.
// - Stance: aggressive (default), defensive (chases at most rules.stances.defendRadius from where it
//   stood), hold (never moves on its own), passive (never attacks on its own).
// - Retreat: units run to the nearest own retreatPoint, faster and taking less damage, without fighting.
// - Transport: passengers board (Board order), ride off the map (kUnitInCargo) at the carrier's position,
//   the first `fireFrom` of them shoot from inside, `damagePerCargo` strengthens the carrier's own
//   weapons, they unload one by one, and share `killOnDeath` of the carrier's fate.
#include "RtsInternal.h"
#include <algorithm>

namespace rts::game {
using namespace detail;

namespace {
bool own(Simulation& sim, Entity e, uint8_t player) {
    World& w = sim.world();
    return w.alive(e) && w.has<Unit>(e) && w.has<Owner>(e) && w.get<Owner>(e).player == player;
}
} // namespace

// ================================================================= commands

bool RtsModule::applyMechCommand(Simulation& sim, const Command& cmd) {
    World& w = sim.world();
    switch (cmd.type) {
    case kCmdStance: {
        state(sim).mechActive = true;
        const uint8_t mode = uint8_t(std::clamp<int64_t>(cmd.param, 0, 3));
        std::vector<Entity> add;
        for (Entity e : cmd.units) {
            if (!own(sim, e, cmd.player) || rules_.types[w.get<Unit>(e).type].structure || (w.get<Unit>(e).flags & kUnitOffMap)) continue;
            if (!w.has<Stance>(e)) add.push_back(e);
        }
        for (Entity e : add) w.add<Stance>(e, Stance{});
        for (Entity e : cmd.units) {
            auto* st = w.alive(e) ? w.tryGet<Stance>(e) : nullptr;
            if (!st || !own(sim, e, cmd.player)) continue;
            st->mode = mode;
            st->home = w.get<Transform>(e).pos;
            st->homeSet = true;
            Unit& u = w.get<Unit>(e);
            if (mode == uint8_t(StanceMode::Hold)) { u.flags |= kUnitHold; stopMoving(sim, e); }
            else u.flags &= uint8_t(~kUnitHold);
            auto* ord = w.tryGet<Orders>(e);
            if (mode == uint8_t(StanceMode::Passive)) if (auto* c = w.tryGet<Combat>(e); c && c->autoTarget) {
                c->target = kNullEntity;
                if (!ord || ord->queue.empty()) stopMoving(sim, e);   // was chasing on its own
            }
        }
        return true;
    }
    case kCmdRetreat: {
        if (!rules_.mech.retreat) return true;
        state(sim).mechActive = true;
        std::vector<std::pair<Entity, Entity>> go;
        for (Entity e : cmd.units) {
            if (!own(sim, e, cmd.player) || !w.has<Mobile>(e) || (w.get<Unit>(e).flags & kUnitOffMap)) continue;
            Entity to = nearestRetreatPoint(sim, cmd.player, w.get<Transform>(e).pos);
            if (to.valid()) go.push_back({e, to});
        }
        for (auto& [e, to] : go) {
            Order o;
            o.type = OrderType::Move;
            o.point = flat(w.get<Transform>(to).pos);
            o.param = int32_t(go.size());
            issue(sim, e, o, false);
            if (!w.has<Retreating>(e)) w.add<Retreating>(e, Retreating{});
            w.get<Retreating>(e) = Retreating{to, o.point};
        }
        return true;
    }
    case kCmdLoad: {
        if (!rules_.mech.transports) return true;
        std::vector<Entity> passengers = cmd.units;
        Entity carrier = cmd.target;
        // transports selected, a unit clicked: that unit boards the first transport with room
        if (!passengers.empty() && own(sim, passengers[0], cmd.player) && rules_.types[w.get<Unit>(passengers[0]).type].mech.transport.slots > 0 &&
            own(sim, cmd.target, cmd.player) && rules_.types[w.get<Unit>(cmd.target).type].mech.transport.slots == 0) {
            Entity p = cmd.target;
            carrier = kNullEntity;
            for (Entity c : passengers) if (own(sim, c, cmd.player) && accepts(sim, c, p)) { carrier = c; break; }
            passengers = {p};
        }
        if (!own(sim, carrier, cmd.player)) return true;
        for (Entity p : passengers) {
            if (p == carrier || !own(sim, p, cmd.player) || !accepts(sim, carrier, p)) continue;
            Order o;
            o.type = OrderType::Board;
            o.target = carrier;
            issue(sim, p, o, cmd.flags & kCmdQueue);
        }
        return true;
    }
    case kCmdUnload: {
        for (Entity c : cmd.units) {
            if (!own(sim, c, cmd.player)) continue;
            auto* tp = w.tryGet<Transport>(c);
            if (!tp || tp->cargo.empty()) continue;
            if (cmd.param == 1 && w.has<Mobile>(c)) {
                Order o;
                o.type = OrderType::Unload;
                o.point = flat(cmd.point);
                issue(sim, c, o, cmd.flags & kCmdQueue);
            } else {
                tp->unloading = true;
                tp->nextUnload = sim.tick();
            }
        }
        return true;
    }
    case kCmdSmart: {
        // right click on an own transport with room: board it
        if (!rules_.mech.transports || !own(sim, cmd.target, cmd.player) || cmd.units.empty()) return false;
        if (rules_.types[w.get<Unit>(cmd.target).type].mech.transport.slots == 0) return false;
        bool any = false;
        for (Entity p : cmd.units) if (p != cmd.target && own(sim, p, cmd.player) && accepts(sim, cmd.target, p)) any = true;
        if (!any) return false;
        Command c = cmd;
        c.type = kCmdLoad;
        return applyMechCommand(sim, c);
    }
    default: return applyMechCommand2(sim, cmd);   // RtsMechUnits.cpp
    }
}

// ================================================================= transports

bool RtsModule::accepts(Simulation& sim, Entity carrier, Entity p) const {
    World& w = sim.world();
    if (!w.alive(carrier) || !w.alive(p) || !w.has<Unit>(carrier) || !w.has<Unit>(p)) return false;
    const Unit& cu = w.get<Unit>(carrier);
    const Unit& pu = w.get<Unit>(p);
    const TransportDef& d = rules_.types[cu.type].mech.transport;
    const UnitType& pt = rules_.types[pu.type];
    if (d.slots == 0 || cu.hp.raw <= 0 || pu.hp.raw <= 0 || (cu.flags & (kUnitUnderConstruction | kUnitOffMap)) || (pu.flags & kUnitOffMap)) return false;
    if (pt.structure || !w.has<Mobile>(p) || pt.mech.transport.slots > 0) return false;
    if (w.get<Owner>(carrier).player != w.get<Owner>(p).player) return false;
    if (!(pt.layer & d.acceptLayers)) return false;
    if (d.acceptTags && !(pt.tags & d.acceptTags)) return false;
    int used = 0;
    if (auto* tp = w.tryGet<Transport>(carrier)) used = tp->used;
    return used + pt.mech.cargoSize <= d.slots;
}

namespace {
void refreshShooters(World& w, const Rules& R, Entity carrier, Transport& tp) {
    const TransportDef& d = R.types[w.get<Unit>(carrier).type].mech.transport;
    int shooters = d.weaponFromCargo ? std::max(1, d.fireFrom) : d.fireFrom;
    for (Entity p : tp.cargo) {
        auto* b = w.tryGet<Boarded>(p);
        if (!b) continue;
        b->fires = shooters > 0 && !R.types[w.get<Unit>(p).type].weapons.empty();
        if (b->fires) --shooters;
    }
}
} // namespace

bool RtsModule::board(Simulation& sim, Entity p, Entity carrier) {
    World& w = sim.world();
    if (!accepts(sim, carrier, p)) return false;
    if (!w.has<Transport>(carrier)) w.add<Transport>(carrier, Transport{});
    if (!w.has<Boarded>(p)) w.add<Boarded>(p, Boarded{});
    w.get<Boarded>(p) = Boarded{carrier, false};
    stopMoving(sim, p);
    if (auto* ord = w.tryGet<Orders>(p)) ord->queue.clear();
    if (auto* c = w.tryGet<Combat>(p)) c->target = kNullEntity;
    Unit& u = w.get<Unit>(p);
    u.flags = uint8_t((u.flags | kUnitInCargo) & ~kUnitHold);
    Transport& tp = w.get<Transport>(carrier);
    tp.cargo.push_back(p);
    tp.used += rules_.types[u.type].mech.cargoSize;
    refreshShooters(w, rules_, carrier, tp);
    Transform& pt = w.get<Transform>(p);
    pt.pos = pt.prevPos = w.get<Transform>(carrier).pos;
    sim.emit({kEvLoaded, w.get<Owner>(p).player, p, carrier, int64_t(tp.cargo.size()), pt.pos});
    return true;
}

void RtsModule::unloadOne(Simulation& sim, Entity carrier, size_t index) {
    World& w = sim.world();
    Transport& tp = w.get<Transport>(carrier);
    if (index >= tp.cargo.size()) return;
    Entity p = tp.cargo[index];
    tp.cargo.erase(tp.cargo.begin() + ptrdiff_t(index));
    if (!w.alive(p) || !w.has<Unit>(p)) { refreshShooters(w, rules_, carrier, tp); return; }
    Unit& u = w.get<Unit>(p);
    const UnitType& t = rules_.types[u.type];
    tp.used = std::max(0, tp.used - t.mech.cargoSize);
    u.flags &= uint8_t(~kUnitInCargo);
    if (w.has<Boarded>(p)) w.remove<Boarded>(p);
    Vec3 at;
    if (rules_.types[w.get<Unit>(carrier).type].structure) at = freeSpotNear(sim, carrier, t.layer);
    else {
        // a ring around the carrier, one spot per passenger already out (deterministic)
        static const int dx[8] = {1, 0, -1, 0, 1, -1, -1, 1}, dz[8] = {0, 1, 0, -1, 1, 1, -1, -1};
        const int k = int(tp.cargo.size()) % 8;
        Fixed r = rules_.types[w.get<Unit>(carrier).type].radius + t.radius + Fixed::fromRatio(1, 5);
        at = w.get<Transform>(carrier).pos + Vec3{r * dx[k], Fixed{}, r * dz[k]};
        at = flat(at);
    }
    Transform& pt = w.get<Transform>(p);
    pt.pos = pt.prevPos = at;
    refreshShooters(w, rules_, carrier, tp);
    sim.emit({kEvUnloaded, w.get<Owner>(p).player, p, carrier, int64_t(tp.cargo.size()), at});
}

bool RtsModule::runBoard(Simulation& sim, Entity e, Order& o) {
    World& w = sim.world();
    if (!accepts(sim, o.target, e)) return true;
    const Fixed range = rules_.types[w.get<Unit>(o.target).type].mech.transport.loadRange;
    if (contactDistance(sim, e, o.target) <= range) { board(sim, e, o.target); return true; }
    auto* m = w.tryGet<Mobile>(e);
    if (!m) return true;
    Vec3 tp = w.get<Transform>(o.target).pos;
    if (!m->moving || dist2D(m->goal, tp) > kOne || (sim.tick() + e.index) % 20 == 0) moveTo(sim, e, tp, range);
    return false;
}

bool RtsModule::runUnload(Simulation& sim, Entity e, Order& o) {
    World& w = sim.world();
    auto* tp = w.tryGet<Transport>(e);
    if (!tp || tp->cargo.empty()) return true;
    auto* m = w.tryGet<Mobile>(e);
    if (o.state == 0) { if (m) moveTo(sim, e, o.point, Fixed::fromRatio(1, 2)); o.state = 1; return false; }
    if (m && m->moving) return false;
    tp->unloading = true;
    tp->nextUnload = sim.tick();
    return true;
}

void RtsModule::mechOnDeath(Simulation& sim, Entity e) {
    World& w = sim.world();
    mechVeteranKill(sim, e);
    if (auto* b = w.tryGet<Boarded>(e)) {   // a passenger died inside
        if (auto* tp = w.alive(b->carrier) ? w.tryGet<Transport>(b->carrier) : nullptr) {
            auto it = std::find(tp->cargo.begin(), tp->cargo.end(), e);
            if (it != tp->cargo.end()) {
                tp->cargo.erase(it);
                tp->used = std::max(0, tp->used - rules_.types[w.get<Unit>(e).type].mech.cargoSize);
                refreshShooters(w, rules_, b->carrier, *tp);
            }
        }
        return;
    }
    auto* tp = w.tryGet<Transport>(e);
    if (!tp || tp->cargo.empty()) return;
    const TransportDef& d = rules_.types[w.get<Unit>(e).type].mech.transport;
    const int n = int(tp->cargo.size());
    const int kill = std::clamp(int((d.killOnDeath * int64_t(n)).floorInt()), 0, n);
    std::vector<Entity> victims(tp->cargo.begin(), tp->cargo.begin() + kill);
    for (Entity p : victims) {
        if (!w.alive(p)) continue;
        w.get<Unit>(p).hp = Fixed{};   // dies with the carrier (next death pass, at the carrier's position)
    }
    tp->cargo.erase(tp->cargo.begin(), tp->cargo.begin() + kill);
    for (Entity p : victims) if (w.alive(p) && w.has<Boarded>(p)) w.get<Boarded>(p).fires = false;
    while (!tp->cargo.empty()) unloadOne(sim, e, 0);
}

// ================================================================= retreat / stances

Entity RtsModule::nearestRetreatPoint(Simulation& sim, uint8_t owner, Vec3 from) const {
    World& w = sim.world();
    Entity best[3];
    Fixed bd[3];
    w.each<Unit, Owner, Transform>([&](Entity e, Unit& u, Owner& o, Transform& t) {
        if (o.player != owner || u.hp.raw <= 0 || (u.flags & kUnitUnderConstruction)) return;
        const UnitType& ut = rules_.types[u.type];
        int rank = ut.mech.retreatPoint ? 0 : ut.key ? 1 : (ut.structure && !ut.produces.empty()) ? 2 : -1;
        if (rank < 0) return;
        Fixed d = dist2D(t.pos, from);
        if (!best[rank].valid() || d < bd[rank]) { best[rank] = e; bd[rank] = d; }
    });
    for (auto& b : best) if (b.valid()) return b;
    return kNullEntity;
}

bool RtsModule::mechCanAcquire(Simulation& sim, Entity e) const {
    World& w = sim.world();
    const Unit& u = w.get<Unit>(e);
    if (u.flags & kUnitInCargo) { auto* b = w.tryGet<Boarded>(e); return b && b->fires; }
    if (auto* st = w.tryGet<Stance>(e); st && st->mode == uint8_t(StanceMode::Passive)) return false;
    if (w.has<Retreating>(e)) return false;
    return true;
}

bool RtsModule::mechMayChase(Simulation& sim, Entity e, Vec3 targetPos) {
    World& w = sim.world();
    auto* st = w.tryGet<Stance>(e);
    if (!st || st->mode != uint8_t(StanceMode::Defensive) || !st->homeSet) return true;
    if (dist2D(targetPos, st->home) <= rules_.mech.defendRadius) return true;
    if (dist2D(w.get<Transform>(e).pos, st->home) > kOne) moveTo(sim, e, st->home, kHalf);
    return false;
}

void RtsModule::mechStatMod(Simulation& sim, Entity e, Stat s, Fixed& add, Fixed& mul) const {
    World& w = sim.world();
    if (s == Stat::Speed && w.has<Retreating>(e)) mul = mul * rules_.mech.retreatSpeed;
    if (s == Stat::Damage) {
        if (auto* tp = w.tryGet<Transport>(e)) {
            const Fixed per = rules_.types[w.get<Unit>(e).type].mech.transport.damagePerCargo;
            if (per.raw > 0) mul = mul * (kOne + per * int64_t(tp->cargo.size()));
        }
    }
    if (auto* v = w.tryGet<Veteran>(e); v && v->level > 1) {
        const MechType& mt = rules_.types[w.get<Unit>(e).type].mech;
        const VeterancyTable& tab = mt.veterancy == 1 ? mt.vetTable : rules_.mech.vet;
        for (auto& pl : tab.perLevel)
            if (pl.stat == s) { add += pl.add * (v->level - 1); for (int k = 1; k < v->level; ++k) mul = mul * pl.mul; }
    }
}

Fixed RtsModule::mechDamageTaken(Simulation& sim, Entity e) const {
    return sim.world().has<Retreating>(e) ? rules_.mech.retreatDamageTaken : kOne;
}

// ================================================================= system

void RtsModule::sysMech(Simulation& sim) {
    World& w = sim.world();
    mechTick(sim);   // RtsMechUnits.cpp: veterans, toggles, ...
    std::vector<Entity> addStance, dropRetreat;
    w.each<Unit, Transform>([&](Entity e, Unit& u, Transform& tr) {
        if (u.hp.raw <= 0) return;
        const UnitType& t = rules_.types[u.type];
        auto* st = w.tryGet<Stance>(e);
        if (!st && t.mech.stanceSet && !t.structure) { addStance.push_back(e); return; }
        auto* ord = w.tryGet<Orders>(e);
        const bool idle = !ord || ord->queue.empty();
        if (st && !(u.flags & kUnitOffMap)) {
            if (st->mode == uint8_t(StanceMode::Hold) && idle) u.flags |= kUnitHold;
            if (st->mode == uint8_t(StanceMode::Defensive) && idle) {
                auto* m = w.tryGet<Mobile>(e);
                auto* c = w.tryGet<Combat>(e);
                if ((!m || !m->moving) && (!c || !w.alive(c->target))) { st->home = tr.pos; st->homeSet = true; }
            }
        }
        if (auto* rt = w.tryGet<Retreating>(e)) {
            if (idle || ord->queue.front().type != OrderType::Move || ord->queue.front().point != rt->point) dropRetreat.push_back(e);
            else if (auto* c = w.tryGet<Combat>(e)) c->target = kNullEntity;
        }
    });
    for (Entity e : addStance) {
        Stance s;
        s.mode = uint8_t(rules_.types[w.get<Unit>(e).type].mech.stance);
        s.home = w.get<Transform>(e).pos;
        s.homeSet = true;
        w.add<Stance>(e, s);
    }
    for (Entity e : dropRetreat) w.remove<Retreating>(e);

    if (!rules_.mech.transports) return;
    struct Job { Entity carrier; };
    std::vector<Entity> unloadNow;
    const Fixed dt = sim.dt();
    w.each<Transport, Unit, Transform>([&](Entity c, Transport& tp, Unit& u, Transform& tr) {
        tp.cargo.erase(std::remove_if(tp.cargo.begin(), tp.cargo.end(), [&](Entity p) { return !w.alive(p) || !w.has<Unit>(p); }), tp.cargo.end());
        const TransportDef& d = rules_.types[u.type].mech.transport;
        int used = 0;
        for (Entity p : tp.cargo) {
            Unit& pu = w.get<Unit>(p);
            used += rules_.types[pu.type].mech.cargoSize;
            Transform& pt = w.get<Transform>(p);
            pt.prevPos = pt.pos;
            pt.pos = tr.pos;
            if (d.heal.raw > 0 && pu.hp.raw > 0) pu.hp = fxMin(stat(sim, p, Stat::MaxHp), pu.hp + d.heal * dt);
        }
        tp.used = used;
        if (tp.unloading) {
            if (tp.cargo.empty()) tp.unloading = false;
            else if (sim.tick() >= tp.nextUnload) {
                unloadNow.push_back(c);
                tp.nextUnload = sim.tick() + uint32_t(std::max<int64_t>(1, (d.unloadInterval * int64_t(sim.tickRate())).floorInt()));
            }
        }
    });
    for (Entity c : unloadNow) if (w.alive(c)) unloadOne(sim, c, 0);
}

void RtsModule::hashMech(Simulation& sim, Hasher& h, Entity e) {
    World& w = sim.world();
    if (auto* st = w.tryGet<Stance>(e)) { h.u32(0x57A1); h.u32(st->mode); h.v3(st->home); }
    if (auto* rt = w.tryGet<Retreating>(e)) { h.u32(0x4E7E); h.u64(rt->to.packed()); }
    if (auto* tp = w.tryGet<Transport>(e)) { h.u32(0x7A45); h.u32(uint32_t(tp->cargo.size())); h.u32(uint32_t(tp->used)); h.u32(tp->unloading); }
    if (auto* b = w.tryGet<Boarded>(e)) { h.u32(0xB0A7); h.u64(b->carrier.packed()); h.u32(b->fires); }
    if (auto* v = w.tryGet<Veteran>(e)) { h.u32(0x7E7E); h.i32(v->level); h.fx(v->xp); }
    if (auto* rp = w.tryGet<RallyPath>(e)) { h.u32(0x9A11); h.u32(uint32_t(rp->points.size())); }
    if (auto* sh = w.tryGet<Shield>(e)) { h.u32(0x5E1D); h.fx(sh->hp); }
}

} // namespace rts::game
