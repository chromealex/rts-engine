// agent-mech stage 3, society: buildings run by their staff (workers inside), population growth, power fields
// (pylons), autonomous heroes and bounties (Majesty), rounds (auto-battlers: prepare / fight / reset), upgrades paid
// with garrison units (Mushroom Wars 2).
#include "RtsInternal.h"
#include <algorithm>

namespace rts::game {
using namespace detail;

Fixed RtsModule::staffFraction(Simulation& sim, Entity e) const {
    World& w = sim.world();
    const int need = rules_.types[w.get<Unit>(e).type].mech.staff;
    if (need <= 0) return kOne;
    auto* tp = w.tryGet<Transport>(e);
    const int have = tp ? int(tp->cargo.size()) : 0;
    return Fixed::fromInt(std::min(have, need)) / need;
}

bool RtsModule::mechPowered(Simulation& sim, uint8_t player, Vec3 at) const {
    World& w = sim.world();
    bool ok = false;
    w.each<Unit, Owner, Transform>([&](Entity, Unit& u, Owner& o, Transform& t) {
        if (ok || o.player != player || u.hp.raw <= 0 || (u.flags & kUnitUnderConstruction)) return;
        const Fixed r = rules_.types[u.type].mech.powerField;
        if (r.raw > 0 && dist2D(t.pos, at) <= r) ok = true;
    });
    return ok;
}

bool RtsModule::mechSwitchedOff(Simulation& sim, Entity e) const {
    World& w = sim.world();
    const MechType& mt = rules_.types[w.get<Unit>(e).type].mech;
    if (mt.needsPower && !mechPowered(sim, w.get<Owner>(e).player, w.get<Transform>(e).pos)) return true;
    if (mt.staff > 0 && staffFraction(sim, e).raw == 0) return true;
    return false;
}

Fixed RtsModule::mechIncomeMult(Simulation& sim, Entity e) const {
    return rules_.types[sim.world().get<Unit>(e).type].mech.staff > 0 ? staffFraction(sim, e) : kOne;
}

void RtsModule::mechTickSociety(Simulation& sim) {
    World& w = sim.world();
    GameState& gs = state(sim);
    const MechRules& M = rules_.mech;
    const Fixed dt = sim.dt();
    // ---- population growth
    if (M.growth) {
        std::vector<Entity> add;
        struct Birth { Entity home; uint16_t type; uint8_t owner; };
        std::vector<Birth> births;
        w.each<Unit, Owner>([&](Entity e, Unit& u, Owner& o) {
            const MechType& mt = rules_.types[u.type].mech;
            if (mt.growUnit == kNoType || u.hp.raw <= 0 || (u.flags & kUnitUnderConstruction) || o.player == 0) return;
            auto* g = w.tryGet<GrowState>(e);
            if (!g) { add.push_back(e); return; }
            g->timer += dt;
            if (g->timer < mt.growInterval) return;
            const auto& eco = gs.players[o.player];
            const UnitType& nt = rules_.types[mt.growUnit];
            if (eco.supplyUsed + nt.supplyUsed > eco.supplyCap && nt.supplyUsed.raw > 0) return;   // no room: waits
            if (!canAfford(sim, o.player, mt.growCost)) return;
            g->timer = Fixed{};
            births.push_back({e, mt.growUnit, o.player});
        });
        for (Entity e : add) w.add<GrowState>(e, GrowState{});
        for (auto& b : births) {
            if (!pay(sim, b.owner, rules_.types[w.get<Unit>(b.home).type].mech.growCost, {}, false)) continue;
            Entity n = spawn(sim, b.type, b.owner, freeSpotNear(sim, b.home, rules_.types[b.type].pathLayer));
            sim.emit({kEvTrainDone, b.owner, n, b.home, b.type, w.get<Transform>(n).pos});
        }
    }
    // ---- bounties and autonomous heroes
    if (M.autonomy) {
        auto& bs = gs.mech.bounties;
        for (size_t i = 0; i < bs.size();) {   // claims
            Bounty& b = bs[i];
            bool claimed = false;
            if (b.target.valid()) claimed = !w.alive(b.target) || w.get<Unit>(b.target).hp.raw <= 0;
            else w.each<Unit, Owner, Transform>([&](Entity, Unit& u, Owner& o, Transform& t) {
                if (!claimed && o.player == b.player && rules_.types[u.type].mech.autonomous && dist2D(t.pos, b.pos) <= Fixed::fromInt(2)) claimed = true;
            });
            if (claimed) {
                auto& st = gs.players[b.player].stock;
                if (b.res >= 0) st[size_t(b.res)] = fxMax(Fixed{}, st[size_t(b.res)] - b.reward);
                sim.emit({kEvTraded, b.player, kNullEntity, b.target, b.res, b.pos});
                bs.erase(bs.begin() + ptrdiff_t(i));
            } else ++i;
        }
        if (sim.tick() % 20 == 0) {
            struct Go { Entity e; Order o; };
            std::vector<Go> go;
            w.each<Unit, Owner, Transform>([&](Entity e, Unit& u, Owner& o, Transform& t) {
                const UnitType& ut = rules_.types[u.type];
                if (!ut.mech.autonomous || u.hp.raw <= 0 || (u.flags & kUnitOffMap) || !w.has<Mobile>(e)) return;
                auto* ord = w.tryGet<Orders>(e);
                if (ord && !ord->queue.empty()) return;
                Order od;
                // 1. the richest reachable bounty of its player
                const Bounty* best = nullptr;
                for (auto& b : bs) if (b.player == o.player && (!best || b.reward > best->reward)) best = &b;
                if (best) {
                    if (best->target.valid() && w.alive(best->target)) { od.type = OrderType::Attack; od.target = best->target; }
                    else { od.type = OrderType::Move; od.point = best->pos; od.param = 1; }
                    go.push_back({e, od});
                    return;
                }
                // 2. an enemy it can see, 3. somewhere unexplored
                Entity foe; Fixed fd{};
                gs.hash.query(t.pos, stat(sim, e, Stat::Sight), [&](Entity x) {
                    if (!w.has<Unit>(x) || w.get<Unit>(x).hp.raw <= 0 || !hostile(sim, o.player, w.get<Owner>(x).player)) return;
                    Fixed d = dist2D(w.get<Transform>(x).pos, t.pos);
                    if (!foe.valid() || d < fd || (d == fd && x.index < foe.index)) { foe = x; fd = d; }
                });
                if (foe.valid()) { od.type = OrderType::Attack; od.target = foe; go.push_back({e, od}); return; }
                const MapGrid& g = gs.grid;
                for (int k = 0; k < 8; ++k) {
                    int x = int(sim.rng().below(uint32_t(g.w))), y = int(sim.rng().below(uint32_t(g.h)));
                    if (o.player < gs.vision.size() && gs.vision[o.player][size_t(g.index(x, y))] == 0 && g.passable(x, y, ut.pathLayer)) {
                        od.type = OrderType::Move; od.point = g.cellCenter(x, y); od.param = 1;
                        go.push_back({e, od});
                        return;
                    }
                }
            });
            for (auto& g : go) issue(sim, g.e, g.o, false);
        }
    }
    // ---- rounds (auto-battlers)
    if (M.rounds && gs.mech.roundPhase < 2 && sim.tick() >= gs.mech.phaseEnd) {
        MechState& ms = gs.mech;
        const uint32_t rate = sim.tickRate();
        if (ms.roundPhase == 0) {   // prepare -> fight: remember the army, everyone charges the enemy
            ms.roundSnapshot.clear();
            std::vector<std::pair<Entity, Vec3>> charge;
            w.each<Unit, Owner, Transform>([&](Entity e, Unit& u, Owner& o, Transform& t) {
                if (u.hp.raw <= 0 || rules_.types[u.type].structure || o.player == 0 || !w.has<Mobile>(e)) return;
                ms.roundSnapshot.push_back({o.player, u.type, t.pos});
                Vec3 c{}; int n = 0;
                w.each<Unit, Owner, Transform>([&](Entity, Unit& v, Owner& vo, Transform& vt) {
                    if (v.hp.raw > 0 && hostile(sim, o.player, vo.player) && vo.player != 0) { c += vt.pos; ++n; }
                });
                if (n) charge.push_back({e, c / Fixed::fromInt(n)});
            });
            for (auto& [e, goal] : charge) { Order o; o.type = OrderType::AttackMove; o.point = flat(goal); o.param = 4; issue(sim, e, o, false); }
            ms.roundPhase = 1;
            ms.phaseEnd = sim.tick() + uint32_t((M.fightTime * int64_t(rate)).floorInt());
        } else {   // fight over: survivors hurt the other players, then reset
            std::vector<int> survivors(gs.players.size(), 0);
            w.each<Unit, Owner>([&](Entity e, Unit& u, Owner& o) {
                if (u.hp.raw > 0 && !rules_.types[u.type].structure && o.player < survivors.size() && w.has<Mobile>(e)) survivors[o.player]++;
            });
            if (M.livesRes >= 0)
                for (auto& p : sim.players())
                    for (auto& q : sim.players())
                        if (p.slot != q.slot && hostile(sim, p.slot, q.slot) && q.alive) {
                            Fixed& lives = gs.players[q.slot].stock[size_t(M.livesRes)];
                            lives = fxMax(Fixed{}, lives - M.damagePerUnit * survivors[p.slot]);
                        }
            for (auto& p : sim.players()) {
                for (auto& a : M.roundIncome) addResource(sim, p.slot, a.res, a.value);
                if (M.livesRes >= 0 && p.alive && gs.players[p.slot].stock[size_t(M.livesRes)].raw <= 0) {
                    p.alive = false;
                    sim.emit({kEvPlayerDefeated, p.slot, kNullEntity, kNullEntity, 0, {}});
                }
            }
            if (M.roundReset) {   // the armies come back where they stood before the fight
                std::vector<Entity> kill;
                w.each<Unit>([&](Entity e, Unit& u) { if (!rules_.types[u.type].structure && w.has<Mobile>(e)) kill.push_back(e); });
                for (Entity e : kill) { Unit& u = w.get<Unit>(e); u.hp = Fixed{}; u.flags |= kUnitSilent; }
                for (auto& r : ms.roundSnapshot) if (auto* pi = sim.player(r.owner); pi && pi->alive) spawn(sim, r.type, r.owner, r.pos);
            }
            ms.roundIndex++;
            gs.trig.vars["round"] = Fixed::fromInt(ms.roundIndex);
            if (ms.roundIndex >= M.roundCount) {
                ms.roundPhase = 2;
                if (M.livesRes >= 0) {   // most lives left wins
                    uint8_t best = 0; Fixed bl = Fixed::fromInt(-1);
                    for (auto& p : sim.players()) if (p.alive && gs.players[p.slot].stock[size_t(M.livesRes)] > bl) { bl = gs.players[p.slot].stock[size_t(M.livesRes)]; best = p.slot; }
                    if (best) gs.winnerTeam = teamOf(sim, best);
                }
            } else {
                ms.roundPhase = 0;
                ms.phaseEnd = sim.tick() + uint32_t((M.prepareTime * int64_t(rate)).floorInt());
            }
        }
        sim.emit({kEvCyclePhase, 0, kNullEntity, kNullEntity, (int64_t(255) << 8) | ms.roundPhase, {}});
    }
}

bool RtsModule::applyMechSocietyCommand(Simulation& sim, const Command& cmd) {
    World& w = sim.world();
    GameState& gs = state(sim);
    switch (cmd.type) {
    case kCmdBounty: {
        if (!rules_.mech.autonomy) return true;
        const int res = int(cmd.param & 0xFF);
        const Fixed reward = Fixed::fromInt(cmd.param >> 8);
        if (res >= int(rules_.resources.size()) || reward.raw <= 0 || gs.players[cmd.player].stock[size_t(res)] < reward) return true;
        Bounty b;
        b.player = cmd.player; b.res = int16_t(res); b.reward = reward; b.pos = flat(cmd.point);
        if (w.alive(cmd.target) && w.has<Unit>(cmd.target) && hostile(sim, cmd.player, w.get<Owner>(cmd.target).player)) { b.target = cmd.target; b.pos = w.get<Transform>(cmd.target).pos; }
        if (gs.mech.bounties.size() < 64) gs.mech.bounties.push_back(b);
        return true;
    }
    case kCmdUpgrade: {
        // Mushroom Wars 2: upgrading into a type with upgradeGarrison costs that many units from the garrison
        if (inMechUpgrade_ || cmd.param < 0 || size_t(cmd.param) >= rules_.types.size()) return false;
        const int cost = rules_.types[size_t(cmd.param)].mech.upgradeGarrison;
        if (cost <= 0) return false;
        Command sub = cmd;
        sub.units.clear();
        for (Entity e : cmd.units) {
            auto* g = w.alive(e) ? w.tryGet<Garrison>(e) : nullptr;
            if (!g || w.get<Owner>(e).player != cmd.player || g->count < cost) continue;
            auto& ups = rules_.types[w.get<Unit>(e).type].upgrades;
            if (std::find(ups.begin(), ups.end(), uint16_t(cmd.param)) == ups.end()) continue;
            g->count -= cost;
            sub.units.push_back(e);
        }
        if (!sub.units.empty()) { inMechUpgrade_ = true; applyCommand(sim, sub); inMechUpgrade_ = false; }
        return true;
    }
    default: return false;
    }
}

} // namespace rts::game
