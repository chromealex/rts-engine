// agent-mech stage 2, world: gates (own side walks through, enemies are stopped and attack them), bridges
// (walkable ground over water while they stand), hazards (fields / areas that hurt), cycles (day/night,
// seasons: a player status per phase), ammo and docks (aircraft rearm), tunnel networks (nydus).
#include "RtsInternal.h"
#include <algorithm>

namespace rts::game {
using namespace detail;

namespace {
bool friendOf(Simulation& sim, uint8_t owner, uint8_t other, uint8_t gate) {
    if (other == owner) return true;
    return gate == 2 && owner != 0 && other != 0 && owner != kHostileNeutral && other != kHostileNeutral && sim.allied(owner, other);
}
} // namespace

void RtsModule::mechTickWorld(Simulation& sim) {
    World& w = sim.world();
    GameState& gs = state(sim);
    MapGrid& g = gs.grid;
    const MechRules& M = rules_.mech;

    // ---- gates and bridges: open their footprint once complete
    if (M.gates || M.bridges) {
        std::vector<Entity> open;
        w.each<Unit, Footprint>([&](Entity e, Unit& u, Footprint&) {
            const MechType& mt = rules_.types[u.type].mech;
            if ((mt.gate || mt.bridge) && !(u.flags & kUnitUnderConstruction) && u.hp.raw > 0 && !w.has<BridgeLaid>(e)) open.push_back(e);
        });
        for (Entity e : open) {
            const Footprint f = w.get<Footprint>(e);
            BridgeLaid bl;
            bl.laid = true;
            g.block(f.x, f.y, f.w, f.h, -1);
            if (rules_.types[w.get<Unit>(e).type].mech.bridge)
                for (int y = f.y; y < f.y + f.h; ++y)
                    for (int x = f.x; x < f.x + f.w; ++x) {
                        if (!g.inside(x, y)) continue;
                        uint8_t& fl = g.flags[size_t(g.index(x, y))];
                        if (!(fl & kCellGround)) { fl |= kCellGround; bl.cells.push_back(g.index(x, y)); }
                    }
            for (int y = f.y; y < f.y + f.h; ++y) for (int x = f.x; x < f.x + f.w; ++x) if (g.inside(x, y)) g.cellChanged(x, y);
            w.add<BridgeLaid>(e, std::move(bl));
        }
        // enemies can't walk through gates: back out and attack the gate
        if (M.gates) {
            struct Push { Entity u, gate; };
            std::vector<Push> push;
            w.each<Unit, Footprint, BridgeLaid>([&](Entity e, Unit& u, Footprint& f, BridgeLaid& bl) {
                const uint8_t gate = rules_.types[u.type].mech.gate;
                const bool locked = !bl.laid;
                if (!gate) return;
                const uint8_t owner = w.get<Owner>(e).player;
                const Vec3 c = w.get<Transform>(e).pos;
                gs.hash.query(c, Fixed::fromInt(std::max(f.w, f.h)), [&](Entity x) {
                    if (x == e || !w.has<Mobile>(x) || !w.has<Unit>(x)) return;
                    if (!(rules_.types[w.get<Unit>(x).type].layer & kLayerGround) || (!locked && friendOf(sim, owner, w.get<Owner>(x).player, gate))) return;
                    const Vec3 p = w.get<Transform>(x).pos;
                    const int cx = g.cellX(p.x), cy = g.cellY(p.z);
                    if (cx >= f.x && cy >= f.y && cx < f.x + f.w && cy < f.y + f.h) push.push_back({x, e});
                });
            });
            for (auto& p : push) {
                Transform& t = w.get<Transform>(p.u);
                const Footprint& f = w.get<Footprint>(p.gate);
                Vec3 back = t.prevPos;
                const int bx = g.cellX(back.x), by = g.cellY(back.z);
                if (bx >= f.x && by >= f.y && bx < f.x + f.w && by < f.y + f.h) {
                    int nx = bx, ny = by;
                    // spawned inside: step out to the nearest side
                    int dl = bx - f.x + 1, dr = f.x + f.w - bx, du = by - f.y + 1, dd = f.y + f.h - by;
                    int mn = std::min({dl, dr, du, dd});
                    if (mn == dl) nx = f.x - 1; else if (mn == dr) nx = f.x + f.w; else if (mn == du) ny = f.y - 1; else ny = f.y + f.h;
                    back = g.cellCenter(nx, ny);
                }
                t.pos = back;
                stopMoving(sim, p.u);
                if (auto* c = w.tryGet<Combat>(p.u); c && !rules_.types[w.get<Unit>(p.u).type].weapons.empty() && hostile(sim, w.get<Owner>(p.u).player, w.get<Owner>(p.gate).player)) { c->target = p.gate; c->autoTarget = true; }
            }
        }
    }

    // ---- hazards (twice a second)
    if (!M.hazards.empty() && sim.tick() % 10 == 0) {
        struct Hit { Entity e; const HazardDef* h; };
        std::vector<Hit> hits;
        w.each<Unit, Transform>([&](Entity e, Unit& u, Transform& t) {
            if (u.hp.raw <= 0 || (u.flags & kUnitOffMap)) return;
            const UnitType& ut = rules_.types[u.type];
            if (ut.structure) return;
            const int cx = g.cellX(t.pos.x), cy = g.cellY(t.pos.z);
            if (!g.inside(cx, cy)) return;
            const size_t ci = size_t(g.index(cx, cy));
            for (auto& h : M.hazards) {
                if (!(ut.layer & h.layers) || (h.tags && !(ut.tags & h.tags))) continue;
                bool in = false;
                if (h.field >= 0) in = g.fieldRes[ci] == h.field && g.fieldAmt[ci].raw > 0;
                else { int a = triggers_.findArea(h.area); in = a >= 0 && triggers_.areas[size_t(a)].contains(t.pos); }
                if (in) hits.push_back({e, &h});
            }
        });
        for (auto& hit : hits) {
            if (!w.alive(hit.e)) continue;
            if (hit.h->damage.raw > 0) dealDamage(sim, hit.e, hit.h->damage / 2, 0, kNullEntity, 0);
            if (hit.h->status >= 0 && rules_.advancedEffects) {
                EffectCtx ctx; ctx.owner = 0; ctx.target = hit.e; ctx.point = ctx.origin = w.get<Transform>(hit.e).pos;
                addStatus(sim, hit.e, hit.h->status, Fixed::fromInt(1), ctx);
            }
        }
    }

    // ---- cycles: day / night, seasons
    if (!M.cycles.empty()) {
        if (gs.mech.cyclePhase.size() != M.cycles.size()) gs.mech.cyclePhase.assign(M.cycles.size(), -1);
        const Fixed now = Fixed::fromInt(sim.tick()) / int64_t(sim.tickRate());
        for (size_t ci = 0; ci < M.cycles.size(); ++ci) {
            const CycleDef& c = M.cycles[ci];
            Fixed t = now + c.offset;
            Fixed frac = (t - c.length * (t / c.length).floorInt()) / c.length;
            int ph = 0;
            while (ph + 1 < int(c.phases.size()) && frac >= c.phases[size_t(ph)].until) ++ph;
            int& cur = gs.mech.cyclePhase[ci];
            if (ph == cur) continue;
            const int old = cur;
            cur = ph;
            for (auto& p : sim.players()) {
                if (p.slot >= gs.players.size()) continue;
                if (old >= 0) {
                    int16_t os = c.phases[size_t(old)].status;
                    for (auto& in : gs.players[p.slot].statuses) if (os >= 0 && in.def == uint16_t(os)) in.until = sim.tick();
                }
                int16_t ns = c.phases[size_t(ph)].status;
                if (ns >= 0 && rules_.advancedEffects) { EffectCtx ctx; ctx.owner = p.slot; addPlayerStatus(sim, p.slot, ns, Fixed::fromInt(1000000), ctx); }
            }
            gs.trig.vars["cycle." + c.id] = Fixed::fromInt(ph);
            sim.emit({kEvCyclePhase, 0, kNullEntity, kNullEntity, int64_t(ci) << 8 | ph, {}});
        }
    }

    // ---- ammo: give it to new units, send empty ones to a dock
    if (M.ammo) {
        std::vector<Entity> add, needDock;
        w.each<Unit>([&](Entity e, Unit& u) {
            const MechType& mt = rules_.types[u.type].mech;
            if (mt.ammo.empty() || u.hp.raw <= 0) return;
            auto* am = w.tryGet<Ammo>(e);
            if (!am) { add.push_back(e); return; }
            bool empty = true;
            for (size_t i = 0; i < mt.ammo.size() && i < 4; ++i) if (mt.ammo[i] > 0 && am->left[i] > 0) empty = false;
            auto* ord = w.tryGet<Orders>(e);
            if (empty && ord && (ord->queue.empty() || ord->queue.front().type != OrderType::Rearm)) needDock.push_back(e);
        });
        for (Entity e : add) {
            Ammo a;
            const MechType& mt = rules_.types[w.get<Unit>(e).type].mech;
            for (size_t i = 0; i < mt.ammo.size() && i < 4; ++i) a.left[i] = mt.ammo[i];
            a.init = true;
            w.add<Ammo>(e, a);
        }
        for (Entity e : needDock) {
            const uint8_t owner = w.get<Owner>(e).player;
            const Vec3 p = w.get<Transform>(e).pos;
            Entity best; Fixed bd{};
            w.each<Unit, Owner, Transform>([&](Entity d, Unit& du, Owner& o, Transform& t) {
                if (o.player != owner || du.hp.raw <= 0 || (du.flags & kUnitUnderConstruction) || rules_.types[du.type].mech.dockSlots == 0) return;
                Fixed dd = dist2D(t.pos, p);
                if (!best.valid() || dd < bd) { best = d; bd = dd; }
            });
            if (!best.valid()) continue;
            Order o;
            o.type = OrderType::Rearm;
            o.target = best;
            issue(sim, e, o, false);
        }
    }
}

bool RtsModule::mechUseAmmo(Simulation& sim, Entity e, size_t weapon) {
    World& w = sim.world();
    const MechType& mt = rules_.types[w.get<Unit>(e).type].mech;
    if (weapon >= mt.ammo.size() || mt.ammo[weapon] <= 0) return true;
    auto* am = w.tryGet<Ammo>(e);
    if (!am || weapon >= 4) return true;
    if (am->left[weapon] <= 0) return false;
    --am->left[weapon];
    return true;
}

bool RtsModule::runRearm(Simulation& sim, Entity e, Order& o) {
    World& w = sim.world();
    if (!w.alive(o.target) || !w.has<Unit>(o.target)) return true;
    auto* am = w.tryGet<Ammo>(e);
    if (!am) return true;
    const MechType& mt = rules_.types[w.get<Unit>(e).type].mech;
    const MechType& dt = rules_.types[w.get<Unit>(o.target).type].mech;
    auto* m = w.tryGet<Mobile>(e);
    if (o.state < 2) {
        if (contactDistance(sim, e, o.target) > Fixed::fromRatio(3, 2)) {
            if (m && (!m->moving || o.state == 0)) moveTo(sim, e, w.get<Transform>(o.target).pos, kOne);
            o.state = 1;
            return false;
        }
        // a free place at the dock?
        int busy = 0;
        w.each<Orders>([&](Entity x, Orders& od) {
            if (x != e && !od.queue.empty() && od.queue.front().type == OrderType::Rearm && od.queue.front().target == o.target && od.queue.front().state == 2) ++busy;
        });
        if (busy >= dt.dockSlots) { if (m && m->moving) stopMoving(sim, e); return false; }
        if (m) stopMoving(sim, e);
        o.state = 2;
        o.timer = dt.rearmTime;
    }
    o.timer -= sim.dt();
    if (o.timer.raw > 0) return false;
    o.timer = dt.rearmTime;
    bool full = true;
    for (size_t i = 0; i < mt.ammo.size() && i < 4; ++i)
        if (mt.ammo[i] > 0 && am->left[i] < mt.ammo[i]) { ++am->left[i]; full = full && am->left[i] >= mt.ammo[i]; }
    for (size_t i = 0; i < mt.ammo.size() && i < 4; ++i) if (mt.ammo[i] > 0 && am->left[i] < mt.ammo[i]) full = false;
    return full;
}

void RtsModule::mechBridgeDied(Simulation& sim, Entity e) {
    World& w = sim.world();
    GameState& gs = state(sim);
    MapGrid& g = gs.grid;
    auto* bl = w.tryGet<BridgeLaid>(e);
    auto* f = w.tryGet<Footprint>(e);
    if (!bl || !f) return;
    if (bl->laid) g.block(f->x, f->y, f->w, f->h, +1);   // sysDeath unblocks the footprint once more: keep the count right (a locked gate is blocked already)
    for (int32_t ci : bl->cells) g.flags[size_t(ci)] &= uint8_t(~kCellGround);
    for (int y = f->y; y < f->y + f->h; ++y) for (int x = f->x; x < f->x + f->w; ++x) if (g.inside(x, y)) g.cellChanged(x, y);
    // ground units on the vanished deck fall into the water
    for (int32_t ci : bl->cells) {
        const Vec3 c = g.cellCenter(ci % g.w, ci / g.w);
        gs.hash.query(c, Fixed::fromRatio(3, 4), [&](Entity x) {
            if (!w.has<Unit>(x) || !w.has<Mobile>(x)) return;
            const uint8_t layer = rules_.types[w.get<Unit>(x).type].layer;
            if ((layer & kLayerGround) && !(layer & (kLayerAir | kLayerNaval))) {
                const Vec3 p = w.get<Transform>(x).pos;
                if (g.cellX(p.x) == ci % g.w && g.cellY(p.z) == ci / g.w) w.get<Unit>(x).hp = Fixed{};
            }
        });
    }
}

void RtsModule::mechTunnelGather(Simulation& sim, Entity exit) {
    World& w = sim.world();
    const int16_t net = rules_.types[w.get<Unit>(exit).type].mech.tunnel;
    const uint8_t owner = w.get<Owner>(exit).player;
    if (!w.has<Transport>(exit)) w.add<Transport>(exit, Transport{});
    std::vector<Entity> others;
    w.each<Transport, Unit, Owner>([&](Entity e, Transport& tp, Unit& u, Owner& o) {
        if (e != exit && o.player == owner && rules_.types[u.type].mech.tunnel == net && !tp.cargo.empty()) others.push_back(e);
    });
    Transport& dst = w.get<Transport>(exit);
    const Vec3 at = w.get<Transform>(exit).pos;
    for (Entity o : others) {
        Transport& src = w.get<Transport>(o);
        for (Entity p : src.cargo) {
            if (!w.alive(p)) continue;
            dst.cargo.push_back(p);
            dst.used += rules_.types[w.get<Unit>(p).type].mech.cargoSize;
            if (auto* b = w.tryGet<Boarded>(p)) b->carrier = exit;
            w.get<Transform>(p).pos = w.get<Transform>(p).prevPos = at;
        }
        src.cargo.clear();
        src.used = 0;
    }
}

void RtsModule::hashMechGlobal(Simulation& sim, Hasher& h) {
    const MechState& m = state(sim).mech;
    if (rules_.mech.conveyors) hashLogistics(sim, h);
    if (rules_.mech.turns) { h.u32(0x7E5A); h.u32(m.turnPlayer); h.u32(m.turnIndex); h.u32(m.turnStart); }
    for (auto& p : m.prices) h.fx(p);
    h.u32(uint32_t(m.groundItems.size()));
    for (auto& gi : m.groundItems) { h.v3(gi.pos); h.u32(gi.item); h.i32(gi.charges); }
    for (auto& d : m.decks) for (int x : d) h.i32(x);
    for (auto& r : m.allyRequest) for (auto x : r) h.u32(x);
    for (int p : m.cyclePhase) h.i32(p);
    h.u32(m.nextGroup);
    for (auto& o : m.objectives) { h.str(o.id); h.u32(o.state); }
    h.i32(m.missionResult);
    h.u32(uint32_t(m.bounties.size())); h.u32(m.roundPhase); h.i32(m.roundIndex); h.u32(m.phaseEnd);
    for (size_t i = 0; i < m.cover.size(); i += 7) h.u32(m.cover[i]);
}

} // namespace rts::game
