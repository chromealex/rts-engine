#include "RtsAI.h"
#include <algorithm>
#include <vector>

namespace rts::game {

namespace {
Fixed dist(Vec3 a, Vec3 b) { return (a - b).xz().length(); }

struct Snapshot {
    std::vector<Entity> workers, idleWorkers, army, idleArmy, producers, structures, garrisons;
    std::vector<int> typeCount;      // incl. under construction
    Entity base;
    int underConstruction = 0;
};

bool isCombat(const UnitType& t) { return !t.structure && !t.weapons.empty() && !t.worker; }
} // namespace

void RtsAI::send(Simulation& sim, Command c) {
    c.player = player_;
    sim.submit(c);
}

void RtsAI::update(Simulation& sim) {
    auto* pi = sim.player(player_);
    if (!pi || !pi->alive || sim.result() != 0 || sim.tick() < next_) return;
    next_ = sim.tick() + cfg_.thinkTicks + rng_.below(cfg_.thinkTicks / 2 + 1);
    auto* m = dynamic_cast<RtsModule*>(&sim.module());
    if (!m) return;
    bool hasGarrisons = false, hasWorkers = false;
    sim.world().each<Unit, Owner>([&](Entity e, Unit& u, Owner& o) {
        if (o.player != player_) return;
        const UnitType& t = m->rules().types[u.type];
        if (sim.world().has<Garrison>(e) && t.spawnUnit != kNoType) hasGarrisons = true;
        if (t.worker) hasWorkers = true;
    });
    if (hasGarrisons) garrisonPlay(sim, *m);
    if (hasWorkers || !hasGarrisons) { economy(sim, *m); military(sim, *m); }
}

bool RtsAI::build(Simulation& sim, RtsModule& m, uint16_t type) {
    World& w = sim.world();
    const Rules& R = m.rules();
    // choose a builder that can build it: idle first, then gatherers
    Entity builder; Vec3 bpos{};
    int bestScore = 1 << 30;
    w.each<Unit, Owner, Orders, Transform>([&](Entity e, Unit& u, Owner& o, Orders& ord, Transform& tr) {
        if (o.player != player_) return;
        const UnitType& t = R.types[u.type];
        if (std::find(t.builds.begin(), t.builds.end(), type) == t.builds.end()) return;
        int score = ord.queue.empty() ? 0 : ord.queue.front().type == OrderType::Build ? 1 << 20 : 10;
        if (score < bestScore) { bestScore = score; builder = e; bpos = tr.pos; }
    });
    if (!builder.valid() || bestScore >= (1 << 20)) return false;
    if (!m.canAfford(sim, player_, type) || !m.requirementsMet(sim, player_, type)) return false;
    // placement: rings around the base (first structure), keeping a 1-cell gap between buildings
    Vec3 centre = bpos;
    w.each<Unit, Owner, Transform>([&](Entity, Unit& u, Owner& o, Transform& tr) {
        if (o.player == player_ && R.types[u.type].structure && R.types[u.type].dropoffMask) centre = tr.pos;
    });
    const UnitType& bt = R.types[type];
    const int start = int(rng_.below(8));
    for (int ring = 3; ring < 18; ring += 1) {
        for (int k = 0; k < 16; ++k) {
            int a = (k + start) % 16;
            static const int dx16[16] = {4, 4, 3, 2, 0, -2, -3, -4, -4, -4, -3, -2, 0, 2, 3, 4};
            static const int dz16[16] = {0, 2, 3, 4, 4, 4, 3, 2, 0, -2, -3, -4, -4, -4, -3, -2};
            Vec3 p = centre + Vec3{Fixed::fromInt(dx16[a] * ring) / 4, Fixed{}, Fixed::fromInt(dz16[a] * ring) / 4};
            p.x = Fixed::fromInt(p.x.floorInt()); p.z = Fixed::fromInt(p.z.floorInt());
            if (bt.footprintW % 2) p.x += Fixed::fromRatio(1, 2);
            if (bt.footprintH % 2) p.z += Fixed::fromRatio(1, 2);
            // gap check: a slightly larger footprint must be free too
            int x0, y0;
            RtsModule::footprintAt(bt, p, x0, y0);
            auto* gs = w.resource<GameState>();
            if (!gs->grid.areaFree(x0 - 1, y0 - 1, bt.footprintW + 2, bt.footprintH + 2)) continue;
            // keep clear of resource nodes so gathering paths stay open
            bool nearRes = false;
            w.each<ResourceStock, Transform>([&](Entity, ResourceStock&, Transform& rt) { if (dist(rt.pos, p) < Fixed::fromInt(4)) nearRes = true; });
            if (nearRes || !m.canPlace(sim, type, p, builder)) continue;
            Command c;
            c.type = kCmdBuild; c.units = {builder}; c.param = type; c.point = p;
            send(sim, c);
            lastBuildTick_ = sim.tick();
            return true;
        }
    }
    return false;
}

void RtsAI::economy(Simulation& sim, RtsModule& m) {
    World& w = sim.world();
    const Rules& R = m.rules();
    auto& eco = w.resource<GameState>()->players[player_];
    std::vector<int> count(R.types.size(), 0);
    std::vector<Entity> idleWorkers, producers;
    int workers = 0, building = 0;
    w.each<Unit, Owner>([&](Entity e, Unit& u, Owner& o) {
        if (o.player != player_) return;
        const UnitType& t = R.types[u.type];
        count[u.type]++;
        if (u.flags & kUnitUnderConstruction) ++building;
        if (t.worker) {
            ++workers;
            auto* ord = w.tryGet<Orders>(e);
            if (ord && ord->queue.empty()) idleWorkers.push_back(e);
        }
        if (!t.produces.empty() && !(u.flags & kUnitUnderConstruction)) producers.push_back(e);
    });

    // 1) idle workers gather the resource we have least of
    for (Entity wk : idleWorkers) {
        const UnitType& t = R.types[w.get<Unit>(wk).type];
        if (t.gathers.empty()) continue;
        Vec3 p = w.get<Transform>(wk).pos;
        Entity best; Fixed bestScore{};
        for (auto& g : t.gathers) {
            Fixed have = eco.stock[g.res];
            w.each<ResourceStock, Unit, Transform>([&](Entity n, ResourceStock& rs, Unit& nu, Transform& nt) {
                if (R.types[nu.type].resourceNode != g.res || rs.amount.raw <= 0) return;
                Fixed score = dist(nt.pos, p) * 4 + have / 20;
                if (!best.valid() || score < bestScore) { best = n; bestScore = score; }
            });
        }
        if (best.valid()) { Command c; c.type = kCmdGather; c.units = {wk}; c.target = best; send(sim, c); }
    }

    // 2) supply
    Fixed freeSupply = eco.supplyCap - eco.supplyUsed;
    if (eco.supplyCap < R.supplyCap && freeSupply < Fixed::fromInt(4) && sim.tick() > lastBuildTick_ + 60) {
        for (size_t ti = 0; ti < R.types.size(); ++ti) {
            const UnitType& t = R.types[ti];
            if (!t.structure || t.supplyProvided.raw <= 0 || !t.produces.empty()) continue;
            if (build(sim, m, uint16_t(ti))) return;
        }
    }

    // 3) flow economy (Total Annihilation): build income structures
    for (size_t ti = 0; ti < R.types.size(); ++ti) {
        const UnitType& t = R.types[ti];
        if (!t.structure || t.income.empty() || count[ti] >= 6 || sim.tick() < lastBuildTick_ + 80) continue;
        bool positive = false;
        for (auto& a : t.income) if (a.value.raw > 0) positive = true;
        if (positive && build(sim, m, uint16_t(ti))) return;
    }

    // 4) workers
    if (workers < cfg_.workersPerBase) {
        for (Entity pe : producers) {
            auto& prod = w.get<Production>(pe);
            if (!prod.queue.empty()) continue;
            const UnitType& pt = R.types[w.get<Unit>(pe).type];
            for (uint16_t u : pt.produces)
                if (R.types[u].worker && m.canAfford(sim, player_, u)) {
                    Command c; c.type = kCmdTrain; c.units = {pe}; c.param = u; send(sim, c);
                    break;
                }
        }
    }

    // 5) military production buildings (and tech they require)
    if (sim.tick() > lastBuildTick_ + 100 && building == 0) {
        int milProducers = 0;
        std::vector<uint16_t> candidates;
        for (size_t ti = 0; ti < R.types.size(); ++ti) {
            const UnitType& t = R.types[ti];
            if (!t.structure) continue;
            bool makesArmy = false;
            for (uint16_t u : t.produces) if (isCombat(R.types[u])) makesArmy = true;
            if (makesArmy) { milProducers += count[ti]; candidates.push_back(uint16_t(ti)); }
        }
        int want = 1 + workers / 6;
        if (milProducers < want && !candidates.empty()) {
            uint16_t pick = candidates[rng_.below(uint32_t(candidates.size()))];
            if (!m.requirementsMet(sim, player_, pick)) {
                for (uint16_t rq : R.types[pick].requires_) if (count[rq] == 0 && build(sim, m, rq)) return;
            } else if (build(sim, m, pick)) return;
        }
    }
}

void RtsAI::military(Simulation& sim, RtsModule& m) {
    World& w = sim.world();
    const Rules& R = m.rules();
    std::vector<Entity> idleArmy, army;
    w.each<Unit, Owner>([&](Entity e, Unit& u, Owner& o) {
        if (o.player != player_) return;
        const UnitType& t = R.types[u.type];
        if (isCombat(t)) {
            army.push_back(e);
            auto* ord = w.tryGet<Orders>(e);
            if (ord && ord->queue.empty()) idleArmy.push_back(e);
        }
        // train army from idle producers
        if (auto* prod = w.tryGet<Production>(e); prod && prod->queue.size() < 2 && !(u.flags & kUnitUnderConstruction)) {
            std::vector<uint16_t> opts;
            for (uint16_t ut : t.produces) if (isCombat(R.types[ut]) && m.requirementsMet(sim, player_, ut) && m.canAfford(sim, player_, ut)) opts.push_back(ut);
            if (!opts.empty()) { Command c; c.type = kCmdTrain; c.units = {e}; c.param = opts[rng_.below(uint32_t(opts.size()))]; send(sim, c); }
        }
    });
    int needed = cfg_.firstAttackArmy + wave_ * cfg_.armyGrowth;
    if (int(idleArmy.size()) < needed) return;
    // target: nearest enemy structure (or any enemy) to our army's centroid
    Vec3 c{};
    for (Entity e : idleArmy) c += w.get<Transform>(e).pos;
    c = c / Fixed::fromInt(int64_t(idleArmy.size()));
    Entity best; Fixed bestD{};
    w.each<Unit, Owner, Transform>([&](Entity e, Unit& u, Owner& o, Transform& t) {
        if (!m.hostile(sim, player_, o.player)) return;
        Fixed d = dist(t.pos, c) - (R.types[u.type].structure ? Fixed::fromInt(5) : Fixed{});
        if (!best.valid() || d < bestD) { best = e; bestD = d; }
    });
    if (!best.valid()) return;
    Command cmd;
    cmd.type = kCmdAttack;
    cmd.units = idleArmy;
    cmd.point = w.get<Transform>(best).pos; // attack-move
    send(sim, cmd);
    ++wave_;
}

void RtsAI::garrisonPlay(Simulation& sim, RtsModule& m) {
    // Mushroom-Wars-style: send part of the strongest garrisons at the weakest reachable target.
    World& w = sim.world();
    const Rules& R = m.rules();
    struct G { Entity e; int count; uint8_t owner; Vec3 pos; Fixed def; };
    std::vector<G> all;
    w.each<Garrison, Unit, Owner, Transform>([&](Entity e, Garrison& g, Unit& u, Owner& o, Transform& t) {
        all.push_back({e, g.count + g.pendingOut, o.player, t.pos, R.types[u.type].garrisonDefense});
    });
    std::vector<int> incoming(all.size(), 0);
    w.each<Orders, Owner>([&](Entity, Orders& ord, Owner& o) {
        if (o.player != player_ || ord.queue.empty() || ord.queue.front().type != OrderType::Enter) return;
        for (size_t i = 0; i < all.size(); ++i) if (all[i].e == ord.queue.front().target) incoming[i]++;
    });
    Fixed bestScore{}; int bestT = -1; std::vector<std::pair<Entity, int64_t>> bestSrc;
    for (size_t ti = 0; ti < all.size(); ++ti) {
        const G& t = all[ti];
        if (t.owner == player_ || (t.owner != 0 && sim.allied(t.owner, player_))) continue;
        std::vector<size_t> mine;
        for (size_t i = 0; i < all.size(); ++i) if (all[i].owner == player_ && all[i].count > 3) mine.push_back(i);
        std::sort(mine.begin(), mine.end(), [&](size_t a, size_t b) {
            Fixed da = dist(all[a].pos, t.pos), db = dist(all[b].pos, t.pos);
            return da != db ? da < db : a < b;
        });
        int need = int((Fixed::fromInt(t.count) * t.def * Fixed::fromRatio(13, 10)).toInt()) + 3 - incoming[ti];
        if (t.owner != 0) need += int((dist(all[mine.empty() ? ti : mine[0]].pos, t.pos) / 2).toInt()); // growth while walking
        int sent = 0;
        std::vector<std::pair<Entity, int64_t>> src;
        for (size_t i : mine) {
            if (sent >= need || src.size() >= 3) break;
            int avail = all[i].count - 2;
            int take = std::min(avail, need - sent);
            if (take <= 0) continue;
            int64_t pct = std::clamp<int64_t>((int64_t(take) * 100 + all[i].count - 1) / all[i].count, 1, 100);
            src.push_back({all[i].e, pct});
            sent += int(int64_t(all[i].count) * pct / 100);
        }
        if (sent < need) continue;
        Fixed d = dist(all[mine.empty() ? ti : mine[0]].pos, t.pos);
        Fixed value = Fixed::fromInt(R.types[w.get<Unit>(t.e).type].garrisonMax + 10) * (t.owner ? cfg_.aggression + Fixed::fromInt(1) : Fixed::fromInt(1));
        Fixed score = value * 100 / (Fixed::fromInt(sent) + d * 2 + Fixed::fromInt(1));
        if (bestT < 0 || score > bestScore) { bestScore = score; bestT = int(ti); bestSrc = src; }
    }
    if (bestT < 0) return;
    for (auto& [e, pct] : bestSrc) {
        Command c; c.type = kCmdSend; c.units = {e}; c.target = all[size_t(bestT)].e; c.param = pct;
        send(sim, c);
    }
}

} // namespace rts::game
