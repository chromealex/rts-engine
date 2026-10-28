// agent-mech: combat and economy models found missing by the templates audit.
// - upkeepResources: upkeep cuts gathering of these resources only (Warcraft III: gold).
// - resourceNode.maxWorkers: at most N workers gather a node at once; the others wait (WC3 mine, SC minerals).
#include "RtsInternal.h"
#include <algorithm>

namespace rts::game {
using namespace detail;

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

} // namespace rts::game
