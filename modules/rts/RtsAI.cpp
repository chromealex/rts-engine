#include "RtsAI.h"
#include "RtsAITerrain.h"
#include <algorithm>
#include <vector>
#include <cstdio>
#include <cstdlib>

namespace rts::game {

namespace {
Fixed dist(Vec3 a, Vec3 b) { return Surface::onSphere(a) ? (a - b).length() : (a - b).xz().length(); }
bool isCombat(const UnitType& t) { return !t.structure && (!t.weapons.empty() || !t.abilities.empty()) && !t.worker; }
// Units the player controls by hand (possessed, picked up) or that are leaving are not the AI's.
bool controllable(World& w, Entity e, const Unit& u) {
    if (u.flags & (kUnitOffMap | kUnitPossessed)) return false;
    auto* n = w.tryGet<Needs>(e);
    return !n || !n->leaving;
}
// Idle for the AI: no orders, or only interruptible creature work.
bool idleOrders(const Orders& ord) {
    return ord.queue.empty() || (ord.queue.front().type == OrderType::Need && (ord.queue.front().param & 0xFF) == kNeedWork);
}
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
    applyRulesConfig(*m);
    auto* gs = sim.world().resource<GameState>();
    if (std::find(gs->wavePlayers.begin(), gs->wavePlayers.end(), player_) != gs->wavePlayers.end()) return; // scripted side
    bool hasGarrisons = false, hasOther = false;
    sim.world().each<Unit, Owner>([&](Entity e, Unit& u, Owner& o) {
        if (o.player != player_) return;
        const UnitType& t = m->rules().types[u.type];
        if (sim.world().has<Garrison>(e) && t.spawnUnit != kNoType) hasGarrisons = true;
        else hasOther = true;
    });
    if (m->rules().originals) originals(sim, *m);
    if (m->rules().fidelity && hasOther) fidelity(sim, *m);
    if (hasGarrisons) garrisonPlay(sim, *m);
    if (hasOther) {
        special(sim, *m);
        if (!m->rules().diggables.empty() || !m->rules().rooms.empty()) dungeon(sim, *m);
        if (m->rules().settlers) settlers(sim, *m);
        if (m->rules().mech.any) mech(sim, *m);
        watchBuilders(sim, *m);
        economy(sim, *m);
        military(sim, *m);
    }
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

bool RtsAI::build(Simulation& sim, RtsModule& m, uint16_t type, int depth, const Vec3* at) {
    World& w = sim.world();
    const Rules& R = m.rules();
    const UnitType& bt = R.types[type];
    if (!m.requirementsMet(sim, player_, bt.requires_) && depth < 2) {
        // build a missing structure requirement first (mill before farms)
        auto& eco = m.state(sim).players[player_];
        for (auto& rq : bt.requires_)
            if (!rq.tech && rq.index != type && eco.typeCountAll[rq.index] == 0 && R.types[rq.index].structure) return build(sim, m, rq.index, depth + 1);
        return false;
    }
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
    if (at) {
        spot = *at;
    } else if (bt.placeOn != kNoType) {
        // nearest free node (geyser, metal spot) to the builder
        bool found = false;
        Fixed bestD{};
        const uint32_t now = sim.tick();
        w.each<Unit, Transform>([&](Entity, Unit& u, Transform& tr) {
            if (u.type != bt.placeOn) return;
            const int pc = m.state(sim).grid.cellOf(tr.pos);
            for (auto& [c, until] : badSpots_) if (until > now && c == pc) return;
            if (!m.canPlace(sim, player_, type, tr.pos, builder)) return;
            Fixed d = dist(tr.pos, bpos);
            if (!found || d < bestD) { bestD = d; spot = tr.pos; found = true; }
        });
        if (!found) return false;
    } else {
        // own spot finder (lanes kept free, random tie-breaks); fidelity games keep the sim's scoring (concrete...)
        if (!hasHome_) refreshBases(sim, m);
        bool ok = false;
        if (!R.fidelity && hasHome_) {
            const int r = std::max(6, R.buildRadius > 0 ? R.buildRadius : 12);
            ok = findSpot(sim, m, type, home_, r, spot, false) || findSpot(sim, m, type, home_, r + 10, spot, false);
        }
        if (!ok && !m.findAutoSpot(sim, player_, type, spot)) return false;
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
            if (!hasHome_) refreshBases(sim, m);
            const int r = std::max(6, R.buildRadius > 0 ? R.buildRadius : 12);
            if ((!R.fidelity && hasHome_ && findSpot(sim, m, p->ready, home_, r, spot, false)) || m.findAutoSpot(sim, player_, p->ready, spot)) {
                Command c; c.type = kCmdPlace; c.units = {e}; c.point = spot; send(sim, c);
            }
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
    // player skills (effect engine): every primitive gets a target from smartTarget. Cheap skills are cast
    // when ready (keeping a 20% reserve of capped resources); now and then the bot picks an expensive skill and
    // saves up for it, so costly skills get used too.
    if (R.advancedEffects && !R.playerAbilities.empty() && sim.tick() >= lastSkillTick_ + 30) {
        const size_t n = R.playerAbilities.size();
        auto& states = gs.playerAbilities[player_];
        auto affordable = [&](const PlayerAbilityDef& a, bool reserve) {
            for (auto& c : a.cost) {
                Fixed cap = eco.cap[c.res].raw > 0 ? eco.cap[c.res] : R.resources[c.res].storage;
                Fixed keep = reserve && cap.raw > 0 ? cap / 5 : Fixed{};
                if (eco.stock[c.res] - c.value < keep) return false;
            }
            return true;
        };
        auto tryCast = [&](int ai, bool reserve) {
            if (!m.canUsePlayerAbility(sim, player_, ai)) return false;
            const PlayerAbilityDef& a = m.playerAbilityFor(sim, player_, ai);
            if (a.autoEvery.raw > 0 || !affordable(a, reserve)) return false;
            Vec3 pt; Entity tg;
            if (a.smart == SmartRule::Auto && a.target == TargetKind::None) {
                // untargeted skills: only when they clearly help (dispels when we are debuffed, damage when enemies are near)
                Vec3 p2; Entity t2;
                if (!m.smartTarget(sim, player_, a.payload, TargetKind::Point, SmartRule::DebuffedOwn, p2, t2) &&
                    !m.smartTarget(sim, player_, a.payload, TargetKind::Point, SmartRule::DensestEnemy, p2, t2)) return false;
            }
            int64_t pair = 0;
            if (a.target == TargetKind::PointPair) {   // from where to where (Chronosphere)
                Vec3 to;
                if (!m.pairTarget(sim, player_, a.payload, pt, to)) return false;
                pair = RtsModule::packPoint2(to);
            } else if (!m.smartTarget(sim, player_, a.payload, a.target, a.smart, pt, tg)) return false;
            Command c; c.type = kCmdPlayerAbility; c.param = ai | pair; c.point = pt; c.target = tg;
            send(sim, c);
            lastSkillTick_ = sim.tick();
            return true;
        };
        if (savingFor_ >= 0 && (sim.tick() > savingUntil_ || size_t(savingFor_) >= n)) savingFor_ = -1;
        if (savingFor_ >= 0) {
            if (tryCast(savingFor_, false)) savingFor_ = -1;
        } else {
            const size_t start = rng_.below(uint32_t(n));
            const int goal = int((start + n / 2) % n);   // a random skill to save up for if it is out of reach
            for (size_t k = 0; k < n; ++k)
                if (tryCast(int((start + k) % n), true)) break;
            if (sim.tick() != lastSkillTick_ && player_ < gs.playerAbilities.size() && size_t(goal) < states.size()) {
                const PlayerAbilityDef& a = m.playerAbilityFor(sim, player_, goal);
                if (states[size_t(goal)].owned && !a.passive && a.autoEvery.raw == 0 && !a.cost.empty() && !affordable(a, true)) {
                    savingFor_ = goal; savingUntil_ = sim.tick() + 20 * 25;
                }
            }
        }
    }
    // player abilities: strike the densest enemy group (offensive effects only)
    if (!R.advancedEffects && !R.playerAbilities.empty() && sim.tick() % 40 == 0) {
        for (size_t ai = 0; ai < R.playerAbilities.size(); ++ai) {
            const PlayerAbilityDef& a = R.playerAbilities[ai];
            bool offensive = false;
            Fixed radius = Fixed::fromInt(2);
            for (auto& ef : a.effects) {
                if ((ef.kind == EffectKind::Damage || ef.kind == EffectKind::Stun) && ef.enemies) offensive = true;
                radius = fxMax(radius, ef.radius);
            }
            if (!offensive || a.target != TargetKind::Point || !m.canUsePlayerAbility(sim, player_, int(ai))) continue;
            std::vector<std::pair<Vec3, int>> foes, mine;
            w.each<Unit, Owner, Transform>([&](Entity, Unit& u, Owner& o, Transform& tr) {
                if (u.hp.raw <= 0 || R.types[u.type].invulnerable || (u.flags & kUnitOffMap)) return;
                int value = R.types[u.type].structure ? 3 : 1;
                if (o.player == player_ || (o.player != 0 && o.player != kHostileNeutral && sim.allied(o.player, player_))) mine.push_back({tr.pos, value});
                else if (m.hostile(sim, player_, o.player) && o.player != kHostileNeutral) foes.push_back({tr.pos, value});
            });
            if (foes.size() > 300) foes.resize(300);
            int bestScore = 0;
            Vec3 best{};
            for (auto& [p, v] : foes) {
                int score = 0, own = 0;
                for (auto& [q, qv] : foes) if (dist(p, q) <= radius) score += qv;
                for (auto& [q, qv] : mine) if (dist(p, q) <= radius + Fixed::fromInt(1)) own += qv;
                if (own * 2 >= score) continue;
                if (score > bestScore) { bestScore = score; best = p; }
            }
            if (bestScore < 3) continue;
            Command c; c.type = kCmdPlayerAbility; c.param = int64_t(ai); c.point = best; send(sim, c);
            break;
        }
    }
    // research: something affordable every now and then
    if (!R.techs.empty() && sim.tick() > lastTechTick_ + 300) {
        w.each<Production, Unit, Owner>([&](Entity e, Production& p, Unit& u, Owner& o) {
            if (o.player != player_ || !p.queue.empty() || (u.flags & kUnitUnderConstruction) || sim.tick() <= lastTechTick_ + 300) return;
            for (uint16_t k : R.types[u.type].researches) {
                if (int(eco.researched[k]) >= R.techs[k].levels || eco.researching[k] || !m.requirementsMet(sim, player_, R.techs[k].requires_) ||
                    !m.canAfford(sim, player_, m.techCost(sim, player_, k))) continue;
                if (R.techs[k].requiresAnyCount > 0) {
                    int have = 0;
                    for (uint16_t ty : R.techs[k].requiresAny) if (eco.typeCount[ty] > 0) ++have;
                    if (have < R.techs[k].requiresAnyCount) continue;
                }
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
            if (ord && ord->queue.empty() && controllable(w, e, u)) idleWorkers.push_back(e);
        }
    });

    // 7) military production buildings (and the tech they require); firstOnly: only while there is none
    auto armyProducer = [&](bool firstOnly) -> bool {
        int milProducers = 0;
        std::vector<uint16_t> candidates;
        for (size_t ti = 0; ti < R.types.size(); ++ti) {
            const UnitType& t = R.types[ti];
            if (!t.structure) continue;
            bool makesArmy = false;
            for (uint16_t u : t.produces) if (isCombat(R.types[u]) && !R.types[u].unique) makesArmy = true; // hero altars don't count
            if (t.waveUnit != kNoType) makesArmy = true;
            if (!makesArmy) continue;
            milProducers += count[ti];
            if (m.canTrain(sim, player_, uint16_t(ti)) || !m.requirementsMet(sim, player_, t.requires_)) candidates.push_back(uint16_t(ti));   // own faction
        }
        if (firstOnly && milProducers > 0) return false;
        int want = 1 + workers / 6 + std::min(escalation_, 2);
        if (m.surface(sim)) {   // planets: one of each producer kind first (an orbital yard opens the other bodies)
            std::vector<uint16_t> missingKinds;
            for (uint16_t c : candidates) if (count[c] == 0) missingKinds.push_back(c);
            if (!missingKinds.empty() && !firstOnly) { candidates = missingKinds; want = std::max(want, milProducers + 1); }
            if (enemyOffBody_) {   // only orbit fleets reach the enemy: more yards
                std::vector<uint16_t> orbit;
                for (uint16_t c : candidates)
                    for (uint16_t u : R.types[c].produces) if ((R.types[u].layer & kLayerOrbit) && std::find(orbit.begin(), orbit.end(), c) == orbit.end()) orbit.push_back(c);
                int yards = 0;
                for (uint16_t c : orbit) yards += count[c];
                if (!orbit.empty() && yards < 2 + std::min(escalation_, 2)) { candidates = orbit; want = std::max(want, milProducers + 1); }
            }
        }
        if (milProducers < want && !candidates.empty()) {
            uint16_t pick = candidates[rng_.below(uint32_t(candidates.size()))];
            if (!m.requirementsMet(sim, player_, R.types[pick].requires_)) {
                for (auto& rq : R.types[pick].requires_)
                    if (!rq.tech && count[rq.index] == 0 && build(sim, m, rq.index)) return true;
            } else if (build(sim, m, pick)) return true;
        }
        return false;
    };
    // 1) idle workers gather the resource we have least of (nodes or fields); crowded nodes cost extra
    //    (a dozen workers on one mine jam its entrance)
    std::vector<std::pair<uint32_t, int>> onNode;   // (node index, own gatherers)
    if (!idleWorkers.empty())
        w.each<Orders, Owner>([&](Entity, Orders& ord, Owner& o) {
            if (o.player != player_ || ord.queue.empty()) return;
            const Order& f = ord.queue.front();
            if ((f.type != OrderType::Gather && f.type != OrderType::Return) || !f.target.valid()) return;
            for (auto& [n, k] : onNode) if (n == f.target.index) { ++k; return; }
            onNode.push_back({f.target.index, 1});
        });
    auto crowd = [&](Entity n) {
        for (auto& [i, k] : onNode) if (i == n.index) return Fixed::fromInt(std::max(0, k - 5) * 6);
        return Fixed{};
    };
    // trips run between the node and a dropoff: a worker next to a dropoff (just trained, just delivered)
    // measures from the dropoff's centre, not from the side of it where it happens to stand
    std::vector<std::pair<Vec3, uint32_t>> drops;
    if (!idleWorkers.empty())
        w.each<Unit, Owner, Transform>([&](Entity, Unit& u, Owner& o, Transform& tr) {
            const UnitType& dt = R.types[u.type];
            if (o.player == player_ && dt.structure && dt.dropoffMask && !(u.flags & kUnitUnderConstruction)) drops.push_back({tr.pos, dt.dropoffMask});
        });
    for (Entity wk : idleWorkers) {
        const UnitType& t = R.types[w.get<Unit>(wk).type];
        if (t.gathers.empty()) continue;
        Vec3 p = w.get<Transform>(wk).pos;
        {
            Fixed bd{};
            Vec3 bp = p;
            bool f = false;
            for (auto& [dp, mask] : drops) {
                bool takes = false;
                for (auto& g : t.gathers) if (mask & (1u << g.res)) takes = true;
                const Fixed d = dist(dp, p);
                if (takes && d < Fixed::fromInt(6) && (!f || d < bd)) { bd = d; bp = dp; f = true; }
            }
            p = bp;
        }
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
                Fixed score = dist(nt.pos, p) * 4 + have / 20 + crowd(n);
                if (!any || score < bestScore) { any = true; best = n; bestScore = score; field = false; }
            });
        }
        if (!any) continue;
        Command c; c.type = kCmdGather; c.units = {wk};
        if (getenv("AIDBG2")) { Vec3 tp = field ? bestField : w.get<Transform>(best).pos; std::fprintf(stderr, "t=%u p%d gather from %.2f,%.2f -> %s %.2f,%.2f score %.3f\n", sim.tick(), player_, p.x.toDouble(), p.z.toDouble(), field ? "field" : R.types[w.get<Unit>(best).type].id.c_str(), tp.x.toDouble(), tp.z.toDouble(), bestScore.toDouble()); }
        if (field) c.point = bestField; else c.target = best;
        send(sim, c);
        if (!field) {
            bool found = false;
            for (auto& [i, k] : onNode) if (i == best.index) { ++k; found = true; }
            if (!found) onNode.push_back({best.index, 1});
        }
    }

