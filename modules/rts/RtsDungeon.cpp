// Dungeon management and territory: diggable terrain, per-cell territory (DK floor claiming and
// territory-war front lines), painted rooms, portal recruitment, creature needs and morale, the
// hand (pick up / drop / slap), possession, auto-training structures and player abilities.
#include "RtsInternal.h"
#include <algorithm>

namespace rts::game {
using namespace detail;

namespace {
constexpr int kDx4[4] = {1, -1, 0, 0};
constexpr int kDy4[4] = {0, 0, 1, -1};

bool flagged(const Unit& u, uint8_t f) { return (u.flags & f) != 0; }
uint8_t markBit(uint8_t player) { return player >= 1 && player <= 8 ? uint8_t(1u << (player - 1)) : 0; }

// Breadth-first search over cells the layer can walk, nearest first (deterministic neighbour order).
template <class Pred>
int bfsCells(const MapGrid& g, int start, uint8_t layer, int maxNodes, Pred&& pred) {
    if (start < 0) return -1;
    std::vector<int> queue{start};
    std::vector<uint8_t> seen(size_t(g.w * g.h), 0);
    seen[size_t(start)] = 1;
    for (size_t qi = 0; qi < queue.size() && int(qi) < maxNodes; ++qi) {
        int c = queue[qi];
        if (pred(c)) return c;
        int x = c % g.w, y = c / g.w;
        for (int k = 0; k < 4; ++k) {
            int n;
            if (g.surf) {   // planets: across face seams (Surface::kDx/kDy start with the same 4 directions)
                n = g.surf->neighbor(c, k);
                if (n < 0 || seen[size_t(n)] || !g.canMoveCells(c, n, layer)) continue;
            } else {
                int nx = x + kDx4[k], ny = y + kDy4[k];
                if (!g.inside(nx, ny)) continue;
                n = g.index(nx, ny);
                if (seen[size_t(n)] || !g.canStep(x, y, nx, ny, layer)) continue;
            }
            seen[size_t(n)] = 1;
            queue.push_back(n);
        }
    }
    return -1;
}

// 4-neighbour k of a cell (kDx4/kDy4 order; across face seams on planets), -1 none.
int nb4(const MapGrid& g, int c, int k) {
    if (g.surf) return g.surf->neighbor(c, k);
    const int nx = c % g.w + kDx4[k], ny = c / g.w + kDy4[k];
    return g.inside(nx, ny) ? g.index(nx, ny) : -1;
}

// A cell around a footprint given in grid coordinates (x, y may lie one step outside the rect): on planets the
// footprint is a rect inside one face, so the outside cells are offsets from the nearest footprint cell.
int footCell(const MapGrid& g, const Footprint& f, int x, int y) {
    if (!g.surf) return g.inside(x, y) ? g.index(x, y) : -1;
    const int xc = std::clamp(x, f.x, f.x + f.w - 1), yc = std::clamp(y, f.y, f.y + f.h - 1);
    return g.offsetCell(g.index(xc, yc), x - xc, y - yc);
}

bool adjacentOwned(const MapGrid& g, int c, uint8_t player) {
    if (g.surf) {
        for (int k = 0; k < 4; ++k) { const int n = g.surf->neighbor(c, k); if (n >= 0 && g.territory[size_t(n)] == player) return true; }
        return false;
    }
    int x = c % g.w, y = c / g.w;
    for (int k = 0; k < 4; ++k) {
        int nx = x + kDx4[k], ny = y + kDy4[k];
        if (g.inside(nx, ny) && g.territory[size_t(g.index(nx, ny))] == player) return true;
    }
    return false;
}

void addBuffTo(Buffs& b, const Buff& nb) {
    for (auto& x : b.list)
        if (x.stat == nb.stat && x.sourceType == nb.sourceType) { x = nb; return; }
    b.list.push_back(nb);
}
constexpr uint16_t kSlapSource = kNoType - 1;
} // namespace

// ================================================================= territory helpers

void RtsModule::setTerritory(Simulation& sim, int cell, uint8_t owner) {
    GameState& gs = state(sim);
    MapGrid& g = gs.grid;
    if (cell < 0 || size_t(cell) >= g.territory.size()) return;
    uint8_t old = g.territory[size_t(cell)];
    if (old == owner) return;
    if (old < gs.territoryCells.size()) gs.territoryCells[old]--;
    if (owner < gs.territoryCells.size()) gs.territoryCells[owner]++;
    g.territory[size_t(cell)] = owner;
    if (g.room[size_t(cell)] >= 0) { g.room[size_t(cell)] = -1; gs.roomsDirty = true; } // rooms are lost with the floor
    gs.territoryDirty = true;
    sim.emit({kEvTerritory, owner, kNullEntity, kNullEntity, cell, g.cellCenter(cell % g.w, cell / g.w)});
}

int RtsModule::territoryOf(Simulation& sim, uint8_t player) const {
    auto* gs = sim.world().resource<GameState>();
    return player < gs->territoryCells.size() ? gs->territoryCells[player] : 0;
}

int RtsModule::roomCellsOf(Simulation& sim, uint8_t player, int room) const {
    auto* gs = sim.world().resource<GameState>();
    if (player >= gs->roomCells.size() || room < 0 || size_t(room) >= gs->roomCells[player].size()) return 0;
    return gs->roomCells[player][size_t(room)];
}

std::vector<Amount> RtsModule::techCost(Simulation& sim, uint8_t player, uint16_t tech) const {
    std::vector<Amount> cost = rules_.techs[tech].cost;
    auto* gs = sim.world().resource<GameState>();
    int level = player < gs->players.size() && tech < gs->players[player].researched.size() ? gs->players[player].researched[tech] : 0;
    if (rules_.techs[tech].costGrowth.raw != 0 && level > 0)
        for (auto& c : cost) c.value = c.value + c.value * rules_.techs[tech].costGrowth * level;
    return cost;
}

bool RtsModule::canUsePlayerAbility(Simulation& sim, uint8_t player, int index) const {
    auto* gs = sim.world().resource<GameState>();
    if (index < 0 || size_t(index) >= rules_.playerAbilities.size() || player == 0 || player >= gs->playerAbilities.size()) return false;
    if (rules_.mech.deck && !mechInHand(sim, player, index)) return false;   // agent-mech: card deck
    const PlayerAbilityState& st = gs->playerAbilities[player][size_t(index)];
    const PlayerAbilityDef& a = rules_.advancedEffects ? playerAbilityFor(sim, player, index) : rules_.playerAbilities[size_t(index)];
    if (rules_.advancedEffects) {
        if (!st.owned || a.passive || st.pendingAt) return false;
        if (player < gs->players.size() && (gs->players[player].statusFlags & kStSilence)) return false;
        for (int rq : a.requires_) if (!requirementsMet(sim, player, {Requirement{(rq & 0x8000) != 0, uint16_t(rq & 0x7FFF)}})) return false;
    }
    if (st.readyAt > sim.tick()) return false;
    if (a.chargesMax > 0 && st.charges <= 0) return false;
    return canAfford(sim, player, a.cost);
}

// ================================================================= setup

void RtsModule::setupDungeon(Simulation& sim, const Json& map) {
    World& w = sim.world();
    GameState& gs = state(sim);
    MapGrid& g = gs.grid;
    const size_t np = gs.players.size();
    gs.territoryCells.assign(np, 0);
    gs.hand.assign(np, {});
    gs.possessed.assign(np, kNullEntity);
    gs.roomCells.assign(np, std::vector<int>(rules_.rooms.size(), 0));
    gs.playerAbilities.assign(np, std::vector<PlayerAbilityState>(rules_.playerAbilities.size()));
    for (auto& pl : gs.playerAbilities)
        for (size_t k = 0; k < pl.size(); ++k) pl[k].charges = rules_.playerAbilities[k].chargesMax;
    gs.territoryTotal = 0;
    for (size_t i = 0; i < g.flags.size(); ++i) if ((g.flags[i] & kCellGround) || g.dig[i]) ++gs.territoryTotal;
    if (!rules_.territory.enabled) return;

    auto own = [&](int x, int y, uint8_t p) {
        if (!g.inside(x, y) || p == 0 || p >= np) return;
        size_t i = size_t(g.index(x, y));
        if (!(g.flags[i] & kCellGround) || g.territory[i]) return;
        g.territory[i] = p;
    };
    // explicit layer: digits are player slots
    if (const Json& t = map["territory"]; t.isObject()) {
        const Json& rows = t["rows"];
        for (size_t y = 0; y < rows.size(); ++y) {
            std::string row = rows[y].str();
            for (size_t x = 0; x < row.size(); ++x)
                if (row[x] >= '1' && row[x] <= '9') own(int(x), int(y), uint8_t(row[x] - '0'));
        }
    }
    if (rules_.territory.startFromRegions && gs.regionCount > 0)
        for (int y = 0; y < g.h; ++y)
            for (int x = 0; x < g.w; ++x) {
                int ri = g.region[size_t(g.index(x, y))];
                if (ri >= 0) own(x, y, gs.regionOwner[size_t(ri)]);
            }
    if (rules_.territory.startRadius > 0) {
        const int r = rules_.territory.startRadius;
        std::vector<std::pair<Vec3, uint8_t>> anchors;
        for (size_t p = 1; p < np; ++p) {
            const Json& pj = map["players"][p - 1];
            if (pj.has("start")) anchors.push_back({g.surf ? mapPos(sim, pj["start"]) : Vec3{pj["start"][0].fx(), Fixed{}, pj["start"][1].fx()}, uint8_t(p)});
        }
        if (g.surf) {
            // planets: the same squares / discs, as cell offsets across face seams
            auto ownC = [&](int c, uint8_t p) { if (c >= 0) own(c % g.w, c / g.w, p); };
            w.each<Unit, Owner, Footprint>([&](Entity, Unit&, Owner& o, Footprint& f) {
                if (o.player == 0 || o.player >= np) return;
                for (int y = f.y; y < f.y + f.h; ++y)
                    for (int x = f.x; x < f.x + f.w; ++x)
                        for (int dy = -r; dy <= r; ++dy)
                            for (int dx = -r; dx <= r; ++dx) ownC(g.offsetCell(g.index(x, y), dx, dy), o.player);
                for (int y = f.y; y < f.y + f.h; ++y)
                    for (int x = f.x; x < f.x + f.w; ++x)
                        if (g.inside(x, y) && !g.territory[size_t(g.index(x, y))]) g.territory[size_t(g.index(x, y))] = o.player;
            });
            for (auto& [pos, p] : anchors) {
                const int c0 = g.cellOf(pos);
                if (c0 < 0) continue;
                for (int dy = -r; dy <= r; ++dy)
                    for (int dx = -r; dx <= r; ++dx)
                        if (dx * dx + dy * dy <= r * r) ownC(g.offsetCell(c0, dx, dy), p);
            }
            anchors.clear();
        } else
        w.each<Unit, Owner, Footprint>([&](Entity, Unit&, Owner& o, Footprint& f) {
            if (o.player == 0 || o.player >= np) return;
            for (int y = f.y - r; y < f.y + f.h + r; ++y)
                for (int x = f.x - r; x < f.x + f.w + r; ++x) own(x, y, o.player);
            for (int y = f.y; y < f.y + f.h; ++y)
                for (int x = f.x; x < f.x + f.w; ++x)
                    if (g.inside(x, y) && !g.territory[size_t(g.index(x, y))]) g.territory[size_t(g.index(x, y))] = o.player;
        });
        for (auto& [pos, p] : anchors) {
            int cx = g.cellX(pos.x), cy = g.cellY(pos.z);
            for (int y = cy - r; y <= cy + r; ++y)
                for (int x = cx - r; x <= cx + r; ++x)
                    if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r) own(x, y, p);
        }
    }
    for (auto t : g.territory) if (t < np) gs.territoryCells[t]++;
}

