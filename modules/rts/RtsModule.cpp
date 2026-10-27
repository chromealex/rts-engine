#include "RtsModule.h"
#include <algorithm>
#include <stdexcept>

namespace rts::game {

namespace {
const Fixed kHalf = Fixed::fromRatio(1, 2);
const Fixed kOne = Fixed::fromInt(1);

Fixed dist2D(Vec3 a, Vec3 b) { return (a - b).xz().length(); }
Vec3 flat(Vec3 v) { v.y = Fixed{}; return v; }

bool canHit(const WeaponDef& w, uint8_t layer) { return (w.targets & layer) != 0; }

Fixed maxWeaponRange(const UnitType& t) {
    Fixed r{};
    for (auto& w : t.weapons) r = fxMax(r, w.range);
    return r;
}
} // namespace

// ================================================================= setup

void RtsModule::setup(Simulation& sim, const Json& rulesJson, const Json& map) {
    rules_ = loadRules(rulesJson);
    World& w = sim.world();
    GameState& gs = w.setResource<GameState>();

    int mw = int(map["size"][0].integer(64)), mh = int(map["size"][1].integer(64));
    if (mw < 4 || mh < 4 || mw > 1024 || mh > 1024) throw std::runtime_error("map: size must be 4..1024");
    gs.grid.init(mw, mh);
    const Json& terrain = map["terrain"];
    // "rows": array of strings, '.' ground, '~' water, '#' blocked, ',' ground (no build), '=' shallow (ground+water)
    const Json& rows = terrain["rows"];
    for (size_t y = 0; y < rows.size() && int(y) < mh; ++y) {
        std::string row = rows[y].str();
        for (size_t x = 0; x < row.size() && int(x) < mw; ++x) {
            uint8_t f = kCellGround;
            switch (row[x]) {
                case '~': f = kCellWater; break;
                case '#': f = 0; break;
                case ',': f = kCellGround | kCellNoBuild; break;
                case '=': f = kCellGround | kCellWater | kCellNoBuild; break;
                default: break;
            }
            gs.grid.flags[size_t(gs.grid.index(int(x), int(y)))] = f;
        }
    }
    for (auto& r : terrain["rects"].items()) {
        int x0 = int(r["rect"][0].integer()), y0 = int(r["rect"][1].integer());
        int rw = int(r["rect"][2].integer()), rh = int(r["rect"][3].integer());
        std::string t = r.getStr("type", "ground");
        uint8_t f = t == "water" ? uint8_t(kCellWater) : t == "blocked" ? uint8_t(0) : t == "nobuild" ? uint8_t(kCellGround | kCellNoBuild) : uint8_t(kCellGround);
        for (int y = y0; y < y0 + rh; ++y)
            for (int x = x0; x < x0 + rw; ++x)
                if (gs.grid.inside(x, y)) gs.grid.flags[size_t(gs.grid.index(x, y))] = f;
    }
    const Json& heights = terrain["heights"];
    for (size_t i = 0; i < heights.size() && i < gs.grid.height.size(); ++i) gs.grid.height[i] = int16_t(heights[i].integer());

    const size_t np = sim.players().size();
    gs.players.assign(np + 1, {});
    for (size_t p = 0; p <= np; ++p) {
        auto& e = gs.players[p];
        e.stock.assign(rules_.resources.size(), Fixed{});
        e.cap.assign(rules_.resources.size(), Fixed{});
        e.income.assign(rules_.resources.size(), Fixed{});
        e.typeCount.assign(rules_.types.size(), 0);
        for (size_t r = 0; r < rules_.resources.size(); ++r) e.stock[r] = rules_.resources[r].start;
        if (p >= 1) {
            const Json& pr = map["players"][p - 1]["resources"];
            for (auto& [rid, v] : pr.members()) {
                int ri = rules_.findResource(rid);
                if (ri >= 0) e.stock[size_t(ri)] = v.fx();
            }
        }
    }
    gs.vision.assign(np + 1, std::vector<uint8_t>(size_t(mw * mh), 0));
    gs.roadsOnly = map.getBool("roadsOnly", false);

    std::vector<std::pair<std::string, Entity>> ids;
    for (auto& ej : map["entities"].items()) {
        int t = rules_.findType(ej.getStr("type"));
        if (t < 0) throw std::runtime_error("map entity: unknown type '" + ej.getStr("type") + "'");
        int owner = int(ej.getInt("owner", 0));
        if (owner < 0 || owner > int(np)) throw std::runtime_error("map entity: owner out of range");
        const Json& pj = ej["pos"];
        Vec3 pos{pj[0].fx(), Fixed{}, pj[pj.size() >= 3 ? 2 : 1].fx()};
        Entity e = spawn(sim, uint16_t(t), uint8_t(owner), pos);
        if (auto* g = w.tryGet<Garrison>(e)) g->count = int(ej.getInt("garrison", 0));
        if (auto* rs = w.tryGet<ResourceStock>(e); rs && ej.has("amount")) rs->amount = ej.getFx("amount");
        if (ej.has("id")) ids.emplace_back(ej.getStr("id"), e);
    }
    // roads
    for (auto& r : map["roads"].items()) {
        Entity a, b;
        for (auto& [id, e] : ids) { if (id == r[0].str()) a = e; if (id == r[1].str()) b = e; }
        if (!a.valid() || !b.valid()) throw std::runtime_error("map road: unknown entity id");
        int ia = gs.roads.nodeOf(a), ib = gs.roads.nodeOf(b);
        if (ia < 0) { gs.roads.nodes.push_back(a); gs.roads.adj.emplace_back(); ia = int(gs.roads.nodes.size() - 1); }
        if (ib < 0) { gs.roads.nodes.push_back(b); gs.roads.adj.emplace_back(); ib = int(gs.roads.nodes.size() - 1); }
        gs.roads.adj[size_t(ia)].push_back(uint16_t(ib));
        gs.roads.adj[size_t(ib)].push_back(uint16_t(ia));
    }

    sim.addSystem("economy", [this](Simulation& s) { sysEconomy(s); });
    sim.addSystem("production", [this](Simulation& s) { sysProduction(s); });
    sim.addSystem("garrison", [this](Simulation& s) { sysGarrison(s); });
    sim.addSystem("orders", [this](Simulation& s) { sysOrders(s); });
    sim.addSystem("targeting", [this](Simulation& s) { sysTargeting(s); });
    sim.addSystem("movement", [this](Simulation& s) { sysMovement(s); });
    sim.addSystem("weapons", [this](Simulation& s) { sysWeapons(s); });
    sim.addSystem("projectiles", [this](Simulation& s) { sysProjectiles(s); });
    sim.addSystem("death", [this](Simulation& s) { sysDeath(s); });
    sim.addSystem("vision", [this](Simulation& s) { sysVision(s); });
    sim.addSystem("victory", [this](Simulation& s) { sysVictory(s); });
    sysEconomy(sim); // initial supply/tech counts
}

void RtsModule::footprintAt(const UnitType& t, Vec3 c, int& x0, int& y0) {
    // footprint origin so that the structure is centred on `c`, snapped to cells
    x0 = int((c.x - Fixed::fromInt(t.footprintW) / 2 + kHalf).floorInt());
    y0 = int((c.z - Fixed::fromInt(t.footprintH) / 2 + kHalf).floorInt());
}

Entity RtsModule::spawn(Simulation& sim, uint16_t type, uint8_t owner, Vec3 pos, bool underConstruction) {
    World& w = sim.world();
    GameState& gs = state(sim);
    const UnitType& t = rules_.types[type];
    Entity e = w.create();
    if (t.structure) {
        int x0, y0;
        footprintAt(t, pos, x0, y0);
        pos = {Fixed::fromInt(x0) + Fixed::fromInt(t.footprintW) / 2, Fixed{}, Fixed::fromInt(y0) + Fixed::fromInt(t.footprintH) / 2};
        w.add<Footprint>(e, Footprint{x0, y0, t.footprintW, t.footprintH});
        gs.grid.block(x0, y0, t.footprintW, t.footprintH, +1);
    }
    w.add<Transform>(e, Transform{pos, pos, Fixed{}});
    w.add<Owner>(e, Owner{owner});
    Unit u;
    u.type = type;
    u.hp = underConstruction ? fxMax(t.maxHp / 10, kOne) : t.maxHp;
    if (underConstruction) u.flags |= kUnitUnderConstruction;
    w.add<Unit>(e, u);
    if (t.speed.raw > 0) { w.add<Mobile>(e); w.add<Orders>(e); }
    if (!t.weapons.empty()) w.add<Combat>(e);
    if (!t.produces.empty()) w.add<Production>(e);
    if (!t.gathers.empty()) w.add<Cargo>(e);
    if (t.resourceNode >= 0) w.add<ResourceStock>(e, ResourceStock{t.resourceAmount});
    if (t.garrisonMax > 0 || t.spawnUnit != kNoType) w.add<Garrison>(e);
    if (underConstruction) w.add<Construction>(e);
    sim.emit({kEvSpawned, owner, e, kNullEntity, type, pos});
    return e;
}

// ================================================================= queries

bool RtsModule::hostile(Simulation& sim, uint8_t a, uint8_t b) const {
    if (a == 0 || b == 0) return false; // neutral objects are not attacked automatically
    return !sim.allied(a, b);
}

bool RtsModule::requirementsMet(Simulation& sim, uint8_t player, uint16_t type) const {
    auto* gs = sim.world().resource<GameState>();
    for (uint16_t r : rules_.types[type].requires_)
        if (gs->players[player].typeCount[r] <= 0) return false;
    return true;
}

bool RtsModule::canAfford(Simulation& sim, uint8_t player, uint16_t type, int* missing) const {
    auto* gs = sim.world().resource<GameState>();
    const auto& eco = gs->players[player];
    for (auto& c : rules_.types[type].cost)
        if (eco.stock[c.res] < c.value) { if (missing) *missing = c.res; return false; }
    const auto& t = rules_.types[type];
    if (t.supplyUsed.raw > 0 && eco.supplyUsed + t.supplyUsed > eco.supplyCap) { if (missing) *missing = 255; return false; }
    return true;
}

bool RtsModule::pay(Simulation& sim, uint8_t player, uint16_t type) {
    int missing = -1;
    if (!canAfford(sim, player, type, &missing)) {
        sim.emit({kEvNotEnough, player, kNullEntity, kNullEntity, missing, {}});
        return false;
    }
    auto& eco = state(sim).players[player];
    for (auto& c : rules_.types[type].cost) eco.stock[c.res] -= c.value;
    eco.supplyUsed += rules_.types[type].supplyUsed; // reserved until the next economy pass
    return true;
}

void RtsModule::refund(Simulation& sim, uint8_t player, uint16_t type) {
    auto& eco = state(sim).players[player];
    for (auto& c : rules_.types[type].cost) eco.stock[c.res] += c.value;
}

bool RtsModule::canPlace(Simulation& sim, uint16_t type, Vec3 centre, Entity ignore) const {
    const UnitType& t = rules_.types[type];
    if (!t.structure) return false;
    auto* gs = sim.world().resource<GameState>();
    int x0, y0;
    footprintAt(t, centre, x0, y0);
    if (!gs->grid.areaFree(x0, y0, t.footprintW, t.footprintH)) return false;
    // no ground units inside the footprint (except `ignore`, the builder)
    bool clear = true;
    World& w = const_cast<Simulation&>(sim).world();
    w.each<Unit, Transform, Mobile>([&](Entity e, Unit& u, Transform& tr, Mobile&) {
        if (e == ignore || !(rules_.types[u.type].layer & kLayerGround)) return;
        int cx = int(tr.pos.x.floorInt()), cy = int(tr.pos.z.floorInt());
        if (cx >= x0 && cx < x0 + t.footprintW && cy >= y0 && cy < y0 + t.footprintH) clear = false;
    });
    return clear;
}

bool RtsModule::isVisible(Simulation& sim, uint8_t player, Vec3 p) const {
    auto* gs = sim.world().resource<GameState>();
    if (player == 0 || player >= gs->vision.size()) return true;
    int x = int(p.x.floorInt()), y = int(p.z.floorInt());
    if (!gs->grid.inside(x, y)) return false;
    return gs->vision[player][size_t(gs->grid.index(x, y))] == 2;
}

Fixed RtsModule::contactDistance(Simulation& sim, Entity a, Entity b) {
    World& w = sim.world();
    auto* ta = w.tryGet<Transform>(a);
    auto* tb = w.tryGet<Transform>(b);
    if (!ta || !tb) return Fixed::fromInt(1 << 20);
    const UnitType& A = rules_.types[w.get<Unit>(a).type];
    const UnitType& B = rules_.types[w.get<Unit>(b).type];
    if (B.structure) {
        // distance to the footprint rectangle
        auto& f = w.get<Footprint>(b);
        Fixed px = fxClamp(ta->pos.x, Fixed::fromInt(f.x), Fixed::fromInt(f.x + f.w));
        Fixed pz = fxClamp(ta->pos.z, Fixed::fromInt(f.y), Fixed::fromInt(f.y + f.h));
        return fxMax(Vec2{ta->pos.x - px, ta->pos.z - pz}.length() - A.radius, Fixed{});
    }
    return fxMax(dist2D(ta->pos, tb->pos) - A.radius - B.radius, Fixed{});
}

Entity RtsModule::findNearestDropoff(Simulation& sim, uint8_t owner, Vec3 p, int res) {
    World& w = sim.world();
    Entity best; Fixed bestD{};
    w.each<Unit, Owner, Transform>([&](Entity e, Unit& u, Owner& o, Transform& t) {
        if (o.player != owner || (u.flags & kUnitUnderConstruction)) return;
        if (!(rules_.types[u.type].dropoffMask & (1u << res))) return;
        Fixed d = dist2D(p, t.pos);
        if (!best.valid() || d < bestD) { best = e; bestD = d; }
    });
    return best;
}

Entity RtsModule::findNearestResource(Simulation& sim, Vec3 p, int res, Fixed maxDist) {
    World& w = sim.world();
    Entity best; Fixed bestD{};
    w.each<ResourceStock, Unit, Transform>([&](Entity e, ResourceStock& rs, Unit& u, Transform& t) {
        if (rules_.types[u.type].resourceNode != res || rs.amount.raw <= 0) return;
        Fixed d = dist2D(p, t.pos);
        if (d > maxDist) return;
        if (!best.valid() || d < bestD) { best = e; bestD = d; }
    });
    return best;
}

Vec3 RtsModule::freeSpotNear(Simulation& sim, Entity s, uint8_t layer) {
    World& w = sim.world();
    GameState& gs = state(sim);
    Vec3 c = w.get<Transform>(s).pos;
    int x0 = int(c.x.floorInt()), y0 = int(c.z.floorInt());
    int fw = 1, fh = 1;
    if (auto* f = w.tryGet<Footprint>(s)) { x0 = f->x; y0 = f->y; fw = f->w; fh = f->h; }
    // ring around the footprint, starting at the bottom-right corner (towards the map centre is a later improvement)
    for (int ring = 0; ring < 8; ++ring)
        for (int y = y0 - 1 - ring; y <= y0 + fh + ring; ++y)
            for (int x = x0 - 1 - ring; x <= x0 + fw + ring; ++x) {
                bool edge = x == x0 - 1 - ring || x == x0 + fw + ring || y == y0 - 1 - ring || y == y0 + fh + ring;
                if (!edge || !gs.grid.passable(x, y, layer)) continue;
                bool occupied = false;
                Vec3 cc = gs.grid.cellCenter(x, y);
                gs.hash.query(cc, kOne, [&](Entity o) {
                    if (auto* t = w.tryGet<Transform>(o); t && dist2D(t->pos, cc) < Fixed::fromRatio(6, 10) && w.has<Mobile>(o)) occupied = true;
                });
                if (!occupied) return cc;
            }
    return c;
}

// ================================================================= commands

void RtsModule::issue(Simulation& sim, Entity e, const Order& o, bool queue) {
    World& w = sim.world();
    auto* ord = w.tryGet<Orders>(e);
    if (!ord) return;
    if (!queue) {
        ord->queue.clear();
        stopMoving(sim, e);
        if (auto* c = w.tryGet<Combat>(e)) c->target = kNullEntity;
        if (auto* u = w.tryGet<Unit>(e)) u->flags &= uint8_t(~kUnitHold);
    }
    ord->queue.push_back(o);
}

void RtsModule::applyCommand(Simulation& sim, const Command& cmd) {
    World& w = sim.world();
    auto* pi = sim.player(cmd.player);
    if (!pi || !pi->alive) return;
    const bool queue = cmd.flags & kCmdQueue;

    // owned, alive, completed objects only
    std::vector<Entity> sel;
    for (Entity e : cmd.units) {
        auto* o = w.tryGet<Owner>(e);
        auto* u = w.tryGet<Unit>(e);
        if (!o || !u || o->player != cmd.player) continue;
        if ((u->flags & kUnitUnderConstruction) && cmd.type != kCmdCancelTrain) continue;
        sel.push_back(e);
    }
    if (sel.empty()) return;
    // group arrival radius grows with the number of movers
    int movers = 0;
    for (Entity e : sel) if (w.has<Mobile>(e)) ++movers;
    Vec3 target = cmd.point;
    if (w.alive(cmd.target)) if (auto* t = w.tryGet<Transform>(cmd.target)) target = t->pos;

    for (Entity e : sel) {
        const UnitType& t = rules_.types[w.get<Unit>(e).type];
        Order o;
        o.point = flat(target);
        o.target = w.alive(cmd.target) ? cmd.target : kNullEntity;
        switch (cmd.type) {
        case kCmdMove:
            o.type = OrderType::Move; o.target = kNullEntity; o.param = movers;
            issue(sim, e, o, queue); break;
        case kCmdAttack:
            o.type = o.target.valid() ? OrderType::Attack : OrderType::AttackMove; o.param = movers;
            if (t.weapons.empty()) o.type = OrderType::Move;
            issue(sim, e, o, queue); break;
        case kCmdPatrol:
            o.type = OrderType::Patrol; o.point2 = w.get<Transform>(e).pos; o.target = kNullEntity;
            issue(sim, e, o, queue); break;
        case kCmdStop:
            if (auto* ord = w.tryGet<Orders>(e)) { ord->queue.clear(); stopMoving(sim, e); }
            if (auto* c = w.tryGet<Combat>(e)) c->target = kNullEntity;
            w.get<Unit>(e).flags &= uint8_t(~kUnitHold);
            break;
        case kCmdHold:
            if (auto* ord = w.tryGet<Orders>(e)) { ord->queue.clear(); stopMoving(sim, e); }
            w.get<Unit>(e).flags |= kUnitHold;
            break;
        case kCmdGather:
            if (t.gathers.empty() || !o.target.valid()) break;
            if (w.has<ResourceStock>(o.target)) { o.type = OrderType::Gather; issue(sim, e, o, queue); }
            else { o.type = OrderType::Return; issue(sim, e, o, queue); }
            break;
        case kCmdBuild: {
            if (cmd.param < 0 || size_t(cmd.param) >= rules_.types.size()) break;
            uint16_t bt = uint16_t(cmd.param);
            if (std::find(t.builds.begin(), t.builds.end(), bt) == t.builds.end()) break;
            if (!requirementsMet(sim, cmd.player, bt)) break;
            o.type = OrderType::Build; o.param = bt; o.target = kNullEntity; o.point = flat(cmd.point);
            issue(sim, e, o, queue);
            break;
        }
        case kCmdTrain: {
            auto* prod = w.tryGet<Production>(e);
            if (!prod || cmd.param < 0 || size_t(cmd.param) >= rules_.types.size()) break;
            uint16_t ut = uint16_t(cmd.param);
            if (std::find(t.produces.begin(), t.produces.end(), ut) == t.produces.end()) break;
            if (!requirementsMet(sim, cmd.player, ut) || int(prod->queue.size()) >= rules_.maxQueue) break;
            if (!pay(sim, cmd.player, ut)) break;
            prod->queue.push_back(ut);
            sim.emit({kEvTrainStarted, cmd.player, e, kNullEntity, ut, w.get<Transform>(e).pos});
            break;
        }
        case kCmdCancelTrain: {
            if (auto* c = w.tryGet<Construction>(e)) { // cancel a building under construction
                (void)c; refund(sim, cmd.player, w.get<Unit>(e).type); w.get<Unit>(e).hp = Fixed{}; break;
            }
            auto* prod = w.tryGet<Production>(e);
            if (!prod || prod->queue.empty()) break;
            size_t slot = cmd.param < 0 || size_t(cmd.param) >= prod->queue.size() ? prod->queue.size() - 1 : size_t(cmd.param);
            refund(sim, cmd.player, prod->queue[slot]);
            if (slot == 0) prod->progress = Fixed{};
            prod->queue.erase(prod->queue.begin() + long(slot));
            break;
        }
        case kCmdRally: {
            if (auto* prod = w.tryGet<Production>(e)) { prod->rally = flat(target); prod->hasRally = true; prod->rallyTarget = o.target; }
            break;
        }
        case kCmdSmart: {
            if (auto* prod = w.tryGet<Production>(e); prod && !w.has<Mobile>(e)) {
                prod->rally = flat(target); prod->hasRally = true; prod->rallyTarget = o.target; break;
            }
            if (!w.has<Orders>(e)) break;
            Entity tg = o.target;
            if (tg.valid()) {
                uint8_t towner = w.get<Owner>(tg).player;
                const UnitType& tt = rules_.types[w.get<Unit>(tg).type];
                auto* cargo = w.tryGet<Cargo>(e);
                if (t.entersGarrison && w.has<Garrison>(tg)) { o.type = OrderType::Enter; issue(sim, e, o, queue); break; }
                if (hostile(sim, cmd.player, towner) && !t.weapons.empty()) { o.type = OrderType::Attack; issue(sim, e, o, queue); break; }
                if (tt.resourceNode >= 0) {
                    bool can = false;
                    for (auto& g : t.gathers) if (g.res == tt.resourceNode) can = true;
                    if (can) { o.type = OrderType::Gather; issue(sim, e, o, queue); break; }
                }
                if (cargo && cargo->amount.raw > 0 && towner == cmd.player && (tt.dropoffMask & (1u << cargo->res))) {
                    o.type = OrderType::Return; issue(sim, e, o, queue); break;
                }
                if (towner == cmd.player && (w.get<Unit>(tg).flags & kUnitUnderConstruction) && !t.builds.empty()) {
                    o.type = OrderType::Build; o.param = -1; issue(sim, e, o, queue); break; // help build
                }
            }
            o.type = OrderType::Move; o.target = kNullEntity; o.param = movers;
            issue(sim, e, o, queue);
            break;
        }
        case kCmdSend: {
            auto* g = w.tryGet<Garrison>(e);
            if (!g || !w.alive(cmd.target) || cmd.target == e || !w.has<Garrison>(cmd.target) || t.spawnUnit == kNoType) break;
            int64_t pct = std::clamp<int64_t>(cmd.param, 1, 100);
            int n = int(int64_t(g->count) * pct / 100);
            if (n <= 0 && g->count > 0) n = 1;
            g->count -= n;
            g->pendingOut += n;
            g->pendingTarget = cmd.target;
            break;
        }
        default: break;
        }
    }
}

// ================================================================= economy / production

void RtsModule::sysEconomy(Simulation& sim) {
    World& w = sim.world();
    GameState& gs = state(sim);
    const Fixed dt = sim.dt();
    const size_t nr = rules_.resources.size();
    for (auto& p : gs.players) {
        std::fill(p.income.begin(), p.income.end(), Fixed{});
        std::fill(p.typeCount.begin(), p.typeCount.end(), 0);
        for (size_t r = 0; r < nr; ++r) p.cap[r] = rules_.resources[r].storage;
        p.supplyUsed = Fixed{}; p.supplyCap = Fixed{};
    }
    w.each<Unit, Owner>([&](Entity e, Unit& u, Owner& o) {
        const UnitType& t = rules_.types[u.type];
        if (u.hp < t.maxHp && t.regen.raw > 0 && !(u.flags & kUnitUnderConstruction)) u.hp = fxMin(t.maxHp, u.hp + t.regen * dt);
        if (o.player == 0) return;
        auto& p = gs.players[o.player];
        p.supplyUsed += t.supplyUsed;
        if (u.flags & kUnitUnderConstruction) return;
        p.typeCount[u.type]++;
        p.supplyCap += t.supplyProvided;
        for (auto& a : t.income) p.income[a.res] += a.value;
        for (auto& a : t.storage) p.cap[a.res] += a.value;
        if (auto* prod = w.tryGet<Production>(e)) for (uint16_t q : prod->queue) p.supplyUsed += rules_.types[q].supplyUsed;
    });
    for (size_t pi = 1; pi < gs.players.size(); ++pi) {
        auto& p = gs.players[pi];
        p.supplyCap = fxMin(p.supplyCap, rules_.supplyCap);
        for (size_t r = 0; r < nr; ++r) {
            p.stock[r] = fxMax(Fixed{}, p.stock[r] + p.income[r] * dt);
            if (rules_.resources[r].kind == ResourceKind::Flow && p.cap[r].raw > 0) p.stock[r] = fxMin(p.stock[r], p.cap[r]);
        }
    }
}

void RtsModule::sysProduction(Simulation& sim) {
    World& w = sim.world();
    const Fixed dt = sim.dt();
    // Spawning adds components (and can reallocate pools), so finished units are created after the loop.
    struct Done { Entity producer; uint16_t type; uint8_t owner; };
    std::vector<Done> done;
    w.each<Production, Unit, Owner>([&](Entity e, Production& p, Unit& u, Owner& o) {
        if ((u.flags & kUnitUnderConstruction) || p.queue.empty()) return;
        const UnitType& ut = rules_.types[p.queue.front()];
        p.progress += dt;
        if (p.progress < ut.buildTime) return;
        done.push_back({e, p.queue.front(), o.player});
        p.queue.erase(p.queue.begin());
        p.progress = Fixed{};
    });
    for (auto& d : done) {
        Vec3 spot = freeSpotNear(sim, d.producer, rules_.types[d.type].layer);
        Entity n = spawn(sim, d.type, d.owner, spot);
        sim.emit({kEvTrainDone, d.owner, n, d.producer, d.type, spot});
        auto& p = w.get<Production>(d.producer);
        if (p.hasRally && w.has<Orders>(n)) {
            Command c;
            c.player = d.owner; c.type = kCmdSmart; c.units = {n}; c.point = p.rally;
            c.target = w.alive(p.rallyTarget) ? p.rallyTarget : kNullEntity;
            applyCommand(sim, c);
        }
    }
    // construction: progresses while a builder is in contact
    w.each<Construction, Unit, Owner>([&](Entity e, Construction& c, Unit& u, Owner& o) {
        const UnitType& t = rules_.types[u.type];
        bool working = false;
        w.each<Orders>([&](Entity b, Orders& ord) {
            if (working || ord.queue.empty() || ord.queue.front().type != OrderType::Build || ord.queue.front().target != e) return;
            if (contactDistance(sim, b, e) <= Fixed::fromRatio(8, 10)) working = true;
        });
        if (!working) return;
        Fixed step = dt / fxMax(t.buildTime, Fixed::fromRatio(1, 10));
        c.progress += step;
        u.hp = fxMin(t.maxHp, u.hp + t.maxHp * step * Fixed::fromRatio(9, 10));
        if (c.progress >= kOne) {
            u.flags &= uint8_t(~kUnitUnderConstruction);
            w.remove<Construction>(e);
            sim.emit({kEvBuildDone, o.player, e, kNullEntity, u.type, w.get<Transform>(e).pos});
        }
    });
}

void RtsModule::sysGarrison(Simulation& sim) {
    World& w = sim.world();
    const Fixed dt = sim.dt();
    struct Release { Entity from, target; uint16_t unit; uint8_t owner; Fixed radius; };
    std::vector<Release> releases;
    w.each<Garrison, Unit, Owner>([&](Entity e, Garrison& g, Unit& u, Owner& o) {
        const UnitType& t = rules_.types[u.type];
        if (o.player != 0 && t.spawnInterval.raw > 0) {
            if (g.count < t.garrisonMax) {
                g.growth += dt / t.spawnInterval;
                while (g.growth >= kOne && g.count < t.garrisonMax) { ++g.count; g.growth -= kOne; }
                if (g.count >= t.garrisonMax) g.growth = Fixed{};
            } else if (g.count > t.garrisonMax) {
                g.growth -= dt * t.garrisonDecay;
                while (g.growth <= -kOne && g.count > t.garrisonMax) { --g.count; g.growth += kOne; }
            } else {
                g.growth = Fixed{};
            }
        }
        // release sent units, a few per tick so they stream out
        for (int k = 0; k < 2 && g.pendingOut > 0; ++k) {
            if (!w.alive(g.pendingTarget) || t.spawnUnit == kNoType) { g.count += g.pendingOut; g.pendingOut = 0; break; }
            --g.pendingOut;
            releases.push_back({e, g.pendingTarget, t.spawnUnit, o.player, t.radius});
        }
    });
    for (auto& r : releases) {
        {
            Entity e = r.from;
            Vec3 from = w.get<Transform>(e).pos, to = w.get<Transform>(r.target).pos;
            Vec3 d = flat(to - from);
            Fixed len = d.length();
            Vec3 start = len.raw > 0 ? from + d * ((r.radius + Fixed::fromRatio(3, 10)) / len) : from;
            Entity n = spawn(sim, r.unit, r.owner, start);
            auto* m = w.tryGet<Mobile>(n);
            if (!m) continue;
            // start the road route from this building
            GameState& gs = state(sim);
            int a = gs.roads.nodeOf(e), b = gs.roads.nodeOf(r.target);
            if (a >= 0 && b >= 0) m->roadPath = gs.roads.path(w, a, b);
            Order ord;
            ord.type = OrderType::Enter; ord.target = r.target; ord.point = flat(to);
            ord.state = m->roadPath.empty() ? 0 : 2; // 2 = road route prepared
            issue(sim, n, ord, false);
            w.get<Orders>(n).queue.front().state = ord.state;
        }
    }
}

// ================================================================= orders

void RtsModule::stopMoving(Simulation& sim, Entity e) {
    if (auto* m = sim.world().tryGet<Mobile>(e)) { m->moving = false; m->path.clear(); m->pathPos = 0; m->stuck = Fixed{}; }
}

void RtsModule::moveTo(Simulation& sim, Entity e, Vec3 goal, Fixed radius) {
    World& w = sim.world();
    auto* m = w.tryGet<Mobile>(e);
    if (!m) return;
    const UnitType& t = rules_.types[w.get<Unit>(e).type];
    m->goal = goal;
    m->goalRadius = radius;
    m->stuck = Fixed{};
    m->pathPos = 0;
    state(sim).grid.findPath(w.get<Transform>(e).pos, goal, t.layer, m->path);
    m->moving = !m->path.empty();
}

bool RtsModule::runOrder(Simulation& sim, Entity e, Order& o) {
    World& w = sim.world();
    Unit& u = w.get<Unit>(e);
    const UnitType& t = rules_.types[u.type];
    Transform& tr = w.get<Transform>(e);
    auto* m = w.tryGet<Mobile>(e);
    auto* combat = w.tryGet<Combat>(e);
    const Fixed tick = sim.dt();

    auto chase = [&](Entity target) -> bool { // returns true while target is valid
        if (!w.alive(target) || !w.has<Unit>(target)) return false;
        Fixed range = maxWeaponRange(t);
        if (contactDistance(sim, e, target) <= range) {
            if (m && m->moving) stopMoving(sim, e);
            return true;
        }
        if (!m || (u.flags & kUnitHold)) return true;
        Vec3 tp = w.get<Transform>(target).pos;
        if (!m->moving || dist2D(m->goal, tp) > kOne || (sim.tick() + e.index) % 20 == 0) moveTo(sim, e, tp, range);
        return true;
    };

    switch (o.type) {
    case OrderType::Move: {
        if (o.state == 0) {
            Fixed r = Fixed::fromRatio(3, 10) + fxSqrt(Fixed::fromInt(std::max(1, o.param))) * Fixed::fromRatio(4, 10);
            moveTo(sim, e, o.point, r);
            o.state = 1;
            return !m;
        }
        return !m || !m->moving;
    }
    case OrderType::Attack: {
        if (!combat) return true;
        combat->target = o.target; combat->autoTarget = false;
        if (!chase(o.target)) { combat->target = kNullEntity; return true; }
        return false;
    }
    case OrderType::AttackMove:
    case OrderType::Patrol: {
        if (combat && w.alive(combat->target)) {
            // fight whatever was acquired, then resume the march
            if (chase(combat->target)) { o.state = 0; return false; }
            combat->target = kNullEntity;
        }
        if (o.state == 0) {
            Fixed r = Fixed::fromRatio(3, 10) + fxSqrt(Fixed::fromInt(std::max(1, o.param))) * Fixed::fromRatio(4, 10);
            moveTo(sim, e, o.point, r);
            o.state = 1;
            return false;
        }
        if (m && !m->moving) {
            if (o.type == OrderType::AttackMove) return true;
            std::swap(o.point, o.point2);
            o.state = 0;
        }
        return false;
    }
    case OrderType::Gather: {
        auto* cargo = w.tryGet<Cargo>(e);
        if (!cargo) return true;
        if (!w.alive(o.target) || !w.has<ResourceStock>(o.target)) {
            int res = cargo->res;
            Entity n = res >= 0 ? findNearestResource(sim, cargo->lastNodePos, res, Fixed::fromInt(14)) : kNullEntity;
            if (!n.valid()) return true;
            o.target = n; o.state = 0;
        }
        const UnitType& nt = rules_.types[w.get<Unit>(o.target).type];
        const GatherDef* gd = nullptr;
        for (auto& g : t.gathers) if (g.res == nt.resourceNode) gd = &g;
        if (!gd) return true;
        if (o.state == 0) { moveTo(sim, e, w.get<Transform>(o.target).pos, nt.radius + t.radius + Fixed::fromRatio(4, 10)); o.state = 1; }
        if (o.state == 1) {
            if (contactDistance(sim, e, o.target) <= Fixed::fromRatio(6, 10)) { stopMoving(sim, e); o.state = 2; o.timer = gd->time; }
            else if (m && !m->moving) o.state = 0; // re-path
            return false;
        }
        if (o.state == 2) {
            o.timer -= tick;
            if (o.timer.raw > 0) return false;
            auto& rs = w.get<ResourceStock>(o.target);
            Fixed take = fxMin(gd->carry, rs.amount);
            rs.amount -= take;
            if (cargo->res != nt.resourceNode) cargo->amount = Fixed{};
            cargo->res = nt.resourceNode;
            cargo->amount = take;
            cargo->lastNode = o.target;
            cargo->lastNodePos = w.get<Transform>(o.target).pos;
            if (rs.amount.raw <= 0) w.get<Unit>(o.target).hp = Fixed{}; // depleted: removed by the death system
            Order ret; ret.type = OrderType::Return;
            o = ret; // become a return order (continues next tick)
            return false;
        }
        return true;
    }
    case OrderType::Return: {
        auto* cargo = w.tryGet<Cargo>(e);
        if (!cargo || cargo->res < 0 || cargo->amount.raw <= 0) return true;
        if (!w.alive(o.target) || !(rules_.types[w.get<Unit>(o.target).type].dropoffMask & (1u << cargo->res)) ||
            (w.get<Unit>(o.target).flags & kUnitUnderConstruction)) {
            o.target = findNearestDropoff(sim, w.get<Owner>(e).player, tr.pos, cargo->res);
            o.state = 0;
            if (!o.target.valid()) return true;
        }
        if (o.state == 0) { moveTo(sim, e, w.get<Transform>(o.target).pos, rules_.types[w.get<Unit>(o.target).type].radius + t.radius + Fixed::fromRatio(4, 10)); o.state = 1; }
        if (contactDistance(sim, e, o.target) <= Fixed::fromRatio(6, 10)) {
            stopMoving(sim, e);
            auto& eco = state(sim).players[w.get<Owner>(e).player];
            eco.stock[size_t(cargo->res)] += cargo->amount;
            sim.emit({kEvGathered, w.get<Owner>(e).player, e, o.target, cargo->res, tr.pos});
            cargo->amount = Fixed{};
            Order g; g.type = OrderType::Gather; g.target = w.alive(cargo->lastNode) ? cargo->lastNode : kNullEntity;
            if (!g.target.valid()) g.target = findNearestResource(sim, cargo->lastNodePos, cargo->res, Fixed::fromInt(14));
            if (!g.target.valid()) return true;
            o = g;
            return false;
        }
        if (m && !m->moving) o.state = 0;
        return false;
    }
    case OrderType::Build: {
        if (o.state == 0) {
            if (o.target.valid()) { o.state = 2; return false; } // helping an existing construction
            if (o.param < 0) return true;
            const UnitType& bt = rules_.types[size_t(o.param)];
            moveTo(sim, e, o.point, bt.radius + t.radius + Fixed::fromRatio(5, 10));
            o.state = 1;
            return false;
        }
        if (o.state == 1) {
            const UnitType& bt = rules_.types[size_t(o.param)];
            Fixed d = dist2D(tr.pos, o.point) - bt.radius - t.radius;
            if (d > kOne && m && m->moving) return false;
            if (d > Fixed::fromInt(2)) { o.state = 0; return m && !m->moving && d > Fixed::fromInt(4); }
            stopMoving(sim, e);
            uint8_t owner = w.get<Owner>(e).player;
            if (!canPlace(sim, uint16_t(o.param), o.point, e)) return true;
            if (!pay(sim, owner, uint16_t(o.param))) return true;
            Entity s = spawn(sim, uint16_t(o.param), owner, o.point, true); // NOTE: invalidates component references
            w.get<Construction>(s).builder = e;
            sim.emit({kEvBuildPlaced, owner, s, e, o.param, o.point});
            o.target = s;
            o.state = 2;
            // step out of the footprint if we ended up inside it
            auto& f = w.get<Footprint>(s);
            Transform& tr2 = w.get<Transform>(e);
            int cx = int(tr2.pos.x.floorInt()), cy = int(tr2.pos.z.floorInt());
            if (cx >= f.x && cx < f.x + f.w && cy >= f.y && cy < f.y + f.h) { Vec3 spot = freeSpotNear(sim, s, t.layer); w.get<Transform>(e).pos = spot; }
            return false;
        }
        // state 2: building
        if (!w.alive(o.target) || !w.has<Construction>(o.target)) return true;
        if (contactDistance(sim, e, o.target) > Fixed::fromRatio(8, 10)) {
            if (m && !m->moving) moveTo(sim, e, w.get<Transform>(o.target).pos, rules_.types[w.get<Unit>(o.target).type].radius + t.radius + Fixed::fromRatio(3, 10));
        } else if (m && m->moving) stopMoving(sim, e);
        return false;
    }
    case OrderType::Enter: {
        if (!w.alive(o.target)) {
            // target destroyed: go home to the nearest own garrison or stop
            return true;
        }
        if (o.state == 2 && m && !m->roadPath.empty()) {
            // follow the road graph node by node
            if (!m->moving) {
                GameState& gs = state(sim);
                uint16_t next = m->roadPath.front();
                Entity ne = gs.roads.nodes[next];
                if (ne == o.target) { m->roadPath.clear(); o.state = 0; }
                else {
                    Vec3 p = w.get<Transform>(ne).pos;
                    m->goal = p; m->goalRadius = Fixed::fromRatio(3, 10);
                    m->path = {p}; m->pathPos = 0; m->moving = true;
                    if (dist2D(tr.pos, p) < Fixed::fromRatio(4, 10)) m->roadPath.erase(m->roadPath.begin());
                }
            }
            if (!m->roadPath.empty()) {
                // arrived at the current node? advance
                GameState& gs = state(sim);
                Vec3 p = w.get<Transform>(gs.roads.nodes[m->roadPath.front()]).pos;
                if (dist2D(tr.pos, p) < Fixed::fromRatio(4, 10)) { m->roadPath.erase(m->roadPath.begin()); m->moving = false; }
                return false;
            }
        }
        if (o.state == 0) {
            Vec3 tp = w.get<Transform>(o.target).pos;
            m->goal = tp; m->goalRadius = Fixed{}; m->path = {tp}; m->pathPos = 0; m->moving = true;
            if (!state(sim).roadsOnly) moveTo(sim, e, tp, Fixed{});
            o.state = 1;
        }
        if (contactDistance(sim, e, o.target) > Fixed::fromRatio(2, 10)) {
            if (m && !m->moving) o.state = 0;
            return false;
        }
        // arrived: merge or assault
        auto* g = w.tryGet<Garrison>(o.target);
        Owner& so = w.get<Owner>(e);
        Owner& to = w.get<Owner>(o.target);
        const UnitType& tt = rules_.types[w.get<Unit>(o.target).type];
        if (g) {
            if (to.player == so.player || (to.player != 0 && sim.allied(to.player, so.player))) {
                ++g->count;
            } else if (g->count > 0) {
                Fixed defMult = tt.garrisonDefense;
                g->damageAcc += kOne / fxMax(defMult, Fixed::fromRatio(1, 10));
                while (g->damageAcc >= kOne && g->count > 0) { --g->count; g->damageAcc -= kOne; }
            } else if (tt.capturable) {
                uint8_t old = to.player;
                to.player = so.player;
                g->count = 1; g->damageAcc = Fixed{}; g->growth = Fixed{}; g->pendingOut = 0;
                if (auto* p = w.tryGet<Production>(o.target)) { p->queue.clear(); p->progress = Fixed{}; }
                sim.emit({kEvCaptured, so.player, o.target, e, old, w.get<Transform>(o.target).pos});
            }
        }
        u.hp = Fixed{};
        w.get<Unit>(e).flags |= kUnitHold;
        sim.emit({kEvDied, so.player, e, kNullEntity, u.type, tr.pos});
        w.destroyLater(e);
        return true;
    }
    }
    return true;
}

void RtsModule::sysOrders(Simulation& sim) {
    World& w = sim.world();
    w.each<Orders, Unit>([&](Entity e, Orders& ord, Unit&) {
        if (ord.queue.empty()) return;
        Order& o = ord.queue.front();
        if (runOrder(sim, e, o)) {
            if (!w.alive(e)) return;
            auto& q = w.get<Orders>(e).queue;
            if (!q.empty()) q.erase(q.begin());
            if (q.empty()) stopMoving(sim, e);
        }
    });
}

// ================================================================= combat

void RtsModule::sysTargeting(Simulation& sim) {
    World& w = sim.world();
    GameState& gs = state(sim);
    w.each<Combat, Unit, Owner, Transform>([&](Entity e, Combat& c, Unit& u, Owner& o, Transform& tr) {
        if (o.player == 0 || (u.flags & kUnitUnderConstruction)) return;
        if (++c.acquireTimer < 4) return;
        c.acquireTimer = 0;
        const UnitType& t = rules_.types[u.type];
        auto* ord = w.tryGet<Orders>(e);
        // explicit orders that do not want auto-targeting
        if (ord && !ord->queue.empty()) {
            OrderType ot = ord->queue.front().type;
            if (ot != OrderType::AttackMove && ot != OrderType::Patrol) return;
        }
        if (t.worker && ord && !ord->queue.empty()) return;
        if (w.alive(c.target) && w.has<Unit>(c.target)) {
            if (!c.autoTarget) return;
            if (dist2D(w.get<Transform>(c.target).pos, tr.pos) <= t.sight + kOne) return;
        }
        uint8_t layers = 0;
        for (auto& wd : t.weapons) layers |= wd.targets;
        Entity best; Fixed bestD{};
        gs.hash.query(tr.pos, t.sight, [&](Entity cand) {
            if (cand == e) return;
            auto* cu = w.tryGet<Unit>(cand);
            if (!cu) return;
            const UnitType& ct = rules_.types[cu->type];
            if (ct.invulnerable || !(ct.layer & layers) || w.has<Projectile>(cand)) return;
            if (!hostile(sim, o.player, w.get<Owner>(cand).player)) return;
            Fixed d = dist2D(w.get<Transform>(cand).pos, tr.pos);
            if (d > t.sight) return;
            // prefer things that can shoot back
            if (ct.weapons.empty()) d += Fixed::fromInt(4);
            if (!best.valid() || d < bestD) { best = cand; bestD = d; }
        });
        c.target = best;
        c.autoTarget = true;
    });
}

void RtsModule::dealDamage(Simulation& sim, Entity target, Fixed dmg, uint16_t dmgType, Entity) {
    World& w = sim.world();
    auto* u = w.tryGet<Unit>(target);
    if (!u) return;
    const UnitType& t = rules_.types[u->type];
    if (t.invulnerable) return;
    Fixed raw = dmg * rules_.damageMult(dmgType, t.armorType);
    Fixed fin = fxMax(raw - t.armor, raw / 5);
    u->hp -= fin;
    sim.emit({kEvHit, 0, target, kNullEntity, fin.raw, w.get<Transform>(target).pos});
}

void RtsModule::sysWeapons(Simulation& sim) {
    World& w = sim.world();
    const Fixed dt = sim.dt();
    struct Shot { Projectile pr; Vec3 start; };
    std::vector<Shot> shots;
    w.each<Combat, Unit, Owner, Transform>([&](Entity e, Combat& c, Unit& u, Owner& o, Transform& tr) {
        const UnitType& t = rules_.types[u.type];
        for (size_t i = 0; i < t.weapons.size() && i < 4; ++i) c.cooldown[i] = fxMax(Fixed{}, c.cooldown[i] - dt);
        if (u.flags & kUnitUnderConstruction) return;
        if (!w.alive(c.target) || !w.has<Unit>(c.target)) { c.target = kNullEntity; return; }
        const Unit& tu = w.get<Unit>(c.target);
        const UnitType& tt = rules_.types[tu.type];
        if (tu.hp.raw <= 0) return;
        // idle units with an automatic target chase it (unless holding)
        auto* ord = w.tryGet<Orders>(e);
        auto* m = w.tryGet<Mobile>(e);
        Fixed dist = contactDistance(sim, e, c.target);
        Fixed range = maxWeaponRange(t);
        if (c.autoTarget && ord && ord->queue.empty() && m && !(u.flags & kUnitHold) && dist > range) {
            if (dist > t.sight + Fixed::fromInt(2)) { c.target = kNullEntity; return; }
            if (!m->moving || (sim.tick() + e.index) % 20 == 0) moveTo(sim, e, w.get<Transform>(c.target).pos, range);
            return;
        }
        if (c.autoTarget && ord && ord->queue.empty() && m && m->moving && dist <= range) stopMoving(sim, e);
        Vec3 tp = w.get<Transform>(c.target).pos;
        Vec3 face = flat(tp - tr.pos);
        if (face.length().raw > 0) u.facing = face / face.length();
        for (size_t i = 0; i < t.weapons.size() && i < 4; ++i) {
            const WeaponDef& wd = t.weapons[i];
            if (!canHit(wd, tt.layer) || c.cooldown[i].raw > 0 || dist > wd.range || dist < wd.minRange) continue;
            if (m && m->moving && ord && !ord->queue.empty() && ord->queue.front().type == OrderType::Move) continue;
            c.cooldown[i] = wd.cooldown;
            sim.emit({kEvAttack, o.player, e, c.target, int64_t(i), tr.pos});
            if (wd.projectileSpeed.raw <= 0) {
                dealDamage(sim, c.target, wd.damage, wd.damageType, e);
                if (wd.splash.raw > 0) {
                    state(sim).hash.query(tp, wd.splash, [&](Entity s) {
                        if (s == c.target || !w.has<Unit>(s) || w.has<Projectile>(s)) return;
                        if (!hostile(sim, o.player, w.get<Owner>(s).player)) return;
                        if (dist2D(w.get<Transform>(s).pos, tp) <= wd.splash) dealDamage(sim, s, wd.damage, wd.damageType, e);
                    });
                }
            } else {
                Projectile pr;
                pr.target = c.target; pr.source = e; pr.aim = tp; pr.speed = wd.projectileSpeed;
                pr.damage = wd.damage; pr.splash = wd.splash; pr.damageType = wd.damageType; pr.owner = o.player; pr.layers = wd.targets;
                Vec3 start = tr.pos; start.y = Fixed::fromRatio(6, 10);
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
    const Fixed dt = sim.dt();
    w.each<Projectile, Transform>([&](Entity p, Projectile& pr, Transform& tr) {
        tr.prevPos = tr.pos;
        if (w.alive(pr.target) && w.has<Unit>(pr.target)) { pr.aim = w.get<Transform>(pr.target).pos; pr.aim.y = Fixed::fromRatio(4, 10); }
        if (moveTowards(tr.pos, pr.aim, pr.speed * dt)) {
            if (w.alive(pr.target) && w.has<Unit>(pr.target)) dealDamage(sim, pr.target, pr.damage, pr.damageType, pr.source);
            if (pr.splash.raw > 0) {
                state(sim).hash.query(pr.aim, pr.splash, [&](Entity s) {
                    if (s == pr.target || !w.has<Unit>(s) || w.has<Projectile>(s)) return;
                    if (!hostile(sim, pr.owner, w.get<Owner>(s).player)) return;
                    if (dist2D(w.get<Transform>(s).pos, pr.aim) <= pr.splash) dealDamage(sim, s, pr.damage, pr.damageType, pr.source);
                });
            }
            w.destroyLater(p);
        }
    });
}

// ================================================================= movement

void RtsModule::sysMovement(Simulation& sim) {
    World& w = sim.world();
    GameState& gs = state(sim);
    const Fixed dt = sim.dt();

    gs.hash.reset(gs.grid.w, gs.grid.h, 4);
    w.each<Unit, Transform>([&](Entity e, Unit&, Transform& t) { t.prevPos = t.pos; gs.hash.insert(e, t.pos); });

    // 1) desired movement along paths
    w.each<Mobile, Unit, Transform>([&](Entity, Mobile& m, Unit& u, Transform& tr) {
        if (!m.moving) return;
        const UnitType& t = rules_.types[u.type];
        if (m.goalRadius.raw > 0 && dist2D(tr.pos, m.goal) <= m.goalRadius) { m.moving = false; m.path.clear(); return; }
        Fixed step = t.speed * dt;
        while (m.pathPos < m.path.size() && step.raw > 0) {
            Vec3 wp = m.path[m.pathPos];
            Vec3 d = flat(wp - tr.pos);
            Fixed len = d.length();
            Vec3 next = len <= step ? Vec3{wp.x, tr.pos.y, wp.z} : tr.pos + d * (step / len);
            int cx = gs.grid.cellX(next.x), cy = gs.grid.cellY(next.z);
            const bool ignoreGrid = (t.layer & kLayerAir) || (gs.roadsOnly && t.entersGarrison); // road walkers pass through buildings
            if (!ignoreGrid && !gs.grid.passable(cx, cy, t.layer) &&
                gs.grid.passable(gs.grid.cellX(tr.pos.x), gs.grid.cellY(tr.pos.z), t.layer)) {
                m.stuck += dt;
                break;
            }
            if (len.raw > 0) u.facing = d / len;
            if (len <= step) { tr.pos = next; step -= len; ++m.pathPos; }
            else { tr.pos = next; step = Fixed{}; }
        }
        if (m.pathPos >= m.path.size()) { m.moving = false; m.path.clear(); }
        if (m.stuck > kOne) { // blocked by a new structure: re-path
            m.stuck = Fixed{};
            gs.grid.findPath(tr.pos, m.goal, t.layer, m.path);
            m.pathPos = 0;
        }
    });

    // 2) separation between overlapping mobile units on the same layer
    struct Push { Entity e; Vec3 d; };
    std::vector<Push> pushes;
    w.each<Mobile, Unit, Transform>([&](Entity e, Mobile& m, Unit& u, Transform& tr) {
        const UnitType& t = rules_.types[u.type];
        Vec3 acc{};
        bool any = false;
        gs.hash.query(tr.pos, t.radius * 2 + kOne, [&](Entity o) {
            if (o == e) return;
            auto* ou = w.tryGet<Unit>(o);
            if (!ou || !w.has<Mobile>(o)) return;
            const UnitType& ot = rules_.types[ou->type];
            if (!(ot.layer & t.layer)) return;
            Vec3 d = flat(tr.pos - w.get<Transform>(o).pos);
            Fixed len = d.length();
            Fixed overlap = t.radius + ot.radius - len;
            if (overlap.raw <= 0) return;
            if (len.raw == 0) { // exact overlap: deterministic direction from entity indices
                d = (e.index < o.index) ? Vec3{kOne, Fixed{}, Fixed{}} : Vec3{-kOne, Fixed{}, Fixed{}};
                len = kOne;
            }
            // moving units yield less to idle ones
            Fixed share = m.moving ? Fixed::fromRatio(1, 2) : (w.get<Mobile>(o).moving ? Fixed::fromRatio(3, 4) : Fixed::fromRatio(1, 2));
            acc += d * (overlap * share / len);
            any = true;
        });
        if (any) {
            Fixed len = acc.length(), maxPush = t.speed * dt + Fixed::fromRatio(1, 20);
            if (len > maxPush) acc = acc * (maxPush / len);
            pushes.push_back({e, acc});
        }
    });
    for (auto& p : pushes) {
        Transform& tr = w.get<Transform>(p.e);
        const UnitType& t = rules_.types[w.get<Unit>(p.e).type];
        Vec3 np = tr.pos + p.d;
        if ((t.layer & kLayerAir) || (gs.roadsOnly && t.entersGarrison) || gs.grid.passable(gs.grid.cellX(np.x), gs.grid.cellY(np.z), t.layer)) tr.pos = np;
    }
}

// ================================================================= death / vision / victory

void RtsModule::sysDeath(Simulation& sim) {
    World& w = sim.world();
    GameState& gs = state(sim);
    w.each<Unit, Owner, Transform>([&](Entity e, Unit& u, Owner& o, Transform& tr) {
        if (u.hp.raw > 0) return;
        const UnitType& t = rules_.types[u.type];
        if (t.invulnerable && t.resourceNode < 0) { u.hp = t.maxHp; return; }
        if (auto* f = w.tryGet<Footprint>(e)) gs.grid.block(f->x, f->y, f->w, f->h, -1);
        sim.emit({kEvDied, o.player, e, kNullEntity, u.type, tr.pos});
        w.destroyLater(e);
    });
}

void RtsModule::sysVision(Simulation& sim) {
    if (sim.tick() % 5 != 0) return;
    World& w = sim.world();
    GameState& gs = state(sim);
    const int W = gs.grid.w, H = gs.grid.h;
    const size_t np = gs.vision.size();
    for (size_t p = 1; p < np; ++p) for (auto& c : gs.vision[p]) if (c == 2) c = 1;
    // which players see through each player's eyes (allies share vision)
    std::vector<std::vector<uint8_t>> sharers(np);
    for (size_t p = 1; p < np; ++p) for (size_t q = 1; q < np; ++q) if (sim.allied(uint8_t(p), uint8_t(q))) sharers[p].push_back(uint8_t(q));
    w.each<Unit, Owner, Transform>([&](Entity, Unit& u, Owner& o, Transform& tr) {
        if (o.player == 0 || o.player >= np) return;
        int r = int(rules_.types[u.type].sight.toInt());
        int cx = int(tr.pos.x.floorInt()), cy = int(tr.pos.z.floorInt());
        for (int y = std::max(0, cy - r); y <= std::min(H - 1, cy + r); ++y)
            for (int x = std::max(0, cx - r); x <= std::min(W - 1, cx + r); ++x) {
                int dx = x - cx, dy = y - cy;
                if (dx * dx + dy * dy > r * r) continue;
                for (uint8_t q : sharers[o.player]) gs.vision[q][size_t(y * W + x)] = 2;
            }
    });
}

void RtsModule::sysVictory(Simulation& sim) {
    if (sim.tick() % 10 != 0) return;
    World& w = sim.world();
    const std::string& v = rules_.victory;
    for (auto& p : sim.players()) {
        if (!p.alive) continue;
        bool has = false;
        w.each<Unit, Owner>([&](Entity e, Unit& u, Owner& o) {
            if (has || o.player != p.slot) return;
            const UnitType& t = rules_.types[u.type];
            if (!t.countsForVictory) return;
            if (v == "noStructures" && !t.structure) return;
            if (v == "noUnits" && t.structure) return;
            if (v == "noStructures" && (u.flags & kUnitUnderConstruction)) return;
            (void)e;
            has = true;
        });
        if (!has) {
            p.alive = false;
            sim.emit({kEvPlayerDefeated, p.slot, kNullEntity, kNullEntity, 0, {}});
        }
    }
}

int RtsModule::result(Simulation& sim) {
    int team = 0;
    for (auto& p : sim.players()) {
        if (!p.alive) continue;
        if (team == 0) team = p.team;
        else if (team != p.team) return 0;
    }
    return team == 0 ? -1 : team;
}

void RtsModule::hashState(Simulation& sim, Hasher& h) {
    World& w = sim.world();
    GameState& gs = state(sim);
    for (auto& p : gs.players) {
        for (auto& s : p.stock) h.fx(s);
        h.fx(p.supplyUsed); h.fx(p.supplyCap);
    }
    h.u32(uint32_t(w.count<Unit>()));
    w.each<Unit, Owner, Transform>([&](Entity e, Unit& u, Owner& o, Transform& t) {
        h.u64(e.packed()); h.u32(u.type); h.fx(u.hp); h.u32(u.flags); h.u32(o.player); h.v3(t.pos);
        if (auto* ord = w.tryGet<Orders>(e)) { h.u32(uint32_t(ord->queue.size())); if (!ord->queue.empty()) { h.u32(uint32_t(ord->queue.front().type)); h.u32(ord->queue.front().state); } }
        if (auto* c = w.tryGet<Combat>(e)) h.u64(c->target.packed());
        if (auto* g = w.tryGet<Garrison>(e)) { h.i32(g->count); h.fx(g->growth); h.i32(g->pendingOut); }
        if (auto* p = w.tryGet<Production>(e)) { h.u32(uint32_t(p->queue.size())); h.fx(p->progress); }
        if (auto* r = w.tryGet<ResourceStock>(e)) h.fx(r->amount);
        if (auto* c = w.tryGet<Cargo>(e)) { h.i32(c->res); h.fx(c->amount); }
    });
    h.u32(uint32_t(w.count<Projectile>()));
    w.each<Projectile, Transform>([&](Entity e, Projectile&, Transform& t) { h.u64(e.packed()); h.v3(t.pos); });
}

} // namespace rts::game
