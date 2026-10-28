// Computer player for foundations and traps (RtsFidelity.cpp): keeps a few concrete cells paved ahead of the
// next building (Dune II) and sets traps at the front of the base. Like the rest of the AI it only submits
// Commands; where things go is decided by findAutoSpot (RtsModule::spotScoreFidelity).
#include "RtsAI.h"
#include <algorithm>
#include <vector>

namespace rts::game {

void RtsAI::fidelity(Simulation& sim, RtsModule& m) {
    World& w = sim.world();
    const Rules& R = m.rules();
    GameState& gs = m.state(sim);
    if (player_ >= gs.players.size()) return;
    auto& eco = gs.players[player_];
    auto rich = [&](const UnitType& t, int times, int extra) {
        for (auto& a : t.cost)
            if (eco.stock[a.res] < a.value * times + Fixed::fromInt(extra)) return false;
        return true;
    };
    auto pending = [&](auto pred) {
        bool any = false;
        w.each<Production, Owner>([&](Entity, Production& p, Owner& o) {
            if (o.player != player_) return;
            if (p.ready != kNoType && pred(p.ready)) any = true;
            for (auto& it : p.queue) if (it.item >= 0 && pred(uint16_t(it.item))) any = true;
        });
        w.each<Construction, Unit, Owner>([&](Entity, Construction&, Unit& u, Owner& o) { if (o.player == player_ && pred(u.type)) any = true; });
        return any;
    };
    // concrete: a small stock of free paved cells near the base
    const MapGrid& g = gs.grid;
    if (R.foundation.enabled && g.foundation.size() == g.flags.size()) {
        int freeCells = 0;
        for (size_t i = 0; i < g.foundation.size(); ++i) if (g.foundation[i] == player_ && !g.blocked[i]) ++freeCells;
        if (freeCells < 8 && !pending([&](uint16_t t) { return R.types[t].foundation; })) {
            int best = -1;
            for (size_t k = 0; k < R.types.size(); ++k) {
                const UnitType& t = R.types[k];
                if (!t.foundation || !m.canTrain(sim, player_, uint16_t(k)) || !rich(t, 4, 150)) continue;
                if (best < 0 || t.footprintW * t.footprintH > R.types[size_t(best)].footprintW * R.types[size_t(best)].footprintH) best = int(k);
            }
            if (best >= 0) build(sim, m, uint16_t(best));
        }
    }
    // traps: a few at the front, more as the match goes on
    if (!R.traps.empty() && rng_.below(3) == 0) {
        int have = 0;
        w.each<TrapState, Owner>([&](Entity, TrapState&, Owner& o) { if (o.player == player_) ++have; });
        const int want = std::min(8, 2 + int(sim.tick() / std::max<uint32_t>(1, sim.tickRate() * 120)));
        if (have < want && !pending([&](uint16_t t) { return R.types[t].trap >= 0; })) {

            std::vector<uint16_t> options;
            for (size_t k = 0; k < R.types.size(); ++k)
                if (R.types[k].trap >= 0 && m.canTrain(sim, player_, uint16_t(k)) && rich(R.types[k], 1, 10)) options.push_back(uint16_t(k));
            if (!options.empty()) build(sim, m, options[rng_.below(uint32_t(options.size()))]);
        }
    }
}

} // namespace rts::game
