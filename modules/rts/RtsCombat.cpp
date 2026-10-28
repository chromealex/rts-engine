// Combat: target acquisition, weapons (height advantage, misses, squads), projectiles, damage,
// abilities (autocast, auras, buffs, stuns, summons, blink), hero experience, death (bounty, score,
// wrecks, depleted extractors).
#include "RtsInternal.h"
#include <algorithm>

namespace rts::game {
using namespace detail;

// ================================================================= targeting

void RtsModule::sysTargeting(Simulation& sim) {
    World& w = sim.world();
    GameState& gs = state(sim);
    w.each<Combat, Unit, Owner, Transform>([&](Entity e, Combat& c, Unit& u, Owner& o, Transform& tr) {
        if (o.player == 0 || (u.flags & (kUnitUnderConstruction | kUnitDisabled | kUnitInHand | kUnitPossessed))) return;
        if (++c.acquireTimer < 4) return;
        c.acquireTimer = 0;
        const UnitType& t = rules_.types[u.type];
        auto* ord = w.tryGet<Orders>(e);
        if (ord && !ord->queue.empty()) {
            OrderType ot = ord->queue.front().type;
            if (ot != OrderType::AttackMove && ot != OrderType::Patrol) return;
        }
        if (auto* gd = w.tryGet<Guard>(e); gd && gd->returning) return;
        if (w.alive(c.target) && w.has<Unit>(c.target)) {
            if (!c.autoTarget) return;
            if (dist2D(w.get<Transform>(c.target).pos, tr.pos) <= stat(sim, e, Stat::Sight) + kOne) return;
        }
        uint8_t layers = 0;
        for (auto& wd : t.weapons) layers |= wd.targets;
        Fixed sight = stat(sim, e, Stat::Sight);
        int myLevel = levelAt(sim, tr.pos);
        Entity best;
        Fixed bestD{};
        gs.hash.query(tr.pos, sight, [&](Entity cand) {
            if (cand == e) return;
            auto* cu = w.tryGet<Unit>(cand);
            if (!cu || cu->hp.raw <= 0) return;
            const UnitType& ct = rules_.types[cu->type];
            if (ct.invulnerable || !(ct.layer & layers)) return;
            if (!hostile(sim, o.player, w.get<Owner>(cand).player)) return;
            Vec3 cp = w.get<Transform>(cand).pos;
            Fixed d = dist2D(cp, tr.pos);
            if (d > sight) return;
            // high ground hides units from those below unless revealed (any ally vision of that cell)
            if (rules_.heights.blockVision && !(ct.layer & kLayerAir) && levelAt(sim, cp) > myLevel && !isVisible(sim, o.player, cp)) return;
            if (ct.weapons.empty()) d += Fixed::fromInt(4); // prefer things that can shoot back
            if (!best.valid() || d < bestD) { best = cand; bestD = d; }
        });
        c.target = best;
        c.autoTarget = true;
    });
}

// ================================================================= damage

void RtsModule::dealDamage(Simulation& sim, Entity target, Fixed dmg, uint16_t dmgType, Entity source, uint8_t sourceOwner) {
    World& w = sim.world();
    auto* u = w.tryGet<Unit>(target);
    if (!u || u->hp.raw <= 0) return;
    const UnitType& t = rules_.types[u->type];
    if (t.invulnerable) return;
    Fixed raw = dmg * rules_.damageMult(dmgType, t.armorType);
    Fixed armor = stat(sim, target, Stat::Armor);
    Fixed fin = fxMax(raw - armor, raw / 5);
    u->hp -= fin;
    if (sourceOwner != 0) u->lastAttacker = sourceOwner;
    sim.emit({kEvHit, sourceOwner, target, source, fin.raw, w.get<Transform>(target).pos});
    // creeps fight back as a camp
    if (auto* g = w.tryGet<Guard>(target); g && w.alive(source) && w.has<Unit>(source)) {
        if (auto* c = w.tryGet<Combat>(target); c && !w.alive(c->target)) { c->target = source; c->autoTarget = true; }
    }
}

void RtsModule::sysWeapons(Simulation& sim) {
    World& w = sim.world();
    GameState& gs = state(sim);
    const Fixed dt = sim.dt();
    struct Shot { Projectile pr; Vec3 start; };
    std::vector<Shot> shots;
    w.each<Combat, Unit, Owner, Transform>([&](Entity e, Combat& c, Unit& u, Owner& o, Transform& tr) {
        const UnitType& t = rules_.types[u.type];
        Fixed atkSpeed = fxMax(stat(sim, e, Stat::AttackSpeed), Fixed::fromRatio(1, 10));
        for (size_t i = 0; i < t.weapons.size() && i < 4; ++i) c.cooldown[i] = fxMax(Fixed{}, c.cooldown[i] - dt * atkSpeed);
        if (u.flags & (kUnitUnderConstruction | kUnitDisabled | kUnitInHand)) return;
        if (auto* b = w.tryGet<Buffs>(e); b && b->stunUntil > sim.tick()) return;
        if (!w.alive(c.target) || !w.has<Unit>(c.target) || (w.get<Unit>(c.target).flags & kUnitInHand)) { c.target = kNullEntity; return; }
        const Unit& tu = w.get<Unit>(c.target);
        const UnitType& tt = rules_.types[tu.type];
        if (tu.hp.raw <= 0) return;
        auto* ord = w.tryGet<Orders>(e);
        auto* m = w.tryGet<Mobile>(e);
        Fixed dist = contactDistance(sim, e, c.target);
        Vec3 tp = w.get<Transform>(c.target).pos;
        const int adv = (t.layer & kLayerAir) || (tt.layer & kLayerAir) ? 0 : levelAt(sim, tr.pos) - levelAt(sim, tp);
        Fixed rangeBonus = stat(sim, e, Stat::Range) + (adv > 0 ? rules_.heights.rangePerLevel * adv : Fixed{});
        Fixed range = maxWeaponRange(t) + rangeBonus;
        // idle units with an automatic target chase it (unless holding / guarding a structure)
        if (c.autoTarget && ord && ord->queue.empty() && m && !(u.flags & kUnitHold) && dist > range) {
            if (dist > stat(sim, e, Stat::Sight) + Fixed::fromInt(2)) { c.target = kNullEntity; return; }
            if (!m->moving || (sim.tick() + e.index) % 20 == 0) moveTo(sim, e, tp, range);
            return;
        }
        if (c.autoTarget && ord && ord->queue.empty() && m && m->moving && dist <= range) stopMoving(sim, e);
        Vec3 face = flat(tp - tr.pos);
        if (face.length().raw > 0) u.facing = face / face.length();
        // damage scaling: techs/buffs, squad members alive, height advantage
        Fixed mult = stat(sim, e, Stat::Damage);
        if (t.members > 1) mult = mult * membersAlive(sim, e) / t.members;
        if (adv > 0) mult = mult * (kOne + rules_.heights.damagePerLevel * adv);
        for (size_t i = 0; i < t.weapons.size() && i < 4; ++i) {
            const WeaponDef& wd = t.weapons[i];
            if (!canHit(wd, tt.layer) || c.cooldown[i].raw > 0 || dist > wd.range + rangeBonus || dist < wd.minRange) continue;
            if (m && m->moving && ord && !ord->queue.empty() && ord->queue.front().type == OrderType::Move) continue;
            c.cooldown[i] = wd.cooldown;
            sim.emit({kEvAttack, o.player, e, c.target, int64_t(i), tr.pos});
            // shooting uphill can miss (StarCraft); the roll uses the shared deterministic RNG
            if (adv < 0 && rules_.heights.missUphill.raw > 0 && sim.rng().unit() < rules_.heights.missUphill) {
                sim.emit({kEvMiss, o.player, e, c.target, 0, tp});
                continue;
            }
            Fixed dmg = wd.damage * mult;
            if (wd.projectileSpeed.raw <= 0) {
                dealDamage(sim, c.target, dmg, wd.damageType, e, o.player);
                if (wd.splash.raw > 0) {
                    gs.hash.query(tp, wd.splash, [&](Entity s) {
                        if (s == c.target || !w.has<Unit>(s)) return;
                        if (!hostile(sim, o.player, w.get<Owner>(s).player)) return;
                        if (dist2D(w.get<Transform>(s).pos, tp) <= wd.splash) dealDamage(sim, s, dmg, wd.damageType, e, o.player);
                    });
                }
            } else {
                Projectile pr;
                pr.target = c.target; pr.source = e; pr.aim = tp; pr.speed = wd.projectileSpeed;
                pr.damage = dmg; pr.splash = wd.splash; pr.damageType = wd.damageType; pr.owner = o.player; pr.layers = wd.targets;
                Vec3 start = tr.pos;
                start.y = Fixed::fromRatio(6, 10);
                shots.push_back({pr, start});
            }
        }
    });
    for (auto& s : shots) { // created after the loop: adding components may reallocate pools
        Entity p = w.create();
        w.add<Transform>(p, Transform{s.start, s.start, Fixed{}});
        w.add<Projectile>(p, s.pr);
        w.add<Owner>(p, Owner{s.pr.owner});
    }
}

void RtsModule::sysProjectiles(Simulation& sim) {
    World& w = sim.world();
    GameState& gs = state(sim);
    const Fixed dt = sim.dt();
    w.each<Projectile, Transform>([&](Entity p, Projectile& pr, Transform& tr) {
        tr.prevPos = tr.pos;
        if (w.alive(pr.target) && w.has<Unit>(pr.target)) { pr.aim = w.get<Transform>(pr.target).pos; pr.aim.y = Fixed::fromRatio(4, 10); }
        if (!moveTowards(tr.pos, pr.aim, pr.speed * dt)) return;
        if (w.alive(pr.target) && w.has<Unit>(pr.target)) dealDamage(sim, pr.target, pr.damage, pr.damageType, pr.source, pr.owner);
        if (pr.splash.raw > 0) {
            gs.hash.query(pr.aim, pr.splash, [&](Entity s) {
                if (s == pr.target || !w.has<Unit>(s)) return;
                if (!hostile(sim, pr.owner, w.get<Owner>(s).player)) return;
                if (dist2D(w.get<Transform>(s).pos, pr.aim) <= pr.splash) dealDamage(sim, s, pr.damage, pr.damageType, pr.source, pr.owner);
            });
        }
        w.destroyLater(p);
    });
}

// ================================================================= abilities

bool RtsModule::canCast(Simulation& sim, Entity caster, int index) const {
    World& w = sim.world();
    auto* u = w.tryGet<Unit>(caster);
    if (!u || u->hp.raw <= 0 || (u->flags & (kUnitUnderConstruction | kUnitDisabled))) return false;
    const UnitType& t = rules_.types[u->type];
    if (index < 0 || size_t(index) >= t.abilities.size() || index >= 8) return false;
    const AbilityDef& a = t.abilities[size_t(index)];
    if (a.aura) return false;
    if (auto* b = w.tryGet<Buffs>(caster); b && b->stunUntil > sim.tick()) return false;
    if (auto* h = w.tryGet<Hero>(caster); h && h->level < a.requiredLevel) return false;
    if (u->energy < a.energyCost) return false;
    auto* st = w.tryGet<AbilityState>(caster);
    return !st || st->readyAt[index] <= sim.tick();
}

namespace {
void addBuff(Buffs& b, const Buff& nb) {
    for (auto& x : b.list)
        if (x.stat == nb.stat && x.sourceType == nb.sourceType) { x = nb; return; } // refresh, don't stack
    b.list.push_back(nb);
}
} // namespace

void RtsModule::castAbility(Simulation& sim, Entity caster, int index, Entity target, Vec3 point) {
    World& w = sim.world();
    if (!canCast(sim, caster, index)) return;
    Unit& u = w.get<Unit>(caster);
    const uint16_t casterType = u.type;
    const AbilityDef a = rules_.types[casterType].abilities[size_t(index)]; // copy: spawning may move vectors? (rules are stable, but be safe)
    const uint8_t owner = w.get<Owner>(caster).player;
    u.energy -= a.energyCost;
    if (auto* st = w.tryGet<AbilityState>(caster)) st->readyAt[index] = sim.tick() + uint32_t((a.cooldown * int64_t(sim.tickRate())).toInt());
    if (a.target == TargetKind::Self || a.target == TargetKind::None) { target = caster; point = w.get<Transform>(caster).pos; }
    if (w.alive(target) && w.has<Transform>(target)) point = w.get<Transform>(target).pos;
    sim.emit({kEvAbility, owner, caster, target, index, point});
    applyEffects(sim, caster, owner, casterType, a.effects, target, point);
}

// Shared by unit abilities and player abilities (caster may be null: nukes, air strikes).
void RtsModule::applyEffects(Simulation& sim, Entity caster, uint8_t owner, uint16_t casterType, const std::vector<EffectDef>& effects, Entity target, Vec3 point) {
    World& w = sim.world();
    const uint32_t ticks = sim.tickRate();

    struct Summon_ { uint16_t type; int count; uint32_t expire; };
    std::vector<Summon_> summons;
    for (auto& ef : effects) {
        // collect affected entities
        std::vector<Entity> hits;
        if (ef.radius.raw > 0) {
            state(sim).hash.query(point, ef.radius, [&](Entity s) {
                auto* su = w.tryGet<Unit>(s);
                if (!su || su->hp.raw <= 0 || (su->flags & kUnitInHand) || dist2D(w.get<Transform>(s).pos, point) > ef.radius) return;
                uint8_t so = w.get<Owner>(s).player;
                bool enemy = hostile(sim, owner, so);
                bool ally = so == owner || (so != 0 && so != kHostileNeutral && owner != kHostileNeutral && sim.allied(so, owner));
                if ((enemy && ef.enemies) || (ally && ef.allies)) hits.push_back(s);
            });
            std::sort(hits.begin(), hits.end(), [](Entity x, Entity y) { return x.packed() < y.packed(); });
        } else if (w.alive(target) && w.has<Unit>(target)) {
            hits.push_back(target);
        }
        const uint32_t until = sim.tick() + uint32_t((ef.duration * int64_t(ticks)).toInt());
        switch (ef.kind) {
        case EffectKind::Damage:
            for (Entity h : hits) dealDamage(sim, h, ef.amount, ef.damageType, caster, owner);
            break;
        case EffectKind::Heal:
            for (Entity h : hits) { Unit& hu = w.get<Unit>(h); hu.hp = fxMin(stat(sim, h, Stat::MaxHp), hu.hp + ef.amount); }
            break;
        case EffectKind::Stun:
            for (Entity h : hits) if (auto* b = w.tryGet<Buffs>(h)) b->stunUntil = std::max(b->stunUntil, until);
            break;
        case EffectKind::Buff:
            for (Entity h : hits) if (auto* b = w.tryGet<Buffs>(h)) addBuff(*b, {ef.mod.stat, ef.mod.add, ef.mod.mul, until, casterType});
            break;
        case EffectKind::Spawn:
            summons.push_back({ef.spawnType, ef.count, ef.duration.raw > 0 ? until : 0});
            break;
        case EffectKind::Blink: {
            if (!w.alive(caster)) break;
            auto& g = state(sim).grid;
            int x = g.cellX(point.x), y = g.cellY(point.z);
            const UnitType& ct = rules_.types[w.get<Unit>(caster).type];
            if (g.passable(x, y, ct.layer)) {
                Transform& tr = w.get<Transform>(caster);
                tr.pos = flat(point);
                tr.prevPos = tr.pos;
                stopMoving(sim, caster);
            }
            break;
        }
        }
    }
    for (auto& s : summons)
        for (int k = 0; k < s.count; ++k) {
            Vec3 p = point + Vec3{Fixed::fromRatio((k % 3) - 1, 1), Fixed{}, Fixed::fromRatio((k / 3) % 3 - 1, 1)};
            Entity n = spawn(sim, s.type, owner, p);
            if (s.expire) w.add<Summon>(n, Summon{s.expire});
        }
}

void RtsModule::sysAbilities(Simulation& sim) {
    World& w = sim.world();
    GameState& gs = state(sim);
    const uint32_t tick = sim.tick();
    // expire buffs and summons
    if (tick % 5 == 0) {
        w.each<Buffs>([&](Entity, Buffs& b) {
            b.list.erase(std::remove_if(b.list.begin(), b.list.end(), [&](const Buff& x) { return x.until <= tick; }), b.list.end());
        });
        w.each<Summon, Unit>([&](Entity, Summon& s, Unit& u) {
            if (s.expire && s.expire <= tick) { u.hp = Fixed{}; u.flags |= kUnitSilent; }
        });
    }
    // auras: refreshed buffs around their owner
    if (tick % 10 == 0) {
        struct AuraHit { Entity target; Buff buff; };
        std::vector<AuraHit> hits;
        w.each<AbilityState, Unit, Owner, Transform>([&](Entity e, AbilityState&, Unit& u, Owner& o, Transform& tr) {
            if (u.hp.raw <= 0 || (u.flags & kUnitUnderConstruction)) return;
            const UnitType& t = rules_.types[u.type];
            for (auto& a : t.abilities) {
                if (!a.aura) continue;
                if (auto* h = w.tryGet<Hero>(e); h && h->level < a.requiredLevel) continue;
                for (auto& ef : a.effects) {
                    if (ef.kind != EffectKind::Buff) continue;
                    gs.hash.query(tr.pos, a.auraRadius, [&](Entity s) {
                        if (!w.has<Unit>(s) || dist2D(w.get<Transform>(s).pos, tr.pos) > a.auraRadius) return;
                        uint8_t so = w.get<Owner>(s).player;
                        bool enemy = hostile(sim, o.player, so);
                        bool ally = so == o.player || (so != 0 && so != kHostileNeutral && o.player != kHostileNeutral && sim.allied(so, o.player));
                        if ((enemy && ef.enemies) || (ally && ef.allies)) hits.push_back({s, {ef.mod.stat, ef.mod.add, ef.mod.mul, tick + 15, u.type}});
                    });
                }
            }
        });
        for (auto& h : hits) if (auto* b = w.tryGet<Buffs>(h.target)) addBuff(*b, h.buff);
    }
    // autocast (also what makes AI heroes and casters use their spells)
    if (tick % 8 == 0) {
        struct Cast { Entity caster; int index; Entity target; Vec3 point; };
        std::vector<Cast> casts;
        w.each<AbilityState, Unit, Owner, Transform>([&](Entity e, AbilityState&, Unit& u, Owner& o, Transform& tr) {
            const UnitType& t = rules_.types[u.type];
            auto* c = w.tryGet<Combat>(e);
            Entity enemy = c && w.alive(c->target) ? c->target : kNullEntity;
            if (auto* ord = w.tryGet<Orders>(e); ord && !ord->queue.empty() && ord->queue.front().type == OrderType::Cast) return;
            for (size_t i = 0; i < t.abilities.size() && i < 8; ++i) {
                const AbilityDef& a = t.abilities[i];
                if (!a.autocast || a.aura || !canCast(sim, e, int(i))) continue;
                bool heal = !a.effects.empty() && a.effects[0].kind == EffectKind::Heal;
                bool helpful = !a.effects.empty() && (heal || (a.effects[0].kind == EffectKind::Buff && !a.effects[0].enemies));
                if (heal) {
                    // most wounded ally in range
                    Entity best; Fixed bestFrac = Fixed::fromRatio(7, 10);
                    gs.hash.query(tr.pos, a.range, [&](Entity s) {
                        auto* su = w.tryGet<Unit>(s);
                        if (!su || su->hp.raw <= 0 || w.get<Owner>(s).player != o.player) return;
                        if (rules_.types[su->type].structure || dist2D(w.get<Transform>(s).pos, tr.pos) > a.range) return;
                        Fixed frac = su->hp / fxMax(stat(sim, s, Stat::MaxHp), kOne);
                        if (frac < bestFrac) { bestFrac = frac; best = s; }
                    });
                    if (best.valid()) { casts.push_back({e, int(i), best, w.get<Transform>(best).pos}); break; }
                    continue;
                }
                if (!enemy.valid()) continue;
                Vec3 ep = w.get<Transform>(enemy).pos;
                if (a.target == TargetKind::None || a.target == TargetKind::Self) {
                    if (helpful || dist2D(ep, tr.pos) <= fxMax(a.range, Fixed::fromInt(4))) { casts.push_back({e, int(i), e, tr.pos}); break; }
                    continue;
                }
                if (dist2D(ep, tr.pos) > a.range) continue;
                casts.push_back({e, int(i), a.target == TargetKind::Unit ? enemy : kNullEntity, ep});
                break;
            }
        });
        for (auto& c : casts) if (w.alive(c.caster)) castAbility(sim, c.caster, c.index, c.target, c.point);
    }
}

// ================================================================= experience / death

void RtsModule::grantKill(Simulation& sim, Entity victim, uint8_t killer) {
    World& w = sim.world();
    GameState& gs = state(sim);
    const Unit& vu = w.get<Unit>(victim);
    const UnitType& vt = rules_.types[vu.type];
    const Vec3 vp = w.get<Transform>(victim).pos;
    const uint8_t victimOwner = w.get<Owner>(victim).player;
    if (killer == 0 || killer == kHostileNeutral || !hostile(sim, killer, victimOwner)) return;
    // bounty
    for (auto& b : vt.bounty) addResource(sim, killer, b.res, b.value);
    // score (lane battlers, arcade modes)
    uint8_t team = teamOf(sim, killer);
    if (team < gs.score.size()) gs.score[team] += vt.score;
    // hero experience, shared by the killer's heroes nearby
    std::vector<Entity> heroes;
    w.each<Hero, Owner, Transform>([&](Entity e, Hero&, Owner& o, Transform& t) {
        if (o.player == killer && dist2D(t.pos, vp) <= Fixed::fromInt(12)) heroes.push_back(e);
    });
    if (heroes.empty()) return;
    Fixed share = vt.xpValue / int64_t(heroes.size());
    for (Entity h : heroes) addHeroXp(sim, h, share, killer);
}

void RtsModule::addHeroXp(Simulation& sim, Entity h, Fixed xp, uint8_t player) {
    World& w = sim.world();
    auto* hr = w.tryGet<Hero>(h);
    if (!hr) return;
    Unit& hu = w.get<Unit>(h);
    const UnitType& ht = rules_.types[hu.type];
    hr->xp += xp;
    while (size_t(hr->level - 1) < ht.xpLevels.size() && hr->xp >= Fixed::fromInt(ht.xpLevels[size_t(hr->level - 1)])) {
        Fixed before = stat(sim, h, Stat::MaxHp);
        ++hr->level;
        Fixed after = stat(sim, h, Stat::MaxHp);
        hu.hp += after - before;
        sim.emit({kEvLevelUp, player, h, kNullEntity, hr->level, w.get<Transform>(h).pos});
    }
}

void RtsModule::sysDeath(Simulation& sim) {
    World& w = sim.world();
    GameState& gs = state(sim);
    std::vector<Entity> dead;
    w.each<Unit>([&](Entity e, Unit& u) {
        if (u.hp.raw > 0) return;
        const UnitType& t = rules_.types[u.type];
        if (t.invulnerable && t.resourceNode < 0 && !(u.flags & kUnitSilent)) { u.hp = t.maxHp; return; }
        dead.push_back(e);
    });
    struct Leftover { uint16_t type; Vec3 pos; int16_t res; Fixed amount; };
    std::vector<Leftover> leftovers;
    const int wreckType = rules_.findType("wreck");
    for (Entity e : dead) {
        Unit& u = w.get<Unit>(e);
        const UnitType& t = rules_.types[u.type];
        const uint8_t owner = w.get<Owner>(e).player;
        const Vec3 pos = w.get<Transform>(e).pos;
        const bool silent = u.flags & kUnitSilent;
        if (auto* f = w.tryGet<Footprint>(e)) gs.grid.block(f->x, f->y, f->w, f->h, -1);
        if (owner < gs.players.size() && !gs.players[owner].typeCountAll.empty() && gs.players[owner].typeCountAll[u.type] > 0)
            gs.players[owner].typeCountAll[u.type]--;
        if (auto* ex = w.tryGet<Extractor>(e)) {
            // a destroyed refinery gives the geyser back with what was left in it
            auto* rs = w.tryGet<ResourceStock>(e);
            leftovers.push_back({ex->nodeType, pos, rs ? rs->res : int16_t(-1), rs ? rs->amount : Fixed{}});
        }
        if (!silent) {
            grantKill(sim, e, u.lastAttacker);
            if (!t.wreck.empty() && wreckType >= 0) leftovers.push_back({uint16_t(wreckType), pos, int16_t(t.wreck[0].res), t.wreck[0].value});
            sim.emit({kEvDied, owner, e, kNullEntity, u.type, pos});
        }
        w.destroyLater(e);
    }
    if (!dead.empty()) w.flush(); // free footprints/ids before leftovers are placed
    for (auto& l : leftovers) {
        Entity n = spawn(sim, l.type, 0, l.pos);
        if (auto* rs = w.tryGet<ResourceStock>(n); rs && l.res >= 0) { rs->res = l.res; rs->amount = l.amount; rs->infinite = l.amount.raw < 0; }
    }
}

} // namespace rts::game
