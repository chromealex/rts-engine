// Skill primitives for garrison games (Mushroom Wars / Bunker Wars style) and player-level skills:
// garrison kill/add/convert/panic, temporary capture, swap, transform, level change, equalise,
// links and production redirects; group effects (send back, clone, formation, convert); player
// effects (resources, statuses, equalise, steal skill, cooldowns); player skill casting with
// loadouts, levels, activation delay, auto skills; smart targeting shared by autocast and the AI.
#include "RtsInternal.h"
#include <algorithm>
#include <cstdio>

namespace rts::game {
using namespace detail;

namespace {
uint32_t ticksOf(Fixed s, uint32_t rate) { return s.raw <= 0 ? 0 : uint32_t((s * int64_t(rate)).toInt()); }
int portion(Fixed amount, Fixed percent, int of) {
    Fixed n = amount + percent * of + Fixed::fromRaw(32760);   // round to nearest (0.4 * 10 = 4, not 3.99)
    return std::clamp<int>(int(n.floorInt()), 0, 1 << 20);
}
bool offensiveKind(EffectKind k) {
    switch (k) {
    case EffectKind::Damage: case EffectKind::Stun: case EffectKind::Kill: case EffectKind::EnergyBurn: case EffectKind::Knockback:
    case EffectKind::GarrisonKill: case EffectKind::GarrisonConvert: case EffectKind::GarrisonPanic: case EffectKind::Capture:
    case EffectKind::Swap: case EffectKind::SendBack: case EffectKind::Convert: case EffectKind::StealSkill: return true;
    default: return false;
    }
}
} // namespace

namespace detail {
// (defined in RtsEffects.cpp)
void changeUnitType(RtsModule& m, Simulation& sim, Entity e, uint16_t type);
} // namespace detail

// ================================================================= garrisons / structures

void RtsModule::effectGarrison(Simulation& sim, const EffectDef& e, EffectCtx& ctx, const std::vector<Entity>& targets) {
    World& w = sim.world();
    const uint32_t tick = sim.tick();
    auto nearestGarrison = [&](Vec3 from, Entity skip, auto&& pred) {
        Entity best; Fixed bd{};
        w.each<Garrison, Unit, Owner, Transform>([&](Entity c, Garrison&, Unit& u, Owner& o, Transform& t) {
            if (c == skip || u.hp.raw <= 0 || !pred(o.player)) return;
            Fixed d = dist2D(t.pos, from);
            if (!best.valid() || d < bd) { best = c; bd = d; }
        });
        return best;
    };
    auto temporary = [&](Entity h, const EffectCtx& c) -> StatusInst* {
        if (e.duration.raw <= 0 || (e.keepChance.raw > 0 && sim.rng().unit() < e.keepChance)) return nullptr;
        addStatus(sim, h, rules_.stTemp, e.duration, c);
        auto& list = w.get<Statuses>(h).list;
        return list.empty() ? nullptr : &list.back();
    };
    std::vector<uint8_t> equalizeOwners;
    for (Entity h : targets) {
        if (!w.alive(h)) continue;
        auto* g = w.tryGet<Garrison>(h);
        Owner& o = w.get<Owner>(h);
        Unit& u = w.get<Unit>(h);
        const UnitType& t = rules_.types[u.type];
        const Vec3 pos = w.get<Transform>(h).pos;
        switch (e.kind) {
        case EffectKind::GarrisonKill:
        case EffectKind::GarrisonPanic: {
            if (!g) break;
            int n = std::min(g->count, portion(rollAmount(sim, e) * ctx.scale, e.percent, g->count));
            g->count -= n; g->lost += n;
            sim.emit({kEvGarrisonChanged, o.player, h, ctx.caster, -n, pos});
            break;
        }
        case EffectKind::GarrisonAdd: {
            if (!g) break;
            int base = e.ofLost ? g->lost : g->count;
            int n = portion(rollAmount(sim, e) * ctx.scale, e.percent, base);
            if (e.ofLost) { n = std::min(n, g->lost); g->lost -= n; }
            g->count += n;
            sim.emit({kEvGarrisonChanged, o.player, h, ctx.caster, n, pos});
            break;
        }
        case EffectKind::GarrisonConvert: {
            if (!g || t.spawnUnit == kNoType || o.player == ctx.owner) break;
            int n = std::min(g->count, portion(rollAmount(sim, e), e.percent, g->count));
            g->count -= n;
            sim.emit({kEvGarrisonChanged, o.player, h, ctx.caster, -n, pos});
            Entity dest = nearestGarrison(pos, h, [&](uint8_t p) { return p == ctx.owner; });
            uint32_t gid = state(sim).nextGroupId++;
            for (int k = 0; k < n; ++k) {
                Entity d = spawn(sim, t.spawnUnit, ctx.owner, freeSpotNear(sim, h, rules_.types[t.spawnUnit].layer));
                w.add<GroupTag>(d, GroupTag{gid, h, dest});
                if (dest.valid() && w.has<Orders>(d)) { Order ord; ord.type = OrderType::Enter; ord.target = dest; ord.point = flat(w.get<Transform>(dest).pos); issue(sim, d, ord, false); }
            }
            break;
        }
        case EffectKind::Capture: {
            if (o.player == ctx.owner) break;
            uint8_t old = o.player;
            o.player = ctx.owner;
            if (auto* p = w.tryGet<Production>(h)) p->queue.clear();
            sim.emit({kEvCaptured, ctx.owner, h, ctx.caster, old, pos});
            if (auto* in = temporary(h, ctx)) { in->revertsOwner = true; in->revertOwner = old; in->capturedBy = ctx.owner; }
            break;
        }
        case EffectKind::Swap: {
            if (!g) break;
            const bool mine = o.player == ctx.owner;
            Entity other = nearestGarrison(pos, h, [&](uint8_t p) { return mine ? (p != 0 && hostile(sim, ctx.owner, p)) : p == ctx.owner; });
            if (!other.valid()) break;
            Owner& o2 = w.get<Owner>(other);
            Garrison& g2 = w.get<Garrison>(other);
            std::swap(o.player, o2.player);
            std::swap(g->count, g2.count);
            sim.emit({kEvCaptured, o.player, h, other, o2.player, pos});
            sim.emit({kEvCaptured, o2.player, other, h, o.player, w.get<Transform>(other).pos});
            break;
        }
        case EffectKind::Transform:
        case EffectKind::Level: {
            uint16_t from = u.type, to = e.toType;
            if (e.kind == EffectKind::Level) {
                to = from;
                for (int k = 0; k < std::abs(e.levelDelta); ++k) {
                    if (e.levelDelta > 0) { if (rules_.types[to].upgrades.empty()) break; to = rules_.types[to].upgrades[0]; }
                    else {
                        bool found = false;
                        for (size_t ti = 0; ti < rules_.types.size() && !found; ++ti)
                            for (uint16_t up : rules_.types[ti].upgrades) if (up == to) { to = uint16_t(ti); found = true; break; }
                        if (!found) break;
                    }
                }
            }
            if (to == from || to == kNoType || rules_.types[to].structure != t.structure) break;
            changeUnitType(*this, sim, h, to);
            if (auto* in = temporary(h, ctx)) in->revertType = from;
            break;
        }
        case EffectKind::Equalize:
            if (std::find(equalizeOwners.begin(), equalizeOwners.end(), o.player) == equalizeOwners.end()) equalizeOwners.push_back(o.player);
            break;
        case EffectKind::Link:
        case EffectKind::Redirect: {
            if (!g) break;
            const bool link = e.kind == EffectKind::Link;
            const uint8_t partnerOwner = link ? o.player : ctx.owner;
            Entity other = nearestGarrison(pos, h, [&](uint8_t p) { return p == partnerOwner; });
            if (!other.valid()) break;
            EffectCtx c2 = ctx;
            addStatus(sim, h, link ? rules_.stLink : rules_.stRedirect, e.duration, c2);
            w.get<Statuses>(h).list.back().link = other;
            if (link) { addStatus(sim, other, rules_.stLink, e.duration, c2); w.get<Statuses>(other).list.back().link = h; }
            break;
        }
        case EffectKind::Sacrifice: {
            int n = 0;
            if (g) { n = std::min(g->count, portion(rollAmount(sim, e), e.percent, g->count)); g->count -= n; g->lost += n; }
            else if (!t.structure) { u.hp = Fixed{}; u.flags |= kUnitSilent; n = 1; }
            for (auto& a : e.gain) addResource(sim, ctx.owner, a.res, a.value * n);
            break;
        }
        default: break;
        }
    }
    for (uint8_t p : equalizeOwners) {
        std::vector<Garrison*> gs;
        int total = 0;
        w.each<Garrison, Owner>([&](Entity, Garrison& g, Owner& o) { if (o.player == p) { gs.push_back(&g); total += g.count; } });
        if (gs.empty()) continue;
        int each = total / int(gs.size()), rest = total % int(gs.size());
        for (size_t i = 0; i < gs.size(); ++i) gs[i]->count = each + (int(i) < rest ? 1 : 0);
    }
    (void)tick;
}

// ================================================================= groups in transit

void RtsModule::effectGroup(Simulation& sim, const EffectDef& e, EffectCtx& ctx, const std::vector<Entity>& targets) {
    World& w = sim.world();
    GameState& gs = state(sim);
    const int n = e.percent.raw > 0 ? std::max(1, int((e.percent * int64_t(targets.size()) + Fixed::fromRaw(65535)).floorInt())) : int(targets.size());
    int done = 0;
    for (Entity h : targets) {
        if (!w.alive(h) || done >= n) continue;
        ++done;
        auto* gt = w.tryGet<GroupTag>(h);
        auto* lw = w.tryGet<LaneWalker>(h);
        auto* ord = w.tryGet<Orders>(h);
        switch (e.kind) {
        case EffectKind::SendBack: {
            if (e.mode == 2) { // throw back
                if (lw) lw->pos = uint16_t(std::max<int>(0, int(lw->pos) - int((rollAmount(sim, e) * 2).toInt())));
                break;
            }
            if (!gt || !ord || ord->queue.empty() || ord->queue.front().type != OrderType::Enter) break;
            Entity home = gt->from;
            if (!w.alive(home) || !w.has<Garrison>(home)) break;
            std::swap(gt->from, gt->to);
            Order& o = ord->queue.front();
            o.target = home;
            o.point = flat(w.get<Transform>(home).pos);
            if (lw && lw->lane >= 0 && size_t(lw->lane) < gs.lanes.size()) {
                size_t len = gs.lanes[size_t(lw->lane)].points.size();
                lw->reverse = !lw->reverse;
                lw->pos = uint16_t(len > lw->pos ? len - 1 - lw->pos : 0);
            } else { o.state = 0; stopMoving(sim, h); }
            break;
        }
        case EffectKind::Clone: {
            const Unit& u = w.get<Unit>(h);
            Vec3 p = w.get<Transform>(h).pos + Vec3{Fixed::fromRatio(1, 5), Fixed{}, Fixed::fromRatio(1, 5)};
            Entity c = spawn(sim, u.type, w.get<Owner>(h).player, p);
            // the pools may have moved: re-fetch the original's components
            if (auto* l0 = w.tryGet<LaneWalker>(h)) w.add<LaneWalker>(c, *l0);
            if (auto* g0 = w.tryGet<GroupTag>(h)) w.add<GroupTag>(c, *g0);
            if (auto* o0 = w.tryGet<Orders>(h); o0 && w.has<Orders>(c)) w.get<Orders>(c) = *o0;
            if (e.fake) addStatus(sim, c, rules_.stFake, Fixed{}, ctx);
            break;
        }
        case EffectKind::Formation: {
            if (!gt || !lw) break;
            auto lanes = lanesBetween(sim, gt->from, gt->to);
            if (lanes.empty() || lanes[0] == lw->lane || lw->lane < 0) break;
            size_t oldLen = gs.lanes[size_t(lw->lane)].points.size(), newLen = gs.lanes[size_t(lanes[0])].points.size();
            lw->pos = uint16_t(oldLen ? size_t(lw->pos) * newLen / oldLen : 0);
            lw->lane = lanes[0];
            lw->reverse = gs.lanes[size_t(lanes[0])].a != gt->from;
            break;
        }
        case EffectKind::Convert: {
            Owner& o = w.get<Owner>(h);
            if (o.player == ctx.owner) break;
            uint8_t old = o.player;
            o.player = ctx.owner;
            if (auto* c = w.tryGet<Combat>(h)) c->target = kNullEntity;
            sim.emit({kEvCaptured, ctx.owner, h, ctx.caster, old, w.get<Transform>(h).pos});
            break;
        }
        default: break;
        }
    }
}

// ================================================================= players

void RtsModule::effectPlayer(Simulation& sim, const EffectDef& e, EffectCtx& ctx) {
    GameState& gs = state(sim);
    const uint32_t tick = sim.tick();
    auto players = effectPlayers(sim, e, ctx);
    switch (e.kind) {
    case EffectKind::Resource:
        for (uint8_t p : players) {
            if (p >= gs.players.size() || e.resource < 0) continue;
            Fixed& stock = gs.players[p].stock[size_t(e.resource)];
            Fixed v = rollAmount(sim, e) * ctx.scale + e.percent * stock;
            if (v.raw >= 0) { addResource(sim, p, uint16_t(e.resource), v); continue; }
            Fixed take = fxMin(stock, -v);
            stock -= take;
            if (isGood(uint16_t(e.resource))) withdrawGoods(sim, p, uint16_t(e.resource), take);
            if (e.transfer && ctx.owner != p) addResource(sim, ctx.owner, uint16_t(e.resource), take);
        }
        break;
    case EffectKind::PlayerStatus:
        for (uint8_t p : players) addPlayerStatus(sim, p, e.status, e.duration, ctx);
        break;
    case EffectKind::EqualizeResource: {
        if (players.empty() || e.resource < 0) break;
        Fixed total{};
        for (uint8_t p : players) total += gs.players[p].stock[size_t(e.resource)];
        Fixed each = total / int64_t(players.size());
        for (uint8_t p : players) gs.players[p].stock[size_t(e.resource)] = each;
        break;
    }
    case EffectKind::StealSkill: {
        if (ctx.owner >= gs.playerAbilities.size()) break;
        auto& mine = gs.playerAbilities[ctx.owner];
        for (uint8_t p : players) {
            if (p == ctx.owner || p >= gs.playerAbilities.size()) continue;
            std::vector<size_t> cand;
            for (size_t k = 0; k < gs.playerAbilities[p].size(); ++k)
                if (gs.playerAbilities[p][k].owned && !mine[k].owned && !rules_.playerAbilities[k].passive) cand.push_back(k);
            if (cand.empty()) continue;
            size_t k = cand[sim.rng().below(uint32_t(cand.size()))];
            mine[k].owned = true;
            mine[k].level = gs.playerAbilities[p][k].level;
            mine[k].stolenUntil = e.duration.raw > 0 ? tick + ticksOf(e.duration, sim.tickRate()) : 0;
            mine[k].readyAt = tick;
            if (e.transfer) { gs.playerAbilities[p][k].owned = false; gs.playerAbilities[p][k].stolenUntil = 0; }
            break;
        }
        break;
    }
    case EffectKind::Cooldown:
        for (uint8_t p : players) {
            if (p >= gs.playerAbilities.size()) continue;
            int64_t d = (rollAmount(sim, e) * int64_t(sim.tickRate())).toInt();
            for (auto& st : gs.playerAbilities[p]) {
                int64_t r = std::max<int64_t>(int64_t(st.readyAt), int64_t(tick)) + d;
                st.readyAt = uint32_t(std::max<int64_t>(int64_t(tick), r));
            }
        }
        break;
    default: break;
    }
}

// ================================================================= player skills

const PlayerAbilityDef& RtsModule::playerAbilityFor(Simulation& sim, uint8_t player, int index) const {
    auto* gs = sim.world().resource<GameState>();
    const PlayerAbilityDef& base = rules_.playerAbilities[size_t(index)];
    int level = player < gs->playerAbilities.size() ? gs->playerAbilities[player][size_t(index)].level : 1;
    if (level >= 2 && !base.levels.empty()) return base.levels[size_t(std::min<int>(level - 2, int(base.levels.size()) - 1))];
    return base;
}

void RtsModule::setupSkills(Simulation& sim, const Json& map) {
    GameState& gs = state(sim);
    if (rules_.playerAbilities.empty()) return;
    for (size_t p = 1; p < gs.players.size() && p < gs.playerAbilities.size(); ++p) {
        const Json& lo = map["players"][p - 1]["loadout"];
        auto& st = gs.playerAbilities[p];
        if (lo.isArray()) {
            for (auto& s : st) s.owned = false;
            int slots = 0;
            for (auto& item : lo.items()) {
                std::string id = item.isString() ? item.str() : item.getStr("id");
                int k = rules_.findPlayerAbility(id);
                if (k < 0) throw std::runtime_error("map player " + std::to_string(p) + " loadout: unknown player ability '" + id + "'");
                if (rules_.loadoutSlots > 0 && ++slots > rules_.loadoutSlots) throw std::runtime_error("map player " + std::to_string(p) + " loadout: more than " + std::to_string(rules_.loadoutSlots) + " skills");
                st[size_t(k)].owned = true;
                st[size_t(k)].level = uint8_t(std::clamp<int64_t>(item.isObject() ? item.getInt("level", 1) : 1, 1, 1 + int64_t(rules_.playerAbilities[size_t(k)].levels.size())));
            }
        }
        for (size_t k = 0; k < st.size(); ++k) {
            const PlayerAbilityDef& a = playerAbilityFor(sim, uint8_t(p), int(k));
            st[k].readyAt = ticksOf(a.initialCooldown, sim.tickRate());
            st[k].nextAuto = ticksOf(a.autoEvery, sim.tickRate());
            if (st[k].owned && a.passive && a.status >= 0) {
                EffectCtx ctx; ctx.owner = uint8_t(p);
                addPlayerStatus(sim, uint8_t(p), a.status, Fixed{}, ctx);
            }
        }
    }
}

bool RtsModule::castPlayerAbility(Simulation& sim, uint8_t player, int idx, Vec3 point, Entity target) {
    World& w = sim.world();
    GameState& gs = state(sim);
    if (!canUsePlayerAbility(sim, player, idx)) return false;
    const PlayerAbilityDef& a = playerAbilityFor(sim, player, idx);
    // resolve the clicked object for structure / group targets
    if (a.target == TargetKind::Structure || a.target == TargetKind::Group) {
        if (!w.alive(target) || !w.has<Unit>(target) || (a.target == TargetKind::Structure && !rules_.types[w.get<Unit>(target).type].structure) ||
            (a.target == TargetKind::Group && !w.has<GroupTag>(target))) {
            target = kNullEntity;
            Fixed bd{};
            w.each<Unit, Transform>([&](Entity c, Unit& u, Transform& t) {
                bool ok = a.target == TargetKind::Structure ? rules_.types[u.type].structure : w.has<GroupTag>(c);
                Fixed d = dist2D(t.pos, point);
                if (!ok || u.hp.raw <= 0 || d > Fixed::fromInt(4)) return;
                if (!target.valid() || d < bd) { target = c; bd = d; }
            });
            if (!target.valid()) return false;
        }
        point = w.get<Transform>(target).pos;
    }
    if (!pay(sim, player, a.cost, {})) return false;
    PlayerAbilityState& st = gs.playerAbilities[player][size_t(idx)];
    if (a.chargesMax > 0) --st.charges;
    Fixed rate = fxMax(playerStat(sim, player, Stat::CooldownRate, kOne), Fixed::fromRatio(1, 10));
    st.readyAt = sim.tick() + ticksOf(a.cooldown / rate, sim.tickRate());
    if (a.castTime.raw > 0) {
        st.pendingAt = sim.tick() + ticksOf(a.castTime, sim.tickRate());
        st.pendingPoint = flat(point); st.pendingTarget = target;
        sim.emit({kEvCastStart, player, kNullEntity, target, idx, flat(point)});
        return true;
    }
    sim.emit({kEvPlayerAbility, player, kNullEntity, target, idx, flat(point)});
    EffectCtx ctx;
    ctx.owner = player; ctx.point = flat(point); ctx.origin = ctx.point; ctx.target = target;
    if (w.alive(target)) if (auto* gt = w.tryGet<GroupTag>(target)) ctx.group = gt->id;
    applyPayload(sim, a.payload, ctx);
    return true;
}

void RtsModule::tickPlayerSkills(Simulation& sim) {
    World& w = sim.world();
    GameState& gs = state(sim);
    const uint32_t tick = sim.tick();
    for (size_t p = 1; p < gs.playerAbilities.size() && p < gs.players.size(); ++p) {
        auto* pi = sim.player(uint8_t(p));
        if (!pi || !pi->alive) continue;
        for (size_t k = 0; k < gs.playerAbilities[p].size(); ++k) {
            PlayerAbilityState& st = gs.playerAbilities[p][k];
            if (st.stolenUntil && tick >= st.stolenUntil) { st.owned = false; st.stolenUntil = 0; }
            if (st.pendingAt && tick >= st.pendingAt) {
                st.pendingAt = 0;
                const PlayerAbilityDef& a = playerAbilityFor(sim, uint8_t(p), int(k));
                sim.emit({kEvPlayerAbility, uint8_t(p), kNullEntity, st.pendingTarget, int64_t(k), st.pendingPoint});
                EffectCtx ctx;
                ctx.owner = uint8_t(p); ctx.point = st.pendingPoint; ctx.origin = ctx.point;
                ctx.target = w.alive(st.pendingTarget) ? st.pendingTarget : kNullEntity;
                if (w.alive(ctx.target)) if (auto* gt = w.tryGet<GroupTag>(ctx.target)) ctx.group = gt->id;
                applyPayload(sim, a.payload, ctx);
            }
            const PlayerAbilityDef& a = playerAbilityFor(sim, uint8_t(p), int(k));
            if (a.autoEvery.raw > 0 && st.owned && tick >= st.nextAuto) {
                st.nextAuto = tick + ticksOf(a.autoEvery, sim.tickRate());
                Vec3 pt; Entity tg;
                if (smartTarget(sim, uint8_t(p), a.payload, a.target, a.smart, pt, tg)) castPlayerAbility(sim, uint8_t(p), int(k), pt, tg);
            }
        }
    }
}

// ================================================================= smart targeting

bool RtsModule::smartTarget(Simulation& sim, uint8_t player, int payload, TargetKind kind, SmartRule rule, Vec3& point, Entity& target,
                            Vec3 from, Fixed range) {
    World& w = sim.world();
    target = kNullEntity;
    // what does the payload do (one level deep)?
    bool offensive = false, healing = false, dispelOwn = false, capture = false;
    Fixed radius = Fixed::fromInt(2);
    std::vector<int> stack{payload};
    for (size_t si = 0; si < stack.size() && si < 8; ++si) {
        if (stack[si] < 0 || size_t(stack[si]) >= rules_.payloads.size()) continue;
        for (auto& e : rules_.payloads[size_t(stack[si])]) {
            if (e.payload >= 0) stack.push_back(e.payload);
            radius = fxMax(radius, e.radius);
            if (e.kind == EffectKind::Heal || e.kind == EffectKind::GarrisonAdd) healing = true;
            if (e.kind == EffectKind::Capture || e.kind == EffectKind::GarrisonConvert || e.kind == EffectKind::Convert) capture = true;
            if (e.kind == EffectKind::Dispel && e.dispelFrom == 1) dispelOwn = true;
            if (offensiveKind(e.kind) && e.filter.enemies) offensive = true;
            if (e.kind == EffectKind::Status && e.status >= 0 && e.filter.enemies && !e.filter.allies) offensive = true;
            if (e.kind == EffectKind::Resource && e.amount.raw < 0) offensive = true;
            if (e.kind == EffectKind::PlayerStatus && e.filter.enemies) offensive = true;
        }
    }
    if (rule == SmartRule::Auto) {
        if (dispelOwn) rule = SmartRule::DebuffedOwn;
        else if (kind == TargetKind::Group) rule = offensive ? SmartRule::LargestEnemyGroup : SmartRule::LargestEnemyGroup;
        else if (kind == TargetKind::Structure) rule = offensive || capture ? SmartRule::StrongestEnemyStructure : SmartRule::OwnStructureUnderAttack;
        else if (kind == TargetKind::Player) rule = offensive ? SmartRule::EnemyPlayer : SmartRule::Self;
        else rule = healing ? SmartRule::LowestHpAlly : offensive ? SmartRule::DensestEnemy : SmartRule::Self;
    }
    const bool limit = range.raw > 0;
    auto inRange = [&](Vec3 p) { return !limit || dist2D(p, from) <= range; };
    auto isEnemy = [&](uint8_t o) { return o != 0 && hostile(sim, player, o) && o != kHostileNeutral; };
    Fixed best{};
    bool found = false;
    auto pick = [&](Entity c, Vec3 p, Fixed score) {
        if (!found || score > best) { found = true; best = score; target = c; point = p; }
    };
    switch (rule) {
    case SmartRule::Self:
    case SmartRule::EnemyPlayer: {
        bool enemy = rule == SmartRule::EnemyPlayer;
        w.each<Unit, Owner, Transform>([&](Entity c, Unit& u, Owner& o, Transform& t) {
            if (enemy ? !isEnemy(o.player) : o.player != player) return;
            const UnitType& ut = rules_.types[u.type];
            if (!ut.structure) return;
            pick(c, t.pos, Fixed::fromInt(ut.key ? 2 : 1) - Fixed::fromInt(int64_t(c.index)) / 1000000);
        });
        if (!found && limit) { point = from; found = true; }
        break;
    }
    case SmartRule::LowestHpAlly:
        w.each<Unit, Owner, Transform>([&](Entity c, Unit& u, Owner& o, Transform& t) {
            if (o.player != player || u.hp.raw <= 0 || !inRange(t.pos)) return;
            Fixed frac = u.hp / fxMax(stat(sim, c, Stat::MaxHp), kOne);
            if (frac < Fixed::fromRatio(8, 10)) pick(c, t.pos, kOne - frac);
        });
        break;
    case SmartRule::DensestEnemy: {
        std::vector<std::pair<Entity, Vec3>> foes;
        w.each<Unit, Owner, Transform>([&](Entity c, Unit& u, Owner& o, Transform& t) {
            if (isEnemy(o.player) && u.hp.raw > 0 && !rules_.types[u.type].invulnerable && inRange(t.pos)) foes.push_back({c, t.pos});
        });
        if (foes.size() > 200) foes.resize(200);
        for (auto& [c, p] : foes) {
            int n = 0;
            for (auto& [c2, q] : foes) if (dist2D(p, q) <= radius) ++n;
            if (n >= 2) pick(c, p, Fixed::fromInt(n));
        }
        break;
    }
    case SmartRule::StrongestEnemyStructure:
    case SmartRule::WeakestOwnStructure:
    case SmartRule::OwnStructureUnderAttack: {
        std::vector<int> incoming;
        if (rule == SmartRule::OwnStructureUnderAttack)
            w.each<Orders, Owner>([&](Entity, Orders& ord, Owner& o) {
                if (!isEnemy(o.player) || ord.queue.empty() || ord.queue.front().type != OrderType::Enter) return;
                uint32_t i = ord.queue.front().target.index;
                if (incoming.size() <= i) incoming.resize(i + 1, 0);
                incoming[i]++;
            });
        w.each<Unit, Owner, Transform>([&](Entity c, Unit& u, Owner& o, Transform& t) {
            const UnitType& ut = rules_.types[u.type];
            if (!ut.structure || u.hp.raw <= 0 || !inRange(t.pos) || ut.resourceNode >= 0) return;
            auto* g = w.tryGet<Garrison>(c);
            Fixed count = Fixed::fromInt(g ? g->count : 0) + u.hp / 100;
            if (rule == SmartRule::StrongestEnemyStructure) {
                if (!isEnemy(o.player)) return;
                pick(c, t.pos, capture ? -count : count);
            } else if (o.player == player) {
                Fixed inc = c.index < incoming.size() ? Fixed::fromInt(incoming[c.index]) : Fixed{};
                if (rule == SmartRule::OwnStructureUnderAttack && inc.raw <= 0 && healing) return;
                pick(c, t.pos, inc * 10 - count);
            }
        });
        break;
    }
    case SmartRule::LargestEnemyGroup: {
        std::vector<std::pair<uint32_t, int>> sizes;
        std::vector<std::pair<uint32_t, Entity>> first;
        w.each<GroupTag, Unit, Owner, Transform>([&](Entity c, GroupTag& g, Unit& u, Owner& o, Transform& t) {
            if (u.hp.raw <= 0 || !inRange(t.pos) || (offensive ? !isEnemy(o.player) : o.player != player)) return;
            auto it = std::find_if(sizes.begin(), sizes.end(), [&](auto& s) { return s.first == g.id; });
            if (it == sizes.end()) { sizes.push_back({g.id, 1}); first.push_back({g.id, c}); } else it->second++;
        });
        for (size_t i = 0; i < sizes.size(); ++i)
            if (sizes[i].second >= 3) pick(first[i].second, w.get<Transform>(first[i].second).pos, Fixed::fromInt(sizes[i].second));
        break;
    }
    case SmartRule::DebuffedOwn:
        w.each<Statuses, Owner, Transform>([&](Entity c, Statuses& st, Owner& o, Transform& t) {
            if (o.player != player || !inRange(t.pos)) return;
            int n = 0;
            for (auto& in : st.list) if (hostile(sim, in.owner, player) && rules_.statuses[in.def].id[0] != '_') ++n;
            if (n) pick(c, t.pos, Fixed::fromInt(n));
        });
        break;
    }
    return found;
}

} // namespace rts::game

