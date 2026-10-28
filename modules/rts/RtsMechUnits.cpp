// agent-mech mechanics, part 2: detection and toggled abilities (cloak, burrow, siege mode),
// veterancy of ordinary units, guard / follow orders, rally waypoints and wave routes.
#include "RtsInternal.h"
#include <algorithm>

namespace rts::game {
using namespace detail;

// ================================================================= detection

bool RtsModule::detected(Simulation& sim, Entity e, uint8_t viewer) const {
    if (!rules_.mech.detection || viewer == 0) return false;
    World& w = sim.world();
    GameState& gs = *w.resource<GameState>();
    const Vec3 p = w.get<Transform>(e).pos;
    bool found = false;
    gs.hash.query(p, rules_.mech.maxDetect, [&](Entity d) {
        if (found || !w.has<Unit>(d)) return;
        const Unit& du = w.get<Unit>(d);
        const Fixed r = rules_.types[du.type].mech.detects;
        if (r.raw <= 0 || du.hp.raw <= 0 || (du.flags & (kUnitUnderConstruction | kUnitOffMap | kUnitDisabled))) return;
        const uint8_t o = w.get<Owner>(d).player;
        if (o != viewer && !(o != 0 && o != kHostileNeutral && sim.allied(o, viewer))) return;
        if (dist2D(w.get<Transform>(d).pos, p) <= r) found = true;
    });
    return found;
}

bool RtsModule::canSee(Simulation& sim, Entity e, uint8_t viewer) const {
    World& w = sim.world();
    if (!w.alive(e) || !w.has<Unit>(e)) return false;
    if (w.get<Unit>(e).flags & kUnitInCargo) return false;
    const uint8_t o = w.get<Owner>(e).player;
    if (o == viewer || (o != 0 && o != kHostileNeutral && sim.allied(o, viewer))) return true;
    if (!rules_.advancedEffects) return true;
    const uint32_t fl = statusFlags(sim, e);
    return !(fl & kStInvisible) || (fl & kStRevealed) || detected(sim, e, viewer);
}

// ================================================================= toggles

bool RtsModule::mechToggle(Simulation& sim, Entity caster, int ability) {
    World& w = sim.world();
    const MechType& mt = rules_.types[w.get<Unit>(caster).type].mech;
    for (auto& tg : mt.toggles) {
        if (tg.ability != ability) continue;
        auto* st = w.tryGet<Statuses>(caster);
        if (st) for (auto& in : st->list)
            if (in.def == uint16_t(tg.status) && in.until > sim.tick()) { in.until = sim.tick(); return true; }   // off (expires this tick)
        EffectCtx ctx;
        ctx.caster = caster; ctx.owner = w.get<Owner>(caster).player; ctx.sourceType = w.get<Unit>(caster).type;
        ctx.target = caster; ctx.point = ctx.origin = w.get<Transform>(caster).pos;
        addStatus(sim, caster, tg.status, Fixed::fromInt(1000000), ctx);   // on until toggled off
        stopMoving(sim, caster);
        return true;
    }
    return false;
}

// ================================================================= veterancy

void RtsModule::mechOnHit(Simulation& sim, Entity target, Entity source, Fixed dmg) {
    if (auto* v = sim.world().tryGet<Veteran>(target)) v->lastHit = source;
    if (rules_.mech.morale) mechMoraleHit(sim, target, source, dmg);
}

void RtsModule::mechVeteranKill(Simulation& sim, Entity victim) {
    if (rules_.mech.infection || rules_.mech.ruinsAny) mechTacticsDeath(sim, victim);
    if (!rules_.mech.veterancy) return;
    World& w = sim.world();
    auto* vv = w.tryGet<Veteran>(victim);
    Entity killer = vv ? vv->lastHit : kNullEntity;
    if (!w.alive(killer) || !w.has<Veteran>(killer) || !w.has<Unit>(killer) || w.get<Unit>(killer).hp.raw <= 0) return;
    if (w.get<Owner>(killer).player == w.get<Owner>(victim).player) return;
    const UnitType& vt = rules_.types[w.get<Unit>(victim).type];
    const Fixed xp = vt.mech.killXp;
    const UnitType& kt = rules_.types[w.get<Unit>(killer).type];
    const MechType& mt = kt.mech;
    if (mt.veterancy == 0 || kt.structure || kt.hero || (mt.veterancy != 1 && !rules_.mech.vet.set)) return;   // can't rank up
    Veteran& k = w.get<Veteran>(killer);
    k.xp += xp;
    const VeterancyTable& tab = mt.veterancy == 1 ? mt.vetTable : rules_.mech.vet;
    while (size_t(k.level - 1) < tab.xp.size() && k.xp >= tab.xp[size_t(k.level - 1)]) {
        ++k.level;
        if (rules_.mech.vetHeal) w.get<Unit>(killer).hp = stat(sim, killer, Stat::MaxHp);
        sim.emit({kEvLevelUp, w.get<Owner>(killer).player, killer, kNullEntity, k.level, w.get<Transform>(killer).pos});
    }
}

// ================================================================= follow / guard, rally waypoints, wave routes

bool RtsModule::runFollow(Simulation& sim, Entity e, Order& o) {
    World& w = sim.world();
    if (!w.alive(o.target) || !w.has<Unit>(o.target) || w.get<Unit>(o.target).hp.raw <= 0) return true;
    auto* m = w.tryGet<Mobile>(e);
    if (!m) return true;
    const Vec3 me = w.get<Transform>(e).pos, tp = w.get<Transform>(o.target).pos;
    const uint8_t owner = w.get<Owner>(e).player;
    if (o.param == 1) {   // guard: fight enemies close to the guarded unit
        auto* c = w.tryGet<Combat>(e);
        if (c) {
            if (!w.alive(c->target) || !w.has<Unit>(c->target) || dist2D(w.get<Transform>(c->target).pos, tp) > Fixed::fromInt(8)) {
                c->target = kNullEntity;
                Fixed bd{};
                state(sim).hash.query(tp, Fixed::fromInt(6), [&](Entity x) {
                    if (!w.has<Unit>(x) || w.get<Unit>(x).hp.raw <= 0 || !hostile(sim, owner, w.get<Owner>(x).player)) return;
                    if (rules_.types[w.get<Unit>(x).type].invulnerable || !canSee(sim, x, owner)) return;
                    Fixed d = dist2D(w.get<Transform>(x).pos, tp);
                    if (!c->target.valid() || d < bd || (d == bd && x.index < c->target.index)) { c->target = x; bd = d; }
                });
                c->autoTarget = false;
            }
            if (w.alive(c->target)) {
                const UnitType& t = rules_.types[w.get<Unit>(e).type];
                Fixed range = maxWeaponRange(t) + stat(sim, e, Stat::Range);
                if (contactDistance(sim, e, c->target) > range) {
                    Vec3 ep = w.get<Transform>(c->target).pos;
                    if (!m->moving || dist2D(m->goal, ep) > kOne || (sim.tick() + e.index) % 20 == 0) moveTo(sim, e, ep, range);
                } else if (m->moving) stopMoving(sim, e);
                return false;
            }
        }
    }
    const Fixed d = dist2D(me, tp);
    if (d > Fixed::fromInt(3)) {
        if (!m->moving || dist2D(m->goal, tp) > kOne || (sim.tick() + e.index) % 20 == 0) moveTo(sim, e, tp, Fixed::fromInt(2));
    } else if (m->moving && d < Fixed::fromInt(2)) stopMoving(sim, e);
    return false;   // until another order
}

void RtsModule::mechOnSpawn(Simulation& sim, Entity e, Entity producer) {
    World& w = sim.world();
    auto* rp = w.tryGet<RallyPath>(producer);
    if (!rp || !w.has<Orders>(e)) return;
    for (auto& p : rp->points) {
        Order o;
        o.type = OrderType::Move;
        o.point = p;
        o.param = 1;
        issue(sim, e, o, true);
    }
}

void RtsModule::mechWaveRoute(Simulation& sim, Entity e, const std::vector<Vec3>& route) {
    World& w = sim.world();
    auto* ord = w.tryGet<Orders>(e);
    if (!ord) return;
    std::vector<Order> after = ord->queue;   // the wave's attack on the objective comes last
    ord->queue.clear();
    stopMoving(sim, e);
    for (auto& p : route) {
        Order o;
        o.type = OrderType::AttackMove;
        o.point = flat(p);
        o.param = 4;
        ord->queue.push_back(o);
    }
    for (auto& o : after) ord->queue.push_back(o);
}

bool RtsModule::applyMechCommand2(Simulation& sim, const Command& cmd) {
    World& w = sim.world();
    auto own = [&](Entity e) { return w.alive(e) && w.has<Unit>(e) && w.get<Owner>(e).player == cmd.player; };
    switch (cmd.type) {
    case kCmdGuard:
    case kCmdFollow: {
        if (!w.alive(cmd.target) || !w.has<Unit>(cmd.target)) return true;
        state(sim).mechActive = true;
        for (Entity e : cmd.units) {
            if (!own(e) || e == cmd.target || !w.has<Mobile>(e) || (w.get<Unit>(e).flags & kUnitOffMap)) continue;
            Order o;
            o.type = OrderType::Follow;
            o.target = cmd.target;
            o.param = cmd.type == kCmdGuard ? 1 : 0;
            issue(sim, e, o, cmd.flags & kCmdQueue);
        }
        return true;
    }
    case kCmdRally: {
        // Shift + rally adds a waypoint after the rally point; a plain rally clears them (and is handled as usual)
        const bool queue = cmd.flags & kCmdQueue;
        std::vector<Entity> add;
        for (Entity e : cmd.units) if (own(e) && w.has<Production>(e) && queue && !w.has<RallyPath>(e)) add.push_back(e);
        for (Entity e : add) w.add<RallyPath>(e, RallyPath{});
        for (Entity e : cmd.units) {
            if (!own(e) || !w.has<Production>(e)) continue;
            if (!queue) { if (w.has<RallyPath>(e)) w.remove<RallyPath>(e); continue; }
            auto& pp = w.get<Production>(e);
            if (!pp.hasRally) { pp.rally = flat(cmd.point); pp.hasRally = true; pp.rallyTarget = kNullEntity; continue; }
            auto& rp = w.get<RallyPath>(e);
            if (rp.points.size() < 16) rp.points.push_back(flat(cmd.point));
        }
        if (queue) state(sim).mechActive = true;
        return queue;
    }
    default: return false;
    }
}

// ================================================================= per tick

void RtsModule::mechTick(Simulation& sim) {
    mechTickTactics(sim);
    mechTickSociety(sim);
    mechTickBattle(sim);
    if (rules_.mech.conveyors) mechTickLogistics(sim);
    if (rules_.mech.turns) mechTickTurns(sim);
    if (rules_.mech.altitude) mechTickAltitude(sim);
    World& w = sim.world();
    const Fixed dt = sim.dt();
    std::vector<Entity> addVet;
    w.each<Unit>([&](Entity e, Unit& u) {
        if (u.hp.raw <= 0) return;
        const UnitType& t = rules_.types[u.type];
        if ((rules_.mech.veterancy || rules_.mech.infection) && !w.has<Veteran>(e)) addVet.push_back(e);   // every object records its last hitter
        if (!t.mech.toggles.empty()) {
            auto* st = w.tryGet<Statuses>(e);
            if (!st) return;
            for (auto& tg : t.mech.toggles) {
                if (tg.drain.raw <= 0) continue;
                for (auto& in : st->list) {
                    if (in.def != uint16_t(tg.status) || in.until <= sim.tick()) continue;
                    u.energy -= tg.drain * dt;
                    if (u.energy.raw <= 0) { u.energy = Fixed{}; in.until = sim.tick(); }
                }
            }
        }
    });
    for (Entity e : addVet) w.add<Veteran>(e, Veteran{});
}

} // namespace rts::game
