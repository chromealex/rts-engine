// Settlers / Knights and Merchants economy: goods stored in building inventories (storehouses
// form the player's stock), production chains (recipes), carriers hauling goods on request,
// construction materials delivered to sites, the player-built road network.
#include "RtsInternal.h"
#include <algorithm>

namespace rts::game {
using namespace detail;

namespace {
constexpr int kDx4[4] = {1, -1, 0, 0};
constexpr int kDy4[4] = {0, 0, 1, -1};

Fixed sum(const std::vector<Fixed>& v) { Fixed s{}; for (auto x : v) s += x; return s; }

// Cells around (and under) a structure footprint.
template <class Fn> void forRing(const Footprint& f, Fn&& fn) {
    for (int y = f.y - 1; y <= f.y + f.h; ++y)
        for (int x = f.x - 1; x <= f.x + f.w; ++x) fn(x, y);
}
} // namespace

bool RtsModule::isGood(uint16_t res) const {
    return rules_.buildingStorage && res < rules_.resources.size() && rules_.resources[res].kind == ResourceKind::Stock && !rules_.resources[res].global;
}

std::vector<Amount> RtsModule::upfrontCost(const std::vector<Amount>& cost) const {
    if (!rules_.buildingStorage) return cost;
    std::vector<Amount> out;
    for (auto& c : cost) if (!isGood(c.res)) out.push_back(c);
    return out;
}

// ================================================================= storehouses

void RtsModule::depositGoods(Simulation& sim, uint8_t player, uint16_t res, Fixed amount) {
    if (amount.raw <= 0) return;
    sim.world().each<Inventory, Unit, Owner>([&](Entity, Inventory& inv, Unit& u, Owner& o) {
        if (amount.raw <= 0 || o.player != player || (u.flags & kUnitUnderConstruction)) return;
        const UnitType& t = rules_.types[u.type];
        if (t.storeCapacity.raw <= 0) return;
        Fixed put = fxMin(amount, fxMax(Fixed{}, t.storeCapacity - sum(inv.have)));
        inv.have[res] += put;
        amount -= put;
    });
}

void RtsModule::withdrawGoods(Simulation& sim, uint8_t player, uint16_t res, Fixed amount) {
    if (amount.raw <= 0) return;
    sim.world().each<Inventory, Unit, Owner>([&](Entity, Inventory& inv, Unit& u, Owner& o) {
        if (amount.raw <= 0 || o.player != player || (u.flags & kUnitUnderConstruction) || rules_.types[u.type].storeCapacity.raw <= 0) return;
        Fixed take = fxMin(amount, inv.have[res]);
        inv.have[res] -= take;
        amount -= take;
    });
}

void RtsModule::syncStorehouses(Simulation& sim) {
    GameState& gs = state(sim);
    const Fixed dt = sim.dt();
    for (size_t p = 1; p < gs.players.size(); ++p)
        for (size_t r = 0; r < rules_.resources.size(); ++r) {
            if (!isGood(uint16_t(r))) continue;
            Fixed inc = gs.players[p].income[r] * dt;
            if (inc.raw > 0) depositGoods(sim, uint8_t(p), uint16_t(r), inc);
            else if (inc.raw < 0) withdrawGoods(sim, uint8_t(p), uint16_t(r), -inc);
        }
    for (auto& p : gs.players)
        for (size_t r = 0; r < rules_.resources.size(); ++r) if (isGood(uint16_t(r))) p.stock[r] = Fixed{};
    sim.world().each<Inventory, Unit, Owner>([&](Entity, Inventory& inv, Unit& u, Owner& o) {
        if (o.player == 0 || o.player >= gs.players.size() || (u.flags & kUnitUnderConstruction) || rules_.types[u.type].storeCapacity.raw <= 0) return;
        for (size_t r = 0; r < rules_.resources.size(); ++r) if (isGood(uint16_t(r))) gs.players[o.player].stock[r] += inv.have[r];
    });
}

// ================================================================= roads

void RtsModule::recomputeRoads(Simulation& sim) {
    GameState& gs = state(sim);
    MapGrid& g = gs.grid;
    gs.roadsDirty = false;
    gs.roadComp.assign(g.flags.size(), -1);
    int comp = 0;
    for (size_t i = 0; i < g.flags.size(); ++i) {
        if (!(g.flags[i] & kCellRoad) || gs.roadComp[i] >= 0) continue;
        std::vector<int> q{int(i)};
        gs.roadComp[i] = comp;
        for (size_t qi = 0; qi < q.size(); ++qi) {
            int x = q[qi] % g.w, y = q[qi] / g.w;
            for (int k = 0; k < 4; ++k) {
                int nx = x + kDx4[k], ny = y + kDy4[k];
                if (!g.inside(nx, ny)) continue;
                size_t n = size_t(g.index(nx, ny));
                if (!(g.flags[n] & kCellRoad) || gs.roadComp[n] >= 0) continue;
                gs.roadComp[n] = comp;
                q.push_back(int(n));
            }
        }
        ++comp;
    }
}

namespace {
std::vector<int32_t> roadCompsOf(World& w, const GameState& gs, Entity e) {
    std::vector<int32_t> out;
    const MapGrid& g = gs.grid;
    auto* f = w.tryGet<Footprint>(e);
    if (!f || gs.roadComp.empty()) return out;
    forRing(*f, [&](int x, int y) {
        if (!g.inside(x, y)) return;
        int32_t c = gs.roadComp[size_t(g.index(x, y))];
        if (c >= 0 && std::find(out.begin(), out.end(), c) == out.end()) out.push_back(c);
    });
    return out;
}
} // namespace

bool RtsModule::roadConnected(Simulation& sim, Entity a, Entity b) {
    if (!rules_.roadNetwork.enabled || !rules_.roadNetwork.required) return true;
    World& w = sim.world();
    GameState& gs = state(sim);
    if (gs.roadsDirty) recomputeRoads(sim);
    if (!w.has<Footprint>(a) || !w.has<Footprint>(b)) return true; // units deliver anywhere
    auto ca = roadCompsOf(w, gs, a), cb = roadCompsOf(w, gs, b);
    for (auto c : ca) if (std::find(cb.begin(), cb.end(), c) != cb.end()) return true;
    return false;
}

// ================================================================= commands

void RtsModule::applySettlersCommand(Simulation& sim, const Command& cmd) {
    World& w = sim.world();
    GameState& gs = state(sim);
    MapGrid& g = gs.grid;
    const uint8_t pl = cmd.player;
    if (!rules_.roadNetwork.enabled || pl == 0 || pl >= gs.players.size()) return;
    auto pave = [&](const std::vector<size_t>& cells) {
        std::vector<Amount> cost = rules_.roadNetwork.cost;
        for (auto& c : cost) c.value = c.value * int64_t(cells.size());
        if (cells.empty() || !pay(sim, pl, cost, {})) return;
        for (size_t i : cells) {
            g.flags[i] |= kCellRoad;
            sim.emit({kEvRoadBuilt, pl, kNullEntity, kNullEntity, int64_t(i), g.cellCenter(int(i) % g.w, int(i) / g.w)});
        }
        gs.roadsDirty = true;
        gs.territoryDirty = true;
    };
    auto pavable = [&](size_t i) {
        return (g.flags[i] & kCellGround) && !(g.flags[i] & kCellRoad) && !g.blocked[i] &&
               (!rules_.territory.enabled || g.territory[i] == pl);
    };
    if (cmd.type == kCmdBuildRoad) {
        int rw, rh, extra;
        unpackRect(cmd.param, rw, rh, extra);
        rw = std::clamp(rw, 1, 64); rh = std::clamp(rh, 1, 64);
        const int x0 = g.cellX(cmd.point.x), y0 = g.cellY(cmd.point.z);
        std::vector<size_t> cells;
        for (int y = y0; y < y0 + rh; ++y)
            for (int x = x0; x < x0 + rw; ++x) {
                if (!g.inside(x, y)) continue;
                size_t i = size_t(g.index(x, y));
                if (extra == 1) {
                    if ((g.flags[i] & kCellRoad) && (!rules_.territory.enabled || g.territory[i] == pl)) {
                        g.flags[i] &= uint8_t(~kCellRoad);
                        gs.roadsDirty = gs.territoryDirty = true;
                        sim.emit({kEvRoadBuilt, 0, kNullEntity, kNullEntity, int64_t(i), g.cellCenter(x, y)});
                    }
                } else if (pavable(i)) cells.push_back(i);
            }
        pave(cells);
        return;
    }
    if (cmd.type != kCmdConnectRoad) return;
    if (gs.roadsDirty) recomputeRoads(sim);
    // targets: road components touching an own storehouse, else the storehouse rings themselves
    std::vector<Entity> stores;
    w.each<Unit, Owner, Footprint>([&](Entity e, Unit& u, Owner& o, Footprint&) {
        if (o.player == pl && rules_.types[u.type].storeCapacity.raw > 0) stores.push_back(e);
    });
    std::vector<int32_t> targetComps;
    std::vector<uint8_t> targetCell(g.flags.size(), 0);
    for (Entity s : stores) {
        for (auto c : roadCompsOf(w, gs, s)) targetComps.push_back(c);
        forRing(w.get<Footprint>(s), [&](int x, int y) { if (g.inside(x, y)) targetCell[size_t(g.index(x, y))] = 1; });
    }
    for (size_t i = 0; i < g.flags.size(); ++i)
        if (gs.roadComp[i] >= 0 && std::find(targetComps.begin(), targetComps.end(), gs.roadComp[i]) != targetComps.end()) targetCell[i] = 1;
    for (Entity e : cmd.units) {
        auto* o = w.tryGet<Owner>(e);
        auto* f = w.tryGet<Footprint>(e);
        if (!o || !f || o->player != pl || std::find(stores.begin(), stores.end(), e) != stores.end()) continue;
        if (!stores.empty() && roadConnected(sim, e, stores[0])) continue;
        // multi-source BFS from the ring of the structure to the nearest target cell
        std::vector<int> parent(g.flags.size(), -2), q;
        forRing(*f, [&](int x, int y) {
            if (!g.inside(x, y)) return;
            size_t i = size_t(g.index(x, y));
            if ((g.flags[i] & kCellGround) && !g.blocked[i] && parent[i] == -2) { parent[i] = -1; q.push_back(int(i)); }
        });
        int hit = -1;
        for (size_t qi = 0; qi < q.size() && hit < 0; ++qi) {
            int c = q[qi];
            if (targetCell[size_t(c)]) { hit = c; break; }
            int x = c % g.w, y = c / g.w;
            for (int k = 0; k < 4; ++k) {
                int nx = x + kDx4[k], ny = y + kDy4[k];
                if (!g.inside(nx, ny)) continue;
                size_t n = size_t(g.index(nx, ny));
                if (parent[n] != -2 || !(g.flags[n] & kCellGround) || g.blocked[n] || !g.canStep(x, y, nx, ny, kLayerGround)) continue;
                if (rules_.territory.enabled && g.territory[n] != pl) continue;
                parent[n] = c;
                q.push_back(int(n));
            }
        }
        if (hit < 0) continue;
        std::vector<size_t> cells;
        for (int c = hit; c >= 0; c = parent[size_t(c)]) if (pavable(size_t(c))) cells.push_back(size_t(c));
        std::sort(cells.begin(), cells.end());
        pave(cells);
        recomputeRoads(sim);
        for (size_t i = 0; i < g.flags.size(); ++i)
            if (gs.roadComp[i] >= 0) for (Entity s : stores) for (auto c : roadCompsOf(w, gs, s)) if (gs.roadComp[i] == c) targetCell[i] = 1;
    }
}

// ================================================================= carriers

bool RtsModule::runHaul(Simulation& sim, Entity e, Order& o) {
    World& w = sim.world();
    auto* c = w.tryGet<Carrier>(e);
    auto* m = w.tryGet<Mobile>(e);
    if (!c || c->res < 0) return true;
    o.timer += sim.dt();
    if (o.timer > Fixed::fromInt(90)) return true; // stuck: give the job up
    auto approach = [&](Entity t) {
        moveTo(sim, e, w.get<Transform>(t).pos, rules_.types[w.get<Unit>(t).type].radius + rules_.types[w.get<Unit>(e).type].radius + Fixed::fromRatio(4, 10));
    };
    if (o.state == 0) {
        if (!w.alive(c->from) || !w.has<Inventory>(c->from)) return true;
        approach(c->from);
        o.state = 1;
        return false;
    }
    if (o.state == 1) {
        if (!w.alive(c->from) || !w.has<Inventory>(c->from)) return true;
        if (contactDistance(sim, e, c->from) > Fixed::fromRatio(8, 10)) { if (m && !m->moving) o.state = 0; return false; }
        stopMoving(sim, e);
        auto& inv = w.get<Inventory>(c->from);
        Fixed take = fxMin(c->amount, inv.have[size_t(c->res)]);
        inv.have[size_t(c->res)] -= take;
        c->loaded = take;
        if (take.raw <= 0) return true;
        o.state = 2;
        if (!w.alive(c->to)) return true;
        approach(c->to);
        return false;
    }
    if (!w.alive(c->to) || !w.has<Inventory>(c->to)) {
        // destination gone: put the load back into a storehouse
        depositGoods(sim, w.get<Owner>(e).player, uint16_t(c->res), c->loaded);
        c->loaded = Fixed{};
        return true;
    }
    if (contactDistance(sim, e, c->to) > Fixed::fromRatio(8, 10)) {
        if (m && !m->moving) approach(c->to);
        return false;
    }
    stopMoving(sim, e);
    w.get<Inventory>(c->to).have[size_t(c->res)] += c->loaded;
    sim.emit({kEvDelivered, w.get<Owner>(e).player, e, c->to, c->res, w.get<Transform>(e).pos});
    c->loaded = Fixed{};
    c->res = -1;
    return true;
}

// ================================================================= per-tick system

void RtsModule::sysSettlers(Simulation& sim) {
    World& w = sim.world();
    GameState& gs = state(sim);
    MapGrid& g = gs.grid;
    const Fixed dt = sim.dt();
    const size_t nr = rules_.resources.size();
    if (gs.roadsDirty && rules_.roadNetwork.enabled) recomputeRoads(sim);

    // ---- workshops: consume inputs, produce outputs (raw producers deplete nearby nodes / fields)
    struct Deplete { Entity node; Fixed amount; int cell; };
    std::vector<Deplete> depletes;
    w.each<Workshop, Inventory, Unit, Owner, Transform>([&](Entity e, Workshop& ws, Inventory& inv, Unit& u, Owner& o, Transform& tr) {
        if ((u.flags & (kUnitUnderConstruction | kUnitDisabled)) || o.player == 0 || o.player >= gs.players.size()) return;
        const RecipeDef& rc = rules_.types[u.type].recipe;
        auto source = [&](Entity& node, int& cell) -> bool {
            node = kNullEntity; cell = -1;
            if (rc.fromRes < 0) return true;
            node = findNearestResource(sim, o.player, tr.pos, rc.fromRes, rc.radius);
            if (node.valid()) return true;
            Vec3 c;
            if (findNearestFieldCell(sim, tr.pos, rc.fromRes, c, int(rc.radius.toInt())).valid() && dist2D(c, tr.pos) <= rc.radius) {
                cell = g.index(g.cellX(c.x), g.cellY(c.z));
                return true;
            }
            return false;
        };
        // goods live in the building; player-wide resources (They Are Billions / Northgard: buildings gather
        // straight into the stock) are taken from and credited to the player
        auto& eco = gs.players[o.player];
        if (!ws.running) {
            for (auto& a : rc.in) if ((isGood(a.res) ? inv.have[a.res] : eco.stock[a.res]) < a.value) return;
            for (auto& a : rc.out) if (isGood(a.res) && inv.have[a.res] >= a.value * rc.buffer) return;
            Entity node; int cell;
            if (!source(node, cell)) return;
            for (auto& a : rc.in) { if (isGood(a.res)) inv.have[a.res] -= a.value; else eco.stock[a.res] -= a.value; }
            ws.running = true;
            ws.progress = Fixed{};
        }
        ws.progress += dt * gs.players[o.player].productionMult * stat(sim, e, Stat::ProductionSpeed) / rc.time;
        if (ws.progress < kOne) return;
        Entity node; int cell;
        if (!source(node, cell)) return; // resource ran out meanwhile: wait
        ws.running = false;
        ws.progress = Fixed{};
        Fixed taken{};
        for (auto& a : rc.out) {
            if (isGood(a.res)) inv.have[a.res] += a.value;
            else addResource(sim, o.player, a.res, a.value);
            if (a.res == rc.fromRes || taken.raw == 0) taken = a.value;
        }
        if (rc.fromRes >= 0) depletes.push_back({node, taken, cell});
        sim.emit({kEvProduced, o.player, e, kNullEntity, rc.out[0].res, tr.pos});
    });
    for (auto& d : depletes) {
        if (d.node.valid() && w.alive(d.node)) {
            auto& rs = w.get<ResourceStock>(d.node);
            if (rs.infinite) continue;
            rs.amount -= d.amount;
            if (rs.amount.raw <= 0) { Unit& nu = w.get<Unit>(d.node); nu.hp = Fixed{}; nu.flags |= kUnitSilent; }
        } else if (d.cell >= 0) {
            g.fieldAmt[size_t(d.cell)] = fxMax(Fixed{}, g.fieldAmt[size_t(d.cell)] - d.amount);
            gs.fieldsDirty = true;
        }
    }

    // ---- carrier jobs (every 10 ticks): construction sites and workshop inputs first, then clear outputs
    if (sim.tick() % 10 != 0) return;
    w.each<Inventory>([&](Entity, Inventory& inv) {
        std::fill(inv.incoming.begin(), inv.incoming.end(), Fixed{});
        std::fill(inv.reserved.begin(), inv.reserved.end(), Fixed{});
    });
    w.each<Carrier, Orders>([&](Entity, Carrier& c, Orders& ord) {
        if (ord.queue.empty() || ord.queue.front().type != OrderType::Haul || c.res < 0) return;
        const bool loaded = ord.queue.front().state >= 2;
        if (!loaded) if (auto* fi = w.tryGet<Inventory>(c.from)) fi->reserved[size_t(c.res)] += c.amount;
        if (auto* ti = w.tryGet<Inventory>(c.to)) ti->incoming[size_t(c.res)] += loaded ? c.loaded : c.amount;
    });
    for (size_t p = 1; p < gs.players.size(); ++p) {
        struct Bld { Entity e; Vec3 pos; bool store, work, site; Fixed cap; };
        std::vector<Bld> blds;
        std::vector<Entity> idle;
        w.each<Inventory, Unit, Owner, Transform>([&](Entity e, Inventory&, Unit& u, Owner& o, Transform& tr) {
            if (o.player != p) return;
            const UnitType& t = rules_.types[u.type];
            const bool site = (u.flags & kUnitUnderConstruction) && w.has<Construction>(e);
            blds.push_back({e, tr.pos, t.storeCapacity.raw > 0 && !site, t.recipe.enabled && !site, site, t.storeCapacity});
        });
        w.each<Carrier, Orders, Unit, Owner>([&](Entity e, Carrier&, Orders& ord, Unit& u, Owner& o) {
            if (o.player == p && ord.queue.empty() && u.hp.raw > 0 && !(u.flags & (kUnitInHand | kUnitPossessed))) idle.push_back(e);
        });
        if (idle.empty() || blds.empty()) continue;
        auto assign = [&](Entity from, Entity to, uint16_t res, Fixed want) -> Fixed {
            Vec3 fp = w.get<Transform>(from).pos;
            size_t bi = 0;
            Fixed bd{};
            for (size_t k = 0; k < idle.size(); ++k) {
                Fixed d = dist2D(w.get<Transform>(idle[k]).pos, fp);
                if (k == 0 || d < bd) { bd = d; bi = k; }
            }
            Entity cr = idle[bi];
            Fixed amount = fxMin(want, rules_.types[w.get<Unit>(cr).type].carrierCapacity);
            idle.erase(idle.begin() + long(bi));
            w.get<Carrier>(cr) = Carrier{from, to, int16_t(res), amount, Fixed{}};
            Order o; o.type = OrderType::Haul; o.target = to;
            issue(sim, cr, o, false);
            w.get<Inventory>(from).reserved[res] += amount;
            w.get<Inventory>(to).incoming[res] += amount;
            return amount;
        };
        auto bestSource = [&](const Bld& dest, uint16_t res) -> Entity {
            Entity best;
            Fixed bestD{};
            int bestRank = 9;
            for (auto& s : blds) {
                if (s.e == dest.e || !(s.store || s.work)) continue;
                if (s.work) {
                    bool outputs = false;
                    for (auto& a : rules_.types[w.get<Unit>(s.e).type].recipe.out) if (a.res == res) outputs = true;
                    if (!outputs) continue;
                }
                auto& inv = w.get<Inventory>(s.e);
                if (inv.have[res] - inv.reserved[res] <= Fixed{}) continue;
                if (!roadConnected(sim, s.e, dest.e)) continue;
                int rank = s.work ? 0 : 1;   // fresh output before the storehouse buffer
                Fixed d = dist2D(s.pos, dest.pos);
                if (!best.valid() || rank < bestRank || (rank == bestRank && d < bestD)) { best = s.e; bestD = d; bestRank = rank; }
            }
            return best;
        };
        for (auto& d : blds) {
            if (idle.empty()) break;
            if (!d.site && !d.work) continue;
            const UnitType& t = rules_.types[w.get<Unit>(d.e).type];
            std::vector<Amount> wants;
            if (d.site) { for (auto& c : t.cost) if (isGood(c.res)) wants.push_back(c); }
            else for (auto& a : t.recipe.in) wants.push_back({a.res, a.value * t.recipe.buffer});
            for (auto& a : wants) {
                for (int guard = 0; guard < 8 && !idle.empty(); ++guard) {
                    auto& inv = w.get<Inventory>(d.e);
                    Fixed need = a.value - inv.have[a.res] - inv.incoming[a.res];
                    if (need.raw <= 0) break;
                    Entity src = bestSource(d, a.res);
                    if (!src.valid()) break;
                    auto& si = w.get<Inventory>(src);
                    assign(src, d.e, a.res, fxMin(need, si.have[a.res] - si.reserved[a.res]));
                }
            }
        }
        // clear workshop outputs into the nearest storehouse with room
        for (auto& s : blds) {
            if (idle.empty()) break;
            if (!s.work) continue;
            for (auto& a : rules_.types[w.get<Unit>(s.e).type].recipe.out) {
                auto& inv = w.get<Inventory>(s.e);
                Fixed avail = inv.have[a.res] - inv.reserved[a.res];
                if (avail.raw <= 0 || idle.empty()) continue;
                Entity best;
                Fixed bestD{};
                for (auto& st : blds) {
                    if (!st.store) continue;
                    auto& si = w.get<Inventory>(st.e);
                    if (sum(si.have) + sum(si.incoming) >= st.cap || !roadConnected(sim, s.e, st.e)) continue;
                    Fixed d = dist2D(st.pos, s.pos);
                    if (!best.valid() || d < bestD) { best = st.e; bestD = d; }
                }
                if (best.valid()) assign(s.e, best, a.res, avail);
            }
        }
    }
    (void)nr;
}

void RtsModule::hashSettlers(Simulation& sim, Hasher& h) {
    World& w = sim.world();
    GameState& gs = state(sim);
    w.each<Inventory>([&](Entity e, Inventory& inv) { h.u64(e.packed()); for (auto x : inv.have) h.fx(x); });
    w.each<Workshop>([&](Entity e, Workshop& ws) { h.u64(e.packed()); h.fx(ws.progress); h.u32(ws.running); });
    w.each<Carrier>([&](Entity e, Carrier& c) { h.u64(e.packed()); h.u64(c.from.packed()); h.u64(c.to.packed()); h.i32(c.res); h.fx(c.amount); h.fx(c.loaded); });
    for (auto f : gs.grid.flags) h.u32(f);
}

} // namespace rts::game
