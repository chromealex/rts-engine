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
        if (mechOn(sim) && !mechCanAcquire(sim, e)) { if (c.autoTarget && w.alive(c.target)) { c.target = kNullEntity; if (!w.has<Orders>(e) || w.get<Orders>(e).queue.empty()) stopMoving(sim, e); } return; }   // agent-mech
        if (++c.acquireTimer < 4) return;
        if (rules_.advancedEffects && (statusFlags(sim, e) & (kStBlind | kStStun | kStDisarm))) return;
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
        const bool hf = gs.grid.hf;
        Entity best;
        Fixed bestD{};
        gs.hash.query(tr.pos, sight, [&](Entity cand) {
            if (cand == e) return;
            auto* cu = w.tryGet<Unit>(cand);
            if (!cu || cu->hp.raw <= 0) return;
            const UnitType& ct = rules_.types[cu->type];
            if (ct.invulnerable || !(ct.layer & layers)) return;
            if (!hostile(sim, o.player, w.get<Owner>(cand).player)) return;
            if (rules_.advancedEffects) { uint32_t fl = statusFlags(sim, cand); if ((fl & kStInvisible) && !(fl & kStRevealed) && !(rules_.mech.detection && detected(sim, cand, o.player))) return; }
            Vec3 cp = w.get<Transform>(cand).pos;
            Fixed d = dist2D(cp, tr.pos);
            if (d > sight) return;
            // high ground hides units from those below unless revealed (any ally vision of that cell)
            // (heightfield: vision already follows line of sight, so the target must simply be in sight)
            if (rules_.heights.blockVision && !(ct.layer & kLayerAir) && (hf || levelAt(sim, cp) > myLevel) && !isVisible(sim, o.player, cp)) return;
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
    if (rules_.mech.shields) { dmg = mechShieldAbsorb(sim, target, dmg); if (dmg.raw <= 0) { sim.emit({kEvHit, sourceOwner, target, source, 0, w.get<Transform>(target).pos}); return; } }
    Fixed raw = dmgType == kDamagePure ? dmg : dmg * rules_.damageMult(dmgType, t.armorType);
    Fixed armor = stat(sim, target, Stat::Armor);
    Fixed fin = rules_.mech.any ? mechArmorDamage(sim, target, raw, dmgType) : fxMax(raw - armor, raw / 5);   // agent-mech armor models
    if (mechOn(sim)) fin = fin * mechDamageTaken(sim, target);   // agent-mech (retreat)
    if (rules_.advancedEffects) {
        uint32_t fl = statusFlags(sim, target);
        if (fl & kStInvulnerable) return;
        if (fl & kStFake) { u->hp = Fixed{}; u->flags |= kUnitSilent; return; }  // illusions pop on any hit
        fin = fin * stat(sim, target, Stat::DamageTaken);
        if (auto* st = w.tryGet<Statuses>(target))
            for (auto& in : st->list) {
                if (in.shield.raw <= 0 || fin.raw <= 0) continue;
                Fixed absorb = fxMin(in.shield, fin);
                in.shield -= absorb; fin -= absorb;
                if (in.shield.raw <= 0 && rules_.statuses[in.def].shield.raw > 0) in.until = sim.tick(); // shield broken
            }
        if (w.alive(source) && w.has<Unit>(source)) {
            Fixed ls = stat(sim, source, Stat::Lifesteal);
            if (ls.raw > 0) { Unit& su = w.get<Unit>(source); su.hp = fxMin(stat(sim, source, Stat::MaxHp), su.hp + fin * ls); }
        }
    }
    u->hp -= fin;
    if (sourceOwner != 0) u->lastAttacker = sourceOwner;
    if (rules_.mech.veterancy && w.alive(source)) mechOnHit(sim, target, source);   // agent-mech
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
    struct OnHit { int16_t payload; Entity caster, target; uint8_t owner; uint16_t type; Vec3 at; };
    std::vector<OnHit> onHits;
    auto statusOnHit = [&](Entity e) -> int16_t {   // first status of the attacker that enchants its weapons
        if (!rules_.advancedEffects) return -1;
        if (auto* st = w.tryGet<Statuses>(e)) for (auto& in : st->list) if (rules_.statuses[in.def].onHit >= 0) return rules_.statuses[in.def].onHit;
        return -1;
    };
    w.each<Combat, Unit, Owner, Transform>([&](Entity e, Combat& c, Unit& u, Owner& o, Transform& tr) {
        const UnitType& t = rules_.types[u.type];
        Fixed atkSpeed = fxMax(stat(sim, e, Stat::AttackSpeed), Fixed::fromRatio(1, 10));
        for (size_t i = 0; i < t.weapons.size() && i < 4; ++i) c.cooldown[i] = fxMax(Fixed{}, c.cooldown[i] - dt * atkSpeed);
        if (u.flags & (kUnitUnderConstruction | kUnitDisabled | kUnitInHand)) return;
        if (auto* b = w.tryGet<Buffs>(e); b && b->stunUntil > sim.tick()) return;
        if (rules_.advancedEffects && (statusFlags(sim, e) & (kStStun | kStDisarm))) return;
        if (!w.alive(c.target) || !w.has<Unit>(c.target) || (w.get<Unit>(c.target).flags & kUnitOffMap)) { c.target = kNullEntity; return; }
        const Unit& tu = w.get<Unit>(c.target);
        const UnitType& tt = rules_.types[tu.type];
        if (tu.hp.raw <= 0) return;
        auto* ord = w.tryGet<Orders>(e);
        auto* m = w.tryGet<Mobile>(e);
        Fixed dist = contactDistance(sim, e, c.target);
        Vec3 tp = w.get<Transform>(c.target).pos;
        // height advantage in levels: whole cliff levels, or height difference / levelHeight on heightfields
        const Fixed adv = (t.layer & kLayerAir) || (tt.layer & kLayerAir) ? Fixed{} : heightAdvantage(sim, tr.pos, tp);
        Fixed rangeBonus = stat(sim, e, Stat::Range) + (adv.raw > 0 ? rules_.heights.rangePerLevel * adv : Fixed{});
        Fixed range = maxWeaponRange(t) + rangeBonus;
        // idle units with an automatic target chase it (unless holding / guarding a structure)
        if (c.autoTarget && ord && ord->queue.empty() && m && !(u.flags & (kUnitHold | kUnitInCargo)) && dist > range) {
            if (dist > stat(sim, e, Stat::Sight) + Fixed::fromInt(2)) { c.target = kNullEntity; return; }
            if (mechOn(sim) && !mechMayChase(sim, e, tp)) { c.target = kNullEntity; return; }   // defensive stance
            if (!m->moving || (sim.tick() + e.index) % 20 == 0) moveTo(sim, e, tp, range);
            return;
        }
        if (c.autoTarget && ord && ord->queue.empty() && m && m->moving && dist <= range) stopMoving(sim, e);
        Vec3 face = flat(tp - tr.pos);
        if (face.length().raw > 0) u.facing = face / face.length();
        // damage scaling: techs/buffs, squad members alive, height advantage
        Fixed mult = stat(sim, e, Stat::Damage);
        if (t.members > 1) mult = mult * membersAlive(sim, e) / t.members;
        if (adv.raw > 0) mult = mult * (kOne + rules_.heights.damagePerLevel * adv);
        for (size_t i = 0; i < t.weapons.size() && i < 4; ++i) {
            const WeaponDef& wd = t.weapons[i];
            if (!canHit(wd, tt.layer) || c.cooldown[i].raw > 0 || dist > wd.range + rangeBonus || dist < wd.minRange) continue;
            if (m && m->moving && ord && !ord->queue.empty() && ord->queue.front().type == OrderType::Move) continue;
            if (wd.chargeTime.raw > 0) {
                // charging shot: the weapon has to stay on the same target for chargeTime
                if (c.chargeTarget != c.target) { c.chargeTarget = c.target; c.chargeStart = sim.tick(); }
                if (sim.tick() - c.chargeStart < uint32_t((wd.chargeTime * int64_t(sim.tickRate())).toInt())) continue;
                c.chargeStart = sim.tick();
            }
            if (rules_.mech.ammo && !mechUseAmmo(sim, e, i)) continue;   // agent-mech: out of ammo
            c.cooldown[i] = wd.cooldown;
            sim.emit({kEvAttack, o.player, e, c.target, int64_t(i), tr.pos});
            // shooting uphill can miss (StarCraft); the roll uses the shared deterministic RNG
            // (a full level or more below: the whole chance; less on a heightfield slope: proportionally less)
            if (adv.raw < 0 && rules_.heights.missUphill.raw > 0 && sim.rng().unit() < rules_.heights.missUphill * fxMin(kOne, -adv)) {
                sim.emit({kEvMiss, o.player, e, c.target, 0, tp});
                continue;
            }
            Fixed dmg = wd.damage * mult;
            uint16_t dtype = wd.damageType;
            if (rules_.mech.armorModel == ArmorModel::Classes) mechClassDamage(sim, e, i, c.target, dmg, dtype);   // agent-mech
            const int16_t enchant = statusOnHit(e);
            const bool proc = wd.onHit >= 0 && (wd.onHitChance >= kOne || sim.rng().unit() < wd.onHitChance);
            if (wd.projectileSpeed.raw <= 0) {
                dealDamage(sim, c.target, dmg, dtype, e, o.player);
                if (proc) onHits.push_back({wd.onHit, e, c.target, o.player, u.type, tp});
                if (enchant >= 0) onHits.push_back({enchant, e, c.target, o.player, u.type, tp});
                if (wd.splash.raw > 0) {
                    gs.hash.query(tp, wd.splash, [&](Entity s) {
                        if (s == c.target || !w.has<Unit>(s)) return;
                        if (!hostile(sim, o.player, w.get<Owner>(s).player)) return;
                        if (dist2D(w.get<Transform>(s).pos, tp) <= wd.splash) dealDamage(sim, s, dmg, dtype, e, o.player);
                    });
                }
            } else {
                Projectile pr;
                pr.target = c.target; pr.source = e; pr.aim = tp; pr.speed = wd.projectileSpeed;
                pr.damage = dmg; pr.splash = wd.splash; pr.damageType = dtype; pr.owner = o.player; pr.layers = wd.targets;
                pr.onHit = proc ? wd.onHit : enchant; pr.sourceType = u.type;
                if (proc && enchant >= 0) onHits.push_back({enchant, e, c.target, o.player, u.type, tp});
                Vec3 start = tr.pos;
                start.y = groundHeight(sim, tr.pos) + Fixed::fromRatio(6, 10);
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
    for (auto& h : onHits) {
        if (!w.alive(h.target)) continue;
        EffectCtx ctx; ctx.caster = h.caster; ctx.owner = h.owner; ctx.sourceType = h.type; ctx.target = h.target; ctx.point = h.at;
        ctx.origin = w.alive(h.caster) ? w.get<Transform>(h.caster).pos : h.at;
        applyPayload(sim, h.payload, ctx);
    }
}

void RtsModule::sysProjectiles(Simulation& sim) {
    World& w = sim.world();
    GameState& gs = state(sim);
    const Fixed dt = sim.dt();
    std::vector<Entity> impacts;
    struct OnHit { int16_t payload; Entity caster, target; uint8_t owner; uint16_t type; Vec3 at; };
    std::vector<OnHit> onHits;
    w.each<Projectile, Transform>([&](Entity p, Projectile& pr, Transform& tr) {
        tr.prevPos = tr.pos;
        if (pr.effList >= 0) {
            // ability projectile: homing / ballistic arc / hits the first enemy on its way
            const EffectDef& e = rules_.payloads[size_t(pr.effList)][size_t(pr.effIndex)];
            if (w.alive(pr.target) && w.has<Unit>(pr.target)) { pr.aim = flat(w.get<Transform>(pr.target).pos); }
            Fixed step = pr.speed * dt;
            if (e.delivery.collide) {
                Entity hit;
                gs.hash.query(tr.pos, kOne, [&](Entity s) {
                    if (hit.valid() || s == pr.source || !w.has<Unit>(s)) return;
                    Fixed r = rules_.types[w.get<Unit>(s).type].radius + Fixed::fromRatio(1, 4);
                    if (dist2D(w.get<Transform>(s).pos, tr.pos) <= r && filterOk(sim, e.filter, s, pr.owner, pr.source)) hit = s;
                });
                if (hit.valid()) { pr.target = hit; impacts.push_back(p); return; }
            }
            if (e.delivery.arc && pr.total.raw > 0) {
                pr.travel += step;
                Fixed t = fxMin(kOne, pr.travel / pr.total);
                Vec3 flatPos = pr.start + (pr.aim - pr.start) * t;
                flatPos.y = groundHeight(sim, flatPos) + Fixed::fromRatio(4, 10) + e.delivery.arcHeight * 4 * t * (kOne - t);
                tr.pos = flatPos;
                if (t >= kOne) impacts.push_back(p);
                return;
            }
            Vec3 aim = pr.aim; aim.y = groundHeight(sim, aim) + Fixed::fromRatio(4, 10);
            if (moveTowards(tr.pos, aim, step)) impacts.push_back(p);
            return;
        }
        if (w.alive(pr.target) && w.has<Unit>(pr.target)) { pr.aim = w.get<Transform>(pr.target).pos; pr.aim.y = groundHeight(sim, pr.aim) + Fixed::fromRatio(4, 10); }
        if (!moveTowards(tr.pos, pr.aim, pr.speed * dt)) return;
        if (w.alive(pr.target) && w.has<Unit>(pr.target)) dealDamage(sim, pr.target, pr.damage, pr.damageType, pr.source, pr.owner);
        if (pr.onHit >= 0 && w.alive(pr.target)) onHits.push_back({pr.onHit, pr.source, pr.target, pr.owner, pr.sourceType, pr.aim});
        if (pr.splash.raw > 0) {
            gs.hash.query(pr.aim, pr.splash, [&](Entity s) {
                if (s == pr.target || !w.has<Unit>(s)) return;
                if (!hostile(sim, pr.owner, w.get<Owner>(s).player)) return;
                if (dist2D(w.get<Transform>(s).pos, pr.aim) <= pr.splash) dealDamage(sim, s, pr.damage, pr.damageType, pr.source, pr.owner);
            });
        }
        w.destroyLater(p);
    });
    for (Entity p : impacts) impactProjectile(sim, p);
    for (auto& h : onHits) {
        if (!w.alive(h.target)) continue;
        EffectCtx ctx; ctx.caster = h.caster; ctx.owner = h.owner; ctx.sourceType = h.type; ctx.target = h.target; ctx.point = flat(h.at); ctx.origin = ctx.point;
        applyPayload(sim, h.payload, ctx);
    }
}

// ================================================================= abilities

bool RtsModule::canCast(Simulation& sim, Entity caster, int index) const {
    World& w = sim.world();
    auto* u = w.tryGet<Unit>(caster);
    if (!u || u->hp.raw <= 0 || (u->flags & (kUnitUnderConstruction | kUnitDisabled))) return false;
    const UnitType& t = rules_.types[u->type];
    if (index < 0 || size_t(index) >= t.abilities.size() || index >= 8) return false;
    const AbilityDef& a = rules_.advancedEffects ? abilityFor(sim, caster, index) : t.abilities[size_t(index)];
    if (a.aura) return false;
    if (auto* b = w.tryGet<Buffs>(caster); b && b->stunUntil > sim.tick()) return false;
    if (auto* h = w.tryGet<Hero>(caster); h && h->level < a.requiredLevel) return false;
    if (u->energy < a.energyCost) return false;
    auto* st = w.tryGet<AbilityState>(caster);
    if (rules_.advancedEffects) {
        if (statusFlags(sim, caster) & (kStStun | kStSilence)) return false;
        const uint8_t owner = w.get<Owner>(caster).player;
        for (int rq : a.requires_) if (!requirementsMet(sim, owner, {Requirement{(rq & 0x8000) != 0, uint16_t(rq & 0x7FFF)}})) return false;
        if (!canAfford(sim, owner, a.cost)) return false;
        if (st && st->busyUntil > sim.tick()) return false;
        if (st && a.chargesMax > 0) return st->charges[index] > 0;
    }
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
    if (rules_.mech.toggles && mechToggle(sim, caster, index)) return;   // agent-mech: cloak / burrow / siege mode
    if (!rules_.advancedEffects) { applyEffects(sim, caster, owner, casterType, a.effects, target, point); return; }
    const AbilityDef& al = abilityFor(sim, caster, index);
    pay(sim, owner, al.cost, {}, false);
    if (auto* st = w.tryGet<AbilityState>(caster)) {
        Fixed rate = fxMax(stat(sim, caster, Stat::CooldownRate), Fixed::fromRatio(1, 10));
        st->readyAt[index] = sim.tick() + uint32_t((al.cooldown / rate * int64_t(sim.tickRate())).toInt());
        if (al.chargesMax > 0) {
            if (st->charges[index] == al.chargesMax) st->chargeAt[index] = st->readyAt[index];
            if (st->charges[index] > 0) st->charges[index]--;
        }
        st->busyUntil = sim.tick() + uint32_t((al.backswing * int64_t(sim.tickRate())).toInt());
    }
    if (al.channel) {
        Channel ch;
        ch.payload = al.payload; ch.target = w.alive(target) && target != caster ? target : kNullEntity; ch.point = point;
        ch.startPos = w.get<Transform>(caster).pos; ch.nextAt = sim.tick(); ch.ability = index; ch.interval = al.channelInterval;
        ch.endAt = sim.tick() + uint32_t((al.channelTime * int64_t(sim.tickRate())).toInt());
        if (w.has<Channel>(caster)) w.get<Channel>(caster) = ch; else w.add<Channel>(caster, ch);
        sim.emit({kEvChannelStart, owner, caster, ch.target, index, point});
        return;
    }
    EffectCtx ctx;
    ctx.caster = caster; ctx.owner = owner; ctx.sourceType = casterType; ctx.target = w.alive(target) ? target : kNullEntity;
    ctx.point = point; ctx.origin = w.get<Transform>(caster).pos;
    if (w.alive(target)) if (auto* gt = w.tryGet<GroupTag>(target)) ctx.group = gt->id;
    applyPayload(sim, al.payload, ctx);
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
                if (!su || su->hp.raw <= 0 || (su->flags & kUnitOffMap) || dist2D(w.get<Transform>(s).pos, point) > ef.radius) return;
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
            if (g.passable(x, y, ct.pathLayer)) {
                Transform& tr = w.get<Transform>(caster);
                tr.pos = flat(point);
                tr.prevPos = tr.pos;
                stopMoving(sim, caster);
            }
            break;
        }
        default: break; // the effect engine (RtsEffects.cpp) handles everything else
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
    // charges regenerate one per cooldown
    if (rules_.advancedEffects)
        w.each<AbilityState, Unit>([&](Entity, AbilityState& st, Unit& u) {
            const UnitType& t = rules_.types[u.type];
            for (size_t i = 0; i < t.abilities.size() && i < 8; ++i) {
                const AbilityDef& a = t.abilities[i];
                if (a.chargesMax <= 0 || st.charges[i] >= a.chargesMax || tick < st.chargeAt[i]) continue;
                ++st.charges[i];
                st.chargeAt[i] = tick + uint32_t((a.cooldown * int64_t(sim.tickRate())).toInt());
            }
        });
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
                const AbilityDef& a = rules_.advancedEffects ? abilityFor(sim, e, int(i)) : t.abilities[i];
                if (!a.autocast || a.aura || !canCast(sim, e, int(i))) continue;
                if (rules_.advancedEffects && (a.smart != SmartRule::Auto || a.target > TargetKind::Point)) {
                    Vec3 pt; Entity tg;
                    if (o.player != 0 && smartTarget(sim, o.player, a.payload, a.target, a.smart, pt, tg, tr.pos, a.range)) {
                        if (a.target == TargetKind::Unit && !tg.valid()) continue;
                        casts.push_back({e, int(i), tg, pt});
                        break;
                    }
                    continue;
                }
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
    struct Leftover { uint16_t type; Vec3 pos; int16_t res; Fixed amount; uint8_t owner = 0; };
    std::vector<std::pair<Entity, Fixed>> soulTraps;   // areas that turn the dead into units for their owner
    if (rules_.advancedEffects && !dead.empty())
        w.each<Area, Transform>([&](Entity a, Area& ar, Transform&) {
            if (ar.effList >= 0 && rules_.payloads[size_t(ar.effList)][size_t(ar.effIndex)].delivery.deathSpawn != kNoType && sim.tick() >= ar.fireAt) soulTraps.push_back({a, ar.radius});
        });
    std::vector<Leftover> leftovers;
    const int wreckType = rules_.findType("wreck");
    for (Entity e : dead) {
        Unit& u = w.get<Unit>(e);
        const UnitType& t = rules_.types[u.type];
        const uint8_t owner = w.get<Owner>(e).player;
        const Vec3 pos = w.get<Transform>(e).pos;
        const bool silent = u.flags & kUnitSilent;
        if (auto* f = w.tryGet<Footprint>(e)) gs.grid.block(f->x, f->y, f->w, f->h, -1);
        if (mechOn(sim)) mechOnDeath(sim, e);   // agent-mech: passengers, veterancy
        if (owner < gs.players.size() && !gs.players[owner].typeCountAll.empty() && gs.players[owner].typeCountAll[u.type] > 0)
            gs.players[owner].typeCountAll[u.type]--;
        if (auto* ex = w.tryGet<Extractor>(e)) {
            // a destroyed refinery gives the geyser back with what was left in it
            auto* rs = w.tryGet<ResourceStock>(e);
            leftovers.push_back({ex->nodeType, pos, rs ? rs->res : int16_t(-1), rs ? rs->amount : Fixed{}});
        }
        if (!silent) {
            for (auto& [a, r] : soulTraps) {
                uint8_t ao = w.get<Owner>(a).player;
                if (t.structure || !hostile(sim, ao, owner) || dist2D(w.get<Transform>(a).pos, pos) > r) continue;
                const Area& ar = w.get<Area>(a);
                leftovers.push_back({rules_.payloads[size_t(ar.effList)][size_t(ar.effIndex)].delivery.deathSpawn, pos, -1, Fixed{}, ao});
                break;
            }
            grantKill(sim, e, u.lastAttacker);
            if (!t.wreck.empty() && wreckType >= 0) leftovers.push_back({uint16_t(wreckType), pos, int16_t(t.wreck[0].res), t.wreck[0].value});
            sim.emit({kEvDied, owner, e, kNullEntity, u.type, pos});
        }
        w.destroyLater(e);
    }
    if (!dead.empty()) w.flush(); // free footprints/ids before leftovers are placed
    for (auto& l : leftovers) {
        Entity n = spawn(sim, l.type, l.owner, l.pos);
        if (auto* rs = w.tryGet<ResourceStock>(n); rs && l.res >= 0) { rs->res = l.res; rs->amount = l.amount; rs->infinite = l.amount.raw < 0; }
    }
}

} // namespace rts::game
