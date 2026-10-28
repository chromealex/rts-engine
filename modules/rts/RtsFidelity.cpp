// Foundations, traps, group teleports and charge effects:
//   foundation  concrete slabs pave cells for their owner and go away; a structure not standing on its owner's
//               concrete starts damaged and weathers down to a floor, scaled by the share of bare cells (Dune II)
//   traps       hidden structures that fire a payload at enemies stepping near, with charges and a rearm time
//               (Dungeon Keeper, Stronghold, They Are Billions); visible for a moment after firing
//   teleport    teleportGroup moves the units around one end (point / caster) to the other (point / caster /
//               the second point of a pointPair player ability), keeping their formation; units with killTags
//               die instead, and with a duration they come back (Chronosphere, Mass Teleport, Strategic Recall)
//   charges     addCharges gives a structure larva charges beyond its cap (inject larvae)
// Everything runs only when the rules use one of these blocks (Rules::fidelity), so other games keep their
// exact checksums.
#include "RtsInternal.h"
#include <algorithm>

namespace rts::game {
using namespace detail;

namespace {
uint32_t ticksFor(Fixed s, uint32_t rate) {
    const int64_t t = (s * int64_t(rate)).toInt();
    return t > 0 ? uint32_t(t) : 0;
}
} // namespace

// ================================================================= foundations

int RtsModule::bareCells(Simulation& sim, uint8_t player, int x0, int y0, int fw, int fh) const {
    const MapGrid& g = sim.world().resource<GameState>()->grid;
    int bare = 0;
    for (int y = y0; y < y0 + fh; ++y)
        for (int x = x0; x < x0 + fw; ++x)
            if (!g.inside(x, y) || g.foundation.size() != g.flags.size() || g.foundation[size_t(g.index(x, y))] != player) ++bare;
    return bare;
}

namespace {
// Concrete under a structure: hp cap of a finished structure with this bare share.
Fixed bareCap(const FoundationRules& fr, Fixed maxHp, Fixed bare) {
    return maxHp * (Fixed::fromInt(1) - bare * (Fixed::fromInt(1) - fr.startHp));
}
} // namespace

void RtsModule::spawnFidelity(Simulation& sim, Entity e, bool underConstruction) {
    World& w = sim.world();
    GameState& gs = state(sim);
    MapGrid& g = gs.grid;
    Unit& u = w.get<Unit>(e);
    const UnitType& t = rules_.types[u.type];
    const uint8_t owner = w.get<Owner>(e).player;
    if (t.trap >= 0) {
        const TrapDef& d = rules_.traps[size_t(t.trap)];
        w.add<TrapState>(e, TrapState{d.charges, sim.tick() + ticksFor(d.armTime, sim.tickRate())});
        if (d.hidden && rules_.stTrapHidden >= 0) {
            EffectCtx ctx; ctx.owner = owner; ctx.caster = e; ctx.sourceType = u.type;
            addStatus(sim, e, rules_.stTrapHidden, Fixed{}, ctx);
        }
    }
    if (!rules_.foundation.enabled || gs.surface) return;   // flat maps only
    if (g.foundation.size() != g.flags.size()) g.foundation.assign(g.flags.size(), 0);
    auto* f = w.tryGet<Footprint>(e);
    if (!f) return;
    if (t.foundation) {
        if (underConstruction) return;
        builtFidelity(sim, e);   // paves at once
        return;
    }
    if (owner == 0 || owner == kHostileNeutral || !t.weathers) return;
    if (sim.tick() == 0) {   // the starting base of a map stands on concrete
        for (int y = f->y; y < f->y + f->h; ++y)
            for (int x = f->x; x < f->x + f->w; ++x)
                if (g.inside(x, y)) g.foundation[size_t(g.index(x, y))] = owner;
        return;
    }
    const int bare = bareCells(sim, owner, f->x, f->y, f->w, f->h);
    if (bare == 0) return;
    const Fixed share = Fixed::fromInt(bare) / Fixed::fromInt(std::max(1, f->w * f->h));
    w.add<Weathering>(e, Weathering{share});
    if (!underConstruction) u.hp = fxMin(u.hp, bareCap(rules_.foundation, t.maxHp, share));
}

void RtsModule::builtFidelity(Simulation& sim, Entity e) {
    World& w = sim.world();
    GameState& gs = state(sim);
    MapGrid& g = gs.grid;
    Unit& u = w.get<Unit>(e);
    const UnitType& t = rules_.types[u.type];
    if (t.trap >= 0) if (auto* ts = w.tryGet<TrapState>(e)) ts->readyAt = sim.tick() + ticksFor(rules_.traps[size_t(t.trap)].armTime, sim.tickRate());
    if (!rules_.foundation.enabled || gs.surface) return;
    if (t.foundation) {
        auto* f = w.tryGet<Footprint>(e);
        const uint8_t owner = w.get<Owner>(e).player;
        if (!f || owner == 0) return;
        if (g.foundation.size() != g.flags.size()) g.foundation.assign(g.flags.size(), 0);
        int n = 0;
        for (int y = f->y; y < f->y + f->h; ++y)
            for (int x = f->x; x < f->x + f->w; ++x)
                if (g.inside(x, y) && g.foundation[size_t(g.index(x, y))] != owner) { g.foundation[size_t(g.index(x, y))] = owner; ++n; }
        u.hp = Fixed{};
        u.flags |= kUnitSilent;   // the slab is now ground: removed without a death
        sim.emit({kEvPaved, owner, e, kNullEntity, n, w.get<Transform>(e).pos});
        return;
    }
    if (auto* wt = w.tryGet<Weathering>(e)) u.hp = fxMin(u.hp, bareCap(rules_.foundation, t.maxHp, wt->bare));
}

void RtsModule::deathFidelity(Simulation& sim, Entity e) {
    World& w = sim.world();
    GameState& gs = state(sim);
    MapGrid& g = gs.grid;
    if (!rules_.foundation.enabled || !rules_.foundation.lostWithBuilding || g.foundation.size() != g.flags.size()) return;
    const UnitType& t = rules_.types[w.get<Unit>(e).type];
    auto* f = w.tryGet<Footprint>(e);
    if (!f || t.foundation || t.resourceNode >= 0) return;
    for (int y = f->y; y < f->y + f->h; ++y)
        for (int x = f->x; x < f->x + f->w; ++x)
            if (g.inside(x, y)) g.foundation[size_t(g.index(x, y))] = 0;
}

Fixed RtsModule::spotScoreFidelity(Simulation& sim, uint8_t player, uint16_t type, int x0, int y0, Vec2 d, Vec2 away) {
    const UnitType& t = rules_.types[type];
    const MapGrid& g = state(sim).grid;
    if (t.trap >= 0) {
        // traps go to the front of the base (toward the enemy), not next to another own trap
        Fixed s = d.dot(away) * Fixed::fromRatio(3, 2);
        const Vec3 c = g.cellCenter(x0, y0);
        bool crowded = false;
        sim.world().each<TrapState, Owner, Transform>([&](Entity, TrapState&, Owner& o, Transform& tr) {
            if (o.player == player && dist2D(tr.pos, c) < Fixed::fromInt(3)) crowded = true;
        });
        return crowded ? s + Fixed::fromInt(8) : s;
    }
    if (!rules_.foundation.enabled || g.foundation.size() != g.flags.size()) return Fixed{};
    const int bare = bareCells(sim, player, x0, y0, t.footprintW, t.footprintH);
    if (t.foundation) {
        if (bare == 0) return Fixed::fromInt(1 << 19);   // already paved
        int adj = 0;   // grow pads: next to own concrete
        for (int y = y0 - 1; y <= y0 + t.footprintH; ++y)
            for (int x = x0 - 1; x <= x0 + t.footprintW; ++x)
                if (g.inside(x, y) && (x < x0 || y < y0 || x >= x0 + t.footprintW || y >= y0 + t.footprintH) &&
                    g.foundation[size_t(g.index(x, y))] == player && !g.blocked[size_t(g.index(x, y))]) ++adj;
        return -Fixed::fromInt(adj) / 2;
    }
    if (!t.weathers) return Fixed{};
    return Fixed::fromInt(bare * 3);   // concrete pads first
}

// ================================================================= effects

void RtsModule::takePoint2(Simulation& sim, uint8_t player, int64_t param) {
    GameState& gs = state(sim);
    if (player >= gs.players.size() || !((param >> 48) & 1)) return;
    const int64_t x = (param >> 16) & 0xFFFF, y = (param >> 32) & 0xFFFF;
    gs.players[player].point2 = {Fixed::fromRatio(x, 16), Fixed{}, Fixed::fromRatio(y, 16)};
}

int64_t RtsModule::packPoint2(Vec3 p) {
    const int64_t x = std::clamp<int64_t>((p.x * 16).toInt(), 0, 0xFFFF), y = std::clamp<int64_t>((p.z * 16).toInt(), 0, 0xFFFF);
    return (x << 16) | (y << 32) | (int64_t(1) << 48);
}

void RtsModule::effectFidelity(Simulation& sim, const EffectDef& e, EffectCtx& ctx) {
    World& w = sim.world();
    GameState& gs = state(sim);
    if (e.kind == EffectKind::AddCharges) {
        for (Entity h : effectTargets(sim, e, ctx)) {
            auto* p = w.tryGet<Production>(h);
            const UnitType& t = rules_.types[w.get<Unit>(h).type];
            if (!p || t.chargesMax <= 0) continue;
            const int cap = e.limit > 0 ? e.limit : t.chargesMax;
            p->charges = std::max(p->charges, std::min(cap, p->charges + int(e.amount.toInt())));
        }
        return;
    }
    // TeleportGroup
    if (gs.surface) return;   // flat maps only
    const Vec3 casterPos = w.alive(ctx.caster) && w.has<Transform>(ctx.caster) ? flat(w.get<Transform>(ctx.caster).pos) : ctx.origin;
    const Vec3 p2 = ctx.owner < gs.players.size() ? gs.players[ctx.owner].point2 : ctx.point;
    auto endOf = [&](uint8_t k) { return k == 1 ? casterPos : k == 2 ? p2 : flat(ctx.point); };
    const Vec3 from = endOf(e.tpFrom), to = endOf(e.tpTo);
    EffectDef q = e;
    q.filter.shape = Shape::Circle;
    q.fromCaster = false;
    EffectCtx c2 = ctx;
    c2.point = from;
    std::vector<Entity> who;
    for (Entity h : effectTargets(sim, q, c2))
        if (w.has<Mobile>(h) && !rules_.types[w.get<Unit>(h).type].structure && !(w.get<Unit>(h).flags & (kUnitInHand | kUnitInTransit))) who.push_back(h);
    if (who.empty()) return;
    Vec3 centre{};
    for (Entity h : who) centre = centre + flat(w.get<Transform>(h).pos);
    centre = centre / Fixed::fromInt(int64_t(who.size()));
    const uint32_t back = e.duration.raw > 0 ? sim.tick() + std::max<uint32_t>(1, ticksFor(e.duration, sim.tickRate())) : 0;
    std::vector<std::pair<Entity, Vec3>> moved;
    for (Entity h : who) {
        Unit& hu = w.get<Unit>(h);
        const UnitType& ht = rules_.types[hu.type];
        if (e.killTags && (ht.tags & e.killTags)) { hu.hp = Fixed{}; hu.lastAttacker = 0; continue; }
        Transform& tr = w.get<Transform>(h);
        Vec3 off = flat(tr.pos) - centre;
        const Fixed ol = off.length();
        if (ol > e.radius && ol.raw > 0) off = off * (e.radius / ol);
        Vec3 p = to + off;
        int cx, cy; gs.grid.cellXY(p, cx, cy);
        if (!(ht.layer & kLayerAir) && (!gs.grid.inside(cx, cy) || !gs.grid.passable(cx, cy, ht.layer) || gs.grid.blocked[size_t(gs.grid.index(cx, cy))])) {
            if (!gs.grid.inside(cx, cy)) { cx = std::clamp(cx, 0, gs.grid.w - 1); cy = std::clamp(cy, 0, gs.grid.h - 1); }
            if (!gs.grid.nearestPassable(cx, cy, ht.layer, 6)) continue;
            p = gs.grid.cellCenter(cx, cy);
        }
        moved.push_back({h, flat(tr.pos)});
        if (gs.grid.hf) p.y = gs.grid.surfaceHeight(p.x, p.z);
        tr.pos = p; tr.prevPos = p;
        stopMoving(sim, h);
        if (auto* ord = w.tryGet<Orders>(h)) ord->queue.clear();
        if (auto* cb = w.tryGet<Combat>(h)) cb->target = kNullEntity;
        sim.emit({kEvGroupTeleport, w.get<Owner>(h).player, h, ctx.caster, 0, p});
    }
    if (back)
        for (auto& [h, home] : moved) {
            if (auto* tb = w.tryGet<TeleportBack>(h)) { tb->at = back; continue; }   // the first home stays
            w.add<TeleportBack>(h, TeleportBack{home, back, ctx.caster});
        }
}

bool RtsModule::pairTarget(Simulation& sim, uint8_t player, int payload, Vec3& from, Vec3& to) {
    World& w = sim.world();
    const EffectDef* tp = nullptr;
    if (payload >= 0 && size_t(payload) < rules_.payloads.size())
        for (auto& e : rules_.payloads[size_t(payload)]) if (e.kind == EffectKind::TeleportGroup && e.tpFrom == 0 && e.tpTo == 2) { tp = &e; break; }
    if (!tp) return false;
    // the own group with most units the effect takes, not already near an enemy structure
    std::vector<std::pair<Entity, Vec3>> mine;
    w.each<Unit, Owner, Transform, Mobile>([&](Entity c, Unit& u, Owner& o, Transform& t, Mobile&) {
        if (o.player != player || rules_.types[u.type].worker || rules_.types[u.type].weapons.empty()) return;
        if (!filterOk(sim, tp->filter, c, player, kNullEntity)) return;
        if (tp->killTags && (rules_.types[u.type].tags & tp->killTags)) return;
        if (mine.size() < 150) mine.push_back({c, flat(t.pos)});
    });
    if (mine.empty()) return false;
    size_t best = 0; int bestN = -1;
    for (size_t i = 0; i < mine.size(); ++i) {
        int n = 0;
        for (auto& [c, p] : mine) if (dist2D(p, mine[i].second) <= tp->radius) ++n;
        if (n > bestN) { bestN = n; best = i; }
    }
    from = mine[best].second;
    // the nearest enemy structure; land a few cells short of it, toward the group
    Fixed bd{}; bool found = false; Vec3 target{};
    w.each<Unit, Owner, Transform>([&](Entity, Unit& u, Owner& o, Transform& t) {
        const UnitType& ut = rules_.types[u.type];
        if (!ut.structure || ut.resourceNode >= 0 || o.player == 0 || o.player == kHostileNeutral || !hostile(sim, player, o.player) || u.hp.raw <= 0) return;
        Fixed d = dist2D(t.pos, from);
        if (!found || d < bd) { bd = d; target = flat(t.pos); found = true; }
    });
    if (!found || bd < Fixed::fromInt(12)) return false;   // already there
    Vec3 dir = from - target;
    const Fixed dl = dir.length();
    to = dl.raw > 0 ? target + dir * (Fixed::fromInt(5) / dl) : target;
    return true;
}

// ================================================================= system

void RtsModule::sysFidelity(Simulation& sim) {
    World& w = sim.world();
    GameState& gs = state(sim);
    const uint32_t tick = sim.tick();
    // weathering: structures off their foundation lose hp down to the floor
    if (rules_.foundation.enabled && tick % 10 == 0) {
        const FoundationRules& fr = rules_.foundation;
        const Fixed step = sim.dt() * 10 / 60;   // minutes since the last pass
        w.each<Weathering, Unit>([&](Entity e, Weathering& wt, Unit& u) {
            if ((u.flags & kUnitUnderConstruction) || u.hp.raw <= 0 || wt.bare.raw <= 0) return;
            const Fixed maxHp = stat(sim, e, Stat::MaxHp);
            const Fixed floorHp = maxHp * (kOne - wt.bare * (kOne - fr.floor));
            if (u.hp <= floorHp) return;
            u.hp = fxMax(floorHp, u.hp - maxHp * fr.decay * wt.bare * step);
        });
    }
    // traps: the first enemy within reach sets one off
    if (!rules_.traps.empty() && tick % 2 == 0) {
        struct Fire { Entity trap, victim; };
        std::vector<Fire> fires;
        w.each<TrapState, Unit, Owner, Transform>([&](Entity e, TrapState& ts, Unit& u, Owner& o, Transform& tr) {
            if ((u.flags & kUnitUnderConstruction) || u.hp.raw <= 0 || tick < ts.readyAt) return;
            const TrapDef& d = rules_.traps[size_t(rules_.types[u.type].trap)];
            if (d.charges > 0 && ts.charges <= 0) return;
            Entity best; Fixed bd{};
            gs.hash.query(tr.pos, d.radius + kOne, [&](Entity c) {
                if (c == e || !w.has<Transform>(c)) return;
                const Fixed dd = dist2D(w.get<Transform>(c).pos, tr.pos);
                if (dd > d.radius || !filterOk(sim, d.filter, c, o.player, e)) return;
                if (!best.valid() || dd < bd || (dd == bd && c.index < best.index)) { best = c; bd = dd; }
            });
            if (best.valid()) fires.push_back({e, best});
        });
        for (auto& f : fires) {
            if (!w.alive(f.trap) || !w.alive(f.victim)) continue;
            Unit& u = w.get<Unit>(f.trap);
            const TrapDef& d = rules_.traps[size_t(rules_.types[u.type].trap)];
            TrapState& ts = w.get<TrapState>(f.trap);
            const uint8_t owner = w.get<Owner>(f.trap).player;
            const Vec3 vp = w.get<Transform>(f.victim).pos;
            EffectCtx ctx;
            ctx.caster = f.trap; ctx.owner = owner; ctx.sourceType = u.type; ctx.target = f.victim;
            ctx.point = flat(vp); ctx.origin = flat(w.get<Transform>(f.trap).pos);
            sim.emit({kEvTrapFired, owner, f.trap, f.victim, u.type, vp});
            applyPayload(sim, d.payload, ctx);
            if (!w.alive(f.trap)) continue;
            ts.readyAt = tick + std::max<uint32_t>(1, ticksFor(d.rearm, sim.tickRate()));
            if (d.charges > 0) --ts.charges;
            if (d.hidden && rules_.stTrapSeen >= 0 && d.revealFor.raw > 0) addStatus(sim, f.trap, rules_.stTrapSeen, d.revealFor, ctx);
            if (d.charges > 0 && ts.charges <= 0 && d.removeWhenEmpty) { u.hp = Fixed{}; u.flags |= kUnitSilent; }
        }
    }
    // group teleports with a return trip
    std::vector<Entity> due;
    w.each<TeleportBack, Unit>([&](Entity e, TeleportBack& tb, Unit& u) { if (tick >= tb.at && u.hp.raw > 0) due.push_back(e); });
    for (Entity e : due) {
        TeleportBack tb = w.get<TeleportBack>(e);
        w.remove<TeleportBack>(e);
        const UnitType& ht = rules_.types[w.get<Unit>(e).type];
        int cx, cy; gs.grid.cellXY(tb.home, cx, cy);
        Vec3 p = tb.home;
        if (!(ht.layer & kLayerAir) && (!gs.grid.passable(cx, cy, ht.layer) || gs.grid.blocked[size_t(gs.grid.index(cx, cy))])) {
            if (!gs.grid.nearestPassable(cx, cy, ht.layer, 6)) continue;
            p = gs.grid.cellCenter(cx, cy);
        }
        if (gs.grid.hf) p.y = gs.grid.surfaceHeight(p.x, p.z);
        Transform& tr = w.get<Transform>(e);
        tr.pos = p; tr.prevPos = p;
        stopMoving(sim, e);
        if (auto* ord = w.tryGet<Orders>(e)) ord->queue.clear();
        if (auto* cb = w.tryGet<Combat>(e)) cb->target = kNullEntity;
        sim.emit({kEvGroupTeleport, w.get<Owner>(e).player, e, tb.caster, 1, p});
    }
}

void RtsModule::hashFidelity(Simulation& sim, Hasher& h) {
    World& w = sim.world();
    GameState& gs = state(sim);
    const MapGrid& g = gs.grid;
    if (!g.foundation.empty()) {
        uint32_t acc = 0, n = 0;
        for (size_t i = 0; i < g.foundation.size(); ++i) if (g.foundation[i]) { acc = acc * 31u + uint32_t(i) * 7u + g.foundation[i]; ++n; }
        h.u32(n); h.u32(acc);
    }
    for (auto& p : gs.players) h.v3(p.point2);
    w.each<Weathering>([&](Entity e, Weathering& wt) { h.u64(e.packed()); h.fx(wt.bare); });
    w.each<TrapState>([&](Entity e, TrapState& ts) { h.u64(e.packed()); h.i32(ts.charges); h.u32(ts.readyAt); });
    w.each<TeleportBack>([&](Entity e, TeleportBack& tb) { h.u64(e.packed()); h.v3(tb.home); h.u32(tb.at); });
}

} // namespace rts::game
