// World rules (vision: RtsVision.cpp): creep, control points / flags / regions, tickets,
// neutral creeps and wanderers (waves and victory conditions: lua/waves.lua, lua/victory.lua).
#include "RtsInternal.h"
#include <algorithm>

namespace rts::game {
using namespace detail;


// ================================================================= creep

void RtsModule::sysCreep(Simulation& sim) {
    if (sim.tick() % 20 != 0) return;
    World& w = sim.world();
    GameState& gs = state(sim);
    MapGrid& g = gs.grid;
    bool any = false;
    for (auto& t : rules_.types) if (t.creepRadius > 0) any = true;
    if (!any) return;
    std::vector<uint8_t> next(g.creep.size(), 0);
    w.each<Unit, Transform>([&](Entity, Unit& u, Transform& tr) {
        const UnitType& t = rules_.types[u.type];
        if (t.creepRadius <= 0) return;
        // under construction: creep grows with progress
        int r = t.creepRadius;
        int cx, cy; g.cellXY(tr.pos, cx, cy);
        for (int y = cy - r; y <= cy + r; ++y)
            for (int x = cx - r; x <= cx + r; ++x)
                if (g.inside(x, y) && (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r && (g.flags[size_t(g.index(x, y))] & kCellGround))
                    next[size_t(g.index(x, y))] = 1;
    });
    if (next != g.creep) { g.creep.swap(next); gs.fieldsDirty = true; }
}

// ================================================================= control points, regions, tickets

void RtsModule::sysCapture(Simulation& sim) {
    World& w = sim.world();
    GameState& gs = state(sim);
    const Fixed dt = sim.dt();
    struct Flip { Entity point; uint8_t newOwner; uint8_t oldOwner; };
    std::vector<Flip> flips;
    w.each<Capture, Unit, Owner, Transform>([&](Entity e, Capture& cp, Unit& u, Owner& o, Transform& tr) {
        const UnitType& t = rules_.types[u.type];
        // who stands on the point?
        uint8_t presentTeam = 0, presentPlayer = 0;
        bool contested = false;
        gs.hash.query(tr.pos, t.captureRadius, [&](Entity s) {
            if (s == e || !w.has<Mobile>(s)) return;
            auto* su = w.tryGet<Unit>(s);
            if (!su || su->hp.raw <= 0) return;
            uint8_t so = w.get<Owner>(s).player;
            if (so == 0 || so == kHostileNeutral || dist2D(w.get<Transform>(s).pos, tr.pos) > t.captureRadius) return;
            uint8_t team = teamOf(sim, so);
            if (presentTeam == 0) { presentTeam = team; presentPlayer = so; }
            else if (presentTeam != team) contested = true;
            else if (so < presentPlayer) presentPlayer = so; // deterministic choice
        });
        if (contested || presentTeam == 0) return;
        const uint8_t ownerTeam = o.player != 0 ? teamOf(sim, o.player) : 0;
        Fixed step = dt / fxMax(t.captureTime, Fixed::fromRatio(1, 10));
        if (ownerTeam == presentTeam) { cp.progress = fxMin(kOne, cp.progress + step); cp.team = presentTeam; return; }
        if (o.player != 0) {
            // neutralise first
            cp.progress -= step;
            if (cp.progress.raw <= 0) { cp.progress = Fixed{}; flips.push_back({e, 0, o.player}); cp.team = 0; }
            return;
        }
        if (cp.team != presentTeam) { cp.team = presentTeam; cp.progress = Fixed{}; }
        cp.progress += step;
        if (cp.progress >= kOne) { cp.progress = kOne; flips.push_back({e, presentPlayer, 0}); }
    });
    for (auto& f : flips) {
        w.get<Owner>(f.point).player = f.newOwner;
        const Vec3 pos = w.get<Transform>(f.point).pos;
        sim.emit({kEvCaptured, f.newOwner, f.point, kNullEntity, f.oldOwner, pos});
        const UnitType& t = rules_.types[w.get<Unit>(f.point).type];
        if (!t.capturesRegion) continue;
        int ri = regionAt(sim, pos);
        if (ri < 0) continue;
        gs.regionOwner[size_t(ri)] = f.newOwner;
        // structures in the sector change sides with the flag (Z)
        w.each<Unit, Owner, Transform>([&](Entity s, Unit& su, Owner& so, Transform& st) {
            if (s == f.point || !rules_.types[su.type].structure || !rules_.types[su.type].capturable) return;
            if (regionAt(sim, st.pos) != ri) return;
            so.player = f.newOwner;
            if (auto* p = w.tryGet<Production>(s)) p->queue.clear();
        });
    }
    // tickets drain for the side holding fewer victory points (Company of Heroes)
    if (gs.victory.mode == VictoryMode::Tickets && sim.tick() % sim.tickRate() == 0) {
        std::vector<int> held(gs.tickets.size(), 0);
        w.each<Capture, Unit, Owner>([&](Entity, Capture&, Unit& u, Owner& o) {
            if (o.player == 0 || !rules_.types[u.type].victoryPoint) return;
            uint8_t team = teamOf(sim, o.player);
            if (team < held.size()) held[team]++;
        });
        for (size_t a = 1; a < held.size(); ++a) {
            int most = 0;
            for (size_t b = 1; b < held.size(); ++b) if (b != a) most = std::max(most, held[b]);
            if (most > held[a]) gs.tickets[a] = fxMax(Fixed{}, gs.tickets[a] - gs.victory.ticketDrain * (most - held[a]));
        }
    }
}

// ================================================================= neutrals

void RtsModule::sysNeutrals(Simulation& sim) {
    World& w = sim.world();
    GameState& gs = state(sim);
    std::vector<std::pair<Entity, Vec3>> moves;
    w.each<Guard, Unit, Transform>([&](Entity e, Guard& g, Unit& u, Transform& tr) {
        const UnitType& t = rules_.types[u.type];
        auto* m = w.tryGet<Mobile>(e);
        if (!m) return;
        Fixed fromHome = dist2D(tr.pos, g.home);
        if (t.wanders) {
            if (!m->moving && sim.tick() >= g.nextWander) {
                Fixed r = t.wanderRadius;
                for (int tries = 0; tries < 6; ++tries) {
                    Vec3 p = g.home + Vec3{(sim.rng().unit() * 2 - kOne) * r, Fixed{}, (sim.rng().unit() * 2 - kOne) * r};
                    if (!gs.grid.passableAt(p, t.pathLayer)) continue;
                    moves.push_back({e, p});
                    break;
                }
                g.nextWander = sim.tick() + sim.tickRate() * (4 + sim.rng().below(8));
            }
            return;
        }
        if (!g.returning && fromHome > g.leash) {
            g.returning = true;
            if (auto* c = w.tryGet<Combat>(e)) c->target = kNullEntity;
            moves.push_back({e, g.home});
        } else if (g.returning && fromHome < Fixed::fromInt(2)) {
            g.returning = false;
            u.hp = stat(sim, e, Stat::MaxHp); // creeps heal when they reset
        } else if (g.returning && !m->moving) {
            moves.push_back({e, g.home});
        }
    });
    for (auto& [e, p] : moves) moveTo(sim, e, p, Fixed::fromRatio(1, 2));
}

// waves: modules/rts/lua/waves.lua (standard Lua library, std.waves)
// victory: modules/rts/lua/victory.lua (standard Lua library, std.victory)

} // namespace rts::game
