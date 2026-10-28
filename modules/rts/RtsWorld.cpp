// World rules: vision with cliff levels, creep, control points / flags / regions, tickets,
// neutral creeps and wanderers, (waves and victory conditions: lua/waves.lua, lua/victory.lua).
#include "RtsInternal.h"
#include <algorithm>

namespace rts::game {
using namespace detail;

// ================================================================= vision

void RtsModule::sysVision(Simulation& sim) {
    if (sim.tick() % 5 != 0) return;
    World& w = sim.world();
    GameState& gs = state(sim);
    if (gs.fogMode == uint8_t(FogMode::None)) return;   // everything stays visible
    const MapGrid& g = gs.grid;
    const int W = g.w, H = g.h;
    const size_t np = gs.vision.size();
    // visible -> explored: only the cells lit last time (a whole-map sweep is too slow on 1024x1024 maps)
    gs.visibleCells.resize(np);
    for (size_t p = 1; p < np; ++p) {
        for (int32_t i : gs.visibleCells[p]) gs.vision[p][size_t(i)] = 1;
        gs.visibleCells[p].clear();
    }
    if (gs.surface) { visionSphere(sim); return; }   // planets (RtsPlanets.cpp)
    std::vector<std::vector<uint8_t>> sharers(np);
    for (size_t p = 1; p < np; ++p) for (size_t q = 1; q < np; ++q) if (q == p || (rules_.fog.shareAllies && sim.allied(uint8_t(p), uint8_t(q)))) sharers[p].push_back(uint8_t(q));
    const bool cliffs = rules_.heights.blockVision;
    const bool los = g.hf && cliffs;   // heightfield: real line of sight over the surface
    // heights in corner-sum units (4 corners of 1/16 = 1/64 units) so the ray test stays integer
    auto sumUnits = [](Fixed v) { return (v.raw * 64) >> Fixed::kFracBits; };
    const int64_t eyeUp = sumUnits(rules_.terrain.eyeHeight), targetUp = sumUnits(rules_.terrain.targetHeight);
    auto cellSum = [&](int x, int y) {
        return int64_t(g.corner[size_t(g.cornerIndex(x, y))]) + g.corner[size_t(g.cornerIndex(x + 1, y))] +
               g.corner[size_t(g.cornerIndex(x, y + 1))] + g.corner[size_t(g.cornerIndex(x + 1, y + 1))];
    };
    auto see = [&](uint8_t owner, size_t i) {
        for (uint8_t q : sharers[owner]) {
            uint8_t& v = gs.vision[q][i];
            if (v != 2) { v = 2; gs.visibleCells[q].push_back(int32_t(i)); }
        }
    };
    // 3D line of sight: ground height per cell (heightfield corners or cliff levels) and an occlusion
    // height that adds dynamic blockers (types with vision.blocks, smoke areas, undug rock).
    // Integer units of 1/64 so the ray test is exact and deterministic.
    const bool losAll = los || rules_.fog.lineOfSight;
    std::vector<int64_t>& base = gs.visionBase;
    std::vector<int64_t>& occ = gs.visionOcc;
    if (losAll) {
        const size_t n = size_t(W) * size_t(H);
        {   // terrain can change (digging, sculpt ops): rebuilt on every vision pass, W*H additions
            base.assign(n, 0);
            const int64_t lv = sumUnits(rules_.levelHeight);
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x)
                    base[size_t(y * W + x)] = g.hf ? cellSum(x, y) : int64_t(g.level[size_t(y * W + x)]) * lv;
        }
        occ = base;
        auto raise = [&](int x, int y, int64_t h) {
            if (x < 0 || y < 0 || x >= W || y >= H) return;
            int64_t& o = occ[size_t(y * W + x)];
            o = std::max(o, base[size_t(y * W + x)] + h);
        };
        const int64_t rock = sumUnits(rules_.fog.rockHeight);
        if (rock > 0 && g.dig.size() == n)
            for (size_t i = 0; i < n; ++i) if (g.dig[i]) occ[i] = std::max(occ[i], base[i] + rock);
        w.each<Unit, Transform>([&](Entity, Unit& u, Transform& tr) {
            const UnitType& t = rules_.types[u.type];
            if (t.visionBlock.raw <= 0 || u.hp.raw <= 0) return;
            const int64_t h = sumUnits(t.visionBlock);
            if (t.structure) {
                const Fixed half = Fixed::fromRatio(1, 2);
                const int x0 = int((tr.pos.x - Fixed::fromInt(t.footprintW) / 2 + half).floorInt()), y0 = int((tr.pos.z - Fixed::fromInt(t.footprintH) / 2 + half).floorInt());
                for (int y = 0; y < t.footprintH; ++y) for (int x = 0; x < t.footprintW; ++x) raise(x0 + x, y0 + y, h);
            } else {
                raise(int(tr.pos.x.floorInt()), int(tr.pos.z.floorInt()), h);
            }
        });
        if (rules_.advancedEffects)
            w.each<Area, Transform>([&](Entity, Area& a, Transform& tr) {
                if (a.effList < 0 || sim.tick() < a.fireAt) return;
                const EffectDef& ef = rules_.payloads[size_t(a.effList)][size_t(a.effIndex)];
                if (ef.delivery.blocksVision.raw <= 0) return;
                const int64_t h = sumUnits(ef.delivery.blocksVision);
                const int rr = int(a.radius.floorInt()), cx = int(tr.pos.x.floorInt()), cy = int(tr.pos.z.floorInt());
                for (int y = cy - rr; y <= cy + rr; ++y)
                    for (int x = cx - rr; x <= cx + rr; ++x)
                        if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= rr * rr) raise(x, y, h);
            });
    }
    w.each<Unit, Owner, Transform>([&](Entity e, Unit& u, Owner& o, Transform& tr) {
        if (o.player == 0 || o.player >= np || (u.flags & kUnitInHand)) return;
        const UnitType& t = rules_.types[u.type];
        int r = int(stat(sim, e, Stat::Sight).toInt());
        int cx, cy; state(sim).grid.cellXY(tr.pos, cx, cy);
        const bool air = (t.layer & kLayerAir) != 0;
        if (losAll && !air && !t.seesOver && g.inside(cx, cy)) {
            const int64_t eye = base[size_t(cy * W + cx)] + eyeUp + sumUnits(t.eyeHeight);
            for (int y = std::max(0, cy - r); y <= std::min(H - 1, cy + r); ++y)
                for (int x = std::max(0, cx - r); x <= std::min(W - 1, cx + r); ++x) {
                    int dx = x - cx, dy = y - cy;
                    if (dx * dx + dy * dy > r * r) continue;
                    // march the cells between eye and target: hidden if ground or a blocker rises above the sight line
                    const int k = std::max(std::abs(dx), std::abs(dy));
                    const int64_t tgt = base[size_t(y * W + x)] + targetUp;
                    bool visible = true;
                    for (int st = 1; st < k && visible; ++st) {
                        int sx = cx + (dx * st * 2 + (dx >= 0 ? k : -k)) / (2 * k), sy = cy + (dy * st * 2 + (dy >= 0 ? k : -k)) / (2 * k);
                        if (occ[size_t(sy * W + sx)] * k > eye * (k - st) + tgt * st) visible = false;
                    }
                    if (visible) see(o.player, size_t(y * W + x));
                }
            return;
        }
        // eyes on a ramp count as the lower level; air units see over cliffs
        int eye = air || !cliffs ? 127 : g.levelAt(cx, cy);
        for (int y = std::max(0, cy - r); y <= std::min(H - 1, cy + r); ++y)
            for (int x = std::max(0, cx - r); x <= std::min(W - 1, cx + r); ++x) {
                int dx = x - cx, dy = y - cy;
                if (dx * dx + dy * dy > r * r) continue;
                size_t i = size_t(y * W + x);
                if (g.level[i] > eye) continue;
                see(o.player, i);
            }
    });
    if (rules_.advancedEffects)  // reveal areas (spy, scouting flares)
        w.each<Area, Owner, Transform>([&](Entity, Area& a, Owner& o, Transform& tr) {
            if (o.player == 0 || o.player >= np || a.effList < 0) return;
            const EffectDef& e = rules_.payloads[size_t(a.effList)][size_t(a.effIndex)];
            Fixed sight = e.delivery.sight.raw > 0 ? e.delivery.sight : e.kind == EffectKind::Reveal ? a.radius : Fixed{};
            if (sight.raw <= 0 || sim.tick() < a.fireAt) return;
            int r = int(sight.floorInt());
            int cx, cy; state(sim).grid.cellXY(tr.pos, cx, cy);
            for (int y = std::max(0, cy - r); y <= std::min(H - 1, cy + r); ++y)
                for (int x = std::max(0, cx - r); x <= std::min(W - 1, cx + r); ++x)
                    if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r)
                        see(o.player, size_t(y * W + x)); // tracked in visibleCells, so it fades back to explored
        });
}

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