// ================================================================= rooms

void RtsModule::recomputeRooms(Simulation& sim) {
    GameState& gs = state(sim);
    MapGrid& g = gs.grid;
    gs.roomsDirty = false;
    gs.roomZones.clear();
    for (auto& pc : gs.roomCells) std::fill(pc.begin(), pc.end(), 0);
    std::vector<uint8_t> seen(g.room.size(), 0);
    for (size_t i = 0; i < g.room.size(); ++i) {
        if (seen[i] || g.room[i] < 0) continue;
        if (g.territory[i] == 0) { g.room[i] = -1; continue; }
        RoomZone z;
        z.type = uint16_t(g.room[i]);
        z.owner = g.territory[i];
        std::vector<int32_t> q{int32_t(i)};
        seen[i] = 1;
        int64_t sx = 0, sy = 0;
        for (size_t qi = 0; qi < q.size(); ++qi) {
            int c = q[qi], x = c % g.w, y = c / g.w;
            sx += x; sy += y;
            for (int k = 0; k < 4; ++k) {
                const int nc = nb4(g, c, k);
                if (nc < 0) continue;
                size_t n = size_t(nc);
                if (seen[n] || g.room[n] != int16_t(z.type) || g.territory[n] != z.owner) continue;
                seen[n] = 1;
                q.push_back(int32_t(n));
            }
        }
        std::sort(q.begin(), q.end());
        int64_t n = int64_t(q.size());
        if (g.surf) {   // planets: the mean of the cell centres, back on the surface
            Vec3 m{};
            for (int32_t c : q) m = m + g.centerOf(c);
            z.center = g.surf->project(m / Fixed::fromInt(n));
        } else z.center = {Fixed::fromInt(sx) / n + kHalf, Fixed{}, Fixed::fromInt(sy) / n + kHalf};
        z.cells = std::move(q);
        if (z.owner < gs.roomCells.size()) gs.roomCells[z.owner][z.type] += int(n);
        gs.roomZones.push_back(std::move(z));
    }
}

void RtsModule::roomEconomy(Simulation& sim) {
    GameState& gs = state(sim);
    if (gs.roomsDirty) recomputeRooms(sim);
    if (rules_.rooms.empty()) return;
    for (size_t p = 1; p < gs.players.size() && p < gs.roomCells.size(); ++p) {
        auto& eco = gs.players[p];
        for (size_t r = 0; r < rules_.rooms.size(); ++r) {
            int n = gs.roomCells[p][r];
            if (n <= 0) continue;
            const RoomDef& rd = rules_.rooms[r];
            eco.supplyCap += rd.supply * n;
            for (auto& a : rd.storage) eco.cap[a.res] += a.value * n;
            for (auto& a : rd.provides) eco.capProvided[a.res] += a.value * n;
            for (auto& a : rd.income) eco.income[a.res] += a.value * n;
        }
    }
}

namespace {
// Nearest cell of an own room of the given type (tie: lowest cell index).
int nearestRoomCell(const GameState& gs, uint8_t owner, int type, Vec3 from) {
    const MapGrid& g = gs.grid;
    int best = -1;
    Fixed bestD{};
    for (auto& z : gs.roomZones) {
        if (z.owner != owner || int(z.type) != type) continue;
        for (int32_t c : z.cells) {
            if (g.blocked[size_t(c)]) continue;
            Fixed d = dist2D(g.centerOf(c), from);   // flat: the xz distance as before
            if (best < 0 || d < bestD) { best = c; bestD = d; }
        }
    }
    return best;
}
} // namespace

// ================================================================= orders

