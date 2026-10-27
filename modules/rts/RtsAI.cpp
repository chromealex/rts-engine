#include "RtsAI.h"
#include <algorithm>
#include <vector>

namespace rts::game {

namespace {
Fixed dist(Vec3 a, Vec3 b) { return (a - b).xz().length(); }
bool isCombat(const UnitType& t) { return !t.structure && (!t.weapons.empty() || !t.abilities.empty()) && !t.worker; }
bool positiveIncome(const UnitType& t) {
    for (auto& a : t.income) if (a.value.raw > 0) return true;
    return false;
}
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
    auto* gs = sim.world().resource<GameState>();
    if (std::find(gs->wavePlayers.begin(), gs->wavePlayers.end(), player_) != gs->wavePlayers.end()) return; // scripted side
    bool hasGarrisons = false, hasOther = false;
    sim.world().each<Unit, Owner>([&](Entity e, Unit& u, Owner& o) {
        if (o.player != player_) return;
        const UnitType& t = m->rules().types[u.type];
        if (sim.world().has<Garrison>(e) && t.spawnUnit != kNoType) hasGarrisons = true;
        else hasOther = true;
    });
    if (hasGarrisons) garrisonPlay(sim, *m);
    if (hasOther) { special(sim, *m); economy(sim, *m); military(sim, *m); }
}

bool RtsAI::train(Simulation& sim, RtsModule& m, uint16_t type) {
    World& w = sim.world();
    const Rules& R = m.rules();
    const UnitType& ut = R.types[type];
    if (!m.canTrain(sim, player_, type) || !m.canAfford(sim, player_, R.costModel == CostModel::Upfront ? ut.cost : std::vector<Amount>{}, ut.supplyUsed)) return false;
    Entity best;
    size_t bestQ = 99;
    w.each<Production, Unit, Owner>([&](Entity e, Production& p, Unit& u, Owner& o) {
        if (o.player != player_ || (u.flags & kUnitUnderConstruction)) return;
        const UnitType& pt = R.types[u.type];
        if (std::find(pt.produces.begin(), pt.produces.end(), type) == pt.produces.end()) return;
        if (pt.chargesMax > 0 && p.charges <= 0) return;
        if (ut.structure && p.ready != kNoType) return;
        if (p.queue.size() < bestQ) { bestQ = p.queue.size(); best = e; }
    });
    if (!best.valid() || bestQ >= 2) return false;
    Command c; c.type = kCmdTrain; c.units = {best}; c.param = type;
    send(sim, c);
    return true;
}

bool RtsAI::build(Simulation& sim, RtsModule& m, uint16_t type) {
    World& w = sim.world();
    const Rules& R = m.rules();
    const UnitType& bt = R.types[type];
    if (!m.canTrain(sim, player_, type)) return false;
    if (R.costModel == CostModel::Upfront && !m.canAfford(sim, player_, bt.cost)) return false;
    // sidebar games: structures come out of a producer
    bool sidebar = false;
    w.each<Unit, Owner>([&](Entity, Unit& u, Owner& o) {
        if (o.player == player_ && std::find(R.types[u.type].produces.begin(), R.types[u.type].produces.end(), type) != R.types[u.type].produces.end()) sidebar = true;
    });
    if (sidebar) { if (train(sim, m, type)) { lastBuildTick_ = sim.tick(); return true; } return false; }

    Entity builder;
    Vec3 bpos{};
    int bestScore = 1 << 30;
    w.each<Unit, Owner, Orders, Transform>([&](Entity e, Unit& u, Owner& o, Orders& ord, Transform& tr) {
        if (o.player != player_) return;
        const UnitType& t = R.types[u.type];
        if (std::find(t.builds.begin(), t.builds.end(), type) == t.builds.end()) return;
        int score = ord.queue.empty() ? 0 : ord.queue.front().type == OrderType::Build ? 1 << 20 : 10;
        if (score < bestScore) { bestScore = score; builder = e; bpos = tr.pos; }
    });
    if (!builder.valid() || bestScore >= (1 << 20)) return false;
    Vec3 spot;
    if (bt.placeOn != kNoType) {
        // nearest free node (geyser, metal spot) to the builder
        bool found = false;
        Fixed bestD{};
        w.each<Unit, Transform>([&](Entity, Unit& u, Transform& tr) {
            if (u.type != bt.placeOn) return;
            if (!m.canPlace(sim, player_, type, tr.pos, builder)) return;
            Fixed d = dist(tr.pos, bpos);
            if (!found || d < bestD) { bestD = d; spot = tr.pos; found = true; }
        });
        if (!found) return false;
    } else if (!m.findAutoSpot(sim, player_, type, spot)) {
        return false;
    }
    Command c;
    c.type = kCmdBuild; c.units = {builder}; c.param = type; c.point = spot;
    send(sim, c);
    lastBuildTick_ = sim.tick();
    return true;
}

void RtsAI::special(Simulation& sim, RtsModule& m) {
    World& w = sim.world();
    const Rules& R = m.rules();
    GameState& gs = m.state(sim);
    auto& eco = gs.players[player_];
    int structures = 0;
    w.each<Unit, Owner>([&](Entity, Unit& u, Owner& o) { if (o.player == player_ && R.types[u.type].structure) ++structures; });
    w.each<Unit, Owner, Transform>([&](Entity e, Unit& u, Owner& o, Transform& tr) {
        if (o.player != player_) return;
        const UnitType& t = R.types[u.type];
        // MCV: deploy where it stands (or nudge it if blocked)
        if (t.deploysTo != kNoType && structures == 0) {
            if (m.canPlace(sim, player_, t.deploysTo, tr.pos, e)) { Command c; c.type = kCmdDeploy; c.units = {e}; send(sim, c); }
            else if (auto* ord = w.tryGet<Orders>(e); ord && ord->queue.empty()) {
                Command c; c.type = kCmdMove; c.units = {e};
                c.point = tr.pos + Vec3{Fixed::fromInt(int(rng_.below(7)) - 3), Fixed{}, Fixed::fromInt(int(rng_.below(7)) - 3)};
                send(sim, c);
            }
        }
        // sidebar: place finished structures
        if (auto* p = w.tryGet<Production>(e); p && p->ready != kNoType) {
            Vec3 spot;
            if (m.findAutoSpot(sim, player_, p->ready, spot)) { Command c; c.type = kCmdPlace; c.units = {e}; c.point = spot; send(sim, c); }
        }
        // squads: reinforce when weakened
        if (t.members > 1 && m.membersAlive(sim, e) < t.members && m.canAfford(sim, player_, t.reinforceCost)) {
            Command c; c.type = kCmdReinforce; c.units = {e}; send(sim, c);
        }
        // in-place upgrades (structure levels)
        if (!t.upgrades.empty() && !(u.flags & kUnitUnderConstruction) && rng_.below(4) == 0) {
            uint16_t up = t.upgrades[0];
            if (m.requirementsMet(sim, player_, R.types[up].requires_) && m.canAfford(sim, player_, R.types[up].cost)) {
                Command c; c.type = kCmdUpgrade; c.units = {e}; c.param = up; send(sim, c);
            }
        }
    });
    // lane battler: press BUILD whenever affordable
    if (!R.autoBuildPool.empty()) {
        for (uint16_t t : R.autoBuildPool)
            if (m.canTrain(sim, player_, t) && m.canAfford(sim, player_, m.autoBuildCost(sim, player_, t))) {
                Command c; c.type = kCmdAutoBuild; c.param = -1; send(sim, c);
                break;
            }
    }
    // regions (Northgard): claim the cheapest neighbouring region we can afford
    if (R.regions.enabled && gs.regionCount > 0) {
        for (int ri = 0; ri < gs.regionCount; ++ri) {
            if (!m.canClaim(sim, player_, ri)) continue;
            Command c; c.type = kCmdClaim; c.point = gs.regionCenter[size_t(ri)]; send(sim, c);
            break;
        }
    }
    // research: something affordable every now and then
    if (!R.techs.empty() && sim.tick() > lastTechTick_ + 300) {
        w.each<Production, Unit, Owner>([&](Entity e, Production& p, Unit& u, Owner& o) {
            if (o.player != player_ || !p.queue.empty() || (u.flags & kUnitUnderConstruction) || sim.tick() <= lastTechTick_ + 300) return;
            for (uint16_t k : R.types[u.type].researches) {
                if (eco.researched[k] || eco.researching[k] || !m.requirementsMet(sim, player_, R.techs[k].requires_) || !m.canAfford(sim, player_, R.techs[k].cost)) continue;
                Command c; c.type = kCmdResearch; c.units = {e}; c.param = k; send(sim, c);
                lastTechTick_ = sim.tick();
                break;
            }
        });
    }
}

void RtsAI::economy(Simulation& sim, RtsModule& m) {
    World& w = sim.world();
    const Rules& R = m.rules();
    auto& eco = m.state(sim).players[player_];
    std::vector<int> count(R.types.size(), 0);
    std::vector<Entity> idleWorkers;
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
    });

    // 1) idle workers gather the resource we have least of (nodes or fields)
    for (Entity wk : idleWorkers) {
        const UnitType& t = R.types[w.get<Unit>(wk).type];
        if (t.gathers.empty()) continue;
        Vec3 p = w.get<Transform>(wk).pos;
        Entity best;
        Vec3 bestField{};
        bool field = false;
        Fixed bestScore{};
        bool any = false;
        for (auto& g : t.gathers) {
            Fixed have = eco.stock[g.res];
            if (R.resources[g.res].field) {
                Vec3 cell;
                if (m.findNearestFieldCell(sim, p, g.res, cell, 40).valid()) {
                    Fixed score = dist(cell, p) * 4 + have / 20;
                    if (!any || score < bestScore) { any = true; bestScore = score; field = true; bestField = cell; best = kNullEntity; }
                }
                continue;
            }
            w.each<ResourceStock, Unit, Owner, Transform>([&](Entity n, ResourceStock& rs, Unit& nu, Owner& no, Transform& nt) {
                if (rs.res != g.res || (!rs.infinite && rs.amount.raw <= 0) || (nu.flags & kUnitUnderConstruction)) return;
                const UnitType& ntp = R.types[nu.type];
                if (ntp.ownerOnly && no.player != player_) return;
                Fixed score = dist(nt.pos, p) * 4 + have / 20;
                if (!any || score < bestScore) { any = true; best = n; bestScore = score; field = false; }
            });
        }
        if (!any) continue;
        Command c; c.type = kCmdGather; c.units = {wk};
        if (field) c.point = bestField; else c.target = best;
        send(sim, c);
    }

    const bool canBuildNow = sim.tick() > lastBuildTick_ + 60;
    // 2) supply (structures like farms/depots, or supply units like overlords)
    Fixed freeSupply = eco.supplyCap - eco.supplyUsed;
    if (eco.supplyCap < R.supplyCap && freeSupply < Fixed::fromInt(4) && canBuildNow) {
        for (size_t ti = 0; ti < R.types.size(); ++ti) {
            const UnitType& t = R.types[ti];
            if (t.supplyProvided.raw <= 0 || !t.produces.empty() || t.key) continue;
            if (t.structure ? build(sim, m, uint16_t(ti)) : train(sim, m, uint16_t(ti))) return;
        }
    }
    // 3) capacity (power): keep provided above consumed
    for (size_t r = 0; r < R.resources.size(); ++r) {
        if (R.resources[r].kind != ResourceKind::Capacity || !canBuildNow) continue;
        if (eco.capProvided[r] - eco.capUsed[r] > Fixed::fromInt(15)) continue;
        for (size_t ti = 0; ti < R.types.size(); ++ti) {
            const UnitType& t = R.types[ti];
            bool gives = false;
            for (auto& a : t.provides) if (a.res == r) gives = true;
            if (gives && t.structure && build(sim, m, uint16_t(ti))) return;
        }
    }
    // 4) income structures (TA metal extractors / energy, refineries on geysers, farms)
    for (size_t ti = 0; ti < R.types.size(); ++ti) {
        const UnitType& t = R.types[ti];
        if (!t.structure || !canBuildNow) continue;
        bool incomeLike = positiveIncome(t) || (t.placeOn != kNoType && t.takesOverNode) || (t.resourceNode >= 0 && t.ownerOnly);
        if (!incomeLike || count[ti] >= (t.placeOn != kNoType ? 8 : 6)) continue;
        if (t.placeOn != kNoType && workers < 8 && R.types[t.placeOn].resourceNode >= 0) continue; // geysers later
        if (build(sim, m, uint16_t(ti))) return;
    }
    // 5) dropoffs near far resources are skipped for simplicity; storage when capped
    for (size_t r = 0; r < R.resources.size(); ++r) {
        const ResourceDef& rd = R.resources[r];
        bool capped = rd.kind == ResourceKind::Flow || rd.capped;
        if (!capped || eco.cap[r].raw <= 0 || eco.stock[r] < eco.cap[r] * Fixed::fromRatio(9, 10) || !canBuildNow) continue;
        for (size_t ti = 0; ti < R.types.size(); ++ti) {
            const UnitType& t = R.types[ti];
            bool stores = false;
            for (auto& a : t.storage) if (a.res == r) stores = true;
            if (stores && t.structure && t.produces.empty() && build(sim, m, uint16_t(ti))) return;
        }
    }
    // 6) workers
    if (workers < cfg_.workersPerBase) {
        for (size_t ti = 0; ti < R.types.size(); ++ti)
            if (R.types[ti].worker && !R.types[ti].structure && R.types[ti].deploysTo == kNoType && train(sim, m, uint16_t(ti))) break;
    }
    // 7) military production buildings (and the tech they require)
    if (sim.tick() > lastBuildTick_ + 100 && building == 0) {
        int milProducers = 0;
        std::vector<uint16_t> candidates;
        for (size_t ti = 0; ti < R.types.size(); ++ti) {
            const UnitType& t = R.types[ti];
            if (!t.structure) continue;
            bool makesArmy = false;
            for (uint16_t u : t.produces) if (isCombat(R.types[u])) makesArmy = true;
            if (t.waveUnit != kNoType) makesArmy = true;
            if (makesArmy) { milProducers += count[ti]; candidates.push_back(uint16_t(ti)); }
        }
        int want = 1 + workers / 6;
        if (milProducers < want && !candidates.empty()) {
            uint16_t pick = candidates[rng_.below(uint32_t(candidates.size()))];
            if (!m.requirementsMet(sim, player_, R.types[pick].requires_)) {
                for (auto& rq : R.types[pick].requires_)
                    if (!rq.tech && count[rq.index] == 0 && build(sim, m, rq.index)) return;
            } else if (build(sim, m, pick)) return;
        }
    }
}

