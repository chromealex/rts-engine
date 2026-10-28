// agent-mech: combat and economy models found missing by the templates audit.
// - shields (StarCraft): absorb damage first (minus shield armor), regenerate after a delay without hits.
// - upkeepResources: upkeep cuts gathering of these resources only (Warcraft III: gold).
// - resourceNode.maxWorkers: at most N workers gather a node at once; the others wait (WC3 mine, SC minerals).
#include "RtsInternal.h"
#include <algorithm>

namespace rts::game {
using namespace detail;

Fixed RtsModule::mechShieldAbsorb(Simulation& sim, Entity target, Fixed dmg) {
    World& w = sim.world();
    auto* sh = w.tryGet<MechShield>(target);
    if (!sh) return dmg;
    sh->lastHit = sim.tick();
    if (sh->hp.raw <= 0) return dmg;
    const Fixed sa = rules_.types[w.get<Unit>(target).type].mech.shieldArmor;
    Fixed eff = fxMax(dmg - sa, Fixed::fromRatio(1, 2));
    if (sh->hp >= eff) { sh->hp -= eff; return Fixed{}; }
    Fixed left = eff - sh->hp;
    sh->hp = Fixed{};
    return left;
}


bool RtsModule::mechNodeHasRoom(Simulation& sim, Entity node, Entity worker) const {
    World& w = sim.world();
    if (!w.alive(node) || !w.has<Unit>(node)) return true;
    const int cap = rules_.types[w.get<Unit>(node).type].mech.maxWorkers;
    if (cap <= 0) return true;
    int n = 0;
    w.each<Orders>([&](Entity e, Orders& ord) {
        if (e == worker || ord.queue.empty()) return;
        const Order& o = ord.queue.front();
        if (o.type == OrderType::Gather && o.target == node && o.state == 2) ++n;
    });
    return n < cap;
}

void RtsModule::mechTickCombat(Simulation& sim) {
    if (!rules_.mech.shields) return;
    World& w = sim.world();
    const Fixed dt = sim.dt();
    std::vector<Entity> add;
    w.each<Unit>([&](Entity e, Unit& u) {
        const MechType& mt = rules_.types[u.type].mech;
        if (mt.shieldMax.raw <= 0 || u.hp.raw <= 0) return;
        auto* sh = w.tryGet<MechShield>(e);
        if (!sh) { add.push_back(e); return; }
        if (sim.tick() >= sh->lastHit + uint32_t((mt.shieldDelay * int64_t(sim.tickRate())).floorInt()) && sh->hp < mt.shieldMax)
            sh->hp = fxMin(mt.shieldMax, sh->hp + mt.shieldRegen * dt);
    });
    for (Entity e : add) w.add<MechShield>(e, MechShield{rules_.types[w.get<Unit>(e).type].mech.shieldMax, 0});
}

} // namespace rts::game