bool RtsModule::runDig(Simulation& sim, Entity e, Order& o) {
    World& w = sim.world();
    GameState& gs = state(sim);
    MapGrid& g = gs.grid;
    const UnitType& t = rules_.types[w.get<Unit>(e).type];
    auto* m = w.tryGet<Mobile>(e);
    const uint8_t owner = w.get<Owner>(e).player;
    const int dc = g.cellOf(o.point2);
    if (dc < 0 || t.digRate.raw <= 0) return true;
    size_t di = size_t(dc);
    if (!g.dig[di] || !(g.digMark[di] & markBit(owner))) return true;
    if (g.wall[di] != 0 && g.wall[di] != owner) { g.digMark[di] &= uint8_t(~markBit(owner)); return true; }   // fortified meanwhile
    Transform& tr = w.get<Transform>(e);
    if (o.state == 0) { moveTo(sim, e, o.point, Fixed::fromRatio(3, 10)); o.state = 1; return false; }
    if (o.state == 1) {
        if (dist2D(tr.pos, o.point) <= Fixed::fromRatio(6, 10)) { stopMoving(sim, e); o.state = 2; }
        else if (!m || !m->moving) return true;
        return false;
    }
    Vec3 face = flat(o.point2 - tr.pos);
    if (face.length().raw > 0) w.get<Unit>(e).facing = face / face.length();
    g.digHp[di] -= t.digRate * stat(sim, e, Stat::GatherRate) * sim.dt();
    if (g.digHp[di].raw > 0) return false;
    const DiggableDef& dd = rules_.diggables[size_t(g.dig[di] - 1)];
    for (auto& y : dd.yields) addResource(sim, owner, y.res, y.value);
    if (dd.infinite) {
        g.digHp[di] = dd.hardness;   // gem seam: never runs out
        sim.emit({kEvDug, owner, e, kNullEntity, int64_t(di), o.point2});
        return false;
    }
    g.dig[di] = 0;
    g.digMark[di] = 0;
    g.wall[di] = 0;
    g.digHp[di] = Fixed{};
    g.flags[di] = kCellGround;
    g.cellChanged(int(di) % g.w, int(di) / g.w);
    gs.territoryDirty = true;
    gs.fieldsDirty = true;
    sim.emit({kEvDug, owner, e, kNullEntity, int64_t(di), o.point2});
    return true;
}

bool RtsModule::runClaimCell(Simulation& sim, Entity e, Order& o) {
    World& w = sim.world();
    MapGrid& g = state(sim).grid;
    const UnitType& t = rules_.types[w.get<Unit>(e).type];
    auto* m = w.tryGet<Mobile>(e);
    const uint8_t owner = w.get<Owner>(e).player;
    const int cc = g.cellOf(o.point);
    if (cc < 0 || t.claimerTime.raw <= 0) return true;
    size_t ci = size_t(cc);
    // param 1: reinforce the rock cell at point2 (standing on the own floor cell at point) into an owned wall
    const bool fortify = o.param == 1;
    const int wc = fortify ? g.cellOf(o.point2) : -1;
    if (fortify && (wc < 0 || rules_.territory.fortifyTime.raw <= 0)) return true;
    if (fortify ? (!g.dig[size_t(wc)] || g.wall[size_t(wc)] == owner || g.territory[ci] != owner)
                : (g.territory[ci] == owner || !(g.flags[ci] & kCellGround))) return true;
    Transform& tr = w.get<Transform>(e);
    if (o.state == 0) { moveTo(sim, e, o.point, Fixed::fromRatio(2, 10)); o.state = 1; return false; }
    if (o.state == 1) {
        if (dist2D(tr.pos, o.point) <= Fixed::fromRatio(45, 100)) {
            stopMoving(sim, e);
            o.state = 2;
            if (fortify) {
                o.timer = rules_.territory.fortifyTime;
                Vec3 face = flat(o.point2 - tr.pos);
                if (face.length().raw > 0) w.get<Unit>(e).facing = face / face.length();
            } else o.timer = t.claimerTime * (g.territory[ci] != 0 ? 2 : 1); // enemy floor takes longer
        } else if (!m || !m->moving) return true;
        return false;
    }
    if (fortify) {
        if (g.wall[size_t(wc)] != 0 && g.wall[size_t(wc)] != owner) return true;   // someone else got it first
        o.timer -= sim.dt() * stat(sim, e, Stat::GatherRate);
        if (o.timer.raw > 0) return false;
        g.wall[size_t(wc)] = owner;
        g.digMark[size_t(wc)] &= markBit(owner);   // other players' marks on it are void now
        state(sim).territoryDirty = true;
        sim.emit({kEvFortified, owner, e, kNullEntity, int64_t(wc), o.point2});
        return true;
    }
    if (!adjacentOwned(g, int(ci), owner)) return true;
    o.timer -= sim.dt() * stat(sim, e, Stat::GatherRate);
    if (o.timer.raw > 0) return false;
    setTerritory(sim, int(ci), owner);
    return true;
}

bool RtsModule::runNeed(Simulation& sim, Entity e, Order& o) {
    World& w = sim.world();
    GameState& gs = state(sim);
    MapGrid& g = gs.grid;
    const UnitType& t = rules_.types[w.get<Unit>(e).type];
    auto* nd = w.tryGet<Needs>(e);
    auto* m = w.tryGet<Mobile>(e);
    if (!nd || !t.hasNeeds) return true;
    const uint8_t owner = w.get<Owner>(e).player;
    const int kind = o.param & 0xFF, room = o.param >> 8;
    const bool atStructure = o.target.valid();
    if (atStructure) {
        // structure-based need (inn): the building must still be ours
        if (!w.alive(o.target) || w.get<Owner>(o.target).player != owner) return true;
    } else {
        const int cc = g.cellOf(o.point);
        if (cc < 0) return true;
        size_t ci = size_t(cc);
        if (g.room[ci] != room || g.territory[ci] != owner) return true; // room gone
    }
    Transform& tr = w.get<Transform>(e);
    if (o.state == 0) {
        if (atStructure) moveTo(sim, e, w.get<Transform>(o.target).pos, rules_.types[w.get<Unit>(o.target).type].radius + t.radius + Fixed::fromRatio(4, 10));
        else moveTo(sim, e, o.point, Fixed::fromRatio(3, 10));
        o.state = 1;
        return false;
    }
    if (o.state == 1) {
        bool arrived = atStructure ? contactDistance(sim, e, o.target) <= Fixed::fromRatio(8, 10) : dist2D(tr.pos, o.point) <= Fixed::fromRatio(7, 10);
        if (arrived) {
            stopMoving(sim, e);
            o.state = 2;
            o.timer = kind == kNeedHunger ? t.needs.hunger.time : kind == kNeedSleep ? t.needs.sleep.time : t.needs.workTime;
        } else if (!m || !m->moving) return true;
        return false;
    }
    const Fixed dt = sim.dt();
    if (kind == kNeedWork) {
        const RoomDef& rd = rules_.rooms[size_t(room)];
        if (!rd.workCost.empty()) {
            std::vector<Amount> c = rd.workCost;
            for (auto& a : c) a.value = a.value * dt;
            if (!pay(sim, owner, c, {}, false)) return true;
        }
        Fixed rate = stat(sim, e, Stat::GatherRate);
        for (auto& a : rd.workYield) addResource(sim, owner, a.res, a.value * dt * rate);
        if (rd.workXp.raw > 0) addHeroXp(sim, e, rd.workXp * dt * rate, owner);
    }
    o.timer -= dt;
    if (o.timer.raw > 0) return false;
    if (kind == kNeedHunger) {
        if (!pay(sim, owner, t.needs.hunger.cost, {}, false)) return true; // no food: still hungry
        nd->hunger = Fixed{};
    } else if (kind == kNeedSleep) {
        nd->sleep = Fixed{};
        Unit& u = w.get<Unit>(e);
        u.hp = stat(sim, e, Stat::MaxHp);
    }
    return true;
}

// ================================================================= commands

