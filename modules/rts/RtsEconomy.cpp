// Economy: resources (stock / flow / capacity), supply and upkeep, resource fields, production
// queues (units, research, sidebar structures, larva charges, in-place upgrades), construction,
// squads reinforcement and Mushroom Wars style garrisons.
#include "RtsInternal.h"
#include <algorithm>

namespace rts::game {
using namespace detail;

namespace {
// Progressive cost (Total Annihilation): pay `cost * step` now. When short of a resource the step
// shrinks to what can be afforded. Returns the step actually achieved.
Fixed payProgressive(const Rules& rules, PlayerEconomy& eco, const std::vector<Amount>& cost, Fixed step) {
    if (step.raw <= 0) return Fixed{};
    Fixed ratio = kOne;
    for (auto& c : cost) {
        if (rules.resources[c.res].kind == ResourceKind::Capacity || c.value.raw <= 0) continue;
        Fixed need = c.value * step;
        if (need.raw <= 0) continue;
        if (eco.stock[c.res] < need) ratio = fxMin(ratio, fxMax(Fixed{}, eco.stock[c.res]) / need);
    }
    if (ratio.raw <= 0) return Fixed{};
    for (auto& c : cost)
        if (rules.resources[c.res].kind != ResourceKind::Capacity) eco.stock[c.res] -= c.value * step * ratio;
    return step * ratio;
}
} // namespace

// ================================================================= economy

void RtsModule::sysEconomy(Simulation& sim) {
    World& w = sim.world();
    GameState& gs = state(sim);
    const Fixed dt = sim.dt();
    const size_t nr = rules_.resources.size();
    for (auto& p : gs.players) {
        std::fill(p.income.begin(), p.income.end(), Fixed{});
        std::fill(p.capProvided.begin(), p.capProvided.end(), Fixed{});
        std::fill(p.capUsed.begin(), p.capUsed.end(), Fixed{});
        std::fill(p.typeCount.begin(), p.typeCount.end(), 0);
        for (size_t r = 0; r < nr; ++r) p.cap[r] = rules_.resources[r].storage;
        p.supplyUsed = Fixed{};
        p.supplyCap = Fixed{};
    }
    w.each<Unit, Owner>([&](Entity e, Unit& u, Owner& o) {
        const UnitType& t = rules_.types[u.type];
        const bool building = u.flags & kUnitUnderConstruction;
        if (!building && u.hp.raw > 0) {
            if (t.regen.raw > 0 && u.hp < t.maxHp) u.hp = fxMin(stat(sim, e, Stat::MaxHp), u.hp + t.regen * dt);
            if (t.maxEnergy.raw > 0) u.energy = fxMin(t.maxEnergy, u.energy + t.energyRegen * (rules_.advancedEffects ? stat(sim, e, Stat::EnergyRegen) : kOne) * dt);
        }
        if (o.player == 0 || o.player >= gs.players.size()) return;
        auto& p = gs.players[o.player];
        p.supplyUsed += t.supplyUsed;
        if (auto* prod = w.tryGet<Production>(e))
            for (auto& it : prod->queue) if (it.item >= 0 && !it.upgrade) p.supplyUsed += rules_.types[size_t(it.item)].supplyUsed;
        if (building) return;
        p.typeCount[u.type]++;
        // power deficits switch off tagged objects (defences, radar, factories)
        const bool off = (t.tags & p.disabledTags) != 0;
        if (off) u.flags |= kUnitDisabled; else u.flags &= uint8_t(~kUnitDisabled);
        p.supplyCap += t.supplyProvided;
        for (auto& a : t.storage) p.cap[a.res] += a.value;
        for (auto& a : t.provides) p.capProvided[a.res] += a.value;
        for (auto& a : t.consumes) p.capUsed[a.res] += a.value;
        if (!off) for (auto& a : t.income) p.income[a.res] += a.value;
    });
    if (rules_.dungeon) roomEconomy(sim); // painted rooms add supply, storage, capacity and income per cell
    for (size_t pi = 1; pi < gs.players.size(); ++pi) {
        auto& p = gs.players[pi];
        p.supplyCap = fxMin(p.supplyCap, rules_.supplyCap);
        // upkeep: gathering yields less with a large army (Warcraft III)
        p.gatherMult = kOne;
        for (auto& tier : rules_.upkeep) if (p.supplyUsed >= tier.supply) p.gatherMult = tier.gatherMult;
        // capacity (power): shortage slows production and disables tagged objects
        Fixed mult = kOne;
        uint64_t disabled = 0;
        for (size_t r = 0; r < nr; ++r) {
            const ResourceDef& rd = rules_.resources[r];
            if (rd.kind == ResourceKind::Capacity) {
                p.stock[r] = p.capProvided[r] - p.capUsed[r];
                if (p.capUsed[r] > p.capProvided[r]) { mult = fxMin(mult, rd.deficitProductionMult); disabled |= rd.deficitDisableTags; }
                continue;
            }
            if (isGood(uint16_t(r))) continue; // goods: income goes through the storehouses (syncStorehouses)
            if (rules_.advancedEffects) {
                // player regen (mana, morale) and income multipliers from player statuses (freeze regen, blessings)
                Fixed regen = (p.statusFlags & kStFreezeRegen) ? Fixed{} : rd.regen;
                Fixed mult = kOne;
                for (auto& in : p.statuses) for (auto& a : rules_.statuses[in.def].incomeMult) if (a.res == r) mult = mult * a.value;
                p.income[r] = (p.income[r] + regen) * mult;
            }
            p.stock[r] = fxMax(Fixed{}, p.stock[r] + p.income[r] * dt);
            // a drained flow resource (TA energy stall) also slows everything down
            if (rd.kind == ResourceKind::Flow && p.stock[r].raw == 0 && p.income[r].raw < 0) {
                mult = fxMin(mult, rd.deficitProductionMult);
                disabled |= rd.deficitDisableTags;
            }
            bool capped = rd.kind == ResourceKind::Flow || rd.capped;
            if (capped && p.cap[r].raw > 0) p.stock[r] = fxMin(p.stock[r], p.cap[r]);
        }
        if (rules_.regions.productionPerRegion.raw > 0 && gs.regionCount > 0) {
            p.regions = 0;
            for (auto o : gs.regionOwner) if (o == pi) ++p.regions;
            mult = mult * (kOne + rules_.regions.productionPerRegion * std::max(0, p.regions - 1));
        }
        if ((mult < kOne) != (p.productionMult < kOne) && mult < kOne)
            sim.emit({kEvLowPower, uint8_t(pi), kNullEntity, kNullEntity, 0, {}});
        p.productionMult = mult;
        p.disabledTags = disabled;
    }
    if (rules_.buildingStorage) syncStorehouses(sim);
}

// ================================================================= resource fields

void RtsModule::sysFields(Simulation& sim) {
    constexpr uint32_t kEvery = 10;
    if (sim.tick() % kEvery != 0) return;
    World& w = sim.world();
    GameState& gs = state(sim);
    MapGrid& g = gs.grid;
    const Fixed dt = sim.dt() * int64_t(kEvery);
    // regrowth of existing fields (Red Alert ore, Dune spice blooms)
    bool anyRegrow = false;
    for (auto& rd : rules_.resources) if (rd.field && rd.fieldRegrow.raw > 0) anyRegrow = true;
    if (anyRegrow) {
        for (size_t i = 0; i < g.fieldRes.size(); ++i) {
            int16_t r = g.fieldRes[i];
            if (r < 0) continue;
            const ResourceDef& rd = rules_.resources[size_t(r)];
            if (rd.fieldRegrow.raw <= 0 || g.fieldAmt[i] >= rd.fieldCellMax || g.blocked[i]) continue;
            g.fieldAmt[i] = fxMin(rd.fieldCellMax, g.fieldAmt[i] + rd.fieldRegrow * dt);
            gs.fieldsDirty = true;
        }
    }
    // spawners grow new field cells around them (ore mines)
    w.each<Unit, Transform>([&](Entity, Unit& u, Transform& tr) {
        const UnitType& t = rules_.types[u.type];
        if (t.fieldSpawnRes < 0 || (u.flags & kUnitUnderConstruction)) return;
        const ResourceDef& rd = rules_.resources[size_t(t.fieldSpawnRes)];
        int r = t.fieldSpawnRadius;
        int cx, cy; g.cellXY(tr.pos, cx, cy);
        int x = cx + sim.rng().range(-r, r), y = cy + sim.rng().range(-r, r);
        if (!g.inside(x, y)) return;
        size_t i = size_t(g.index(x, y));
        if (!(g.flags[i] & kCellGround) || g.blocked[i] || (g.fieldRes[i] >= 0 && g.fieldRes[i] != t.fieldSpawnRes)) return;
        g.fieldRes[i] = t.fieldSpawnRes;
        g.fieldAmt[i] = fxMin(rd.fieldCellMax, g.fieldAmt[i] + t.fieldSpawnRate * dt);
        gs.fieldsDirty = true;
    });
}

// ================================================================= production

namespace {
void ensureTypeComponents(World& w, Entity e, const UnitType& t) {
    if (!t.weapons.empty() && !w.has<Combat>(e)) w.add<Combat>(e);
    if ((!t.produces.empty() || !t.researches.empty() || !t.upgrades.empty()) && !w.has<Production>(e)) w.add<Production>(e);
    if ((t.garrisonMax > 0 || t.spawnUnit != kNoType) && !w.has<Garrison>(e)) w.add<Garrison>(e);
    if (!t.abilities.empty() && !w.has<AbilityState>(e)) w.add<AbilityState>(e);
}
} // namespace

void RtsModule::sysProduction(Simulation& sim) {
    World& w = sim.world();
    GameState& gs = state(sim);
    const Fixed dt = sim.dt();
    // Spawning and component changes may reallocate pools: collect, then apply after the loop.
    struct Done { Entity producer; int32_t item; uint8_t owner; bool upgrade; };
    std::vector<Done> done;
    w.each<Production, Unit, Owner>([&](Entity e, Production& p, Unit& u, Owner& o) {
        const UnitType& t = rules_.types[u.type];
        if (u.flags & kUnitUnderConstruction) return;
        // larva: charges regenerate up to the maximum
        if (t.chargesMax > 0 && p.charges + int(p.queue.size()) < t.chargesMax) {
            p.chargeTimer += dt;
            if (p.chargeTimer >= t.chargeInterval) { p.chargeTimer = Fixed{}; ++p.charges; }
        }
        if (p.queue.empty() || (u.flags & kUnitDisabled) || o.player == 0 || o.player >= gs.players.size()) return;
        auto& eco = gs.players[o.player];
        const bool parallel = t.chargesMax > 0;
        const size_t n = parallel ? p.queue.size() : 1;
        for (size_t k = 0; k < n; ++k) {
            ProdItem& it = p.queue[k];
            const bool isTech = it.item < 0;
            if (!isTech && !it.upgrade && rules_.types[size_t(it.item)].structure && p.ready != kNoType) continue; // place first
            Fixed time = isTech ? rules_.techs[size_t(~it.item)].time : rules_.types[size_t(it.item)].buildTime;
            if (it.upgrade && rules_.advancedEffects) time = time * stat(sim, e, Stat::UpgradeCost);
            const std::vector<Amount> cost = isTech ? techCost(sim, o.player, uint16_t(~it.item)) : rules_.types[size_t(it.item)].cost;
            Fixed step = dt * eco.productionMult * stat(sim, e, Stat::ProductionSpeed) / fxMax(time, Fixed::fromRatio(1, 10));
            if (rules_.costModel == CostModel::Progressive) {
                step = fxMin(step, kOne - it.paid);
                step = payProgressive(rules_, eco, cost, step);
                it.paid += step;
            }
            it.progress += step;
            if (it.progress >= kOne) done.push_back({e, it.item, o.player, it.upgrade});
        }
    });
    for (auto& d : done) {
        if (!w.alive(d.producer)) continue;
        auto& p = w.get<Production>(d.producer);
        // remove the finished item (first match with full progress)
        for (size_t k = 0; k < p.queue.size(); ++k)
            if (p.queue[k].item == d.item && p.queue[k].progress >= kOne) { p.queue.erase(p.queue.begin() + long(k)); break; }
        auto& eco = gs.players[d.owner];
        if (d.item < 0) {
            uint16_t k = uint16_t(~d.item);
            if (eco.researched[k] < 255) eco.researched[k]++;
            eco.researching[k] = 0;
            recomputeMods(sim, d.owner);
            sim.emit({kEvResearched, d.owner, d.producer, kNullEntity, k, w.get<Transform>(d.producer).pos});
            continue;
        }
        const uint16_t type = uint16_t(d.item);
        const UnitType& nt = rules_.types[type];
        if (d.upgrade) {
            Unit& u = w.get<Unit>(d.producer);
            const UnitType& ot = rules_.types[u.type];
            Fixed frac = u.hp / fxMax(ot.maxHp, kOne);
            eco.typeCountAll[u.type]--;
            eco.typeCountAll[type]++;
            if (auto* f = w.tryGet<Footprint>(d.producer); f && (f->w != nt.footprintW || f->h != nt.footprintH)) {
                gs.grid.block(f->x, f->y, f->w, f->h, -1);
                Vec3 c = w.get<Transform>(d.producer).pos;
                footprintAt(nt, c, f->x, f->y);
                f->w = nt.footprintW; f->h = nt.footprintH;
                gs.grid.block(f->x, f->y, f->w, f->h, +1);
            }
            u.type = type;
            u.hp = nt.maxHp * frac;
            ensureTypeComponents(w, d.producer, nt);
            sim.emit({kEvUpgraded, d.owner, d.producer, kNullEntity, type, w.get<Transform>(d.producer).pos});
            continue;
        }
        if (nt.structure) {
            // sidebar construction (Red Alert / C&C): the structure waits to be placed
            p.ready = type;
            eco.typeCountAll[type]--; // counted again by spawn() when placed
            sim.emit({kEvReadyToPlace, d.owner, d.producer, kNullEntity, type, w.get<Transform>(d.producer).pos});
            continue;
        }
        Vec3 spot = freeSpotNear(sim, d.producer, nt.pathLayer);
        eco.typeCountAll[type]--; // counted while queued; spawn() counts the unit itself
        Entity n = spawn(sim, type, d.owner, spot);
        sim.emit({kEvTrainDone, d.owner, n, d.producer, type, spot});
        auto& pp = w.get<Production>(d.producer);
        if (pp.hasRally && w.has<Orders>(n)) {
            Command c;
            c.player = d.owner; c.type = kCmdSmart; c.units = {n}; c.point = pp.rally;
            c.target = w.alive(pp.rallyTarget) ? pp.rallyTarget : kNullEntity;
            applyCommand(sim, c);
        }
    }
    // squad reinforcement: one member at a time
    w.each<Squad, Unit, Owner>([&](Entity e, Squad& sq, Unit& u, Owner&) {
        if (!sq.reinforcing) return;
        sq.reinforceTimer -= dt;
        if (sq.reinforceTimer.raw > 0) return;
        const UnitType& t = rules_.types[u.type];
        Fixed per = stat(sim, e, Stat::MaxHp) / t.members;
        int alive = membersAlive(sim, e);
        u.hp = fxMin(stat(sim, e, Stat::MaxHp), per * (alive + 1));
        sq.reinforcing = false;
    });
}

// ================================================================= construction

void RtsModule::sysConstruction(Simulation& sim) {
    World& w = sim.world();
    GameState& gs = state(sim);
    const Fixed dt = sim.dt();
    // builder power per construction site (Walk mode; several builders add up)
    std::vector<std::pair<uint64_t, Fixed>> power;
    w.each<Orders, Unit>([&](Entity b, Orders& ord, Unit& bu) {
        if (ord.queue.empty()) return;
        const Order& o = ord.queue.front();
        if (o.type != OrderType::Build || o.state != 2 || !w.alive(o.target)) return;
        if (contactDistance(sim, b, o.target) > Fixed::fromRatio(8, 10)) return;
        Fixed pw = rules_.types[bu.type].buildPower;
        for (auto& pr : power) if (pr.first == o.target.packed()) { pr.second += pw; return; }
        power.push_back({o.target.packed(), pw});
    });
    std::vector<Entity> finished;
    w.each<Construction, Unit, Owner>([&](Entity e, Construction& c, Unit& u, Owner& o) {
        const UnitType& t = rules_.types[u.type];
        Fixed pw = c.selfBuild ? kOne : Fixed{};
        for (auto& pr : power) if (pr.first == e.packed()) pw += pr.second;
        if (pw.raw <= 0) return;
        Fixed mult = o.player < gs.players.size() ? gs.players[o.player].productionMult : kOne;
        Fixed step = dt * pw * mult / fxMax(t.buildTime, Fixed::fromRatio(1, 10));
        step = fxMin(step, kOne - c.progress);
        if (auto* inv = w.tryGet<Inventory>(e); inv && rules_.buildingStorage) {
            // settlers: progress can't outrun the delivered materials
            Fixed frac = kOne;
            for (auto& a : t.cost) if (isGood(a.res) && a.value.raw > 0) frac = fxMin(frac, inv->have[a.res] / a.value);
            step = fxMin(step, frac - c.progress);
            if (step.raw <= 0) return;
        }
        if (rules_.costModel == CostModel::Progressive && o.player != 0 && o.player < gs.players.size()) {
            Fixed want = fxMin(step, kOne - c.paid);
            Fixed got = payProgressive(rules_, gs.players[o.player], t.cost, want);
            c.paid += got;
            step = fxMin(step, c.paid - c.progress);
            if (step.raw <= 0) return;
        }
        c.progress += step;
        u.hp = fxMin(t.maxHp, u.hp + t.maxHp * step * Fixed::fromRatio(9, 10));
        if (c.progress >= kOne) finished.push_back(e);
    });
    for (Entity e : finished) {
        Unit& u = w.get<Unit>(e);
        u.flags &= uint8_t(~kUnitUnderConstruction);
        w.remove<Construction>(e);
        if (auto* inv = w.tryGet<Inventory>(e)) { // materials are used up
            const UnitType& t = rules_.types[u.type];
            if (t.storeCapacity.raw > 0 || t.recipe.enabled) for (auto& x : inv->have) x = Fixed{};
            else w.remove<Inventory>(e);
        }
        sim.emit({kEvBuildDone, w.get<Owner>(e).player, e, kNullEntity, u.type, w.get<Transform>(e).pos});
    }
}

// ================================================================= garrisons (Mushroom Wars)

void RtsModule::sysGarrison(Simulation& sim) {
    World& w = sim.world();
    const Fixed dt = sim.dt();
    struct Release { Entity from, target; uint16_t unit; uint8_t owner; Fixed radius; uint32_t group = 0; };
    std::vector<Release> releases;
    const bool adv = rules_.advancedEffects;
    w.each<Garrison, Unit, Owner>([&](Entity e, Garrison& g, Unit& u, Owner& o) {
        const UnitType& t = rules_.types[u.type];
        if (u.flags & kUnitUnderConstruction) return;
        if (adv && o.player != 0 && t.spawnInterval.raw > 0) {
            // effects: lock stops production, production speed / cap stats, redirect sends the growth elsewhere
            if (statusFlags(sim, e) & kStLock) return;
            int cap = int(stat(sim, e, Stat::GarrisonCap).floorInt());
            Garrison* into = &g;
            if (auto* st = w.tryGet<Statuses>(e))
                for (auto& in : st->list)
                    if (in.def == uint16_t(rules_.stRedirect) && w.alive(in.link) && w.has<Garrison>(in.link)) into = &w.get<Garrison>(in.link);
            if (into->count < cap || into != &g) {
                g.growth += dt * stat(sim, e, Stat::ProductionSpeed) / t.spawnInterval;
                while (g.growth >= kOne && (into != &g || into->count < cap)) { ++into->count; g.growth -= kOne; }
                if (into == &g && g.count >= cap) g.growth = Fixed{};
            } else if (g.count > cap) {
                g.growth -= dt * t.garrisonDecay;
                while (g.growth <= -kOne && g.count > cap) { --g.count; g.growth += kOne; }
            } else g.growth = Fixed{};
        } else if (o.player != 0 && t.spawnInterval.raw > 0) {
            if (g.count < t.garrisonMax) {
                g.growth += dt / t.spawnInterval;
                while (g.growth >= kOne && g.count < t.garrisonMax) { ++g.count; g.growth -= kOne; }
                if (g.count >= t.garrisonMax) g.growth = Fixed{};
            } else if (g.count > t.garrisonMax) {
                g.growth -= dt * t.garrisonDecay;
                while (g.growth <= -kOne && g.count > t.garrisonMax) { --g.count; g.growth += kOne; }
            } else {
                g.growth = Fixed{};
            }
        }
        // release sent units, a few per tick so they stream out
        for (int k = 0; k < 2 && g.pendingOut > 0; ++k) {
            if (!w.alive(g.pendingTarget) || t.spawnUnit == kNoType) { g.count += g.pendingOut; g.pendingOut = 0; break; }
            --g.pendingOut;
            releases.push_back({e, g.pendingTarget, t.spawnUnit, o.player, t.radius, g.pendingGroup});
        }
    });
    GameState& gs = state(sim);
    auto tagGroup = [&](Entity n, const Release& r) {
        if (!adv) return;
        w.add<GroupTag>(n, GroupTag{r.group, r.from, r.target});
        // contagious statuses (plague) travel with the units sent from an infected structure
        if (auto* st = w.tryGet<Statuses>(r.from)) {
            std::vector<StatusInst> carry;
            for (auto& in : st->list) if (rules_.statuses[in.def].flags & kStSpreads) carry.push_back(in);
            for (auto& in : carry) {
                EffectCtx ctx; ctx.owner = in.owner; ctx.caster = in.source; ctx.sourceType = in.sourceType;
                Fixed left = in.until == UINT32_MAX ? Fixed{} : Fixed::fromInt(int64_t(in.until - sim.tick())) / int64_t(sim.tickRate());
                addStatus(sim, n, in.def, left, ctx);
            }
        }
    };
    for (auto& r : releases) {
        Vec3 from = w.get<Transform>(r.from).pos, to = w.get<Transform>(r.target).pos;
        if (rules_.lanes.enabled) {
            // Mushroom Wars 2: spread over the generated lanes round-robin, follow the lane strictly
            std::vector<int> lanes = lanesBetween(sim, r.from, r.target);
            if (!lanes.empty()) {
                if (gs.laneRoundRobin.size() <= r.from.index) gs.laneRoundRobin.resize(r.from.index + 1, 0);
                uint32_t& rr = gs.laneRoundRobin[r.from.index];
                // order: main, then alternating outwards (0, -1, +1, -2, +2 ...) is the sorted list
                int lane = lanes[rr++ % lanes.size()];
                const Lane& L = gs.lanes[size_t(lane)];
                bool reverse = L.a != r.from;
                // start at the first lane point outside the source footprint
                uint16_t pos = 0;
                auto& f = w.get<Footprint>(r.from);
                while (pos + 1u < L.points.size()) {
                    Vec3 p = L.points[reverse ? L.points.size() - 1 - pos : pos];
                    int cx, cy; gs.grid.cellXY(p, cx, cy);
                    if (cx < f.x || cy < f.y || cx >= f.x + f.w || cy >= f.y + f.h) break;
                    ++pos;
                }
                Vec3 start = L.points[reverse ? L.points.size() - 1 - pos : pos];
                Entity n = spawn(sim, r.unit, r.owner, start);
                if (!w.has<Orders>(n)) continue;
                w.add<LaneWalker>(n, LaneWalker{lane, pos, reverse});
                tagGroup(n, r);
                Order ord;
                ord.type = OrderType::Enter; ord.target = r.target; ord.point = flat(to); ord.state = 3; // 3 = lane walk
                issue(sim, n, ord, false);
                continue;
            }
        }
        Vec3 d = flat(to - from);
        Fixed len = d.length();
        Vec3 start = len.raw > 0 ? from + d * ((r.radius + Fixed::fromRatio(3, 10)) / len) : from;
        Entity n = spawn(sim, r.unit, r.owner, start);
        tagGroup(n, r);
        auto* m = w.tryGet<Mobile>(n);
        if (!m) continue;
        int a = gs.roads.nodeOf(r.from), b = gs.roads.nodeOf(r.target);
        if (a >= 0 && b >= 0) m->roadPath = gs.roads.path(w, a, b);
        Order ord;
        ord.type = OrderType::Enter; ord.target = r.target; ord.point = flat(to);
        ord.state = m->roadPath.empty() ? 0 : 2; // 2 = road route prepared
        issue(sim, n, ord, false);
        w.get<Orders>(n).queue.front().state = ord.state;
    }
}

} // namespace rts::game
