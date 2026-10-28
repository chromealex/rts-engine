// Computer player for the fidelity mechanics (RtsOriginals.cpp): learns hero skills, mans empty vehicles,
// upgrades garrison buildings with their own units. Like the rest of the AI it only submits Commands.
#include "RtsAI.h"
#include <algorithm>
#include <vector>

namespace rts::game {

void RtsAI::originals(Simulation& sim, RtsModule& m) {
    World& w = sim.world();
    const Rules& R = m.rules();
    // hero skills: the ultimate as soon as possible, otherwise the lowest learnable ability
    if (R.heroes.skills) {
        w.each<Hero, Unit, Owner>([&](Entity e, Hero& h, Unit& u, Owner& o) {
            if (o.player != player_ || h.points == 0) return;
            const UnitType& t = R.types[u.type];
            int pick = -1;
            for (int i = 0; i < int(t.abilities.size()) && i < 8; ++i) {
                if (!m.canLearn(sim, e, i)) continue;
                bool ult = size_t(i) < t.learnAt.size() && !t.learnAt[size_t(i)].empty() && t.learnAt[size_t(i)][0] >= 6;
                if (ult) { pick = i; break; }
                if (pick < 0 || h.skill[i] < h.skill[pick]) pick = i;
            }
            if (pick < 0) return;
            Command c; c.type = kCmdLearn; c.units = {e}; c.param = pick;
            send(sim, c);
        });
    }
    // crews: idle units that can drive go for the nearest empty vehicle or gun
    std::vector<std::pair<Entity, Vec3>> empty;
    w.each<Unit, Owner, Transform>([&](Entity e, Unit& u, Owner& o, Transform& tr) {
        if (o.player == 0 && R.types[u.type].crew != kNoType && u.hp.raw > 0) empty.push_back({e, tr.pos});
    });
    if (!empty.empty()) {
        std::vector<Entity> claimed;
        w.each<Unit, Owner, Transform, Orders>([&](Entity e, Unit& u, Owner& o, Transform& tr, Orders& ord) {
            if (o.player != player_ || !R.types[u.type].crews || !ord.queue.empty()) return;
            Entity best; Fixed bd = Fixed::fromInt(20);
            for (auto& [v, p] : empty) {
                if (std::find(claimed.begin(), claimed.end(), v) != claimed.end()) continue;
                Fixed d = (p - tr.pos).xz().length();
                if (d < bd) { bd = d; best = v; }
            }
            if (!best.valid()) return;
            claimed.push_back(best);
            Command c; c.type = kCmdSmart; c.units = {e}; c.target = best; c.point = w.get<Transform>(best).pos;
            send(sim, c);
        });
    }
    // garrison buildings: spend spare units on the next level (never on conversions)
    w.each<Garrison, Unit, Owner, Production>([&](Entity e, Garrison& g, Unit& u, Owner& o, Production& p) {
        if (o.player != player_ || !p.queue.empty()) return;
        const UnitType& t = R.types[u.type];
        for (uint16_t up : t.upgrades) {
            const UnitType& ut = R.types[up];
            if (ut.upgradeGarrison <= 0 || ut.level <= t.level || ut.garrisonMax < t.garrisonMax) continue;
            if (g.count < ut.upgradeGarrison + t.garrisonMax / 3) continue;
            Command c; c.type = kCmdUpgrade; c.units = {e}; c.param = up;
            send(sim, c);
            break;
        }
    });
}

} // namespace rts::game