void RtsModule::applyDungeonCommand(Simulation& sim, const Command& cmd) {
    World& w = sim.world();
    GameState& gs = state(sim);
    MapGrid& g = gs.grid;
    const uint8_t pl = cmd.player;
    if (pl == 0 || pl >= gs.players.size() || gs.hand.size() <= pl) return;
    auto ownUnit = [&](Entity e) -> Unit* {
        auto* u = w.tryGet<Unit>(e);
        auto* o = w.tryGet<Owner>(e);
        if (!u || !o || o->player != pl || u->hp.raw <= 0) return nullptr;
        return u;
    };
    auto release = [&]() {
        Entity cur = gs.possessed[pl];
        gs.possessed[pl] = kNullEntity;
        if (w.alive(cur)) {
            if (auto* u = w.tryGet<Unit>(cur)) u->flags &= uint8_t(~kUnitPossessed);
            w.remove<Possessed>(cur);
            if (auto* c = w.tryGet<Combat>(cur)) c->target = kNullEntity;
            stopMoving(sim, cur);
        }
        sim.emit({kEvPossessed, pl, kNullEntity, cur, 0, {}});
    };
    int rw = 0, rh = 0, extra = 0;
    unpackRect(cmd.param, rw, rh, extra);
    rw = std::clamp(rw, 1, 64); rh = std::clamp(rh, 1, 64);
    int x0 = g.cellX(cmd.point.x), y0 = g.cellY(cmd.point.z);
    if (g.surf) g.cellXY(cmd.point, x0, y0);   // planets: grid row/column of the cell under the point (-1: none)
    // cell (x0 + dx, y0 + dy) of the command rect: on planets an offset across face seams, -1 none
    auto rectCell = [&](int dx, int dy) {
        if (!g.surf) return g.inside(x0 + dx, y0 + dy) ? g.index(x0 + dx, y0 + dy) : -1;
        return g.inside(x0, y0) ? g.offsetCell(g.index(x0, y0), dx, dy) : -1;
    };

    switch (cmd.type) {
    case kCmdDig: {
        uint8_t bit = markBit(pl);
        if (!bit) return;
        for (int dy = 0; dy < rh; ++dy)
            for (int dx = 0; dx < rw; ++dx) {
                const int c = rectCell(dx, dy);
                if (c < 0) continue;
                size_t i = size_t(c);
                if (!g.dig[i]) continue;
                if (extra != 1 && i < g.wall.size() && g.wall[i] != 0 && g.wall[i] != pl) continue;   // reinforced by someone else
                if (extra == 1) g.digMark[i] &= uint8_t(~bit); else g.digMark[i] |= bit;
                gs.territoryDirty = true;
            }
        return;
    }
    case kCmdPaintRoom: {
        if (extra >= int(rules_.rooms.size()) || extra < -1) return;
        std::vector<size_t> cells;
        for (int dy = 0; dy < rh; ++dy)
            for (int dx = 0; dx < rw; ++dx) {
                const int c = rectCell(dx, dy);
                if (c < 0) continue;
                size_t i = size_t(c);
                if (g.territory[i] != pl || !(g.flags[i] & kCellGround)) continue;
                if (extra < 0 ? g.room[i] < 0 : (g.room[i] == extra || g.blocked[i])) continue;
                cells.push_back(i);
            }
        if (cells.empty()) return;
        if (extra < 0) {
            for (size_t i : cells) { refund(sim, pl, rules_.rooms[size_t(g.room[i])].cost, rules_.sellRefund); g.room[i] = -1; }
        } else {
            std::vector<Amount> cost = rules_.rooms[size_t(extra)].cost;
            for (auto& c : cost) c.value = c.value * int64_t(cells.size());
            if (!pay(sim, pl, cost, {})) return;
            for (size_t i : cells) g.room[i] = int16_t(extra);
        }
        gs.roomsDirty = true;
        gs.territoryDirty = true;
        sim.emit({kEvRoomPainted, pl, kNullEntity, kNullEntity, extra, g.cellCenter(x0, y0)});
        return;
    }
    case kCmdPickUp: {
        if (rules_.hand.max <= 0) return;
        for (Entity e : cmd.units) {
            if (int(gs.hand[pl].size()) >= rules_.hand.max) break;
            Unit* u = ownUnit(e);
            if (!u || flagged(*u, kUnitInHand | kUnitPossessed | kUnitUnderConstruction) || !w.has<Mobile>(e) || rules_.types[u->type].structure) continue;
            if (std::find(gs.hand[pl].begin(), gs.hand[pl].end(), e) != gs.hand[pl].end()) continue;
            Vec3 p = w.get<Transform>(e).pos;
            if (rules_.territory.enabled && rules_.hand.ownTerritoryOnly) {
                const int pc = g.cellOf(p);
                if (pc < 0 || g.territory[size_t(pc)] != pl) continue;
            }
            u->flags |= kUnitInHand;
            if (auto* ord = w.tryGet<Orders>(e)) ord->queue.clear();
            stopMoving(sim, e);
            if (auto* c = w.tryGet<Combat>(e)) c->target = kNullEntity;
            gs.hand[pl].push_back(e);
            sim.emit({kEvPickedUp, pl, e, kNullEntity, 0, p});
        }
        return;
    }
    case kCmdDrop: {
        auto& hand = gs.hand[pl];
        hand.erase(std::remove_if(hand.begin(), hand.end(), [&](Entity e) { return !w.alive(e); }), hand.end());
        if (hand.empty() || !g.inside(x0, y0)) return;
        size_t ci = size_t(g.index(x0, y0));
        if (!g.passable(x0, y0, kLayerGround) || !isVisible(sim, pl, cmd.point)) return;
        if (rules_.territory.enabled && rules_.hand.ownTerritoryOnly && g.territory[ci] != pl) return;
        std::vector<Entity> drop;
        if (!cmd.units.empty()) { for (Entity e : cmd.units) if (std::find(hand.begin(), hand.end(), e) != hand.end() && std::find(drop.begin(), drop.end(), e) == drop.end()) drop.push_back(e); }
        else for (int k = 0; k < std::max<int>(1, int(cmd.param)) && k < int(hand.size()); ++k) drop.push_back(hand[hand.size() - 1 - size_t(k)]);
        int k = 0;
        for (Entity e : drop) {
            hand.erase(std::find(hand.begin(), hand.end(), e));
            Unit& u = w.get<Unit>(e);
            u.flags &= uint8_t(~kUnitInHand);
            Vec3 p = flat(cmd.point) + Vec3{Fixed::fromRatio(k % 3 - 1, 4), Fixed{}, Fixed::fromRatio((k / 3) % 3 - 1, 4)};
            Transform& tr = w.get<Transform>(e);
            tr.pos = p; tr.prevPos = p;
            ++k;
            sim.emit({kEvDropped, pl, e, kNullEntity, 0, p});
        }
        return;
    }
    case kCmdSlap: {
        if (rules_.hand.max <= 0) return;
        const uint32_t until = sim.tick() + uint32_t((rules_.hand.slapDuration * int64_t(sim.tickRate())).toInt());
        for (Entity e : cmd.units) {
            Unit* u = ownUnit(e);
            if (!u || flagged(*u, kUnitInHand) || rules_.types[u->type].structure) continue;
            u->hp = fxMax(kOne, u->hp - rules_.hand.slapDamage);
            if (auto* b = w.tryGet<Buffs>(e)) {
                addBuffTo(*b, {Stat::Speed, Fixed{}, rules_.hand.slapSpeed, until, kSlapSource});
                addBuffTo(*b, {Stat::GatherRate, Fixed{}, rules_.hand.slapWork, until, kSlapSource});
            }
            if (auto* nd = w.tryGet<Needs>(e)) nd->happiness -= rules_.types[u->type].needs.slapLoss;
            sim.emit({kEvSlapped, pl, e, kNullEntity, 0, w.get<Transform>(e).pos});
        }
        return;
    }
    case kCmdPossess: {
        if (!rules_.possession.enabled) return;
        if (cmd.units.empty()) { if (gs.possessed[pl].valid()) release(); return; }
        Entity e = cmd.units[0];
        Unit* u = ownUnit(e);
        if (!u || flagged(*u, kUnitInHand | kUnitPossessed) || !w.has<Mobile>(e)) return;
        if (!pay(sim, pl, rules_.possession.cost, {})) return;
        if (gs.possessed[pl].valid()) release();
        u = w.tryGet<Unit>(e);
        u->flags |= kUnitPossessed;
        if (auto* ord = w.tryGet<Orders>(e)) ord->queue.clear();
        stopMoving(sim, e);
        w.add<Possessed>(e, Possessed{{}, kNullEntity, false, sim.tick()});
        gs.possessed[pl] = e;
        sim.emit({kEvPossessed, pl, e, kNullEntity, 1, w.get<Transform>(e).pos});
        return;
    }
    case kCmdDirect: {
        Entity e = gs.possessed[pl];
        auto* ps = w.alive(e) ? w.tryGet<Possessed>(e) : nullptr;
        if (!ps) return;
        ps->dir = {cmd.point.x, Fixed{}, cmd.point.z};
        Fixed len = ps->dir.length();
        if (len > kOne) ps->dir = ps->dir / len;
        ps->fire = cmd.param & 1;
        ps->target = w.alive(cmd.target) ? cmd.target : kNullEntity;
        ps->lastTick = sim.tick();
        return;
    }
    case kCmdPlayerAbility: {
        int idx = int(cmd.param & 0xFFFF);
        if (rules_.fidelity) takePoint2(sim, pl, cmd.param);   // pointPair abilities carry their second point
        else if (cmd.param >> 16) return;
        if (rules_.advancedEffects) { castPlayerAbility(sim, pl, idx, cmd.point, cmd.target); return; }
        if (!canUsePlayerAbility(sim, pl, idx)) return;
        if (rules_.mech.deck && !mechDeckAllows(sim, pl, idx, cmd.point)) return;   // agent-mech: deploy zone
        const PlayerAbilityDef& a = rules_.playerAbilities[size_t(idx)];
        if (!pay(sim, pl, a.cost, {})) return;
        if (rules_.mech.deck) mechDeckPlayed(sim, pl, idx);
        PlayerAbilityState& st = gs.playerAbilities[pl][size_t(idx)];
        if (a.chargesMax > 0) --st.charges;
        st.readyAt = sim.tick() + uint32_t((a.cooldown * int64_t(sim.tickRate())).toInt());
        sim.emit({kEvPlayerAbility, pl, kNullEntity, kNullEntity, idx, flat(cmd.point)});
        applyEffects(sim, kNullEntity, pl, kNoType, a.effects, kNullEntity, flat(cmd.point));
        return;
    }
    default: return;
    }
}

