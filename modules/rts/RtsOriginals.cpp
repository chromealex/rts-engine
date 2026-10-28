// Fidelity mechanics: what the sample games need to play like their originals.
//   damage     weapon bonuses vs tags (AoE, SC2), melee / pierce armor (AoE), percent armor (WC3), shields (SC)
//   heroes     skill points and learned levels, hero limit and tiers, free first hero, revive with level (WC3)
//   economy    gatherers per node (WC3 mine, SC patch), upkeep per resource / on income (WC3, CoH),
//              power fields (pylons), parallel queues (reactor), territory income per cell (DK mana),
//              cost growing with the number owned (DK imps)
//   units      crews: snipers kill drivers, any robot mans an empty vehicle (Z); infection (They Are Billions);
//              eaters that submerge and resurface (Dune sandworm); death payloads (TA commander)
//   garrisons  upgrades paid with units, losing a level when captured, units staying home, clashes on the
//              road, forge-like player mods, resources gained on kills / captures / upgrades (Mushroom Wars 2)
// Everything runs only when the rules use one of these blocks (Rules::originals), so other games keep
// their exact checksums.
#include "RtsInternal.h"
#include <algorithm>

namespace rts::game {
using namespace detail;

namespace {
int heroCount(const Rules& rules, const PlayerEconomy& eco) {
    int n = 0;
    for (size_t k = 0; k < rules.types.size(); ++k) if (rules.types[k].hero && k < eco.typeCountAll.size()) n += eco.typeCountAll[k];
    return n;
}
int maxSkill(const UnitType& t, int index) {
    const AbilityDef& a = t.abilities[size_t(index)];
    if (size_t(index) < t.learnAt.size() && !t.learnAt[size_t(index)].empty()) return int(t.learnAt[size_t(index)].size());
    return 1 + int(a.levels.size());
}
int learnLevel(const UnitType& t, int index, int skill) {   // hero level needed for skill level skill + 1
    if (size_t(index) < t.learnAt.size() && size_t(skill) < t.learnAt[size_t(index)].size()) return t.learnAt[size_t(index)][size_t(skill)];
    return 1 + 2 * skill;   // WC3: 1, 3, 5
}
} // namespace

// ================================================================= damage

Fixed RtsModule::mitigate(Simulation& sim, Entity target, Fixed raw, const WeaponDef* wd, Entity) {
    World& w = sim.world();
    const UnitType& t = rules_.types[w.get<Unit>(target).type];
    if (wd) for (auto& [tags, add] : wd->bonus) if (t.tags & tags) raw += add;
    const ArmorRules& ar = rules_.armor;
    auto floorOf = [&](Fixed r) {
        Fixed m = ar.minShare == Fixed::fromRatio(1, 5) ? r / 5 : r * ar.minShare;
        return fxMax(m, ar.minDamage);
    };
    if (auto* sh = w.tryGet<Shield>(target); sh && sh->value.raw > 0) {
        // shields soak first with their own armor; what breaks through goes on to the hull
        Fixed onShield = fxMax(raw - t.shieldArmor, floorOf(raw));
        sh->lastHit = sim.tick();
        if (sh->value >= onShield) { sh->value -= onShield; return Fixed{}; }
        Fixed left = onShield - sh->value;
        sh->value = Fixed{};
        raw = onShield.raw > 0 ? raw * left / onShield : Fixed{};
    }
    Fixed armor = stat(sim, target, wd && wd->armorClass == 1 ? Stat::Armor2 : Stat::Armor);
    if (ar.percent) {
        if (armor.raw >= 0) {
            Fixed ak = armor * ar.perPoint;
            return raw - raw * ak / (kOne + ak);
        }
        return raw * (kOne + fxMin(-armor * ar.perPoint, Fixed::fromRatio(71, 100)));
    }
    return fxMax(raw - armor, floorOf(raw));
}

void RtsModule::afterHit(Simulation& sim, Entity target, Entity source, const WeaponDef* wd, bool killed) {
    World& w = sim.world();
    Unit& u = w.get<Unit>(target);
    const UnitType& t = rules_.types[u.type];
    const bool src = w.alive(source) && w.has<Unit>(source);
    if (!killed && wd && wd->killsCrew.raw > 0 && t.crew != kNoType) {
        Owner& o = w.get<Owner>(target);
        if (o.player != 0 && sim.rng().unit() < wd->killsCrew) {
            GameState& gs = state(sim);
            if (o.player < gs.players.size() && gs.players[o.player].typeCountAll[u.type] > 0) gs.players[o.player].typeCountAll[u.type]--;
            o.player = 0;   // empty: anyone may man it
            if (auto* ord = w.tryGet<Orders>(target)) ord->queue.clear();
            if (w.has<Mobile>(target)) stopMoving(sim, target);
            if (auto* c = w.tryGet<Combat>(target)) c->target = kNullEntity;
            if (auto* p = w.tryGet<Production>(target)) p->queue.clear();
            sim.emit({kEvCrewKilled, 0, target, source, u.type, w.get<Transform>(target).pos});
        }
    }
    if (!killed || !src) return;
    const UnitType& st = rules_.types[w.get<Unit>(source).type];
    if (t.infectSpawn != kNoType && (st.tags & t.infectBy)) u.flags |= kUnitInfected;
    if (auto* eater = w.tryGet<Eater>(source)) ++eater->kills;
}

// ================================================================= death

void RtsModule::onDeathOriginals(Simulation& sim, Entity e, uint8_t owner, Vec3 pos, bool silent) {
    World& w = sim.world();
    GameState& gs = state(sim);
    const Unit& u = w.get<Unit>(e);
    const UnitType& t = rules_.types[u.type];
    if (!silent && t.onDeath >= 0) {
        EffectCtx ctx;
        ctx.owner = owner; ctx.sourceType = u.type; ctx.point = flat(pos); ctx.origin = ctx.point;
        applyPayload(sim, t.onDeath, ctx);
    }
    if (t.deathFieldRes >= 0) {
        // a spice bloom bursts into a field around it
        MapGrid& g = gs.grid;
        const int r = int(t.deathFieldRadius.floorInt()) + 1;
        const int cx = g.cellX(pos.x), cy = g.cellY(pos.z);
        const Fixed cap = rules_.resources[size_t(t.deathFieldRes)].fieldCellMax;
        for (int y = cy - r; y <= cy + r; ++y)
            for (int x = cx - r; x <= cx + r; ++x) {
                if (!g.inside(x, y)) continue;
                Fixed d = Vec2{Fixed::fromInt(x - cx), Fixed::fromInt(y - cy)}.length();
                if (d > t.deathFieldRadius) continue;
                size_t i = size_t(g.index(x, y));
                if (!(g.flags[i] & kCellGround) || (g.fieldRes[i] >= 0 && g.fieldRes[i] != t.deathFieldRes)) continue;
                g.fieldRes[i] = t.deathFieldRes;
                g.fieldAmt[i] = fxMin(cap, g.fieldAmt[i] + t.deathFieldAmount * (kOne - d / (t.deathFieldRadius + kOne)));
            }
        gs.fieldsDirty = true;
    }
    if ((u.flags & kUnitInfected) && t.infectSpawn != kNoType) {
        deathSpawns_.push_back({t.infectSpawn, u.lastAttacker, pos, t.infectCount});
        sim.emit({kEvInfected, u.lastAttacker, e, kNullEntity, t.infectSpawn, pos});
    }
    if (t.hero && rules_.heroes.revive && owner != 0 && owner < gs.players.size() && !silent) {
        if (auto* h = w.tryGet<Hero>(e)) {
            auto& dead = gs.players[owner].deadHeroes;
            dead.erase(std::remove_if(dead.begin(), dead.end(), [&](const PlayerEconomy::DeadHero& d) { return d.type == u.type; }), dead.end());
            PlayerEconomy::DeadHero d{u.type, h->level, h->xp, {}};
            std::copy(std::begin(h->skill), std::end(h->skill), d.skill);
            dead.push_back(d);
        }
    }
}

void RtsModule::flushDeathSpawns(Simulation& sim) {
    if (deathSpawns_.empty()) return;
    World& w = sim.world();
    auto spawns = std::move(deathSpawns_);
    deathSpawns_.clear();
    for (auto& d : spawns)
        for (int k = 0; k < d.count; ++k) {
            Vec3 p = d.pos + Vec3{Fixed::fromRatio((k % 3) - 1, 2), Fixed{}, Fixed::fromRatio((k / 3) % 3 - 1, 2)};
            Entity n = spawn(sim, d.type, d.owner, p);
            if (!w.has<Orders>(n)) continue;
            // the risen go for the nearest enemy structure
            Entity best; Fixed bd{};
            w.each<Unit, Owner, Transform>([&](Entity s, Unit& su, Owner& so, Transform& st) {
                if (!rules_.types[su.type].structure || !hostile(sim, d.owner, so.player) || rules_.types[su.type].invulnerable) return;
                Fixed dd = dist2D(st.pos, p);
                if (!best.valid() || dd < bd) { best = s; bd = dd; }
            });
            if (!best.valid()) continue;
            Order o; o.type = OrderType::AttackMove; o.point = flat(w.get<Transform>(best).pos); o.param = 1;
            issue(sim, n, o, false);
        }
}

// ================================================================= heroes

void RtsModule::onHeroSpawn(Simulation& sim, Entity e, uint8_t owner) {
    World& w = sim.world();
    GameState& gs = state(sim);
    auto* h = w.tryGet<Hero>(e);
    if (!h || owner == 0 || owner >= gs.players.size()) return;
    auto& eco = gs.players[owner];
    Unit& u = w.get<Unit>(e);
    if (rules_.heroes.skills) h->points = 1;
    auto& dead = eco.deadHeroes;
    for (size_t k = 0; k < dead.size(); ++k) {
        if (dead[k].type != u.type) continue;
        h->level = dead[k].level;
        h->xp = dead[k].xp;
        std::copy(std::begin(dead[k].skill), std::end(dead[k].skill), h->skill);
        int spent = 0;
        for (auto s : h->skill) spent += s;
        h->points = uint8_t(std::max(0, (rules_.heroes.skills ? h->level : 0) - spent));
        u.hp = stat(sim, e, Stat::MaxHp);
        dead.erase(dead.begin() + long(k));
        break;
    }
    eco.heroesMade++;
}

bool RtsModule::heroAllowed(Simulation& sim, uint8_t player, uint16_t type) const {
    auto* gs = sim.world().resource<GameState>();
    if (player == 0 || player >= gs->players.size() || !rules_.types[type].hero) return true;
    const auto& eco = gs->players[player];
    int have = heroCount(rules_, eco);
    if (rules_.heroes.max > 0 && have >= rules_.heroes.max) return false;
    const auto& tiers = rules_.heroes.tiers;
    if (!tiers.empty() && size_t(have) < tiers.size()) {
        bool ok = false;
        for (size_t k = size_t(have); k < tiers.size(); ++k) if (eco.typeCount[tiers[k]] > 0) ok = true;
        if (!ok) return false;
    }
    return true;
}

std::vector<Amount> RtsModule::trainCost(Simulation& sim, uint8_t player, uint16_t type, Fixed* timeMult) const {
    const UnitType& t = rules_.types[type];
    std::vector<Amount> cost = t.cost;
    if (timeMult) *timeMult = kOne;
    auto* gs = sim.world().resource<GameState>();
    if (player == 0 || player >= gs->players.size()) return cost;
    const auto& eco = gs->players[player];
    if (t.costGrowth.raw != 0) {
        int owned = eco.typeCountAll[type];
        for (auto& c : cost) c.value = c.value + c.value * t.costGrowth * owned;
    }
    if (!t.hero) return cost;
    for (auto& d : eco.deadHeroes) {
        if (d.type != type || !rules_.heroes.revive) continue;
        Fixed f = rules_.heroes.reviveShare + rules_.heroes.revivePerLevel * (d.level - 1);
        for (auto& c : cost) {
            c.value = c.value * f;
            for (auto& m : rules_.heroes.reviveMaxCost) if (m.res == c.res) c.value = fxMin(c.value, m.value);
        }
        if (timeMult) {
            Fixed tm = rules_.heroes.reviveTimeShare * f;
            if (rules_.heroes.reviveMaxTime.raw > 0 && t.buildTime.raw > 0) tm = fxMin(tm, rules_.heroes.reviveMaxTime / t.buildTime);
            *timeMult = tm;
        }
        return cost;
    }
    if (rules_.heroes.freeFirst && eco.heroesMade == 0 && heroCount(rules_, eco) == 0) cost.clear();
    return cost;
}

bool RtsModule::skillLearned(Simulation& sim, Entity e, int index) const {
    auto* h = sim.world().tryGet<Hero>(e);
    return !h || index < 0 || index >= 8 || h->skill[index] > 0;
}

bool RtsModule::canLearn(Simulation& sim, Entity e, int index) const {
    World& w = sim.world();
    auto* h = w.tryGet<Hero>(e);
    if (!h || h->points == 0 || index < 0 || index >= 8) return false;
    const UnitType& t = rules_.types[w.get<Unit>(e).type];
    if (size_t(index) >= t.abilities.size()) return false;
    int s = h->skill[index];
    if (s >= maxSkill(t, index)) return false;
    return h->level >= learnLevel(t, index, s);
}

void RtsModule::applyLearn(Simulation& sim, Entity e, int index) {
    if (!canLearn(sim, e, index)) return;
    World& w = sim.world();
    Hero& h = w.get<Hero>(e);
    h.skill[index]++;
    h.points--;
    sim.emit({kEvLearned, w.get<Owner>(e).player, e, kNullEntity, int64_t(index | (h.skill[index] << 8)), w.get<Transform>(e).pos});
}

// ================================================================= economy

Fixed RtsModule::gatherMultFor(Simulation& sim, uint8_t player, int res) const {
    auto* gs = sim.world().resource<GameState>();
    if (player >= gs->players.size()) return kOne;
    const auto& p = gs->players[player];
    if (res >= 0 && size_t(res) < p.gatherMultRes.size()) return p.gatherMultRes[size_t(res)];
    return p.gatherMult;
}

int RtsModule::gatherersAt(Simulation& sim, Entity node) const {
    int n = 0;
    sim.world().each<Orders>([&](Entity, Orders& ord) {
        if (!ord.queue.empty() && ord.queue.front().type == OrderType::Gather && ord.queue.front().state == 2 && ord.queue.front().target == node) ++n;
    });
    return n;
}

bool RtsModule::inField(Simulation& sim, uint8_t player, int16_t field, Vec3 centre, const UnitType&) const {
    bool in = false;
    sim.world().each<Unit, Owner, Transform>([&](Entity, Unit& u, Owner& o, Transform& tr) {
        if (in || o.player != player || (u.flags & kUnitUnderConstruction) || u.hp.raw <= 0) return;
        const UnitType& pt = rules_.types[u.type];
        if (pt.fieldId == field && dist2D(tr.pos, centre) <= pt.fieldRadius) in = true;
    });
    return in;
}

void RtsModule::economyOriginals(Simulation& sim) {
    World& w = sim.world();
    GameState& gs = state(sim);
    const size_t nr = rules_.resources.size();
    for (size_t pi = 1; pi < gs.players.size(); ++pi) {
        auto& p = gs.players[pi];
        // upkeep: per resource (WC3 taxes gold only) and on per-second income (CoH manpower)
        p.gatherMultRes.assign(nr, kOne);
        std::vector<Fixed> incMult(nr, kOne);
        for (auto& tier : rules_.upkeep) {
            if (p.supplyUsed < tier.supply) continue;
            for (size_t r = 0; r < nr; ++r) if (tier.resMask & (1u << r)) { p.gatherMultRes[r] = tier.gatherMult; incMult[r] = tier.incomeMult; }
        }
        for (size_t r = 0; r < nr; ++r) if (p.income[r].raw > 0) p.income[r] = p.income[r] * incMult[r];
        // territory income (Dungeon Keeper: mana per claimed tile)
        if (pi < gs.territoryCells.size())
            for (auto& a : rules_.territory.incomePerCell) p.income[a.res] += a.value * gs.territoryCells[pi];
    }
    // power fields: structures that need one switch off outside every own field (pylons)
    bool anyField = false;
    for (auto& t : rules_.types) if (t.needsField >= 0) anyField = true;
    if (!anyField) return;
    struct Src { uint8_t owner; int16_t id; Vec3 pos; Fixed r; };
    std::vector<Src> srcs;
    w.each<Unit, Owner, Transform>([&](Entity, Unit& u, Owner& o, Transform& tr) {
        const UnitType& t = rules_.types[u.type];
        if (t.fieldId >= 0 && !(u.flags & kUnitUnderConstruction) && u.hp.raw > 0) srcs.push_back({o.player, t.fieldId, tr.pos, t.fieldRadius});
    });
    w.each<Unit, Owner, Transform>([&](Entity, Unit& u, Owner& o, Transform& tr) {
        const UnitType& t = rules_.types[u.type];
        if (t.needsField < 0 || (u.flags & kUnitUnderConstruction)) return;
        bool powered = false;
        for (auto& s : srcs) if (s.owner == o.player && s.id == t.needsField && dist2D(s.pos, tr.pos) <= s.r) powered = true;
        if (!powered) u.flags |= kUnitDisabled;
    });
}

void RtsModule::gainOn(Simulation& sim, uint8_t player, Fixed ResourceDef::*field, Fixed count) {
    auto& gs = state(sim);
    if (player == 0 || player >= gs.players.size()) return;
    for (size_t r = 0; r < rules_.resources.size(); ++r) {
        Fixed v = rules_.resources[r].*field;
        if (v.raw == 0) continue;
        if (v.raw > 0) addResource(sim, player, uint16_t(r), v * count);
        else gs.players[player].stock[r] = fxMax(rules_.resources[r].min, gs.players[player].stock[r] + v * count);
    }
}

Fixed RtsModule::playerModsStat(Simulation& sim, uint8_t player, uint16_t type, Stat s, Fixed& mul) const {
    auto* gs = sim.world().resource<GameState>();
    Fixed add{};
    if (player == 0 || player >= gs->players.size()) return add;
    const auto& eco = gs->players[player];
    for (uint16_t src : rules_.playerModTypes) {
        const UnitType& st = rules_.types[src];
        int n = src < eco.typeCount.size() ? eco.typeCount[src] : 0;
        if (st.playerModsMax > 0) n = std::min(n, st.playerModsMax);
        if (n <= 0) continue;
        for (auto& [target, mod] : st.playerMods) {
            if (mod.stat != s || !rules_.matches(target, type)) continue;
            add += mod.add * n;
            for (int k = 0; k < n; ++k) mul = mul * mod.mul;
        }
    }
    for (uint16_t r : rules_.perUnitResources) {
        int n = int(fxMax(Fixed{}, eco.stock[r]).floorInt());
        if (n <= 0) continue;
        for (auto& [tags, mod] : rules_.resources[r].perUnitMods) {
            if (mod.stat != s || (tags && !(rules_.types[type].tags & tags))) continue;
            add += mod.add * n;
            for (int k = 0; k < n; ++k) mul = mul * mod.mul;
        }
    }
    return add;
}

// ================================================================= garrisons

void RtsModule::assaultOriginals(Simulation& sim, Entity unit, Entity structure) {
    World& w = sim.world();
    gainOn(sim, w.get<Owner>(unit).player, &ResourceDef::gainLost, kOne);
    gainOn(sim, w.get<Owner>(structure).player, &ResourceDef::gainDefended, kOne);
}

void RtsModule::captureOriginals(Simulation& sim, Entity s, uint8_t from, uint8_t to) {
    World& w = sim.world();
    gainOn(sim, to, &ResourceDef::gainCapture, kOne);
    gainOn(sim, from, &ResourceDef::gainCaptured, kOne);
    Unit& u = w.get<Unit>(s);
    const uint16_t into = rules_.types[u.type].captureAs;
    if (into == kNoType || into == u.type) return;
    // loses a level (Mushroom Wars 2): same footprint expected; keep the hp share
    GameState& gs = state(sim);
    const UnitType& nt = rules_.types[into];
    Fixed frac = u.hp / fxMax(rules_.types[u.type].maxHp, kOne);
    if (to < gs.players.size()) { if (gs.players[to].typeCountAll[u.type] > 0) gs.players[to].typeCountAll[u.type]--; gs.players[to].typeCountAll[into]++; }
    u.type = into;
    u.hp = nt.maxHp * frac;
    if (auto* p = w.tryGet<Production>(s)) p->queue.clear();
    sim.emit({kEvUpgraded, to, s, kNullEntity, into, w.get<Transform>(s).pos});
}

// ================================================================= crews

bool RtsModule::canCrew(Simulation& sim, Entity unit, Entity vehicle) const {
    World& w = sim.world();
    if (!w.alive(unit) || !w.alive(vehicle) || unit == vehicle || !w.has<Unit>(vehicle)) return false;
    const UnitType& ut = rules_.types[w.get<Unit>(unit).type];
    const UnitType& vt = rules_.types[w.get<Unit>(vehicle).type];
    return ut.crews && vt.crew != kNoType && w.get<Owner>(vehicle).player == 0 && w.get<Unit>(vehicle).hp.raw > 0;
}

bool RtsModule::crewEnter(Simulation& sim, Entity unit, Entity vehicle) {
    if (!canCrew(sim, unit, vehicle)) return false;
    World& w = sim.world();
    GameState& gs = state(sim);
    const uint8_t me = w.get<Owner>(unit).player;
    Unit& vu = w.get<Unit>(vehicle);
    w.get<Owner>(vehicle).player = me;
    if (me < gs.players.size()) gs.players[me].typeCountAll[vu.type]++;
    Unit& uu = w.get<Unit>(unit);
    uu.hp = Fixed{};
    uu.flags |= kUnitSilent;
    sim.emit({kEvCrewed, me, vehicle, unit, vu.type, w.get<Transform>(vehicle).pos});
    return true;
}

// ================================================================= per tick

void RtsModule::sysOriginals(Simulation& sim) {
    World& w = sim.world();
    GameState& gs = state(sim);
    const Fixed dt = sim.dt();
    const uint32_t tick = sim.tick();
    // shields regenerate after a pause without hits
    w.each<Shield, Unit>([&](Entity, Shield& sh, Unit& u) {
        const UnitType& t = rules_.types[u.type];
        if (u.hp.raw <= 0 || sh.value >= t.shieldMax) return;
        if (tick - sh.lastHit < uint32_t((t.shieldDelay * int64_t(sim.tickRate())).toInt())) return;
        sh.value = fxMin(t.shieldMax, sh.value + t.shieldRegen * dt);
    });
    // clashes: hostile units in transit (sends) that meet on the road destroy each other one for one
    {
        struct Walker { Entity e; uint8_t owner; Vec3 pos; Fixed r; };
        std::vector<Walker> walkers;
        w.each<Unit, Owner, Transform, Orders>([&](Entity e, Unit& u, Owner& o, Transform& tr, Orders& ord) {
            if (u.hp.raw <= 0 || !rules_.types[u.type].clash || ord.queue.empty() || ord.queue.front().type != OrderType::Enter) return;
            walkers.push_back({e, o.player, tr.pos, rules_.types[u.type].radius});
        });
        std::vector<uint8_t> dead(walkers.size(), 0);
        for (size_t i = 0; i < walkers.size(); ++i) {
            if (dead[i]) continue;
            for (size_t j = i + 1; j < walkers.size(); ++j) {
                if (dead[j] || !hostile(sim, walkers[i].owner, walkers[j].owner)) continue;
                if (dist2D(walkers[i].pos, walkers[j].pos) > walkers[i].r + walkers[j].r + Fixed::fromRatio(1, 10)) continue;
                // the stronger side (attack stat) may survive the exchange
                Fixed a = stat(sim, walkers[i].e, Stat::Damage), b = stat(sim, walkers[j].e, Stat::Damage);
                dead[i] = 1; dead[j] = 1;
                if (a > b && sim.rng().unit() < (a - b) / a) dead[i] = 0;
                else if (b > a && sim.rng().unit() < (b - a) / b) dead[j] = 0;
                break;
            }
        }
        for (size_t i = 0; i < walkers.size(); ++i)
            if (dead[i]) {
                Unit& u = w.get<Unit>(walkers[i].e);
                u.hp = Fixed{}; u.flags |= kUnitSilent;
                gainOn(sim, walkers[i].owner, &ResourceDef::gainLost, kOne);
            }
    }
    // crushing: vehicles running over infantry (and spice blooms) kill them
    {
        struct Crusher { Entity e; uint8_t owner; Vec3 pos; Fixed r; uint64_t tags; };
        std::vector<Crusher> crushers;
        w.each<Unit, Owner, Transform, Mobile>([&](Entity e, Unit& u, Owner& o, Transform& tr, Mobile& mb) {
            const UnitType& t = rules_.types[u.type];
            if (t.crushTags && mb.moving && u.hp.raw > 0 && !(u.flags & (kUnitInHand | kUnitUnderConstruction))) crushers.push_back({e, o.player, tr.pos, t.radius, t.crushTags});
        });
        for (auto& c : crushers) {
            std::vector<Entity> victims;
            gs.hash.query(c.pos, c.r + kOne, [&](Entity v) {
                auto* vu = w.tryGet<Unit>(v);
                if (!vu || v == c.e || vu->hp.raw <= 0 || (vu->flags & kUnitInHand)) return;
                const UnitType& vt = rules_.types[vu->type];
                if (!(vt.tags & c.tags) || vt.invulnerable || (vt.layer & kLayerAir)) return;
                const uint8_t vo = w.get<Owner>(v).player;
                if (vo == c.owner || (vo != 0 && !hostile(sim, c.owner, vo))) return;
                if (dist2D(w.get<Transform>(v).pos, c.pos) > c.r + vt.radius + Fixed::fromRatio(15, 100)) return;   // touching: avoidance never lets them overlap
                victims.push_back(v);
            });
            for (Entity v : victims) {
                Unit& vu = w.get<Unit>(v);
                if (vu.hp.raw <= 0) continue;
                vu.hp = Fixed{};
                if (c.owner != 0) vu.lastAttacker = c.owner;
                sim.emit({kEvHit, c.owner, v, c.e, 0, w.get<Transform>(v).pos});
            }
        }
    }
    // eaters (sandworms) submerge after enough meals and come back somewhere else later
    std::vector<Entity> sink;
    w.each<Eater, Unit>([&](Entity e, Eater& ea, Unit& u) {
        const UnitType& t = rules_.types[u.type];
        if (u.hp.raw > 0 && t.eats > 0 && ea.kills >= t.eats) sink.push_back(e);
    });
    for (Entity e : sink) {
        Unit& u = w.get<Unit>(e);
        gs.resurface.push_back({u.type, w.get<Owner>(e).player, tick + uint32_t((rules_.types[u.type].resurface * int64_t(sim.tickRate())).toInt())});
        sim.emit({kEvSubmerged, w.get<Owner>(e).player, e, kNullEntity, u.type, w.get<Transform>(e).pos});
        u.hp = Fixed{}; u.flags |= kUnitSilent;
    }
    for (size_t k = 0; k < gs.resurface.size();) {
        if (gs.resurface[k].at > tick) { ++k; continue; }
        const auto rs = gs.resurface[k];
        const UnitType& t = rules_.types[rs.type];
        MapGrid& g = gs.grid;
        bool placed = false;
        for (int tries = 0; tries < 64 && !placed; ++tries) {
            int x = int(sim.rng().below(uint32_t(g.w))), y = int(sim.rng().below(uint32_t(g.h)));
            if (!g.passable(x, y, t.pathLayer)) continue;
            spawn(sim, rs.type, rs.owner, g.cellCenter(x, y));
            placed = true;
        }
        if (placed) gs.resurface.erase(gs.resurface.begin() + long(k));
        else { gs.resurface[k].at = tick + sim.tickRate(); ++k; }
    }
}

void RtsModule::hashOriginals(Simulation& sim, Hasher& h) {
    World& w = sim.world();
    GameState& gs = state(sim);
    for (auto& p : gs.players) {
        h.i32(p.heroesMade);
        h.u32(uint32_t(p.deadHeroes.size()));
        for (auto& d : p.deadHeroes) { h.u32(d.type); h.i32(d.level); h.fx(d.xp); }
    }
    for (auto& r : gs.resurface) { h.u32(r.type); h.u32(r.owner); h.u32(r.at); }
    w.each<Shield>([&](Entity e, Shield& s) { h.u64(e.packed()); h.fx(s.value); h.u32(s.lastHit); });
    w.each<Eater>([&](Entity e, Eater& ea) { h.u64(e.packed()); h.i32(ea.kills); });
    w.each<Hero>([&](Entity e, Hero& hr) { h.u64(e.packed()); h.u32(hr.points); for (auto s : hr.skill) h.u32(s); });
}

} // namespace rts::game