void RtsAI::military(Simulation& sim, RtsModule& m) {
    World& w = sim.world();
    const Rules& R = m.rules();
    GameState& gs = m.state(sim);
    std::vector<Entity> idleArmy, army;
    Vec3 home{};
    bool hasHome = false;
    w.each<Unit, Owner, Transform>([&](Entity e, Unit& u, Owner& o, Transform& tr) {
        if (o.player != player_) return;
        const UnitType& t = R.types[u.type];
        if (t.structure && (!hasHome || t.key)) { home = tr.pos; hasHome = true; }
        if (isCombat(t)) {
            army.push_back(e);
            auto* ord = w.tryGet<Orders>(e);
            if (ord && ord->queue.empty()) idleArmy.push_back(e);
        }
        if (auto* prod = w.tryGet<Production>(e); prod && prod->queue.size() < 2 && !(u.flags & kUnitUnderConstruction)) {
            std::vector<uint16_t> opts;
            for (uint16_t ut : t.produces) {
                const UnitType& cand = R.types[ut];
                if (!isCombat(cand) || !m.canTrain(sim, player_, ut)) continue;
                if (!m.canAfford(sim, player_, R.costModel == CostModel::Upfront ? cand.cost : std::vector<Amount>{}, cand.supplyUsed)) continue;
                if (t.chargesMax > 0 && prod->charges <= 0) continue;
                opts.push_back(ut);
                if (cand.hero) { opts.assign(1, ut); break; } // heroes first
            }
            if (!opts.empty()) { Command c; c.type = kCmdTrain; c.units = {e}; c.param = opts[rng_.below(uint32_t(opts.size()))]; send(sim, c); }
        }
    });
    // tickets: grab victory points first
    if (gs.victory.mode == VictoryMode::Tickets && !idleArmy.empty()) {
        Entity best; Fixed bestD{};
        Vec3 from = w.get<Transform>(idleArmy[0]).pos;
        w.each<Capture, Unit, Owner, Transform>([&](Entity e, Capture&, Unit&, Owner& o, Transform& t) {
            if (o.player == player_ || (o.player != 0 && sim.allied(o.player, player_))) return;
            Fixed d = dist(t.pos, from);
            if (!best.valid() || d < bestD) { best = e; bestD = d; }
        });
        if (best.valid()) {
            std::vector<Entity> squad(idleArmy.begin(), idleArmy.begin() + std::min<long>(4, long(idleArmy.size())));
            Command c; c.type = kCmdAttack; c.units = squad; c.point = w.get<Transform>(best).pos; send(sim, c);
            idleArmy.erase(idleArmy.begin(), idleArmy.begin() + long(squad.size()));
        }
    }
    const bool defend = gs.victory.mode == VictoryMode::Survive;
    int needed = defend ? 1 : cfg_.firstAttackArmy + wave_ * cfg_.armyGrowth;
    if (int(idleArmy.size()) < needed) return;
    Vec3 c{};
    for (Entity e : idleArmy) c += w.get<Transform>(e).pos;
    c = c / Fixed::fromInt(int64_t(idleArmy.size()));
    Entity best;
    Fixed bestD{};
    w.each<Unit, Owner, Transform>([&](Entity e, Unit& u, Owner& o, Transform& t) {
        if (!m.hostile(sim, player_, o.player) || u.hp.raw <= 0) return;
        if (defend && hasHome && dist(t.pos, home) > Fixed::fromInt(25)) return;
        Fixed d = dist(t.pos, c) - (R.types[u.type].structure ? Fixed::fromInt(5) : Fixed{});
        if (o.player == kHostileNeutral) d += Fixed::fromInt(10); // creeps only when they are on the way
        if (!best.valid() || d < bestD) { best = e; bestD = d; }
    });
    if (!best.valid()) return;
    Command cmd;
    cmd.type = kCmdAttack;
    cmd.units = idleArmy;
    cmd.point = w.get<Transform>(best).pos; // attack-move
    send(sim, cmd);
    if (!defend) ++wave_;
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
    Fixed bestScore{};
    int bestT = -1;
    std::vector<std::pair<Entity, int64_t>> bestSrc;
    for (size_t ti = 0; ti < all.size(); ++ti) {
        const G& t = all[ti];
        if (t.owner == player_ || (t.owner != 0 && t.owner != kHostileNeutral && sim.allied(t.owner, player_))) continue;
        std::vector<size_t> mine;
        for (size_t i = 0; i < all.size(); ++i) if (all[i].owner == player_ && all[i].count > 3) mine.push_back(i);
        std::sort(mine.begin(), mine.end(), [&](size_t a, size_t b) {
            Fixed da = dist(all[a].pos, t.pos), db = dist(all[b].pos, t.pos);
            return da != db ? da < db : a < b;
        });
        int need = int((Fixed::fromInt(t.count) * t.def * Fixed::fromRatio(13, 10)).toInt()) + 3 - incoming[ti];
        if (t.owner != 0) need += int((dist(all[mine.empty() ? ti : mine[0]].pos, t.pos) / 2).toInt());
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