// ================================================================= possession movement

void RtsModule::movePossessed(Simulation& sim) {
    World& w = sim.world();
    GameState& gs = state(sim);
    MapGrid& g = gs.grid;
    const Fixed dt = sim.dt();
    w.each<Possessed, Mobile, Unit, Transform>([&](Entity e, Possessed& ps, Mobile& m, Unit& u, Transform& tr) {
        m.moving = false;
        if (sim.tick() > ps.lastTick + 10) { ps.dir = {}; ps.fire = false; } // client stopped steering
        const UnitType& t = rules_.types[u.type];
        auto* b = w.tryGet<Buffs>(e);
        const bool stunned = (b && b->stunUntil > sim.tick()) || (rules_.advancedEffects && (statusFlags(sim, e) & (kStStun | kStRoot)));
        if (ps.dir.length().raw > 0 && !stunned) {
            Vec3 d = ps.dir * (stat(sim, e, Stat::Speed) * dt);
            auto ok = [&](Vec3 np) {
                if ((t.layer & kLayerAir) && !g.airSolid) return true;
                int cx0 = g.cellX(tr.pos.x), cy0 = g.cellY(tr.pos.z), cx = g.cellX(np.x), cy = g.cellY(np.z);
                if (cx == cx0 && cy == cy0) return true;
                if (cx != cx0 && cy != cy0) return g.canStep(cx0, cy0, cx, cy0, t.pathLayer) && g.canStep(cx, cy0, cx, cy, t.pathLayer) && g.canStep(cx0, cy0, cx, cy, t.pathLayer);
                return g.canStep(cx0, cy0, cx, cy, t.pathLayer);
            };
            Vec3 np = tr.pos + d;
            if (ok(np)) tr.pos = np;
            else if (Vec3 nx = tr.pos + Vec3{d.x, Fixed{}, Fixed{}}; ok(nx)) tr.pos = nx;
            else if (Vec3 nz = tr.pos + Vec3{Fixed{}, Fixed{}, d.z}; ok(nz)) tr.pos = nz;
            u.facing = ps.dir / ps.dir.length();
        }
        auto* c = w.tryGet<Combat>(e);
        if (!c) return;
        c->autoTarget = false;
        if (!ps.fire) { c->target = kNullEntity; return; }
        const uint8_t owner = w.get<Owner>(e).player;
        if (w.alive(ps.target) && w.has<Unit>(ps.target) && hostile(sim, owner, w.get<Owner>(ps.target).player)) { c->target = ps.target; return; }
        // fire at the nearest enemy in front
        Fixed range = maxWeaponRange(t) + stat(sim, e, Stat::Range) + kHalf;
        Entity best;
        Fixed bestD{};
        gs.hash.query(tr.pos, range, [&](Entity s) {
            auto* su = w.tryGet<Unit>(s);
            if (!su || su->hp.raw <= 0 || rules_.types[su->type].invulnerable || !hostile(sim, owner, w.get<Owner>(s).player)) return;
            Vec3 dd = flat(w.get<Transform>(s).pos - tr.pos);
            Fixed dl = dd.length();
            if (dl > range || dd.dot(u.facing).raw < 0) return;
            if (!best.valid() || dl < bestD || (dl == bestD && s.index < best.index)) { best = s; bestD = dl; }
        });
        c->target = best;
    });
}

// ================================================================= per-tick system