    const bool canBuildNow = sim.tick() > lastBuildTick_ + 60;
    // abandoned construction sites (builder killed or re-tasked): send a worker back
    if (sim.tick() % 100 == 0) {
        std::vector<Entity> sites, busy;
        w.each<Construction, Owner>([&](Entity e, Construction& c, Owner& o) { if (o.player == player_ && !c.selfBuild) sites.push_back(e); });
        w.each<Orders, Owner>([&](Entity, Orders& ord, Owner& o) {
            if (o.player == player_ && !ord.queue.empty() && ord.queue.front().type == OrderType::Build && ord.queue.front().target.valid()) busy.push_back(ord.queue.front().target);
        });
        for (Entity s : sites) {
            if (std::find(busy.begin(), busy.end(), s) != busy.end()) continue;
            Entity best; Fixed bestD{};
            Vec3 sp = w.get<Transform>(s).pos;
            w.each<Unit, Owner, Orders, Transform>([&](Entity e, Unit& u, Owner& o, Orders& ord, Transform& tr) {
                if (o.player != player_ || R.types[u.type].builds.empty() || !controllable(w, e, u)) return;
                if (!ord.queue.empty() && ord.queue.front().type == OrderType::Build) return;
                Fixed d = dist(tr.pos, sp);
                if (!best.valid() || d < bestD) { best = e; bestD = d; }
            });
            if (!best.valid()) break;
            Command c; c.type = kCmdSmart; c.units = {best}; c.target = s; send(sim, c);
        }
    }
    // 2) supply (structures like farms/depots, or supply units like overlords)
    Fixed freeSupply = eco.supplyCap - eco.supplyUsed;
    if (eco.supplyCap < R.supplyCap && freeSupply < Fixed::fromInt(4) && canBuildNow) {
        // plain supply first (farms, depots, overlords); producers that also give supply when there is nothing else
        for (int pass = 0; pass < 2; ++pass)
            for (size_t ti = 0; ti < R.types.size(); ++ti) {
                const UnitType& t = R.types[ti];
                if (t.supplyProvided.raw <= 0 || (pass == 0 && !t.produces.empty()) || (pass == 1 && (t.produces.empty() || !t.structure)) || t.key) continue;
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
    // 3b) a flow resource running dry (TA energy stall): build something that produces it
    for (size_t r = 0; r < R.resources.size(); ++r) {
        if (R.resources[r].kind != ResourceKind::Flow || !canBuildNow) continue;
        Fixed cap = eco.cap[r].raw > 0 ? eco.cap[r] : R.resources[r].storage;
        if (eco.income[r].raw > 0 && eco.stock[r] > cap / 5) continue;
        for (size_t ti = 0; ti < R.types.size(); ++ti) {
            const UnitType& t = R.types[ti];
            bool gives = false, costsIt = false;
            for (auto& a : t.income) if (a.res == r && a.value.raw > 0) gives = true;
            for (auto& a : t.cost) if (a.res == r && a.value.raw > 0) costsIt = true;
            if (gives && !costsIt && t.structure && build(sim, m, uint16_t(ti))) return;
        }
    }
    // 3c) the first army producer comes early: an economy that never fields an army never wins
    if (canBuildNow && sim.tick() > 40 * sim.tickRate() && armyProducer(true)) return;
    // 4) income structures (TA metal extractors / energy, refineries on geysers, farms)
    for (size_t ti = 0; ti < R.types.size(); ++ti) {
        const UnitType& t = R.types[ti];
        if (!t.structure || !canBuildNow) continue;
        // converters (metal makers: energy -> metal) only with a clear surplus of what they consume
        bool converterStarved = false;
        for (auto& a : t.income)
            if (a.value.raw < 0) {
                Fixed cap = eco.cap[a.res].raw > 0 ? eco.cap[a.res] : R.resources[a.res].storage;
                if (eco.income[a.res] < -a.value * 2 || eco.stock[a.res] * 2 < cap) converterStarved = true;
            }
        if (converterStarved) continue;
        bool incomeLike = positiveIncome(t) || (t.placeOn != kNoType && t.takesOverNode) || (t.resourceNode >= 0 && t.ownerOnly);
        if (!incomeLike || count[ti] >= (t.placeOn != kNoType ? 8 : 6)) continue;
        if (t.placeOn != kNoType && workers < 8 && R.types[t.placeOn].resourceNode >= 0) continue; // geysers later
        if (build(sim, m, uint16_t(ti))) return;
    }
    // 4b) a dropoff for every resource our workers gather (refinery for harvesters, lumber camp...)
    if (canBuildNow) {
        uint32_t gathered = 0, dropped = 0;
        for (size_t ti = 0; ti < R.types.size(); ++ti) {
            if (!count[ti]) continue;
            for (auto& g : R.types[ti].gathers) if (R.resources[g.res].kind != ResourceKind::Flow) gathered |= 1u << g.res;
            if (R.types[ti].structure) dropped |= R.types[ti].dropoffMask;
        }
        // also the resources of worker types we could train but have none of yet (harvesters)
        for (size_t ti = 0; ti < R.types.size(); ++ti)
            if (R.types[ti].worker && !R.types[ti].structure && count[ti] == 0 && workers == 0)
                for (auto& g : R.types[ti].gathers) if (R.resources[g.res].kind != ResourceKind::Flow) gathered |= 1u << g.res;
        uint32_t missingDrop = gathered & ~dropped;
        for (size_t ti = 0; ti < R.types.size() && missingDrop; ++ti) {
            const UnitType& t = R.types[ti];
            if (t.structure && (t.dropoffMask & missingDrop) && t.produces.size() <= 1 && build(sim, m, uint16_t(ti))) return;
        }
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
    saveForWorkers_ = false;
    if (workers < 3)
        for (size_t ti = 0; ti < R.types.size(); ++ti) {
            if (!R.types[ti].worker || R.types[ti].structure || R.types[ti].deploysTo != kNoType || !m.canTrain(sim, player_, uint16_t(ti))) continue;
            for (size_t pi = 0; pi < R.types.size(); ++pi)
                if (count[pi] && std::find(R.types[pi].produces.begin(), R.types[pi].produces.end(), uint16_t(ti)) != R.types[pi].produces.end()) saveForWorkers_ = true;
        }
    if (workers < cfg_.workersPerBase) {
        // the worker type we have fewest of; builders that gather nothing are capped at 2
        int bestType = -1, bestCount = 1 << 30;
        for (size_t ti = 0; ti < R.types.size(); ++ti) {
            const UnitType& t = R.types[ti];
            if (!t.worker || t.structure || t.deploysTo != kNoType || !m.canTrain(sim, player_, uint16_t(ti))) continue;
            if (t.gathers.empty() && t.carrierCapacity.raw <= 0 && t.digRate.raw <= 0 && t.claimerTime.raw <= 0 && count[ti] >= 2) continue;
            bool producible = false;
            for (size_t pi = 0; pi < R.types.size(); ++pi)
                if (count[pi] && std::find(R.types[pi].produces.begin(), R.types[pi].produces.end(), uint16_t(ti)) != R.types[pi].produces.end()) producible = true;
            if (producible && count[ti] < bestCount) { bestCount = count[ti]; bestType = int(ti); }
        }
        if (bestType >= 0) train(sim, m, uint16_t(bestType));
    }
    // 6b) unlock the army: own producers make combat units whose structure requirements are missing
    if (canBuildNow && building == 0) {
        bool anyTrainable = false;
        std::vector<uint16_t> missing;
        for (size_t ti = 0; ti < R.types.size(); ++ti) {
            const UnitType& t = R.types[ti];
            if (!t.structure || count[ti] == 0) continue;
            for (uint16_t u : t.produces) {
                if (!isCombat(R.types[u])) continue;
                if (m.canTrain(sim, player_, u)) { anyTrainable = true; continue; }
                for (auto& rq : R.types[u].requires_)
                    if (!rq.tech && count[rq.index] == 0 && std::find(missing.begin(), missing.end(), rq.index) == missing.end()) missing.push_back(rq.index);
            }
        }
        if (!anyTrainable)
            for (uint16_t rq : missing) if (build(sim, m, rq)) return;
    }
    // 7) military production buildings (and the tech they require)
    if (sim.tick() > lastBuildTick_ + 100 && building == 0 && armyProducer(false)) return;
    // 8) towers at the chokepoint toward the enemy (agent-ai-terrain), once the army is under way
    if (sim.tick() > lastBuildTick_ + 100 && building == 0 && sim.tick() > 240 * sim.tickRate() && wave_ > 0) buildTowers(sim, m);
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
        if (isCombat(t) && controllable(w, e, u)) {
            army.push_back(e);
            auto* ord = w.tryGet<Orders>(e);
            if (ord && idleOrders(*ord)) idleArmy.push_back(e);
        }
        if (auto* prod = w.tryGet<Production>(e); prod && !saveForWorkers_ && prod->queue.size() < 2 && !(u.flags & kUnitUnderConstruction)) {
            std::vector<uint16_t> opts;
            for (uint16_t ut : t.produces) {
                const UnitType& cand = R.types[ut];
                if (!isCombat(cand) || !m.canTrain(sim, player_, ut)) continue;
                if (!m.canAfford(sim, player_, R.costModel == CostModel::Upfront ? cand.cost : std::vector<Amount>{}, cand.supplyUsed)) continue;
                if (t.chargesMax > 0 && prod->charges <= 0) continue;
                opts.push_back(ut);
                if (cand.hero) { opts.assign(1, ut); break; } // heroes first
            }
            if (!opts.empty()) { Command c; c.type = kCmdTrain; c.units = {e}; c.param = opts.size() == 1 ? opts[0] : pickUnit(sim, m, opts); send(sim, c); }
        }
    });
    // capture points / flags not ours: send a small squad to the nearest one (tickets, Z flags, resource points)
    if (!idleArmy.empty() && (gs.victory.mode == VictoryMode::Tickets || sim.tick() % 60 < cfg_.thinkTicks + 1)) {
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
    (void)hasHome; (void)home;
    tactics(sim, m, idleArmy, army, defend);
}

void RtsAI::dungeon(Simulation& sim, RtsModule& m) {
    World& w = sim.world();
    const Rules& R = m.rules();
    GameState& gs = m.state(sim);
    MapGrid& g = gs.grid;
    if (sim.tick() < lastDungeonTick_ + 60) return;
    lastDungeonTick_ = sim.tick();
    // anchor: own key structure (dungeon heart)
    Vec3 home{};
    bool hasHome = false, diggers = false;
    w.each<Unit, Owner, Transform>([&](Entity, Unit& u, Owner& o, Transform& tr) {
        if (o.player != player_) return;
        const UnitType& t = R.types[u.type];
        if (t.structure && (!hasHome || t.key)) { home = tr.pos; hasHome = true; }
        if (t.digRate.raw > 0) diggers = true;
    });
    if (!hasHome) return;
    // 1) digging: keep a few marks open, tunnelling toward the nearest portal we don't own, then the enemy
    if (diggers && !R.diggables.empty() && player_ >= 1 && player_ <= 8) {
        const uint8_t bit = uint8_t(1u << (player_ - 1));
        int marked = 0;
        for (auto mk : g.digMark) if (mk & bit) ++marked;
        if (marked < 4) {
            Vec3 goal = home;
            bool found = false;
            Fixed best{};
            w.each<Unit, Owner, Transform>([&](Entity, Unit& u, Owner& o, Transform& tr) {
                const UnitType& t = R.types[u.type];
                bool want = (t.claimedByTerritory && o.player != player_) || (m.hostile(sim, player_, o.player) && o.player != kHostileNeutral && t.structure);
                if (!want) return;
                Fixed d = dist(tr.pos, home) - (t.claimedByTerritory ? Fixed::fromInt(1000) : Fixed{});
                if (!found || d < best) { found = true; best = d; goal = tr.pos; }
            });
            // cells reachable from home
            std::vector<uint8_t> reach(g.flags.size(), 0);
            std::vector<int> q;
            int hx, hy; g.cellXY(home, hx, hy);
            g.nearestPassable(hx, hy, kLayerGround, 4);
            if (g.inside(hx, hy)) { q.push_back(g.index(hx, hy)); reach[size_t(q[0])] = 1; }
            static const int dx4[4] = {1, -1, 0, 0}, dy4[4] = {0, 0, 1, -1};
            // 4-neighbour k of a cell (planets: across face seams), -1 none
            auto nb = [&](int c, int k) {
                if (g.surf) return g.surf->neighbor(c, k);
                const int nx = c % g.w + dx4[k], ny = c / g.w + dy4[k];
                return g.inside(nx, ny) ? g.index(nx, ny) : -1;
            };
            for (size_t qi = 0; qi < q.size(); ++qi) {
                int x = q[qi] % g.w, y = q[qi] / g.w;
                for (int k = 0; k < 4; ++k) {
                    const int n = nb(q[qi], k);
                    if (n < 0 || reach[size_t(n)]) continue;
                    if (g.surf ? !g.canMoveCells(q[qi], n, kLayerGround) : !g.canStep(x, y, n % g.w, n / g.w, kLayerGround)) continue;
                    reach[size_t(n)] = 1;
                    q.push_back(n);
                }
            }
            std::vector<std::pair<Fixed, int>> cands;
            for (int c : q) {
                for (int k = 0; k < 4; ++k) {
                    const int nc = nb(c, k);
                    if (nc < 0) continue;
                    size_t n = size_t(nc);
                    if (!g.dig[n] || (g.digMark[n] & bit)) continue;
                    Fixed score = dist(g.centerOf(nc), goal);
                    if (!R.diggables[g.dig[n] - 1u].yields.empty()) score -= Fixed::fromInt(6); // veins first
                    cands.push_back({score, int(n)});
                }
            }
            std::sort(cands.begin(), cands.end());
            cands.erase(std::unique(cands.begin(), cands.end()), cands.end());
            for (size_t k = 0; k < cands.size() && k < 3; ++k) {
                Command c; c.type = kCmdDig;
                c.point = g.centerOf(cands[k].second);
                c.param = rectParam(1, 1, 0);
                send(sim, c);
            }
        }
    }
    // 2) rooms: a few cells of every room type, more of the ones creatures need
    if (!R.rooms.empty()) {
        std::vector<int> want(R.rooms.size(), 4);
        for (auto& t : R.types) {
            for (auto& [room, cells] : t.attract.rooms) want[room] = std::max(want[room], cells + 2);
            if (t.needs.sleep.room >= 0) want[size_t(t.needs.sleep.room)] = std::max(want[size_t(t.needs.sleep.room)], 9);
        }
        auto& eco = gs.players[player_];
        for (size_t r = 0; r < R.rooms.size(); ++r) {
            // capacity rooms grow with the population
            if (R.rooms[r].supply.raw > 0 && eco.supplyUsed + Fixed::fromInt(2) >= eco.supplyCap && eco.supplyCap < R.supplyCap)
                want[r] = std::max(want[r], m.roomCellsOf(sim, player_, int(r)) + 6);
            if (m.roomCellsOf(sim, player_, int(r)) >= want[r]) continue;
            // a free 3x2 rect of own territory, nearest to home
            int bestCell = -1;
            Fixed bestD{};
            for (int y = 1; y + 2 < g.h; ++y)
                for (int x = 1; x + 3 < g.w; ++x) {
                    bool ok = true;
                    for (int yy = y; yy < y + 2 && ok; ++yy)
                        for (int xx = x; xx < x + 3 && ok; ++xx) {
                            size_t i = size_t(g.index(xx, yy));
                            if (g.territory[i] != player_ || g.room[i] >= 0 || g.blocked[i] || !(g.flags[i] & kCellGround)) ok = false;
                        }
                    if (!ok) continue;
                    Fixed d = dist(g.cellCenter(x, y), home);
                    if (bestCell < 0 || d < bestD) { bestCell = g.index(x, y); bestD = d; }
                }
            if (bestCell < 0) break;
            std::vector<Amount> cost = R.rooms[r].cost;
            for (auto& c : cost) c.value = c.value * 6;
            if (!m.canAfford(sim, player_, cost)) break;
            Command c; c.type = kCmdPaintRoom;
            c.point = g.cellCenter(bestCell % g.w, bestCell / g.w);
            c.param = rectParam(3, 2, int(r));
            send(sim, c);
            break; // one room per think
        }
    }
}

void RtsAI::settlers(Simulation& sim, RtsModule& m) {
    World& w = sim.world();
    const Rules& R = m.rules();
    if (sim.tick() < lastSettlersTick_ + 40) return;
    lastSettlersTick_ = sim.tick();
    std::vector<int> count(R.types.size(), 0);
    Entity store;
    std::vector<Entity> structures;
    w.each<Unit, Owner>([&](Entity e, Unit& u, Owner& o) {
        if (o.player != player_) return;
        count[u.type]++;
        const UnitType& t = R.types[u.type];
        if (t.storeCapacity.raw > 0 && !(u.flags & kUnitUnderConstruction) && !store.valid()) store = e;
        if (t.structure && (w.has<Inventory>(e) || !t.produces.empty())) structures.push_back(e);
    });
    // 1) every building that trades goods must be on the road network
    if (R.roadNetwork.enabled && store.valid()) {
        std::vector<Entity> off;
        for (Entity e : structures) if (e != store && !m.roadConnected(sim, e, store)) off.push_back(e);
        if (!off.empty()) { Command c; c.type = kCmdConnectRoad; c.units = {off[0]}; send(sim, c); }
    }
    // 2) production chain: raw producers first, then workplaces whose inputs we produce
    if (sim.tick() > lastBuildTick_ + 60) {
        uint32_t produced = 0;
        for (size_t ti = 0; ti < R.types.size(); ++ti)
            if (count[ti] && R.types[ti].recipe.enabled) for (auto& a : R.types[ti].recipe.out) produced |= 1u << a.res;
        for (int pass = 0; pass < 2; ++pass)
            for (size_t ti = 0; ti < R.types.size(); ++ti) {
                const UnitType& t = R.types[ti];
                if (!t.recipe.enabled || count[ti] >= (t.recipe.in.empty() ? 2 : 1)) continue;
                bool ready = true;
                for (auto& a : t.recipe.in) if (!(produced & (1u << a.res))) ready = false;
                if ((pass == 0) != t.recipe.in.empty() || !ready) continue;
                if (build(sim, m, uint16_t(ti))) return;
            }
    }
    // 3) recruits for producers that need them
    for (size_t ti = 0; ti < R.types.size(); ++ti) {
        const UnitType& t = R.types[ti];
        if (t.trainFrom == kNoType || count[t.trainFrom] >= 2) continue;
        bool canMake = false;
        for (size_t pi = 0; pi < R.types.size(); ++pi)
            if (count[pi] && std::find(R.types[pi].produces.begin(), R.types[pi].produces.end(), uint16_t(ti)) != R.types[pi].produces.end()) canMake = true;
        if (canMake && train(sim, m, t.trainFrom)) break;
    }
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
