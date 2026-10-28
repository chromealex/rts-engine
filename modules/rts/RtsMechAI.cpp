// Computer player for the agent-mech mechanics: wounded units retreat, infantry mans bunkers and town
// centres when enemies come close and leaves them when the danger is gone.
#include "RtsAI.h"
#include "RtsInternal.h"
#include <algorithm>

namespace rts::game {
using namespace detail;

void RtsAI::mech(Simulation& sim, RtsModule& m) {
    if (sim.tick() < lastMechTick_ + sim.tickRate()) return;
    lastMechTick_ = sim.tick();
    World& w = sim.world();
    const Rules& R = m.rules();
    GameState& gs = m.state(sim);
    auto enemyNear = [&](Vec3 p, Fixed r) {
        bool found = false;
        gs.hash.query(p, r, [&](Entity c) {
            if (found || !w.has<Unit>(c) || w.get<Unit>(c).hp.raw <= 0) return;
            const UnitType& ct = R.types[w.get<Unit>(c).type];
            if (ct.weapons.empty() || ct.invulnerable) return;
            if (m.hostile(sim, player_, w.get<Owner>(c).player) && dist2D(w.get<Transform>(c).pos, p) <= r) found = true;
        });
        return found;
    };

    // wounded fighters retreat
    if (R.mech.retreat) {
        Command c; c.player = player_; c.type = kCmdRetreat;
        w.each<Unit, Owner, Transform>([&](Entity e, Unit& u, Owner& o, Transform& t) {
            if (o.player != player_ || u.hp.raw <= 0 || (u.flags & kUnitOffMap) || w.has<Retreating>(e) || !w.has<Mobile>(e)) return;
            const UnitType& ut = R.types[u.type];
            if (ut.weapons.empty() || u.hp * 10 > m.stat(sim, e, Stat::MaxHp) * 3) return;   // above 30% hp
            if (enemyNear(t.pos, m.stat(sim, e, Stat::Sight))) c.units.push_back(e);
        });
        if (!c.units.empty()) send(sim, c);
    }

    // man shooting structures under threat, leave town centres when it is over
    if (!R.mech.transports) return;
    struct Spot { Entity s; int room; bool shoots; bool busy; Vec3 pos; };
    std::vector<Spot> spots;
    w.each<Unit, Owner, Transform>([&](Entity e, Unit& u, Owner& o, Transform& t) {
        if (o.player != player_ || u.hp.raw <= 0 || (u.flags & kUnitUnderConstruction)) return;
        const UnitType& ut = R.types[u.type];
        const TransportDef& d = ut.mech.transport;
        if (d.slots == 0 || !ut.structure || (d.fireFrom == 0 && d.damagePerCargo.raw == 0)) return;
        auto* tp = w.tryGet<Transport>(e);
        spots.push_back({e, d.slots - (tp ? tp->used : 0), d.fireFrom > 0, tp && !tp->cargo.empty(), t.pos});
    });
    for (auto& s : spots) {
        const bool danger = enemyNear(s.pos, Fixed::fromInt(10));
        if (!danger) {
            // the town centre lets its workers out again; bunkers stay manned
            if (!s.shoots && s.busy && !enemyNear(s.pos, Fixed::fromInt(14))) {
                Command c; c.player = player_; c.type = kCmdUnload; c.units = {s.s};
                send(sim, c);
            }
            continue;
        }
        if (s.room <= 0) continue;
        std::vector<std::pair<Fixed, Entity>> cand;
        w.each<Unit, Owner, Transform>([&](Entity e, Unit& u, Owner& o, Transform& t) {
            if (o.player != player_ || u.hp.raw <= 0 || !m.accepts(sim, s.s, e)) return;
            const UnitType& ut = R.types[u.type];
            if (s.shoots == ut.weapons.empty()) return;   // bunkers: fighters; town centres: workers
            auto* ord = w.tryGet<Orders>(e);
            if (s.shoots && ord && !ord->queue.empty()) return;   // fighters busy with orders keep them
            Fixed d = dist2D(t.pos, s.pos);
            if (d <= Fixed::fromInt(12)) cand.push_back({d, e});
        });
        std::sort(cand.begin(), cand.end(), [](auto& a, auto& b) { return a.first != b.first ? a.first < b.first : a.second.index < b.second.index; });
        Command c; c.player = player_; c.type = kCmdLoad; c.target = s.s;
        int room = s.room;
        for (auto& [d, e] : cand) {
            int size = R.types[w.get<Unit>(e).type].mech.cargoSize;
            if (size > room) continue;
            room -= size;
            c.units.push_back(e);
        }
        if (!c.units.empty()) send(sim, c);
    }
}

} // namespace rts::game