void RtsModule::sysDungeon(Simulation& sim) {
    World& w = sim.world();
    GameState& gs = state(sim);
    MapGrid& g = gs.grid;
    const uint32_t tick = sim.tick();
    const Fixed dt = sim.dt();
    const size_t np = gs.players.size();
    auto alivePlayer = [&](uint8_t p) { auto* pi = sim.player(p); return pi && pi->alive; };

    // ---- possession upkeep
    for (size_t p = 1; p < np && p < gs.possessed.size(); ++p) {
        Entity e = gs.possessed[p];
        if (!e.valid()) continue;
        bool drop = !w.alive(e) || !w.has<Possessed>(e);
        if (!drop && !rules_.possession.drain.empty()) {
            std::vector<Amount> c = rules_.possession.drain;
            for (auto& a : c) a.value = a.value * dt;
            if (!pay(sim, uint8_t(p), c, {}, false)) drop = true;
        }
        if (drop) {
            if (w.alive(e)) { w.get<Unit>(e).flags &= uint8_t(~kUnitPossessed); w.remove<Possessed>(e); }
            gs.possessed[p] = kNullEntity;
            sim.emit({kEvPossessed, uint8_t(p), kNullEntity, e, 0, {}});
        }
    }

    // ---- player ability charges rearm (paying the rearm cost)
    for (size_t p = 1; p < np && p < gs.playerAbilities.size(); ++p)
        for (size_t k = 0; k < rules_.playerAbilities.size(); ++k) {
            const PlayerAbilityDef& a = rules_.playerAbilities[k];
            PlayerAbilityState& st = gs.playerAbilities[p][k];
            if (a.chargesMax <= 0 || st.charges >= a.chargesMax) { st.rearm = Fixed{}; continue; }
            st.rearm += dt;
            if (st.rearm < a.rearmTime) continue;
            if (pay(sim, uint8_t(p), a.rearmCost, {}, false)) { st.charges++; st.rearm = Fixed{}; }
            else st.rearm = a.rearmTime;
        }

    // ---- territory: structures follow the ground under / next to them
    if (rules_.territory.enabled && tick % 20 == 0) {
        struct Flip { Entity e; uint8_t to, from; };
        std::vector<Flip> flips;
        w.each<Unit, Owner, Footprint>([&](Entity e, Unit& u, Owner& o, Footprint& f) {
            const UnitType& t = rules_.types[u.type];
            if (t.claimedByTerritory) {
                std::vector<int> cnt(np, 0);
                for (int y = f.y - 1; y <= f.y + f.h; ++y)
                    for (int x = f.x - 1; x <= f.x + f.w; ++x) {
                        bool edge = x == f.x - 1 || x == f.x + f.w || y == f.y - 1 || y == f.y + f.h;
                        if (!edge) continue;
                        const int ec = footCell(g, f, x, y);
                        if (ec < 0) continue;
                        uint8_t tw = g.territory[size_t(ec)];
                        if (tw > 0 && tw < np) cnt[tw]++;
                    }
                if (o.player < np && o.player != 0 && cnt[o.player] > 0) return;
                int best = 0;
                for (size_t p = 1; p < np; ++p) if (cnt[p] > (best ? cnt[size_t(best)] : 0)) best = int(p);
                if (best > 0 && best != o.player) flips.push_back({e, uint8_t(best), o.player});
                return;
            }
            if (!t.capturable || u.hp.raw <= 0 || o.player == kHostileNeutral) return;
            int cx = f.x + f.w / 2, cy = f.y + f.h / 2;
            if (!g.inside(cx, cy)) return;
            uint8_t tw = g.territory[size_t(g.index(cx, cy))];
            if (tw != 0 && tw != o.player) flips.push_back({e, tw, o.player});
        });
        for (auto& fl : flips) {
            w.get<Owner>(fl.e).player = fl.to;
            if (auto* p = w.tryGet<Production>(fl.e)) p->queue.clear();
            if (auto* f = w.tryGet<Footprint>(fl.e))
                for (int y = f->y; y < f->y + f->h; ++y)
                    for (int x = f->x; x < f->x + f->w; ++x) if (g.inside(x, y)) setTerritory(sim, g.index(x, y), fl.to);
            sim.emit({kEvCaptured, fl.to, fl.e, kNullEntity, fl.from, w.get<Transform>(fl.e).pos});
        }
    }

    // ---- reinforced walls stand only while their keeper owns floor next to them
    if (rules_.territory.fortifyTime.raw > 0 && tick % 20 == 10)
        for (size_t i = 0; i < g.wall.size(); ++i) {
            if (!g.wall[i]) continue;
            if (!g.dig[i]) { g.wall[i] = 0; continue; }
            if (adjacentOwned(g, int(i), g.wall[i])) continue;
            g.wall[i] = 0;
            gs.territoryDirty = true;
            sim.emit({kEvFortified, 0, kNullEntity, kNullEntity, int64_t(i), g.centerOf(int(i))});
        }

    // ---- area claims by units (territory war front line)
    if (rules_.territory.enabled && rules_.territory.claimBy != ClaimBy::Workers && tick % 10 == 0) {
        const Fixed step = dt * 10;
        w.each<ClaimState, Unit, Owner, Transform>([&](Entity, ClaimState& cs, Unit& u, Owner& o, Transform& tr) {
            if (o.player == 0 || o.player >= np || u.hp.raw <= 0 || flagged(u, kUnitInHand | kUnitUnderConstruction)) return;
            const UnitType& t = rules_.types[u.type];
            cs.acc = fxMin(cs.acc + t.claimRate * step, Fixed::fromInt(4));
            const int r = int((t.claimRadius + Fixed::fromRaw((int64_t(1) << Fixed::kFracBits) - 1)).floorInt());
            int cx = g.cellX(tr.pos.x), cy = g.cellY(tr.pos.z);
            const int c0 = g.surf ? g.cellOf(tr.pos) : -1;
            if (g.surf) { if (c0 < 0) return; cx = c0 % g.w; cy = c0 / g.w; }
            const Fixed r2 = t.claimRadius * t.claimRadius;
            while (cs.acc >= kOne) {
                int pick = -1, pickD = 0;
                for (int y = cy - r; y <= cy + r; ++y)
                    for (int x = cx - r; x <= cx + r; ++x) {
                        int i;
                        if (g.surf) { i = g.offsetCell(c0, x - cx, y - cy); if (i < 0) continue; }   // planets: across seams
                        else { if (!g.inside(x, y)) continue; i = g.index(x, y); }
                        int d2 = (x - cx) * (x - cx) + (y - cy) * (y - cy);
                        if (Fixed::fromInt(d2) > r2 || (pick >= 0 && d2 >= pickD)) continue;
                        if (g.territory[size_t(i)] == o.player || !(g.flags[size_t(i)] & kCellGround)) continue;
                        if (!adjacentOwned(g, i, o.player)) continue;
                        bool contested = false;
                        Vec3 cc = g.surf ? g.centerOf(i) : g.cellCenter(x, y);
                        gs.hash.query(cc, Fixed::fromRatio(3, 2), [&](Entity s) {
                            auto* su = w.tryGet<Unit>(s);
                            if (contested || !su || su->hp.raw <= 0 || rules_.types[su->type].structure) return;
                            if (hostile(sim, o.player, w.get<Owner>(s).player) && dist2D(w.get<Transform>(s).pos, cc) <= Fixed::fromRatio(3, 2)) contested = true;
                        });
                        if (contested) continue;
                        pick = i; pickD = d2;
                    }
                if (pick < 0) { cs.acc = fxMin(cs.acc, kOne); break; }
                setTerritory(sim, pick, o.player);
                cs.acc -= kOne;
            }
        });
    }

    // ---- worker jobs: dig marked cells, claim territory (idle workers, staggered)
    {
        struct Job { Entity e; OrderType type; Vec3 a, b; int32_t param = 0; };
        std::vector<Job> jobs;
        std::vector<Entity> idle;
        bool anyWorker = false;
        w.each<Orders, Unit, Owner>([&](Entity e, Orders& ord, Unit& u, Owner& o) {
            const UnitType& t = rules_.types[u.type];
            const bool claimer = t.claimerTime.raw > 0 && rules_.territory.enabled && rules_.territory.claimBy != ClaimBy::Units;
            if (t.digRate.raw <= 0 && !claimer) return;
            anyWorker = true;
            if (!ord.queue.empty() || o.player == 0 || o.player >= np || u.hp.raw <= 0 || flagged(u, kUnitInHand | kUnitPossessed)) return;
            if ((tick + e.index) % 10 != 0) return;
            idle.push_back(e);
        });
        if (anyWorker && !idle.empty()) {
            std::vector<uint8_t> digTaken(g.dig.size(), 0), claimTaken(g.dig.size(), 0);
            w.each<Orders>([&](Entity, Orders& ord) {
                if (ord.queue.empty()) return;
                const Order& o = ord.queue.front();
                if (o.type == OrderType::Dig) { const int c = g.cellOf(o.point2); if (c >= 0) digTaken[size_t(c)]++; }
                if (o.type == OrderType::ClaimCell) { const int c = g.cellOf(o.param == 1 ? o.point2 : o.point); if (c >= 0) claimTaken[size_t(c)] = 1; }
            });
            for (Entity e : idle) {
                const UnitType& t = rules_.types[w.get<Unit>(e).type];
                const uint8_t owner = w.get<Owner>(e).player;
                const uint8_t bit = markBit(owner);
                const bool claimer = t.claimerTime.raw > 0 && rules_.territory.enabled && rules_.territory.claimBy != ClaimBy::Units;
                Vec3 p = w.get<Transform>(e).pos;
                const int sc = g.cellOf(p);
                if (sc < 0) continue;
                int claimCell = -1, digCell = -1, standCell = -1, wallCell = -1, wallStand = -1;
                const bool fortifier = claimer && rules_.territory.fortifyTime.raw > 0;
                bfsCells(g, sc, t.pathLayer, 6000, [&](int c) {
                    if (t.digRate.raw > 0 && bit)
                        for (int k = 0; k < 4; ++k) {
                            const int nc = nb4(g, c, k);
                            if (nc < 0) continue;
                            size_t n = size_t(nc);
                            if (g.dig[n] && (g.digMark[n] & bit) && digTaken[n] < 2) { digCell = int(n); standCell = c; return true; }
                        }
                    if (claimer && claimCell < 0 && g.territory[size_t(c)] != owner && !g.blocked[size_t(c)] && !claimTaken[size_t(c)] && adjacentOwned(g, c, owner)) {
                        claimCell = c;
                        if (t.digRate.raw <= 0) return true;
                    }
                    // reinforce plain rock next to own floor (Dungeon Keeper walls); dig and claim come first
                    if (fortifier && wallCell < 0 && g.territory[size_t(c)] == owner && !g.blocked[size_t(c)])
                        for (int k = 0; k < 4; ++k) {
                            const int nc = nb4(g, c, k);
                            if (nc < 0) continue;
                            size_t n = size_t(nc);
                            if (!g.dig[n] || g.wall[n] == owner || (g.digMark[n] & bit) || claimTaken[n] || !rules_.diggables[size_t(g.dig[n] - 1)].fortifiable) continue;
                            if (g.wall[n] != 0) continue;   // another keeper's wall: take the floor next to it first
                            wallCell = int(n); wallStand = c; break;
                        }
                    return false;
                });
                if (digCell >= 0) {
                    digTaken[size_t(digCell)]++;
                    jobs.push_back({e, OrderType::Dig, g.centerOf(standCell), g.centerOf(digCell)});
                } else if (claimCell >= 0) {
                    claimTaken[size_t(claimCell)] = 1;
                    jobs.push_back({e, OrderType::ClaimCell, g.centerOf(claimCell), {}});
                } else if (wallCell >= 0) {
                    claimTaken[size_t(wallCell)] = 1;
                    jobs.push_back({e, OrderType::ClaimCell, g.centerOf(wallStand), g.centerOf(wallCell), 1});
                }
            }
            for (auto& j : jobs) { Order o; o.type = j.type; o.point = j.a; o.point2 = j.b; o.param = j.param; issue(sim, j.e, o, false); }
        }
    }

    // ---- creature needs, wages and morale
    if (tick % 10 == 0) {
        const Fixed step = dt * 10;
        struct Act { Entity e; int kind; int room; int cell; bool leave; bool remove; Entity at = kNullEntity; };
        std::vector<Act> acts;
        std::vector<Entity> portals;
        w.each<PortalState, Owner>([&](Entity e, PortalState&, Owner&) { portals.push_back(e); });
        w.each<Needs, Unit, Owner, Transform>([&](Entity e, Needs& n, Unit& u, Owner& o, Transform& tr) {
            if (o.player == 0 || o.player >= np || u.hp.raw <= 0 || flagged(u, kUnitInHand | kUnitPossessed)) return;
            const NeedsDef& nd = rules_.types[u.type].needs;
            if (nd.hunger.enabled) n.hunger += step / nd.hunger.interval;
            if (nd.sleep.enabled) n.sleep += step / nd.sleep.interval;
            if (nd.payInterval.raw > 0) {
                n.payTimer += step;
                if (n.payTimer >= nd.payInterval) {
                    n.payTimer -= nd.payInterval;
                    bool paid = pay(sim, o.player, nd.pay, {}, false);
                    if (!paid) n.happiness -= nd.unpaidLoss;
                    sim.emit({kEvPayday, o.player, e, kNullEntity, paid ? 1 : 0, tr.pos});
                }
            }
            int unmet = (nd.hunger.enabled && n.hunger >= kOne) + (nd.sleep.enabled && n.sleep >= kOne);
            if (nd.hunger.enabled && n.hunger >= kOne && nd.hunger.starve.raw > 0) u.hp -= nd.hunger.starve * step; // starving
            if (nd.sleep.enabled && n.sleep >= kOne && nd.sleep.starve.raw > 0) u.hp -= nd.sleep.starve * step;
            Fixed top = fxMax(kOne, nd.happyStart);
            if (unmet) n.happiness -= nd.unmetRate * unmet * step;
            else n.happiness = fxMin(top, n.happiness + nd.recovery * step);
            n.happiness = fxMax(n.happiness, Fixed{});
            n.unhappyTime = n.happiness < nd.leaveBelow ? n.unhappyTime + step : Fixed{};
            if (!n.leaving && n.unhappyTime >= nd.leaveAfter && nd.leaveAfter.raw > 0) n.leaving = true;
            auto* ord = w.tryGet<Orders>(e);
            if (n.leaving) {
                // walk out through the nearest own portal (or vanish when there is none)
                Entity best; Fixed bestD{};
                for (Entity pe : portals) {
                    if (!w.alive(pe) || w.get<Owner>(pe).player != o.player) continue;
                    Fixed d = dist2D(w.get<Transform>(pe).pos, tr.pos);
                    if (!best.valid() || d < bestD) { best = pe; bestD = d; }
                }
                if (!best.valid() || contactDistance(sim, e, best) < Fixed::fromRatio(3, 2)) acts.push_back({e, 0, 0, -1, true, true});
                else if (ord && (ord->queue.empty() || ord->queue.front().type != OrderType::Move)) {
                    Vec3 pp = w.get<Transform>(best).pos;
                    acts.push_back({e, -1, 0, g.surf ? g.cellOf(pp) : g.index(g.cellX(pp.x), g.cellY(pp.z)), true, false});
                }
                return;
            }
            if (!ord) return;
            const Order* front = ord->queue.empty() ? nullptr : &ord->queue.front();
            auto* c = w.tryGet<Combat>(e);
            bool fighting = c && w.alive(c->target);
            bool free = !front || (!fighting && (front->type == OrderType::AttackMove || front->type == OrderType::Patrol)) ||
                        (front->type == OrderType::Need && (front->param & 0xFF) == kNeedWork);
            if (front && front->type == OrderType::Need && (front->param & 0xFF) != kNeedWork) free = false;
            if (!free || fighting) return;
            const NeedDef* needs[2] = {&nd.hunger, &nd.sleep};
            Fixed level[2] = {n.hunger, n.sleep};
            for (int k = 0; k < 2; ++k) {
                if (!needs[k]->enabled || level[k] < kOne) continue;
                if (needs[k]->structure != kNoType) {
                    Entity best; Fixed bestD{};
                    w.each<Unit, Owner, Transform>([&](Entity s, Unit& su, Owner& so, Transform& st) {
                        if (so.player != o.player || su.type != needs[k]->structure || (su.flags & kUnitUnderConstruction)) return;
                        Fixed d = dist2D(st.pos, tr.pos);
                        if (!best.valid() || d < bestD) { best = s; bestD = d; }
                    });
                    if (best.valid()) { acts.push_back({e, k, 0, 0, false, false, best}); return; }
                    continue;
                }
                if (needs[k]->room < 0) continue;
                int cell = nearestRoomCell(gs, o.player, needs[k]->room, tr.pos);
                if (cell >= 0) { acts.push_back({e, k, needs[k]->room, cell, false, false}); return; }
            }
            if (!front && !nd.works.empty() && (tick / 10 + e.index) % 3 == 0) {
                int bestCell = -1, bestRoom = -1;
                Fixed bestD{};
                for (uint16_t r : nd.works) {
                    int cell = nearestRoomCell(gs, o.player, r, tr.pos);
                    if (cell < 0) continue;
                    Fixed d = dist2D(g.centerOf(cell), tr.pos);
                    if (bestCell < 0 || d < bestD) { bestCell = cell; bestRoom = r; bestD = d; }
                }
                if (bestCell >= 0) acts.push_back({e, kNeedWork, bestRoom, bestCell, false, false});
            }
        });
        for (auto& a : acts) {
            if (!w.alive(a.e)) continue;
            if (a.remove) {
                Unit& u = w.get<Unit>(a.e);
                u.hp = Fixed{};
                u.flags |= kUnitSilent;
                sim.emit({kEvCreatureLeft, w.get<Owner>(a.e).player, a.e, kNullEntity, u.type, w.get<Transform>(a.e).pos});
                continue;
            }
            Order o;
            o.point = g.surf && a.cell < 0 ? w.get<Transform>(a.e).pos : g.centerOf(a.cell);
            if (a.at.valid()) { o.target = a.at; o.point = w.get<Transform>(a.at).pos; }
            if (a.leave) { o.type = OrderType::Move; o.param = 1; }
            else { o.type = OrderType::Need; o.param = int32_t(a.kind | (a.room << 8)); }
            issue(sim, a.e, o, false);
        }
    }

    // ---- portals: recruit attracted creatures up to the capacity rooms provide
    {
        struct Req { Entity portal; uint8_t owner; };
        std::vector<Req> reqs;
        w.each<PortalState, Unit, Owner>([&](Entity e, PortalState& ps, Unit& u, Owner& o) {
            if (o.player == 0 || o.player >= np || flagged(u, kUnitUnderConstruction) || !alivePlayer(o.player)) return;
            ps.timer += dt;
            const UnitType& t = rules_.types[u.type];
            if (ps.timer < t.portalInterval) return;
            ps.timer = Fixed{};
            reqs.push_back({e, o.player});
        });
        for (auto& rq : reqs) {
            const UnitType& pt = rules_.types[w.get<Unit>(rq.portal).type];
            auto& eco = gs.players[rq.owner];
            std::vector<uint16_t> cands;
            std::vector<Fixed> weights;
            Fixed total{};
            auto consider = [&](uint16_t ti) {
                const UnitType& t = rules_.types[ti];
                if (!t.attractable && pt.portalPool.empty()) return;
                if (!canTrain(sim, rq.owner, ti)) return;
                if (eco.supplyUsed + t.supplyUsed > eco.supplyCap) return;
                for (auto& [room, cells] : t.attract.rooms) if (roomCellsOf(sim, rq.owner, room) < cells) return;
                for (auto& a : t.attract.resources) if (eco.stock[a.res] < a.value) return;
                cands.push_back(ti);
                weights.push_back(t.attract.weight);
                total += t.attract.weight;
            };
            if (!pt.portalPool.empty()) for (uint16_t ti : pt.portalPool) consider(ti);
            else for (size_t ti = 0; ti < rules_.types.size(); ++ti) consider(uint16_t(ti));
            if (cands.empty() || total.raw <= 0) continue;
            Fixed roll = sim.rng().unit() * total;
            size_t pick = 0;
            for (; pick + 1 < cands.size(); ++pick) { if (roll < weights[pick]) break; roll -= weights[pick]; }
            const uint16_t type = cands[pick];
            Vec3 spot = freeSpotNear(sim, rq.portal, rules_.types[type].pathLayer);
            Entity n = spawn(sim, type, rq.owner, spot);
            eco.supplyUsed += rules_.types[type].supplyUsed;
            sim.emit({kEvRecruited, rq.owner, n, rq.portal, type, spot});
        }
    }

    // ---- auto-training structures push units to the nearest front
    auto frontFrom = [&](uint8_t owner, Vec3 from, Vec3& out) -> bool {
        if (rules_.territory.enabled && g.surf) {
            // planets: breadth-first over the body's cells (across seams), nearest front cell in steps
            const int c0 = g.cellOf(from);
            if (c0 >= 0) {
                std::vector<int> q{c0};
                std::vector<uint8_t> seen(g.territory.size(), 0);
                seen[size_t(c0)] = 1;
                for (size_t qi = 0; qi < q.size(); ++qi) {
                    const int c = q[qi];
                    if (g.territory[size_t(c)] != owner && (g.flags[size_t(c)] & kCellGround) && adjacentOwned(g, c, owner)) { out = g.centerOf(c); return true; }
                    for (int k = 0; k < 4; ++k) {
                        const int n = g.surf->neighbor(c, k);
                        if (n >= 0 && !seen[size_t(n)]) { seen[size_t(n)] = 1; q.push_back(n); }
                    }
                }
            }
        } else if (rules_.territory.enabled) {
            int cx = g.cellX(from.x), cy = g.cellY(from.z);
            for (int r = 0; r <= std::max(g.w, g.h); ++r) {
                int best = -1, bestD = 0;
                for (int dy = -r; dy <= r; ++dy)
                    for (int dx = -r; dx <= r; ++dx) {
                        if (std::abs(dx) != r && std::abs(dy) != r) continue;
                        int x = cx + dx, y = cy + dy;
                        if (!g.inside(x, y)) continue;
                        size_t i = size_t(g.index(x, y));
                        if (g.territory[i] == owner || !(g.flags[i] & kCellGround) || !adjacentOwned(g, int(i), owner)) continue;
                        int d2 = dx * dx + dy * dy;
                        if (best < 0 || d2 < bestD) { best = int(i); bestD = d2; }
                    }
                if (best >= 0) { out = g.cellCenter(best % g.w, best / g.w); return true; }
            }
        }
        bool found = false;
        Fixed bestD{};
        w.each<Unit, Owner, Transform>([&](Entity, Unit& u, Owner& o, Transform& t) {
            if (!hostile(sim, owner, o.player) || o.player == kHostileNeutral || u.hp.raw <= 0 || !rules_.types[u.type].countsForVictory) return;
            Fixed d = dist2D(t.pos, from);
            if (!found || d < bestD) { found = true; bestD = d; out = t.pos; }
        });
        return found;
    };
    {
        struct Rel { Entity from; uint16_t type; uint8_t owner; };
        std::vector<Rel> rel;
        w.each<AutoTrainState, Unit, Owner>([&](Entity e, AutoTrainState& at, Unit& u, Owner& o) {
            if (o.player == 0 || o.player >= np || flagged(u, kUnitUnderConstruction | kUnitDisabled) || !alivePlayer(o.player)) return;
            const UnitType& t = rules_.types[u.type];
            auto& eco = gs.players[o.player];
            if (eco.supplyUsed + rules_.types[t.autoTrainUnit].supplyUsed > eco.supplyCap && rules_.types[t.autoTrainUnit].supplyUsed.raw > 0) return;
            at.timer += dt * eco.productionMult * stat(sim, e, Stat::ProductionSpeed);
            if (at.timer < t.autoTrainInterval) return;
            at.timer -= t.autoTrainInterval;
            rel.push_back({e, t.autoTrainUnit, o.player});
        });
        for (auto& r : rel) {
            const UnitType& ut = rules_.types[r.type];
            if (!canAfford(sim, r.owner, ut.cost, ut.supplyUsed) || !pay(sim, r.owner, ut.cost, ut.supplyUsed, false)) continue;
            Vec3 spot = freeSpotNear(sim, r.from, ut.pathLayer);
            Entity n = spawn(sim, r.type, r.owner, spot);
            sim.emit({kEvTrainDone, r.owner, n, r.from, r.type, spot});
            if (!rules_.types[w.get<Unit>(r.from).type].autoTrainAdvance) continue;   // births stay home
            w.add<Advancer>(n);
            Vec3 goal;
            auto* prod = w.tryGet<Production>(r.from);
            if (prod && prod->hasRally) goal = prod->rally;
            else if (!frontFrom(r.owner, spot, goal)) continue;
            Order o; o.type = OrderType::AttackMove; o.point = flat(goal); o.param = 4;
            issue(sim, n, o, false);
        }
    }
    if (tick % 20 == 0) {
        std::vector<std::pair<Entity, Vec3>> go;
        w.each<Advancer, Orders, Unit, Owner, Transform>([&](Entity e, Advancer&, Orders& ord, Unit& u, Owner& o, Transform& tr) {
            if (!ord.queue.empty() || flagged(u, kUnitInHand | kUnitPossessed) || u.hp.raw <= 0) return;
            if (auto* c = w.tryGet<Combat>(e); c && w.alive(c->target)) return;
            Vec3 goal;
            if (frontFrom(o.player, tr.pos, goal) && dist2D(goal, tr.pos) > Fixed::fromRatio(1, 2)) go.push_back({e, goal});
        });
        for (auto& [e, p] : go) { Order o; o.type = OrderType::AttackMove; o.point = flat(p); o.param = 4; issue(sim, e, o, false); }
    }
}

