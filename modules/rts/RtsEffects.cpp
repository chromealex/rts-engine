// Effect engine: applies payloads (lists of effects) with target filters and shapes, runs statuses
// (DoT/HoT, shields, flags, stacking, expiry with reverts and follow-up payloads), ground areas
// (delayed strikes, fields, moving hazards, mines, portals, reveals, soul traps), ability projectiles
// (homing / ballistic / colliding) and chains. Garrison, group and player effects: RtsSkills.cpp.
#include "RtsInternal.h"
#include <algorithm>

namespace rts::game {
using namespace detail;

namespace {
constexpr int kMaxDepth = 8;
uint32_t fxTicks(Fixed s, uint32_t rate) { return s.raw <= 0 ? 0 : uint32_t((s * int64_t(rate)).toInt()); }
bool legacyKind(EffectKind k) { return k <= EffectKind::Blink; }
Fixed distToSegment(Vec3 p, Vec3 a, Vec3 b) {
    Vec2 ab = (b - a).xz(), ap = (p - a).xz();
    Fixed len2 = ab.lengthSq();
    Fixed t = len2.raw > 0 ? fxClamp(ap.dot(ab) / len2, Fixed{}, Fixed::fromInt(1)) : Fixed{};
    return (ap - ab * t).length();
}
} // namespace

Fixed RtsModule::rollAmount(Simulation& sim, const EffectDef& e) {
    if (e.randomAmount) return e.amount + (e.amountMax - e.amount) * sim.rng().unit();
    return e.amount;
}

const AbilityDef& RtsModule::abilityFor(Simulation& sim, Entity e, int index) const {
    World& w = sim.world();
    const AbilityDef& base = rules_.types[w.get<Unit>(e).type].abilities[size_t(index)];
    int level = 1;
    if (auto* h = w.tryGet<Hero>(e)) level = rules_.heroes.skills ? std::max<int>(1, h->skill[index & 7]) : h->level;   // learned skill level (WC3)
    if (level >= 2 && !base.levels.empty()) return base.levels[size_t(std::min<int>(level - 2, int(base.levels.size()) - 1))];
    return base;
}

uint32_t RtsModule::statusFlags(Simulation& sim, Entity e) const {
    World& w = sim.world();
    uint32_t f = 0;
    if (auto* st = w.tryGet<Statuses>(e)) f = st->flags;
    if (auto* b = w.tryGet<Buffs>(e); b && b->stunUntil > sim.tick()) f |= kStStun;
    return f;
}

bool RtsModule::isStunned(Simulation& sim, Entity e) const { return (statusFlags(sim, e) & kStStun) != 0; }

Fixed RtsModule::playerStat(Simulation& sim, uint8_t player, Stat s, Fixed base) const {
    auto* gs = sim.world().resource<GameState>();
    if (player >= gs->players.size()) return base;
    Fixed add{}, mul = kOne;
    for (auto& in : gs->players[player].statuses)
        for (auto& m : rules_.statuses[in.def].mods) if (m.stat == s) { add += m.add * in.stacks; for (int k = 0; k < in.stacks; ++k) mul *= m.mul; }
    return (base + add) * mul;
}

// ================================================================= targets

bool RtsModule::filterOk(Simulation& sim, const TargetFilter& f, Entity e, uint8_t owner, Entity caster) const {
    World& w = sim.world();
    auto* u = w.tryGet<Unit>(e);
    if (!u || u->hp.raw <= 0 || (u->flags & kUnitOffMap)) return false;
    const UnitType& t = rules_.types[u->type];
    const uint8_t so = w.get<Owner>(e).player;
    const bool self = e == caster;
    const bool enemy = hostile(sim, owner, so);
    const bool ally = !enemy && so != 0 && (so == owner || (owner != 0 && so != kHostileNeutral && owner != kHostileNeutral && sim.allied(so, owner)));
    if (!((self && f.self) || (enemy && f.enemies) || (ally && f.allies && (!self || f.self || !f.enemies)) || (so == 0 && f.neutral))) return false;
    if (!(t.layer & f.layers) && !(t.structure && (f.layers & kLayerGround))) return false;
    if (t.structure ? !f.structures : !f.units) return false;
    if (f.garrisons && !w.has<Garrison>(e)) return false;
    if (f.inTransit) {
        auto* ord = w.tryGet<Orders>(e);
        if (!w.has<GroupTag>(e) || !ord || ord->queue.empty() || ord->queue.front().type != OrderType::Enter) return false;
    }
    if (f.tags && !(t.tags & f.tags)) return false;
    if (f.type != kNoType && f.type != u->type) return false;
    if (t.level < f.minLevel || t.level > f.maxLevel) return false;
    if (enemy) {
        uint32_t fl = statusFlags(sim, e);
        if ((fl & kStInvisible) && !(fl & kStRevealed) && !(rules_.mech.detection && detected(sim, e, owner))) return false;
    }
    if (!f.area.empty()) {
        int a = triggers_.findArea(f.area);
        if (a < 0 || !triggers_.areas[size_t(a)].contains(w.get<Transform>(e).pos)) return false;
    }
    return true;
}

std::vector<Entity> RtsModule::groupMembers(Simulation& sim, uint32_t group) const {
    std::vector<Entity> out;
    if (!group) return out;
    sim.world().each<GroupTag, Unit>([&](Entity e, GroupTag& g, Unit& u) { if (g.id == group && u.hp.raw > 0) out.push_back(e); });
    return out;
}

std::vector<Entity> RtsModule::effectTargets(Simulation& sim, const EffectDef& e, const EffectCtx& ctx) {
    World& w = sim.world();
    GameState& gs = state(sim);
    const TargetFilter& f = e.filter;
    std::vector<Entity> out;
    const Vec3 casterPos = w.alive(ctx.caster) && w.has<Transform>(ctx.caster) ? w.get<Transform>(ctx.caster).pos : ctx.origin;
    const Vec3 center = e.fromCaster ? casterPos : ctx.point;
    auto consider = [&](Entity c) { if (filterOk(sim, f, c, ctx.owner, ctx.caster)) out.push_back(c); };
    switch (f.shape) {
    case Shape::Target:
        if (f.self && !f.enemies && !f.allies) { if (w.alive(ctx.caster) && w.has<Unit>(ctx.caster)) out.push_back(ctx.caster); }
        else if (w.alive(ctx.target) && w.has<Unit>(ctx.target)) {
            if (legacyKind(e.kind) || filterOk(sim, f, ctx.target, ctx.owner, ctx.caster)) out.push_back(ctx.target);
        }
        break;
    case Shape::Group: {
        uint32_t g = ctx.group;
        if (!g && w.alive(ctx.target)) if (auto* gt = w.tryGet<GroupTag>(ctx.target)) g = gt->id;
        for (Entity m : groupMembers(sim, g)) consider(m);
        break;
    }
    case Shape::Map:
        w.each<Unit, Owner, Transform>([&](Entity c, Unit&, Owner&, Transform&) { consider(c); });
        break;
    default: {
        Fixed r = fxMax(e.radius, kHalf);
        std::vector<Entity> cand;
        if (r > Fixed::fromInt(24)) w.each<Unit, Owner, Transform>([&](Entity c, Unit&, Owner&, Transform&) { cand.push_back(c); });
        else {
            gs.hash.query(center, r + Fixed::fromInt(2), [&](Entity c) { cand.push_back(c); });
            if (f.shape == Shape::Line) gs.hash.query(casterPos, r + Fixed::fromInt(2), [&](Entity c) { cand.push_back(c); });
        }
        std::sort(cand.begin(), cand.end(), [](Entity a, Entity b) { return a.packed() < b.packed(); });
        cand.erase(std::unique(cand.begin(), cand.end()), cand.end());
        Vec3 dir = flat(ctx.point - casterPos);
        Fixed dl = dir.length();
        if (dl.raw > 0) dir = dir / dl; else dir = {kOne, Fixed{}, Fixed{}};
        for (Entity c : cand) {
            if (!w.has<Unit>(c)) continue;
            Vec3 p = w.get<Transform>(c).pos;
            Fixed body = rules_.types[w.get<Unit>(c).type].radius;
            bool in = false;
            switch (f.shape) {
            case Shape::Circle: in = dist2D(p, center) <= r + body * 0; break;
            case Shape::Ring: { Fixed d = dist2D(p, center); in = d <= r && d >= r - f.ring; break; }
            case Shape::Line: in = distToSegment(p, casterPos, casterPos + dir * r) <= f.width / 2 + body; break;
            case Shape::Cone: {
                Vec3 rel = flat(p - casterPos);
                Fixed d = rel.length();
                in = d <= r && (d.raw == 0 || (rel / d).dot(dir) >= f.angle);
                break;
            }
            default: break;
            }
            if (in) consider(c);
        }
        break;
    }
    }
    // ordering and limits
    if (f.sort != SortBy::None || f.ownFirst || f.maxTargets > 0) {
        std::sort(out.begin(), out.end(), [](Entity a, Entity b) { return a.packed() < b.packed(); });
        auto key = [&](Entity c) -> Fixed {
            const Unit& u = w.get<Unit>(c);
            switch (f.sort) {
            case SortBy::Nearest: return dist2D(w.get<Transform>(c).pos, center);
            case SortBy::Farthest: return -dist2D(w.get<Transform>(c).pos, center);
            case SortBy::LowestHp: return u.hp / fxMax(stat(sim, c, Stat::MaxHp), kOne);
            case SortBy::HighestHp: return -u.hp;
            case SortBy::LevelLow: return Fixed::fromInt(rules_.types[u.type].level);
            case SortBy::LevelHigh: return -Fixed::fromInt(rules_.types[u.type].level);
            case SortBy::GarrisonLow: { auto* g = w.tryGet<Garrison>(c); return Fixed::fromInt(g ? g->count : 0); }
            case SortBy::GarrisonHigh: { auto* g = w.tryGet<Garrison>(c); return -Fixed::fromInt(g ? g->count : 0); }
            default: return Fixed{};
            }
        };
        if (f.sort == SortBy::Random)
            for (size_t i = out.size(); i > 1; --i) std::swap(out[i - 1], out[sim.rng().below(uint32_t(i))]);
        else if (f.sort != SortBy::None)
            std::stable_sort(out.begin(), out.end(), [&](Entity a, Entity b) { return key(a) < key(b); });
        if (f.ownFirst)
            std::stable_sort(out.begin(), out.end(), [&](Entity a, Entity b) {
                return (w.get<Owner>(a).player == ctx.owner) > (w.get<Owner>(b).player == ctx.owner);
            });
        if (f.maxTargets > 0 && int(out.size()) > f.maxTargets) out.resize(size_t(f.maxTargets));
    }
    return out;
}

std::vector<uint8_t> RtsModule::effectPlayers(Simulation& sim, const EffectDef& e, const EffectCtx& ctx) {
    World& w = sim.world();
    std::vector<uint8_t> out;
    if (e.filter.shape == Shape::Target && e.kind != EffectKind::EqualizeResource && w.alive(ctx.target) && w.has<Owner>(ctx.target)) {
        uint8_t p = w.get<Owner>(ctx.target).player;
        if (p != 0 && p != kHostileNeutral) out.push_back(p);
        return out;
    }
    for (auto& pi : sim.players()) {
        if (!pi.alive) continue;
        bool self = pi.slot == ctx.owner, enemy = !self && hostile(sim, ctx.owner, pi.slot), ally = !self && !enemy;
        if ((self && (e.filter.self || (e.filter.allies && !e.filter.enemies))) || (enemy && e.filter.enemies) || (ally && e.filter.allies)) out.push_back(pi.slot);
    }
    return out;
}

// ================================================================= statuses

void RtsModule::addStatus(Simulation& sim, Entity target, int def, Fixed duration, const EffectCtx& ctx) {
    World& w = sim.world();
    auto* st = w.tryGet<Statuses>(target);
    if (!st || def < 0) return;
    const StatusDef& d = rules_.statuses[size_t(def)];
    const uint32_t tick = sim.tick();
    const uint32_t until = duration.raw > 0 ? tick + std::max<uint32_t>(1, fxTicks(duration, sim.tickRate())) : UINT32_MAX;
    if (d.stacking != Stacking::Independent)
        for (auto& in : st->list) {
            if (in.def != def) continue;
            in.until = std::max(in.until, until);
            in.shield = fxMax(in.shield, d.shield * ctx.scale);
            if (d.stacking == Stacking::Stack && in.stacks < d.maxStacks) ++in.stacks;
            return;
        }
    StatusInst in;
    in.def = uint16_t(def);
    in.owner = ctx.owner;
    in.source = ctx.caster;
    in.sourceType = ctx.sourceType;
    in.until = until;
    in.nextTick = tick + std::max<uint32_t>(1, fxTicks(d.tickInterval, sim.tickRate()));
    in.shield = d.shield * ctx.scale;
    st->list.push_back(in);
    st->flags |= d.flags;
    if (d.flags & (kStStun | kStRoot)) stopMoving(sim, target);
    sim.emit({kEvStatusStart, w.get<Owner>(target).player, target, ctx.caster, def, w.get<Transform>(target).pos});
}

void RtsModule::addPlayerStatus(Simulation& sim, uint8_t player, int def, Fixed duration, const EffectCtx& ctx) {
    GameState& gs = state(sim);
    if (player == 0 || player >= gs.players.size() || def < 0) return;
    auto& list = gs.players[player].statuses;
    const StatusDef& d = rules_.statuses[size_t(def)];
    const uint32_t until = duration.raw > 0 ? sim.tick() + std::max<uint32_t>(1, fxTicks(duration, sim.tickRate())) : UINT32_MAX;
    for (auto& in : list)
        if (in.def == def && d.stacking != Stacking::Independent) {
            in.until = std::max(in.until, until);
            if (d.stacking == Stacking::Stack && in.stacks < d.maxStacks) ++in.stacks;
            return;
        }
    StatusInst in;
    in.def = uint16_t(def); in.owner = ctx.owner; in.source = ctx.caster; in.until = until;
    list.push_back(in);
    gs.players[player].statusFlags |= d.flags;
    sim.emit({kEvStatusStart, player, kNullEntity, ctx.caster, def, {}});
}

namespace detail {
void changeUnitType(RtsModule& m, Simulation& sim, Entity e, uint16_t type) {
    World& w = sim.world();
    GameState& gs = m.state(sim);
    Unit& u = w.get<Unit>(e);
    if (u.type == type) return;
    const UnitType& ot = m.rules().types[u.type];
    const UnitType& nt = m.rules().types[type];
    uint8_t owner = w.get<Owner>(e).player;
    Fixed frac = u.hp / fxMax(ot.maxHp, Fixed::fromInt(1));
    if (owner < gs.players.size() && !gs.players[owner].typeCountAll.empty()) {
        if (gs.players[owner].typeCountAll[u.type] > 0) gs.players[owner].typeCountAll[u.type]--;
        gs.players[owner].typeCountAll[type]++;
    }
    if (auto* f = w.tryGet<Footprint>(e); f && (f->w != nt.footprintW || f->h != nt.footprintH) && nt.structure) {
        gs.grid.block(f->x, f->y, f->w, f->h, -1);
        RtsModule::footprintAt(nt, w.get<Transform>(e).pos, f->x, f->y);
        f->w = nt.footprintW; f->h = nt.footprintH;
        gs.grid.block(f->x, f->y, f->w, f->h, +1);
    }
    u.type = type;
    u.hp = fxMax(Fixed::fromInt(1), nt.maxHp * frac);
    if (!nt.weapons.empty() && !w.has<Combat>(e)) w.add<Combat>(e);
    if ((nt.garrisonMax > 0 || nt.spawnUnit != kNoType) && !w.has<Garrison>(e)) w.add<Garrison>(e);
    if (!nt.abilities.empty() && !w.has<AbilityState>(e)) w.add<AbilityState>(e);
    if ((!nt.produces.empty() || !nt.researches.empty() || !nt.upgrades.empty()) && !w.has<Production>(e)) w.add<Production>(e);
    sim.emit({kEvUpgraded, owner, e, kNullEntity, type, w.get<Transform>(e).pos});
}
} // namespace

void RtsModule::tickStatuses(Simulation& sim) {
    World& w = sim.world();
    GameState& gs = state(sim);
    const uint32_t tick = sim.tick();
    struct Ev { Entity holder; StatusInst in; bool expired; };
    std::vector<Ev> evs;
    w.each<Statuses>([&](Entity e, Statuses& st) {
        if (st.list.empty()) return;
        bool changed = false;
        for (size_t i = 0; i < st.list.size();) {
            StatusInst& in = st.list[i];
            const StatusDef& d = rules_.statuses[in.def];
            // a tick that falls on the last tick still happens (duration 3, interval 1 = 3 ticks)
            if (in.nextTick <= tick && in.nextTick <= in.until && (d.tickAmount.raw != 0 || d.tickPayload >= 0 || d.garrisonPerSecond.raw != 0 || in.def == uint16_t(rules_.stLink))) {
                in.nextTick = tick + std::max<uint32_t>(1, fxTicks(d.tickInterval, sim.tickRate()));
                evs.push_back({e, in, false});
            }
            if (in.until <= tick) { evs.push_back({e, in, true}); st.list.erase(st.list.begin() + long(i)); changed = true; continue; }
            ++i;
        }
        if (changed) { st.flags = 0; for (auto& in : st.list) st.flags |= rules_.statuses[in.def].flags; }
    });
    for (auto& ev : evs) {
        if (!w.alive(ev.holder) || !w.has<Unit>(ev.holder)) continue;
        const StatusDef& d = rules_.statuses[ev.in.def];
        const Vec3 pos = w.get<Transform>(ev.holder).pos;
        EffectCtx ctx;
        ctx.caster = ev.holder; ctx.owner = ev.in.owner; ctx.sourceType = ev.in.sourceType; ctx.target = ev.holder; ctx.point = pos; ctx.origin = pos;
        if (auto* gt = w.tryGet<GroupTag>(ev.holder)) ctx.group = gt->id;
        if (ev.expired) {
            sim.emit({kEvStatusEnd, w.get<Owner>(ev.holder).player, ev.holder, ev.in.source, ev.in.def, pos});
            Owner& o = w.get<Owner>(ev.holder);
            if (ev.in.revertsOwner && o.player == ev.in.capturedBy) {
                uint8_t old = o.player;
                o.player = ev.in.revertOwner;
                if (auto* p = w.tryGet<Production>(ev.holder)) p->queue.clear();
                sim.emit({kEvCaptured, o.player, ev.holder, kNullEntity, old, pos});
            }
            if (ev.in.revertType != kNoType) changeUnitType(*this, sim, ev.holder, ev.in.revertType);
            if (d.onExpire >= 0) { ctx.caster = ev.in.source; applyPayload(sim, d.onExpire, ctx); }
            continue;
        }
        if (d.tickAmount.raw > 0) dealDamage(sim, ev.holder, d.tickAmount * ev.in.stacks, d.tickDamageType, ev.in.source, ev.in.owner);
        else if (d.tickAmount.raw < 0) { Unit& u = w.get<Unit>(ev.holder); u.hp = fxMin(stat(sim, ev.holder, Stat::MaxHp), u.hp - d.tickAmount * ev.in.stacks); }
        if (auto* g = w.tryGet<Garrison>(ev.holder)) {
            if (d.garrisonPerSecond.raw != 0) {
                for (auto& in : w.get<Statuses>(ev.holder).list) {
                    if (in.def != ev.in.def || in.owner != ev.in.owner) continue;
                    in.acc += d.garrisonPerSecond * d.tickInterval * in.stacks;
                    while (in.acc >= kOne) { ++g->count; in.acc -= kOne; }
                    while (in.acc <= -kOne && g->count > 0) { --g->count; in.acc += kOne; }
                    break;
                }
            }
            if (ev.in.def == uint16_t(rules_.stLink) && w.alive(ev.in.link)) {
                if (auto* g2 = w.tryGet<Garrison>(ev.in.link); g2 && w.get<Owner>(ev.in.link).player == w.get<Owner>(ev.holder).player && g->count > g2->count + 1) {
                    int move = (g->count - g2->count) / 2;
                    g->count -= move; g2->count += move;
                }
            }
        }
        if (d.tickPayload >= 0) applyPayload(sim, d.tickPayload, ctx);
    }
    // player statuses
    for (size_t p = 1; p < gs.players.size(); ++p) {
        auto& list = gs.players[p].statuses;
        bool changed = false;
        for (size_t i = 0; i < list.size();) {
            if (list[i].until <= tick) {
                StatusInst in = list[i];
                list.erase(list.begin() + long(i));
                changed = true;
                sim.emit({kEvStatusEnd, uint8_t(p), kNullEntity, in.source, in.def, {}});
                if (rules_.statuses[in.def].onExpire >= 0) {
                    EffectCtx ctx; ctx.owner = in.owner; ctx.caster = in.source;
                    applyPayload(sim, rules_.statuses[in.def].onExpire, ctx);
                }
                continue;
            }
            ++i;
        }
        if (changed) { gs.players[p].statusFlags = 0; for (auto& in : list) gs.players[p].statusFlags |= rules_.statuses[in.def].flags; }
    }
}

int RtsModule::dispel(Simulation& sim, Entity target, uint64_t tags, uint8_t from) {
    World& w = sim.world();
    auto* st = w.tryGet<Statuses>(target);
    if (!st) return 0;
    const uint8_t holder = w.get<Owner>(target).player;
    int removed = 0;
    for (auto& in : st->list) {
        const StatusDef& d = rules_.statuses[in.def];
        bool builtin = !d.id.empty() && d.id[0] == '_';
        if (tags ? !(d.tags & tags) : (builtin && in.revertType == kNoType && !in.revertsOwner && in.def != uint16_t(rules_.stFake))) continue;
        bool hostileSrc = hostile(sim, in.owner, holder);
        if ((from == 1 && !hostileSrc) || (from == 2 && hostileSrc)) continue;
        in.until = sim.tick();   // ends (with reverts) on the next status pass; no onExpire for dispelled statuses
        if (d.onExpire >= 0) in.def = uint16_t(rules_.stTemp);
        ++removed;
    }
    if (!tags) {
        if (auto* b = w.tryGet<Buffs>(target)) { if (from != 2) b->stunUntil = 0; b->list.clear(); }
    }
    return removed;
}

// ================================================================= payloads

void RtsModule::applyPayload(Simulation& sim, int payload, const EffectCtx& ctx) {
    if (payload < 0 || size_t(payload) >= rules_.payloads.size() || ctx.depth > kMaxDepth) return;
    const auto& list = rules_.payloads[size_t(payload)];
    for (size_t i = 0; i < list.size(); ++i) applyEffect(sim, list[i], payload, int(i), ctx);
}

void RtsModule::applyEffect(Simulation& sim, const EffectDef& e, int list, int index, EffectCtx ctx) {
    World& w = sim.world();
    GameState& gs = state(sim);
    if (e.chance < kOne && sim.rng().unit() >= e.chance) return;
    const uint32_t tick = sim.tick();
    ctx.depth++;
    const Vec3 casterPos = w.alive(ctx.caster) && w.has<Transform>(ctx.caster) ? w.get<Transform>(ctx.caster).pos : ctx.origin;
    auto targets = [&]() { return effectTargets(sim, e, ctx); };
    const Fixed amount = rollAmount(sim, e) * ctx.scale;
    switch (e.kind) {
    case EffectKind::Damage: {
        for (Entity h : targets()) {
            Fixed dmg = amount;
            if (e.delivery.falloff.raw > 0 && e.radius.raw > 0)
                dmg = dmg * (kOne - e.delivery.falloff * fxMin(kOne, dist2D(w.get<Transform>(h).pos, ctx.point) / e.radius));
            dealDamage(sim, h, dmg, e.damageType, ctx.caster, ctx.owner);
        }
        break;
    }
    case EffectKind::Heal:
        for (Entity h : targets()) { Unit& hu = w.get<Unit>(h); hu.hp = fxMin(stat(sim, h, Stat::MaxHp), hu.hp + amount); }
        break;
    case EffectKind::Stun: {
        const uint32_t until = tick + fxTicks(e.duration, sim.tickRate());
        for (Entity h : targets()) if (auto* b = w.tryGet<Buffs>(h)) { b->stunUntil = std::max(b->stunUntil, until); stopMoving(sim, h); }
        break;
    }
    case EffectKind::Buff: {
        const uint32_t until = tick + fxTicks(e.duration, sim.tickRate());
        for (Entity h : targets()) if (auto* b = w.tryGet<Buffs>(h)) {
            Buff nb{e.mod.stat, e.mod.add, e.mod.mul, until, ctx.sourceType};
            bool found = false;
            for (auto& x : b->list) if (x.stat == nb.stat && x.sourceType == nb.sourceType) { x = nb; found = true; }
            if (!found) b->list.push_back(nb);
        }
        break;
    }
    case EffectKind::Spawn: {
        uint8_t owner = e.mode == 1 ? kHostileNeutral : e.mode == 2 ? 0 : ctx.owner;
        int alive = 0;
        if (e.limit > 0)
            w.each<Summon, Unit>([&](Entity, Summon& s, Unit& u) {
                if (u.type == e.spawnType && u.hp.raw > 0 && s.summoner == ctx.caster && s.summonerType == ctx.sourceType) ++alive;
            });
        const uint32_t expire = e.duration.raw > 0 ? tick + fxTicks(e.duration, sim.tickRate()) : 0;
        Vec3 at = e.fromCaster ? casterPos : ctx.point;
        for (int k = 0; k < e.count; ++k) {
            if (e.limit > 0 && alive >= e.limit) break;
            Vec3 p = at + Vec3{Fixed::fromRatio((k % 3) - 1, 1), Fixed{}, Fixed::fromRatio((k / 3) % 3 - 1, 1)};
            const UnitType& st = rules_.types[e.spawnType];
            if (st.structure) {
                int x0, y0;
                footprintAt(st, snapToFootprint(st, p), x0, y0);
                if (!gs.grid.areaBuildable(x0, y0, st.footprintW, st.footprintH)) continue;
            } else {
                int cx, cy; gs.grid.cellXY(p, cx, cy);
                if (!gs.grid.nearestPassable(cx, cy, st.layer, 4)) continue;
                p = gs.grid.cellCenter(cx, cy);
            }
            Entity n = spawn(sim, e.spawnType, owner, p);
            w.add<Summon>(n, Summon{expire, ctx.sourceType, ctx.caster});
            ++alive;
        }
        break;
    }
    case EffectKind::Blink: {
        if (!w.alive(ctx.caster)) break;
        auto& g = gs.grid;
        const UnitType& ct = rules_.types[w.get<Unit>(ctx.caster).type];
        if (g.passableAt(ctx.point, ct.layer)) {
            Transform& tr = w.get<Transform>(ctx.caster);
            tr.pos = flat(ctx.point); tr.prevPos = tr.pos;
            stopMoving(sim, ctx.caster);
        }
        break;
    }
    case EffectKind::Status:
        for (Entity h : targets()) addStatus(sim, h, e.status, e.duration, ctx);
        break;
    case EffectKind::Dispel:
        for (Entity h : targets()) dispel(sim, h, e.dispelTags, e.dispelFrom);
        if (e.filter.shape == Shape::Map) // whole-map dispel also clears player statuses
            for (uint8_t p : effectPlayers(sim, e, ctx))
                for (auto& in : gs.players[p].statuses) {
                    const StatusDef& d = rules_.statuses[in.def];
                    bool hostileSrc = hostile(sim, in.owner, p);
                    if ((e.dispelTags && !(d.tags & e.dispelTags)) || (e.dispelFrom == 1 && !hostileSrc) || (e.dispelFrom == 2 && hostileSrc)) continue;
                    in.until = tick;
                }
        break;
    case EffectKind::Knockback: {
        Vec3 origin = e.fromCaster ? casterPos : ctx.point;
        for (Entity h : targets()) {
            if (!w.has<Mobile>(h)) continue;
            Transform& tr = w.get<Transform>(h);
            if (auto* lw = w.tryGet<LaneWalker>(h)) { lw->pos = uint16_t(std::max<int>(0, int(lw->pos) - int((amount * 2).toInt()))); continue; }
            Vec3 d = flat(tr.pos - origin);
            Fixed dl = d.length();
            if (dl.raw == 0) { d = {kOne, Fixed{}, Fixed{}}; dl = kOne; }
            d = d / dl;
            Fixed dist = amount.raw < 0 ? -fxMin(-amount, dl) : amount;
            const UnitType& ht = rules_.types[w.get<Unit>(h).type];
            for (int k = 0; k < 8; ++k) {
                Vec3 np = tr.pos + d * (dist / 8);
                if (!(ht.layer & kLayerAir) && !gs.grid.passableAt(np, ht.layer)) break;
                tr.pos = np;
            }
            stopMoving(sim, h);
        }
        break;
    }
    case EffectKind::Teleport: {
        std::vector<Entity> who = e.mode == 1 ? std::vector<Entity>{ctx.caster} : targets();
        int k = 0;
        for (Entity h : who) {
            if (!w.alive(h) || !w.has<Mobile>(h)) continue;
            Vec3 p = flat(ctx.point) + Vec3{Fixed::fromRatio(k % 3 - 1, 2), Fixed{}, Fixed::fromRatio((k / 3) % 3 - 1, 2)};
            ++k;
            const UnitType& ht = rules_.types[w.get<Unit>(h).type];
            int cx, cy; gs.grid.cellXY(p, cx, cy);
            if (!(ht.layer & kLayerAir) && !gs.grid.passable(cx, cy, ht.layer)) {
                if (!gs.grid.nearestPassable(cx, cy, ht.layer, 3)) continue;
                p = gs.grid.cellCenter(cx, cy);
            }
            Transform& tr = w.get<Transform>(h);
            tr.pos = p; tr.prevPos = p;
            stopMoving(sim, h);
            if (auto* lw = w.tryGet<LaneWalker>(h); lw && lw->lane >= 0 && size_t(lw->lane) < gs.lanes.size()) {
                // continue from the nearest point of the own lane
                const auto& pts = gs.lanes[size_t(lw->lane)].points;
                Fixed best{}; uint16_t bi = lw->pos;
                for (size_t i = 0; i < pts.size(); ++i) {
                    Fixed d = dist2D(pts[lw->reverse ? pts.size() - 1 - i : i], p);
                    if (i == 0 || d < best) { best = d; bi = uint16_t(i); }
                }
                lw->pos = bi;
            }
        }
        break;
    }
    case EffectKind::EnergyBurn:
        for (Entity h : targets()) {
            Unit& hu = w.get<Unit>(h);
            Fixed burn = fxMin(hu.energy, amount);
            hu.energy -= burn;
            if (e.percent.raw > 0) dealDamage(sim, h, burn * e.percent, e.damageType, ctx.caster, ctx.owner);
        }
        break;
    case EffectKind::Kill: {
        auto list2 = targets();
        size_t n = e.percent.raw > 0 ? size_t((e.percent * int64_t(list2.size()) + Fixed::fromRaw(65535)).floorInt()) : list2.size();
        for (size_t k = 0; k < list2.size() && k < n; ++k) {
            Unit& hu = w.get<Unit>(list2[k]);
            if (rules_.types[hu.type].invulnerable || (statusFlags(sim, list2[k]) & kStInvulnerable)) continue;
            hu.hp = Fixed{};
            hu.lastAttacker = ctx.owner;
        }
        break;
    }
    case EffectKind::Script:   // deferred to the scripts phase of this tick (never runs inside a system's loop)
        if (script_) script_->queueEffect(e, ctx, targets());
        break;
    case EffectKind::Area:
    case EffectKind::Reveal: {
        if (list < 0) break;
        Entity a = w.create();
        Vec3 at = flat(e.fromCaster || e.delivery.motion == AreaMotion::Toward ? casterPos : ctx.point);
        w.add<Transform>(a, Transform{at, at, Fixed{}});
        w.add<Owner>(a, Owner{ctx.owner});
        Area ar;
        ar.caster = ctx.caster; ar.sourceType = ctx.sourceType; ar.effList = int16_t(list); ar.effIndex = int16_t(index);
        ar.radius = fxMax(e.radius, kHalf);
        ar.scale = ctx.scale;
        ar.fireAt = tick + fxTicks(e.delivery.delay, sim.tickRate());
        ar.nextAt = ar.fireAt;
        Fixed life = e.kind == EffectKind::Reveal ? fxMax(e.duration, e.delivery.lifetime) : e.delivery.lifetime;
        ar.endAt = life.raw > 0 ? ar.fireAt + fxTicks(life, sim.tickRate()) : (e.delivery.trigger == AreaTrigger::Contact ? UINT32_MAX : ar.fireAt);
        ar.dest = flat(ctx.point) + e.delivery.teleportOffset;
        Vec3 d = flat(ctx.point - casterPos);
        if (d.length().raw > 0) ar.dir = d / d.length();
        if (e.delivery.motion == AreaMotion::Follow) ar.follow = w.alive(ctx.target) ? ctx.target : ctx.caster;
        if (e.delivery.motion == AreaMotion::Lane) {
            // along the lane of the targeted group, from its start
            Entity m = ctx.target;
            if (!w.alive(m) || !w.has<LaneWalker>(m)) { auto mem = groupMembers(sim, ctx.group); m = mem.empty() ? kNullEntity : mem[0]; }
            if (auto* lw = w.alive(m) ? w.tryGet<LaneWalker>(m) : nullptr; lw && lw->lane >= 0 && size_t(lw->lane) < gs.lanes.size()) {
                ar.lane = lw->lane; ar.laneReverse = lw->reverse; ar.lanePos = 0;
                const auto& pts = gs.lanes[size_t(lw->lane)].points;
                Vec3 p0 = pts[lw->reverse ? pts.size() - 1 : 0];
                w.get<Transform>(a).pos = p0; w.get<Transform>(a).prevPos = p0;
            }
        }
        w.add<Area>(a, ar);
        sim.emit({kEvAreaStart, ctx.owner, a, ctx.caster, list, at});
        break;
    }
    case EffectKind::Projectile: {
        if (list < 0) break;
        Entity p = w.create();
        Vec3 aim = flat(ctx.point);
        if (e.delivery.scatter.raw > 0) aim = aim + Vec3{(sim.rng().unit() * 2 - kOne) * e.delivery.scatter, Fixed{}, (sim.rng().unit() * 2 - kOne) * e.delivery.scatter};
        Vec3 start = w.alive(ctx.caster) ? casterPos : aim + Vec3{-Fixed::fromInt(3), Fixed{}, -Fixed::fromInt(3)}; // no caster: from the sky
        start.y = w.alive(ctx.caster) ? Fixed::fromRatio(6, 10) : Fixed::fromInt(6);
        Projectile pr;
        pr.target = e.delivery.homing && w.alive(ctx.target) ? ctx.target : kNullEntity;
        pr.source = ctx.caster; pr.aim = aim; pr.speed = fxMax(e.delivery.speed, Fixed::fromRatio(1, 10));
        pr.owner = ctx.owner; pr.layers = 0xFF; pr.effList = int16_t(list); pr.effIndex = int16_t(index);
        pr.sourceType = ctx.sourceType; pr.start = start; pr.total = (aim - start).length(); pr.scale = ctx.scale;
        w.add<Transform>(p, Transform{start, start, Fixed{}});
        w.add<Projectile>(p, pr);
        w.add<Owner>(p, Owner{ctx.owner});
        sim.emit({kEvProjectileLaunch, ctx.owner, p, ctx.caster, list, start});
        break;
    }
    case EffectKind::Chain: {
        std::vector<Entity> hit;
        Entity cur = w.alive(ctx.target) && w.has<Unit>(ctx.target) && filterOk(sim, e.filter, ctx.target, ctx.owner, ctx.caster) ? ctx.target : kNullEntity;
        Vec3 from = ctx.point;
        Fixed scale = ctx.scale;
        auto nearest = [&](Vec3 p, Fixed r) {
            Entity best; Fixed bd{};
            w.each<Unit, Owner, Transform>([&](Entity c, Unit&, Owner&, Transform& t) {
                if (std::find(hit.begin(), hit.end(), c) != hit.end()) return;
                Fixed d = dist2D(t.pos, p);
                if (d > r || !filterOk(sim, e.filter, c, ctx.owner, ctx.caster)) return;
                if (!best.valid() || d < bd) { best = c; bd = d; }
            });
            return best;
        };
        if (!cur.valid()) cur = nearest(from, e.delivery.jumpRadius);
        Entity prev = ctx.caster;
        for (int j = 0; j <= e.delivery.jumps && cur.valid(); ++j) {
            hit.push_back(cur);
            EffectCtx c2 = ctx;
            c2.target = cur; c2.point = w.get<Transform>(cur).pos; c2.scale = scale;
            sim.emit({kEvChainJump, ctx.owner, prev, cur, j, c2.point});
            applyPayload(sim, e.payload, c2);
            prev = cur;
            from = c2.point;
            scale = scale * e.delivery.jumpFalloff;
            cur = nearest(from, e.delivery.jumpRadius);
        }
        break;
    }
    case EffectKind::Cast: {
        auto list2 = e.filter.explicitShape ? targets() : std::vector<Entity>{};
        if (list2.empty()) { applyPayload(sim, e.payload, ctx); break; }
        for (Entity h : list2) { EffectCtx c2 = ctx; c2.target = h; c2.point = w.get<Transform>(h).pos; applyPayload(sim, e.payload, c2); }
        break;
    }
    case EffectKind::GarrisonKill: case EffectKind::GarrisonAdd: case EffectKind::GarrisonConvert: case EffectKind::GarrisonPanic:
    case EffectKind::Capture: case EffectKind::Swap: case EffectKind::Transform: case EffectKind::Level: case EffectKind::Equalize:
    case EffectKind::Link: case EffectKind::Redirect: case EffectKind::Sacrifice:
        effectGarrison(sim, e, ctx, targets());
        break;
    case EffectKind::SendBack: case EffectKind::Clone: case EffectKind::Formation: case EffectKind::Convert:
        effectGroup(sim, e, ctx, targets());
        break;
    case EffectKind::Resource: case EffectKind::PlayerStatus: case EffectKind::EqualizeResource: case EffectKind::StealSkill: case EffectKind::Cooldown:
        effectPlayer(sim, e, ctx);
        break;
    case EffectKind::AddCharges: case EffectKind::TeleportGroup:
        effectFidelity(sim, e, ctx);   // RtsFidelity.cpp
        break;
    }
    if (list >= 0 && (e.kind == EffectKind::Damage || e.kind == EffectKind::Heal || e.kind == EffectKind::Status))
        sim.emit({kEvImpact, ctx.owner, ctx.target, ctx.caster, list, ctx.point});
}

// ================================================================= areas

void RtsModule::tickAreas(Simulation& sim) {
    World& w = sim.world();
    GameState& gs = state(sim);
    const uint32_t tick = sim.tick();
    const Fixed dt = sim.dt();
    std::vector<Entity> areas;
    w.each<Area, Transform>([&](Entity e, Area&, Transform&) { areas.push_back(e); });
    for (Entity a : areas) {
        if (!w.alive(a)) continue;
        Area& ar = w.get<Area>(a);
        const EffectDef& e = rules_.payloads[size_t(ar.effList)][size_t(ar.effIndex)];
        Transform& tr = w.get<Transform>(a);
        tr.prevPos = tr.pos;
        const uint8_t owner = w.get<Owner>(a).player;
        // motion
        if (tick >= ar.fireAt) {
            Fixed step = e.delivery.speed * dt;
            switch (e.delivery.motion) {
            case AreaMotion::Toward: tr.pos = tr.pos + ar.dir * step; break;
            case AreaMotion::Follow: if (w.alive(ar.follow) && w.has<Transform>(ar.follow)) tr.pos = flat(w.get<Transform>(ar.follow).pos); else ar.endAt = tick; break;
            case AreaMotion::Wander:
                if ((tick - ar.fireAt) % (2 * sim.tickRate()) == 0) { ar.dir = {sim.rng().unit() * 2 - kOne, Fixed{}, sim.rng().unit() * 2 - kOne}; if (ar.dir.length().raw > 0) ar.dir = ar.dir / ar.dir.length(); }
                { Vec3 np = tr.pos + ar.dir * step; if ((gs.grid.cellOf(np) >= 0)) tr.pos = np; }
                break;
            case AreaMotion::Lane:
                if (ar.lane >= 0 && size_t(ar.lane) < gs.lanes.size()) {
                    const auto& pts = gs.lanes[size_t(ar.lane)].points;
                    while (step.raw > 0 && ar.lanePos < pts.size()) {
                        Vec3 wp = pts[ar.laneReverse ? pts.size() - 1 - ar.lanePos : ar.lanePos];
                        Vec3 d = flat(wp - tr.pos);
                        Fixed len = d.length();
                        if (len <= step) { tr.pos = wp; step -= len; ++ar.lanePos; }
                        else { tr.pos = tr.pos + d * (step / len); step = Fixed{}; }
                    }
                    if (ar.lanePos >= pts.size()) ar.endAt = tick;
                }
                break;
            default: break;
            }
        }
        EffectCtx ctx;
        ctx.caster = ar.caster; ctx.owner = owner; ctx.sourceType = ar.sourceType; ctx.point = tr.pos; ctx.origin = tr.pos; ctx.scale = ar.scale;
        if (tick >= ar.fireAt && tick <= ar.endAt) {
            if (e.delivery.trigger == AreaTrigger::Contact) {
                std::vector<Entity> touch;
                gs.hash.query(tr.pos, ar.radius + kOne, [&](Entity c) {
                    if (!w.has<Unit>(c) || dist2D(w.get<Transform>(c).pos, tr.pos) > ar.radius) return;
                    if (std::find(ar.touched.begin(), ar.touched.end(), c.packed()) != ar.touched.end()) return;
                    if (filterOk(sim, e.filter, c, owner, ar.caster)) touch.push_back(c);
                });
                std::sort(touch.begin(), touch.end(), [](Entity x, Entity y) { return x.packed() < y.packed(); });
                for (Entity c : touch) {
                    if (!w.alive(a)) break;
                    Area& a2 = w.get<Area>(a);
                    if (e.delivery.maxTriggers > 0 && a2.triggers >= e.delivery.maxTriggers) break;
                    a2.touched.push_back(c.packed());
                    a2.triggers++;
                    sim.emit({kEvAreaTrigger, owner, a, c, ar.effList, tr.pos});
                    if (e.delivery.teleport && w.has<Mobile>(c)) {
                        EffectDef tp; tp.kind = EffectKind::Teleport;
                        EffectCtx c2 = ctx; c2.target = c; c2.point = a2.dest;
                        tp.filter.shape = Shape::Target;
                        tp.filter.allies = tp.filter.enemies = tp.filter.self = tp.filter.neutral = true; // the area's filter already chose
                        applyEffect(sim, tp, -1, -1, c2);
                    }
                    EffectCtx c2 = ctx; c2.target = c; c2.point = w.get<Transform>(c).pos;
                    applyPayload(sim, e.payload, c2);
                }
                if (w.alive(a) && e.delivery.maxTriggers > 0 && w.get<Area>(a).triggers >= e.delivery.maxTriggers) w.get<Area>(a).endAt = tick;
            } else if (tick >= ar.nextAt) {
                ar.nextAt = tick + std::max<uint32_t>(1, fxTicks(e.delivery.interval, sim.tickRate()));
                ar.triggers++;
                sim.emit({kEvAreaTrigger, owner, a, kNullEntity, ar.effList, tr.pos});
                applyPayload(sim, e.payload, ctx);
            }
        }
        if (!w.alive(a)) continue;
        if (tick >= w.get<Area>(a).endAt && tick >= w.get<Area>(a).fireAt) {
            sim.emit({kEvAreaEnd, owner, a, kNullEntity, w.get<Area>(a).effList, w.get<Transform>(a).pos});
            w.destroyLater(a);
        }
    }
}

// ================================================================= projectiles

void RtsModule::impactProjectile(Simulation& sim, Entity p) {
    World& w = sim.world();
    Projectile pr = w.get<Projectile>(p);
    const Vec3 at = flat(w.get<Transform>(p).pos);
    w.destroyLater(p);
    if (pr.effList < 0) return;
    const EffectDef& e = rules_.payloads[size_t(pr.effList)][size_t(pr.effIndex)];
    EffectCtx ctx;
    ctx.caster = pr.source; ctx.owner = pr.owner; ctx.sourceType = pr.sourceType; ctx.point = at; ctx.origin = pr.start; ctx.scale = pr.scale;
    ctx.target = w.alive(pr.target) ? pr.target : kNullEntity;
    sim.emit({kEvImpact, pr.owner, ctx.target, pr.source, e.payload, at});
    applyPayload(sim, e.payload, ctx);
}

// ================================================================= the system

void RtsModule::sysEffects(Simulation& sim) {
    World& w = sim.world();
    const uint32_t tick = sim.tick();
    tickStatuses(sim);
    tickAreas(sim);
    // channels: repeat the payload while the caster keeps still and isn't stunned or silenced
    std::vector<Entity> chans;
    w.each<Channel>([&](Entity e, Channel&) { chans.push_back(e); });
    for (Entity e : chans) {
        if (!w.alive(e)) continue;
        Channel ch = w.get<Channel>(e);
        bool stop = tick >= ch.endAt || (statusFlags(sim, e) & (kStStun | kStSilence)) || dist2D(w.get<Transform>(e).pos, ch.startPos) > Fixed::fromRatio(3, 10) ||
                    w.get<Unit>(e).hp.raw <= 0 || (ch.target.valid() && !w.alive(ch.target));
        if (stop) {
            w.remove<Channel>(e);
            sim.emit({kEvChannelEnd, w.get<Owner>(e).player, e, kNullEntity, ch.ability, w.get<Transform>(e).pos});
            continue;
        }
        if (tick < ch.nextAt) continue;
        w.get<Channel>(e).nextAt = tick + std::max<uint32_t>(1, fxTicks(ch.interval, sim.tickRate()));
        EffectCtx ctx;
        ctx.caster = e; ctx.owner = w.get<Owner>(e).player; ctx.sourceType = w.get<Unit>(e).type; ctx.target = ch.target;
        ctx.point = w.alive(ch.target) ? w.get<Transform>(ch.target).pos : ch.point; ctx.origin = w.get<Transform>(e).pos;
        applyPayload(sim, ch.payload, ctx);
    }
    tickPlayerSkills(sim);
}

void RtsModule::hashEffects(Simulation& sim, Hasher& h) {
    World& w = sim.world();
    GameState& gs = state(sim);
    w.each<Statuses>([&](Entity e, Statuses& st) {
        if (st.list.empty()) return;
        h.u64(e.packed());
        for (auto& in : st.list) { h.u32(in.def); h.u32(in.until); h.u32(in.stacks); h.fx(in.shield); h.u32(in.owner); h.fx(in.acc); }
    });
    w.each<Area, Transform>([&](Entity e, Area& a, Transform& t) { h.u64(e.packed()); h.v3(t.pos); h.u32(a.nextAt); h.u32(a.endAt); h.i32(a.triggers); });
    w.each<Channel>([&](Entity e, Channel& c) { h.u64(e.packed()); h.u32(c.nextAt); h.u32(c.endAt); });
    w.each<GroupTag>([&](Entity e, GroupTag& g) { h.u64(e.packed()); h.u32(g.id); });
    for (auto& p : gs.players) for (auto& in : p.statuses) { h.u32(in.def); h.u32(in.until); h.u32(in.stacks); }
    for (auto& pl : gs.playerAbilities) for (auto& st : pl) { h.u32(st.owned); h.u32(st.level); h.u32(st.pendingAt); h.u32(st.stolenUntil); h.u32(st.nextAuto); }
    h.u32(gs.nextGroupId);
}

} // namespace rts::game