namespace rts::game {
using namespace detail;

// A unit arrives at a garrison structure (advanced effects): illusions vanish, invulnerable structures
// shrug it off, shields absorb, defense can be raised / ignored, contagious statuses spread, a capture
// fires the structure's revenge payloads and cancels temporary-capture reverts.
void RtsModule::enterGarrison(Simulation& sim, Entity e, Entity s) {
    World& w = sim.world();
    auto die = [&]() { Unit& uu = w.get<Unit>(e); uu.hp = Fixed{}; uu.flags |= kUnitSilent; };
    const uint32_t uf = statusFlags(sim, e);
    auto* g = w.tryGet<Garrison>(s);
    if ((uf & kStFake) || !g) { die(); return; }
    const uint8_t me = w.get<Owner>(e).player;
    const uint8_t owner = w.get<Owner>(s).player;
    const UnitType& tt = rules_.types[w.get<Unit>(s).type];
    // statuses that travel with the unit
    if (auto* st = w.tryGet<Statuses>(e)) {
        std::vector<StatusInst> carry;
        for (auto& in : st->list) if (rules_.statuses[in.def].flags & kStSpreads) carry.push_back(in);
        for (auto& in : carry) {
            EffectCtx ctx; ctx.owner = in.owner; ctx.caster = in.source; ctx.sourceType = in.sourceType;
            Fixed left = in.until == UINT32_MAX ? Fixed{} : Fixed::fromInt(int64_t(in.until - sim.tick())) / int64_t(sim.tickRate());
            addStatus(sim, s, in.def, left, ctx);
        }
    }
    if (owner == me || (owner != 0 && owner != kHostileNeutral && sim.allied(owner, me))) { ++g->count; die(); return; }
    const uint32_t sf = statusFlags(sim, s);
    if (sf & kStInvulnerable) { die(); return; }
    if (auto* st = w.tryGet<Statuses>(s))
        for (auto& in : st->list)
            if (in.shield.raw > 0) { in.shield -= kOne; if (in.shield.raw <= 0) in.until = sim.tick(); die(); return; }
    if (g->count > 0) {
        Fixed def = (uf & kStIgnoreDefense) ? kOne : tt.garrisonDefense * stat(sim, s, Stat::Defense);
        g->damageAcc += kOne / fxMax(def, Fixed::fromRatio(1, 10));
        while (g->damageAcc >= kOne && g->count > 0) { --g->count; ++g->lost; g->damageAcc -= kOne; }
    } else if (tt.capturable) {
        // revenge payloads of the defender and recapture of a temporarily captured structure
        std::vector<std::pair<int16_t, uint8_t>> revenge;
        if (auto* st = w.tryGet<Statuses>(s)) {
            for (auto& in : st->list) {
                if (rules_.statuses[in.def].onCaptured >= 0) revenge.push_back({rules_.statuses[in.def].onCaptured, in.owner});
                if (in.revertsOwner) { in.revertsOwner = false; in.until = sim.tick(); }
            }
        }
        Owner& to = w.get<Owner>(s);
        uint8_t old = to.player;
        to.player = me;
        g->count = 1; g->damageAcc = Fixed{}; g->growth = Fixed{}; g->pendingOut = 0;
        if (auto* p = w.tryGet<Production>(s)) p->queue.clear();
        const Vec3 pos = w.get<Transform>(s).pos;
        sim.emit({kEvCaptured, me, s, e, old, pos});
        for (auto& [pl, by] : revenge) {
            EffectCtx ctx; ctx.owner = by; ctx.caster = s; ctx.target = s; ctx.point = pos; ctx.origin = pos;
            applyPayload(sim, pl, ctx);
        }
    }
    die();
}

} // namespace rts::game