// ================================================================= hashing

void RtsModule::hashDungeon(Simulation& sim, Hasher& h) {
    World& w = sim.world();
    GameState& gs = state(sim);
    const MapGrid& g = gs.grid;
    for (size_t i = 0; i < g.dig.size(); ++i) {
        h.u32(uint32_t(g.dig[i]) | uint32_t(g.digMark[i]) << 8 | uint32_t(g.territory[i]) << 16 | uint32_t(g.flags[i]) << 24);
        h.i32(g.room[i]);
        if (g.wall[i]) h.u32(uint32_t(g.wall[i]) | uint32_t(i) << 8);
        if (g.dig[i]) h.fx(g.digHp[i]);
    }
    for (int c : gs.territoryCells) h.i32(c);
    for (auto& hand : gs.hand) { h.u32(uint32_t(hand.size())); for (Entity e : hand) h.u64(e.packed()); }
    for (Entity e : gs.possessed) h.u64(e.packed());
    for (auto& pl : gs.playerAbilities) for (auto& st : pl) { h.u32(st.readyAt); h.i32(st.charges); h.fx(st.rearm); }
    w.each<Needs>([&](Entity e, Needs& n) { h.u64(e.packed()); h.fx(n.hunger); h.fx(n.sleep); h.fx(n.happiness); h.fx(n.payTimer); h.fx(n.unhappyTime); h.u32(n.leaving); });
    w.each<PortalState>([&](Entity e, PortalState& p) { h.u64(e.packed()); h.fx(p.timer); });
    w.each<AutoTrainState>([&](Entity e, AutoTrainState& p) { h.u64(e.packed()); h.fx(p.timer); });
    w.each<ClaimState>([&](Entity e, ClaimState& p) { h.u64(e.packed()); h.fx(p.acc); });
    w.each<Possessed>([&](Entity e, Possessed& p) { h.u64(e.packed()); h.v3(p.dir); h.u32(p.fire); h.u32(p.lastTick); });
}

} // namespace rts::game
