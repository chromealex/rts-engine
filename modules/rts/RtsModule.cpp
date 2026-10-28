// Setup, commands, shared queries and state hashing for the generic RTS module.
#include "RtsInternal.h"
#include <algorithm>
#include <stdexcept>

namespace rts::game {

using namespace detail;
namespace {
Vec3 readPos(const Json& p) { return {p[0].fx(), Fixed{}, p[p.size() >= 3 ? 2 : 1].fx()}; }
} // namespace

// ================================================================= setup

void RtsModule::setup(Simulation& sim, const Json& rulesJson, const Json& map) {
    rules_ = loadRules(rulesJson);
    sim.world().setResource<GameState>();
    setupMap(sim, map);
    loadTriggers(rules_, rulesJson, map, triggers_);
    script_.reset();
    if (!scriptFiles(rulesJson, map).empty()) {   // zero cost for games without scripts: no VM, no system
        script_ = std::make_unique<ScriptEngine>(*this);
        script_->load(sim, rulesJson, map);
    }

    // System order is part of the rules: changing it changes outcomes (and replays).
    sim.addSystem("economy", [this](Simulation& s) { sysEconomy(s); });
    sim.addSystem("fields", [this](Simulation& s) { sysFields(s); });
    sim.addSystem("production", [this](Simulation& s) { sysProduction(s); });
    sim.addSystem("construction", [this](Simulation& s) { sysConstruction(s); });
    if (rules_.settlers) sim.addSystem("settlers", [this](Simulation& s) { sysSettlers(s); });
    if (rules_.lanes.enabled) sim.addSystem("lanes", [this](Simulation& s) { sysLanes(s); });
    sim.addSystem("garrison", [this](Simulation& s) { sysGarrison(s); });
    sim.addSystem("waves", [this](Simulation& s) { sysWaves(s); });
    sim.addSystem("neutrals", [this](Simulation& s) { sysNeutrals(s); });
    if (rules_.dungeon) sim.addSystem("dungeon", [this](Simulation& s) { sysDungeon(s); }); // only for games that use it
    sim.addSystem("orders", [this](Simulation& s) { sysOrders(s); });
    sim.addSystem("abilities", [this](Simulation& s) { sysAbilities(s); });
    if (rules_.advancedEffects) sim.addSystem("effects", [this](Simulation& s) { sysEffects(s); });
    sim.addSystem("targeting", [this](Simulation& s) { sysTargeting(s); });
    sim.addSystem("movement", [this](Simulation& s) { sysMovement(s); });
    sim.addSystem("weapons", [this](Simulation& s) { sysWeapons(s); });
    sim.addSystem("projectiles", [this](Simulation& s) { sysProjectiles(s); });
    sim.addSystem("death", [this](Simulation& s) { sysDeath(s); });
    sim.addSystem("capture", [this](Simulation& s) { sysCapture(s); });
    sim.addSystem("creep", [this](Simulation& s) { sysCreep(s); });
    sim.addSystem("vision", [this](Simulation& s) { sysVision(s); });
    sim.addSystem("victory", [this](Simulation& s) { sysVictory(s); });
    sim.addSystem("triggers", [this](Simulation& s) { sysTriggers(s); }); // last: sees every event of the tick
    if (script_) sim.addSystem("scripts", [this](Simulation& s) { script_->tick(s); }); // after triggers: same events
    sysCreep(sim);
    sysEconomy(sim); // initial supply/tech counts
    // big maps: build the hierarchical pathing graph now (it is derived data, so this only moves the cost
    // from the first long path of the match to the loading screen)
    MapGrid& g = state(sim).grid;
    if (g.bigMap())
        for (auto& t : rules_.types)
            if (t.speed.raw > 0 && !(t.layer & kLayerAir)) g.prepareHpa(t.pathLayer);
}

void RtsModule::setupMap(Simulation& sim, const Json& map) {
    World& w = sim.world();
    GameState& gs = state(sim);
    MapGrid& g = gs.grid;

    int mw = int(map["size"][0].integer(64)), mh = int(map["size"][1].integer(64));
    if (mw < 4 || mh < 4 || mw > kMaxMapSize || mh > kMaxMapSize) throw std::runtime_error("map: size must be 4.." + std::to_string(kMaxMapSize));
    g.init(mw, mh);
    const Json& terrain = map["terrain"];
    const Json& rows = terrain["rows"];
    for (size_t y = 0; y < rows.size() && int(y) < mh; ++y) {
        std::string row = rows[y].str();
        for (size_t x = 0; x < row.size() && int(x) < mw; ++x) {
            size_t i = size_t(g.index(int(x), int(y)));
            int dk = rules_.findDiggable(row[x]);
            if (dk >= 0) { g.flags[i] = 0; g.dig[i] = uint8_t(dk + 1); g.digHp[i] = rules_.diggables[size_t(dk)].hardness; }
            else g.flags[i] = cellFlagsFromChar(row[x]);
        }
    }
    for (auto& r : terrain["rects"].items()) {
        int x0 = int(r["rect"][0].integer()), y0 = int(r["rect"][1].integer());
        int rw = int(r["rect"][2].integer()), rh = int(r["rect"][3].integer());
        std::string t = r.getStr("type", ".");
        char c = t == "water" ? '~' : t == "blocked" ? '#' : t == "nobuild" ? ',' : t == "rock" ? 'R' : t == "ramp" ? '/' : t.size() == 1 ? t[0] : '.';
        for (int y = y0; y < y0 + rh; ++y)
            for (int x = x0; x < x0 + rw; ++x)
                if (g.inside(x, y)) g.flags[size_t(g.index(x, y))] = cellFlagsFromChar(c);
    }
    const Json& levels = terrain["levels"];
    for (size_t y = 0; y < levels.size() && int(y) < mh; ++y) {
        std::string row = levels[y].str();
        for (size_t x = 0; x < row.size() && int(x) < mw; ++x)
            if (row[x] >= '0' && row[x] <= '9') g.level[size_t(g.index(int(x), int(y)))] = int8_t(row[x] - '0');
    }
    const Json& heights = terrain["heights"];
    for (size_t i = 0; i < heights.size() && i < g.height.size(); ++i) g.height[i] = int16_t(heights[i].integer());
    const Json& corners = terrain["corners"];  // heightfield: (w+1)*(h+1) corner heights, 1/16 units
    if (corners.size() > 0) {
        g.corner.assign(size_t((mw + 1) * (mh + 1)), 0);
        for (size_t i = 0; i < corners.size() && i < g.corner.size(); ++i) g.corner[i] = int16_t(corners[i].integer());
    } else if (terrain.has("cornersFile")) { // big maps: raw int16 LE file, attached by applyCornersFile()
        std::string err;
        if (!decodeCorners(terrain, mw, mh, g.corner, &err)) throw std::runtime_error(err);
    }
    if (rules_.terrain.model == TerrainModel::Flat) std::fill(g.level.begin(), g.level.end(), int8_t(0));
    if (rules_.terrain.heightfield() && g.hasCorners()) {
        const TerrainRules& tr = rules_.terrain;
        g.slopeLimit[0] = MapGrid::slopeUnits(tr.maxSlopeGround);
        g.sandLimit = MapGrid::slopeUnits(tr.maxSlopeSand.raw < 0 ? tr.maxSlopeGround : tr.maxSlopeSand);
        g.buildLimit = MapGrid::slopeUnits(tr.buildSlope);
        for (size_t k = 0; k < tr.slopeClasses.size() && k < 15; ++k) g.slopeLimit[k + 1] = MapGrid::slopeUnits(tr.slopeClasses[k]);
        g.enableHeightfield(rules_.levelHeight);
    }

    // resource fields
    for (auto& f : map["fields"].items()) {
        int ri = rules_.findResource(f.getStr("resource"));
        if (ri < 0) throw std::runtime_error("map field: unknown resource '" + f.getStr("resource") + "'");
        const ResourceDef& rd = rules_.resources[size_t(ri)];
        auto setCell = [&](int x, int y, Fixed amt) {
            if (!g.inside(x, y) || amt.raw <= 0) return;
            size_t i = size_t(g.index(x, y));
            if (!(g.flags[i] & kCellGround)) return;
            g.fieldRes[i] = int16_t(ri);
            g.fieldAmt[i] = fxMin(rd.fieldCellMax, g.fieldAmt[i] + amt);
        };
        if (f.has("rows")) {
            const Json& fr = f["rows"];
            for (size_t y = 0; y < fr.size(); ++y) {
                std::string row = fr[y].str();
                for (size_t x = 0; x < row.size(); ++x)
                    if (row[x] >= '1' && row[x] <= '9') setCell(int(x), int(y), rd.fieldCellMax * (row[x] - '0') / 9);
            }
        } else {
            Vec3 c = readPos(f["center"]);
            int r = int(f.getInt("radius", 3));
            Fixed amt = f.getFx("amount", rd.fieldCellMax);
            int cx = g.cellX(c.x), cy = g.cellY(c.z);
            for (int y = cy - r; y <= cy + r; ++y)
                for (int x = cx - r; x <= cx + r; ++x) {
                    int d2 = (x - cx) * (x - cx) + (y - cy) * (y - cy);
                    if (d2 <= r * r) setCell(x, y, amt * (r * r + 1 - d2) / (r * r + 1) + amt / 4);
                }
        }
    }

    // regions
    if (const Json& rg = map["regions"]; rg.isObject()) {
        std::vector<char> letters;
        const Json& rr = rg["rows"];
        for (size_t y = 0; y < rr.size() && int(y) < mh; ++y) {
            std::string row = rr[y].str();
            for (size_t x = 0; x < row.size() && int(x) < mw; ++x) {
                char c = row[x];
                if (c == '.' || c == ' ') continue;
                auto it = std::find(letters.begin(), letters.end(), c);
                if (it == letters.end()) { letters.push_back(c); it = letters.end() - 1; }
                g.region[size_t(g.index(int(x), int(y)))] = int16_t(it - letters.begin());
            }
        }
        gs.regionCount = int(letters.size());
        gs.regionOwner.assign(size_t(gs.regionCount), 0);
        gs.regionAdj.assign(size_t(gs.regionCount), {});
        gs.regionNames.resize(size_t(gs.regionCount));
        for (size_t i = 0; i < letters.size(); ++i) gs.regionNames[i] = rg["names"].getStr(std::string(1, letters[i]), std::string(1, letters[i]));
        std::vector<int64_t> sx(size_t(gs.regionCount), 0), sz(size_t(gs.regionCount), 0), cnt(size_t(gs.regionCount), 0);
        for (int y = 0; y < mh; ++y)
            for (int x = 0; x < mw; ++x) {
                int a = g.region[size_t(g.index(x, y))];
                if (a < 0) continue;
                sx[size_t(a)] += x; sz[size_t(a)] += y; cnt[size_t(a)]++;
                for (auto [dx, dy] : {std::pair{1, 0}, std::pair{0, 1}}) {
                    if (!g.inside(x + dx, y + dy)) continue;
                    int b = g.region[size_t(g.index(x + dx, y + dy))];
                    if (b < 0 || b == a) continue;
                    auto& la = gs.regionAdj[size_t(a)];
                    if (std::find(la.begin(), la.end(), uint16_t(b)) == la.end()) { la.push_back(uint16_t(b)); gs.regionAdj[size_t(b)].push_back(uint16_t(a)); }
                }
            }
        for (int i = 0; i < gs.regionCount; ++i) {
            int64_t c = std::max<int64_t>(1, cnt[size_t(i)]);
            gs.regionCenter.push_back({Fixed::fromInt(sx[size_t(i)]) / c + kHalf, Fixed{}, Fixed::fromInt(sz[size_t(i)]) / c + kHalf});
            std::sort(gs.regionAdj[size_t(i)].begin(), gs.regionAdj[size_t(i)].end());
        }
    }

    // victory
    gs.victory = map.has("victory") ? loadVictory(map["victory"], rules_.victory) : rules_.victory;

    // players
    const size_t np = sim.players().size();
    gs.players.assign(np + 1, {});
    for (size_t p = 0; p <= np; ++p) {
        auto& e = gs.players[p];
        const size_t nr = rules_.resources.size();
        e.stock.assign(nr, Fixed{}); e.cap.assign(nr, Fixed{}); e.income.assign(nr, Fixed{});
        e.capProvided.assign(nr, Fixed{}); e.capUsed.assign(nr, Fixed{});
        e.typeCount.assign(rules_.types.size(), 0);
        e.typeCountAll.assign(rules_.types.size(), 0);
        e.researched.assign(rules_.techs.size(), 0);
        e.researching.assign(rules_.techs.size(), 0);
        e.typeDisabled.assign(rules_.types.size(), 0);
        for (size_t r = 0; r < nr; ++r) e.stock[r] = rules_.resources[r].start;
        if (p >= 1) {
            const Json& pj = map["players"][p - 1];
            for (auto& [rid, v] : pj["resources"].members()) {
                int ri = rules_.findResource(rid);
                if (ri >= 0) e.stock[size_t(ri)] = v.fx();
            }
            e.faction = pj.getStr("faction", rules_.factions.empty() ? "" : rules_.factions[0].id);
            if (pj.getBool("waves", false)) gs.wavePlayers.push_back(uint8_t(p));
        }
        recomputeMods(sim, uint8_t(p));
    }
    {   // fog of war: the map may override the mode of the rules ("fogOfWar": {"mode": "none"})
        FogMode fm = rules_.fog.mode;
        std::string m = map["fogOfWar"].getStr("mode", "");
        if (m == "full") fm = FogMode::Full; else if (m == "explored") fm = FogMode::Explored; else if (m == "none" || m == "off") fm = FogMode::None;
        gs.fogMode = uint8_t(fm);
        gs.vision.assign(np + 1, std::vector<uint8_t>(size_t(mw * mh), fm == FogMode::None ? 2 : fm == FogMode::Explored ? 1 : 0));
    }
    gs.roadsOnly = map.getBool("roadsOnly", false);
    {
        uint8_t maxTeam = 0;
        for (auto& p : sim.players()) maxTeam = std::max(maxTeam, p.team);
        gs.tickets.assign(size_t(maxTeam) + 1, gs.victory.tickets);
        gs.score.assign(size_t(maxTeam) + 1, Fixed{});
    }
    gs.autoBuilt.assign(np + 1, 0);
    if (rules_.waveClock.raw > 0) gs.nextWaveTick = uint32_t((rules_.waveClock * int64_t(sim.tickRate())).toInt());

    // faction start units at player start locations
    for (size_t p = 1; p <= np; ++p) {
        const Json& pj = map["players"][p - 1];
        if (!pj.has("start")) continue;
        Vec3 start = readPos(pj["start"]);
        int fi = rules_.findFaction(gs.players[p].faction);
        if (fi < 0) continue;
        for (auto& st : rules_.factions[size_t(fi)].start) {
            for (int k = 0; k < st.count; ++k) {
                Vec3 pos = start + Vec3{st.dx + Fixed::fromRatio(k % 4, 1) * Fixed::fromRatio(4, 5), Fixed{}, st.dz + Fixed::fromInt(k / 4) * Fixed::fromRatio(4, 5)};
                const UnitType& t = rules_.types[st.type];
                if (t.structure) pos = snapToFootprint(t, pos);
                spawn(sim, st.type, uint8_t(p), pos);
            }
        }
    }

    // placed entities
    std::vector<std::pair<std::string, Entity>> ids;
    std::vector<Entity> placed;
    for (auto& ej : map["entities"].items()) {
        int t = rules_.findType(ej.getStr("type"));
        if (t < 0) throw std::runtime_error("map entity: unknown type '" + ej.getStr("type") + "'");
        int owner = int(ej.getInt("owner", 0));
        if (owner == -1) owner = kHostileNeutral;
        if ((owner < 0 || owner > int(np)) && owner != kHostileNeutral) throw std::runtime_error("map entity: owner out of range");
        Entity e = spawn(sim, uint16_t(t), uint8_t(owner), readPos(ej["pos"]));
        placed.push_back(e);
        if (auto* gg = w.tryGet<Garrison>(e)) gg->count = int(ej.getInt("garrison", 0));
        if (auto* rs = w.tryGet<ResourceStock>(e); rs && ej.has("amount")) rs->amount = ej.getFx("amount");
        if (ej.has("id")) ids.emplace_back(ej.getStr("id"), e);
    }
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

    // starting regions: explicit list, else regions containing the player's structures/start
    if (gs.regionCount > 0) {
        for (size_t p = 1; p <= np; ++p) {
            const Json& pj = map["players"][p - 1];
            for (auto& rn : pj["regions"].items())
                for (int i = 0; i < gs.regionCount; ++i) if (gs.regionNames[size_t(i)] == rn.str()) gs.regionOwner[size_t(i)] = uint8_t(p);
            if (pj.has("start")) { int ri = regionAt(sim, readPos(pj["start"])); if (ri >= 0 && !gs.regionOwner[size_t(ri)]) gs.regionOwner[size_t(ri)] = uint8_t(p); }
        }
        w.each<Unit, Owner, Transform>([&](Entity, Unit& u, Owner& o, Transform& t) {
            if (o.player == 0 || o.player > np || !rules_.types[u.type].structure) return;
            int ri = regionAt(sim, t.pos);
            if (ri >= 0 && !gs.regionOwner[size_t(ri)]) gs.regionOwner[size_t(ri)] = o.player;
        });
    }

    // waves
    for (auto& wj : map["waves"].items()) {
        WaveDef wd;
        wd.tick = uint32_t((wj.getFx("time") * int64_t(sim.tickRate())).toInt());
        wd.player = uint8_t(wj.getInt("player", int64_t(np)));
        for (auto& s : wj["spawn"].items()) {
            int t = rules_.findType(s.getStr("type"));
            if (t < 0) throw std::runtime_error("map wave: unknown type '" + s.getStr("type") + "'");
            wd.spawn.push_back({uint16_t(t), int(s.getInt("count", 1))});
        }
        wd.at = readPos(wj["at"]);
        wd.spread = wj.getFx("spread", wd.spread);
        wd.every = uint32_t((wj.getFx("every") * int64_t(sim.tickRate())).toInt());
        wd.growth = wj.getFx("growth", Fixed::fromInt(1));
        wd.repeats = int(wj.getInt("repeats", 0));
        gs.waves.push_back(wd);
        if (std::find(gs.wavePlayers.begin(), gs.wavePlayers.end(), wd.player) == gs.wavePlayers.end()) gs.wavePlayers.push_back(wd.player);
    }
    setupDungeon(sim, map);
    if (rules_.advancedEffects) setupSkills(sim, map);
    if (rules_.buildingStorage) {
        // starting goods are put into the storehouses (map entities may also set "inventory": {res: n})
        for (size_t p = 1; p < gs.players.size(); ++p)
            for (size_t r = 0; r < rules_.resources.size(); ++r) {
                if (!isGood(uint16_t(r))) continue;
                Fixed amt = gs.players[p].stock[r];
                gs.players[p].stock[r] = Fixed{};
                if (amt.raw > 0) addResource(sim, uint8_t(p), uint16_t(r), amt);
            }
        size_t k = 0;
        for (auto& ej : map["entities"].items()) {
            ++k;
            if (!ej.has("inventory")) continue;
            Entity e = placed[k - 1];
            auto* inv = w.tryGet<Inventory>(e);
            if (!inv) throw std::runtime_error("map entity: 'inventory' on a type without one");
            for (auto& [rid, v] : ej["inventory"].members()) {
                int ri = rules_.findResource(rid);
                if (ri < 0) throw std::runtime_error("map entity inventory: unknown resource '" + rid + "'");
                inv->have[size_t(ri)] += v.fx();
            }
        }
        syncStorehouses(sim);
    }
}

void RtsModule::footprintAt(const UnitType& t, Vec3 c, int& x0, int& y0) {
    x0 = int((c.x - Fixed::fromInt(t.footprintW) / 2 + kHalf).floorInt());
    y0 = int((c.z - Fixed::fromInt(t.footprintH) / 2 + kHalf).floorInt());
}

Vec3 RtsModule::snapToFootprint(const UnitType& t, Vec3 p) {
    int x0, y0;
    footprintAt(t, p, x0, y0);
    return {Fixed::fromInt(x0) + Fixed::fromInt(t.footprintW) / 2, Fixed{}, Fixed::fromInt(y0) + Fixed::fromInt(t.footprintH) / 2};
}

Entity RtsModule::spawn(Simulation& sim, uint16_t type, uint8_t owner, Vec3 pos, bool underConstruction) {
    World& w = sim.world();
    GameState& gs = state(sim);
    const UnitType& t = rules_.types[type];
    Entity e = w.create();
    if (t.structure) {
        pos = snapToFootprint(t, pos);
        int x0, y0;
        footprintAt(t, pos, x0, y0);
        w.add<Footprint>(e, Footprint{x0, y0, t.footprintW, t.footprintH});
        gs.grid.block(x0, y0, t.footprintW, t.footprintH, +1);
    }
    if (gs.grid.hf) pos.y = gs.grid.surfaceHeight(pos.x, pos.z);
    w.add<Transform>(e, Transform{pos, pos, Fixed{}});
    w.add<Owner>(e, Owner{owner});
    Unit u;
    u.type = type;
    u.hp = underConstruction ? fxMax(t.maxHp / 10, kOne) : t.maxHp;
    if (underConstruction) u.flags |= kUnitUnderConstruction;
    u.energy = t.startEnergy;
    w.add<Unit>(e, u);
    if (t.speed.raw > 0) { w.add<Mobile>(e); w.add<Orders>(e); }
    if (!t.weapons.empty()) w.add<Combat>(e);
    if (!t.produces.empty() || !t.researches.empty() || t.members > 1) {
        Production p;
        p.charges = t.chargesMax;
        w.add<Production>(e, p);
    }
    if (!t.gathers.empty()) w.add<Cargo>(e);
    if (t.resourceNode >= 0) w.add<ResourceStock>(e, ResourceStock{t.resourceNode, t.resourceAmount, t.resourceAmount.raw < 0});
    if (t.garrisonMax > 0 || t.spawnUnit != kNoType) w.add<Garrison>(e);
    if (underConstruction) w.add<Construction>(e);
    if (t.hero) w.add<Hero>(e);
    if (t.members > 1) w.add<Squad>(e);
    if (!t.abilities.empty()) w.add<AbilityState>(e);
    if (t.captureTime.raw > 0) w.add<Capture>(e);
    if (t.hasNeeds) { Needs n; n.happiness = t.needs.happyStart; w.add<Needs>(e, n); }
    if (t.portalInterval.raw > 0) w.add<PortalState>(e);
    if (t.autoTrainUnit != kNoType) w.add<AutoTrainState>(e);
    if (t.claimRate.raw > 0) w.add<ClaimState>(e);
    const size_t nres = rules_.resources.size();
    const bool needsInventory = t.storeCapacity.raw > 0 || t.recipe.enabled ||
                                (underConstruction && rules_.buildingStorage && upfrontCost(t.cost).size() != t.cost.size());
    if (needsInventory) w.add<Inventory>(e, Inventory{std::vector<Fixed>(nres), std::vector<Fixed>(nres), std::vector<Fixed>(nres)});
    if (t.recipe.enabled) w.add<Workshop>(e);
    if (t.carrierCapacity.raw > 0) w.add<Carrier>(e);
    w.add<Buffs>(e);
    w.add<Statuses>(e);
    if (rules_.advancedEffects) {
        if (auto* as = w.tryGet<AbilityState>(e)) {
            for (size_t i = 0; i < t.abilities.size() && i < 8; ++i) {
                as->readyAt[i] = sim.tick() + uint32_t((t.abilities[i].initialCooldown * int64_t(sim.tickRate())).toInt());
                as->charges[i] = uint8_t(std::clamp(t.abilities[i].chargesMax, 0, 255));
            }
            as->init = true;
        }
        EffectCtx ctx; ctx.owner = owner; ctx.caster = e; ctx.sourceType = type;
        for (int16_t st : t.statuses) addStatus(sim, e, st, Fixed{}, ctx);
    }
    if (owner < gs.players.size() && !gs.players[owner].typeCountAll.empty()) gs.players[owner].typeCountAll[type]++;
    if ((owner == kHostileNeutral || t.wanders) && t.speed.raw > 0) w.add<Guard>(e, Guard{pos, t.leash, false, 0});
    sim.emit({kEvSpawned, owner, e, kNullEntity, type, pos});
    return e;
}

// ================================================================= queries

uint8_t RtsModule::teamOf(Simulation& sim, uint8_t player) const {
    auto* p = sim.player(player);
    return p ? p->team : 0;
}

bool RtsModule::hostile(Simulation& sim, uint8_t a, uint8_t b) const {
    if (a == 0 || b == 0) return false;            // passive neutral objects
    if (a == kHostileNeutral && b == kHostileNeutral) return false;
    if (a == kHostileNeutral || b == kHostileNeutral) return true;
    return !sim.allied(a, b);
}

bool RtsModule::requirementsMet(Simulation& sim, uint8_t player, const std::vector<Requirement>& req) const {
    auto* gs = sim.world().resource<GameState>();
    if (player == 0 || player >= gs->players.size()) return true;
    for (auto& r : req) {
        if (r.tech ? !gs->players[player].researched[r.index] : gs->players[player].typeCount[r.index] <= 0) return false;
    }
    return true;
}

bool RtsModule::canTrain(Simulation& sim, uint8_t player, uint16_t type) const {
    auto* gs = sim.world().resource<GameState>();
    const UnitType& t = rules_.types[type];
    if (!requirementsMet(sim, player, t.requires_)) return false;
    if (player < gs->players.size() && !gs->players[player].typeDisabled.empty() && gs->players[player].typeDisabled[type]) return false;
    if (t.unique && player < gs->players.size() && gs->players[player].typeCountAll[type] > 0) return false;
    return true;
}

bool RtsModule::canAfford(Simulation& sim, uint8_t player, const std::vector<Amount>& cost, Fixed supply, int* missing) const {
    auto* gs = sim.world().resource<GameState>();
    if (player == 0 || player >= gs->players.size()) return true;
    const auto& eco = gs->players[player];
    for (auto& c : cost)
        if (rules_.resources[c.res].kind != ResourceKind::Capacity && eco.stock[c.res] < c.value) { if (missing) *missing = c.res; return false; }
    if (supply.raw > 0 && eco.supplyUsed + supply > eco.supplyCap) { if (missing) *missing = 255; return false; }
    return true;
}

bool RtsModule::pay(Simulation& sim, uint8_t player, const std::vector<Amount>& cost, Fixed supply, bool emitFail) {
    int missing = -1;
    if (!canAfford(sim, player, cost, supply, &missing)) {
        if (emitFail) sim.emit({kEvNotEnough, player, kNullEntity, kNullEntity, missing, {}});
        return false;
    }
    auto& gs = state(sim);
    if (player == 0 || player >= gs.players.size()) return true;
    auto& eco = gs.players[player];
    for (auto& c : cost) {
        if (rules_.resources[c.res].kind == ResourceKind::Capacity) continue;
        eco.stock[c.res] -= c.value;
        if (isGood(c.res)) withdrawGoods(sim, player, c.res, c.value); // goods leave the storehouses
    }
    eco.supplyUsed += supply; // reserved until the next economy pass
    return true;
}

void RtsModule::refund(Simulation& sim, uint8_t player, const std::vector<Amount>& cost, Fixed fraction) {
    auto& gs = state(sim);
    if (player == 0 || player >= gs.players.size()) return;
    for (auto& c : cost) if (rules_.resources[c.res].kind != ResourceKind::Capacity) addResource(sim, player, c.res, c.value * fraction);
}

void RtsModule::addResource(Simulation& sim, uint8_t player, uint16_t res, Fixed amount) {
    auto& gs = state(sim);
    if (player == 0 || player >= gs.players.size()) return;
    if (rules_.resources[res].convertTo >= 0) { addResource(sim, player, uint16_t(rules_.resources[res].convertTo), amount * rules_.resources[res].convertRate); return; }
    auto& eco = gs.players[player];
    if (isGood(res)) {
        // goods go into storehouses; the stock is their sum
        Fixed before = Fixed{};
        sim.world().each<Inventory, Unit, Owner>([&](Entity, Inventory& inv, Unit& u, Owner& o) {
            if (o.player == player && rules_.types[u.type].storeCapacity.raw > 0) before += inv.have[res];
        });
        depositGoods(sim, player, res, amount);
        Fixed after = Fixed{};
        sim.world().each<Inventory, Unit, Owner>([&](Entity, Inventory& inv, Unit& u, Owner& o) {
            if (o.player == player && rules_.types[u.type].storeCapacity.raw > 0) after += inv.have[res];
        });
        eco.stock[res] += after - before;
        return;
    }
    eco.stock[res] += amount;
    const ResourceDef& rd = rules_.resources[res];
    bool capped = rd.kind == ResourceKind::Flow || (rd.kind == ResourceKind::Stock && rd.capped);
    if (capped && eco.cap[res].raw > 0) eco.stock[res] = fxMin(eco.stock[res], eco.cap[res]);
    if (capped && eco.cap[res].raw == 0 && rd.capped) eco.stock[res] = fxMin(eco.stock[res], rd.storage);
}

void RtsModule::recomputeMods(Simulation& sim, uint8_t player) {
    auto& gs = state(sim);
    if (player >= gs.players.size()) return;
    auto& eco = gs.players[player];
    eco.mods.assign(rules_.types.size(), {});
    for (auto& m : eco.mods) for (size_t s = 0; s < size_t(Stat::Count); ++s) { m.add[s] = Fixed{}; m.mul[s] = kOne; }
    for (size_t k = 0; k < rules_.techs.size(); ++k) {
        for (int lv = 0; lv < int(eco.researched[k]); ++lv) // repeatable techs apply once per level
            for (auto& [target, mod] : rules_.techs[k].effects)
                for (size_t ti = 0; ti < rules_.types.size(); ++ti)
                    if (rules_.matches(target, uint16_t(ti))) {
                        eco.mods[ti].add[size_t(mod.stat)] += mod.add;
                        eco.mods[ti].mul[size_t(mod.stat)] *= mod.mul;
                    }
    }
}

Fixed RtsModule::typeStat(Simulation& sim, uint8_t player, uint16_t type, Stat s, Fixed base) const {
    auto* gs = sim.world().resource<GameState>();
    if (player >= gs->players.size() || gs->players[player].mods.empty()) return base;
    const TypeMods& m = gs->players[player].mods[type];
    return (base + m.add[size_t(s)]) * m.mul[size_t(s)];
}

Fixed RtsModule::stat(Simulation& sim, Entity e, Stat s) const {
    World& w = sim.world();
    const Unit& u = w.get<Unit>(e);
    const UnitType& t = rules_.types[u.type];
    Fixed base{};
    switch (s) {
        case Stat::MaxHp: base = t.maxHp; break;
        case Stat::Armor: base = t.armor; break;
        case Stat::Damage: base = kOne; break;          // multiplier on weapon damage
        case Stat::Range: base = Fixed{}; break;        // bonus added to weapon range
        case Stat::Speed: base = t.speed; break;
        case Stat::Sight: base = t.sight; break;
        case Stat::GatherRate: base = kOne; break;
        case Stat::AttackSpeed: base = kOne; break;
        case Stat::ProductionSpeed: base = kOne; break;
        case Stat::EnergyRegen: case Stat::Defense: case Stat::CooldownRate: case Stat::UpgradeCost: case Stat::DamageTaken: case Stat::Scale: base = kOne; break;
        case Stat::GarrisonCap: base = Fixed::fromInt(t.garrisonMax); break;
        case Stat::Lifesteal: base = Fixed{}; break;
        default: break;
    }
    uint8_t owner = w.get<Owner>(e).player;
    Fixed add{}, mul = kOne;
    if (auto* gs = w.resource<GameState>(); owner < gs->players.size() && !gs->players[owner].mods.empty()) {
        const TypeMods& m = gs->players[owner].mods[u.type];
        add += m.add[size_t(s)]; mul *= m.mul[size_t(s)];
    }
    if (auto* h = w.tryGet<Hero>(e)) for (auto& pl : t.perLevel) if (pl.stat == s) { add += pl.add * (h->level - 1); }
    if (auto* b = w.tryGet<Buffs>(e)) {
        for (auto& bf : b->list) if (bf.stat == s && bf.until > sim.tick()) { add += bf.add; mul *= bf.mul; }
    }
    if (rules_.advancedEffects) {
        if (auto* st = w.tryGet<Statuses>(e))
            for (auto& in : st->list)
                for (auto& m : rules_.statuses[in.def].mods)
                    if (m.stat == s) { add += m.add * in.stacks; for (int k = 0; k < in.stacks; ++k) mul *= m.mul; }
        if (auto* gs = w.resource<GameState>(); owner < gs->players.size())
            for (auto& in : gs->players[owner].statuses)
                for (auto& m : rules_.statuses[in.def].mods)
                    if (m.stat == s) { add += m.add * in.stacks; for (int k = 0; k < in.stacks; ++k) mul *= m.mul; }
    }
    // Damage/Range/GatherRate/AttackSpeed "add" are fractional bonuses on top of 1 / absolute range
    return (base + add) * mul;
}

int RtsModule::membersAlive(Simulation& sim, Entity e) const {
    const Unit& u = sim.world().get<Unit>(e);
    const UnitType& t = rules_.types[u.type];
    if (t.members <= 1) return u.hp.raw > 0 ? 1 : 0;
    Fixed per = stat(sim, e, Stat::MaxHp) / t.members;
    if (per.raw <= 0) return 0;
    int n = int(((u.hp + per - Fixed::fromRaw(1)) / per).toInt());
    return std::clamp(n, 0, t.members);
}

int RtsModule::levelAt(Simulation& sim, Vec3 p) const {
    auto* gs = sim.world().resource<GameState>();
    return gs->grid.levelAt(gs->grid.cellX(p.x), gs->grid.cellY(p.z));
}

Fixed RtsModule::groundHeight(Simulation& sim, Vec3 p) const {
    const MapGrid& g = sim.world().resource<GameState>()->grid;
    return g.hf ? g.surfaceHeight(p.x, p.z) : Fixed{};
}

Fixed RtsModule::heightAdvantage(Simulation& sim, Vec3 a, Vec3 b) const {
    const MapGrid& g = sim.world().resource<GameState>()->grid;
    if (!g.hf || rules_.levelHeight.raw <= 0) return Fixed::fromInt(levelAt(sim, a) - levelAt(sim, b));
    return (g.surfaceHeight(a.x, a.z) - g.surfaceHeight(b.x, b.z)) / rules_.levelHeight;
}

Fixed RtsModule::slopeSpeed(Simulation& sim, Vec3 p, Vec2 dir) const {
    const MapGrid& g = sim.world().resource<GameState>()->grid;
    if (!g.hf) return kOne;
    Vec3 gr = g.gradient(p.x, p.z);
    Fixed climb = gr.x * dir.x + gr.z * dir.y;   // rise per cell along the move
    Fixed f = climb.raw > 0 ? kOne - rules_.terrain.uphillSpeed * climb : kOne - rules_.terrain.downhillSpeed * climb;
    return fxClamp(f, Fixed::fromRatio(1, 4), Fixed::fromInt(2));
}

int RtsModule::regionAt(Simulation& sim, Vec3 p) const {
    auto* gs = sim.world().resource<GameState>();
    int x = gs->grid.cellX(p.x), y = gs->grid.cellY(p.z);
    return gs->grid.inside(x, y) ? gs->grid.region[size_t(gs->grid.index(x, y))] : -1;
}

bool RtsModule::canClaim(Simulation& sim, uint8_t player, int region, std::vector<Amount>* costOut) const {
    auto* gs = sim.world().resource<GameState>();
    if (!rules_.regions.enabled || region < 0 || region >= gs->regionCount || gs->regionOwner[size_t(region)] != 0) return false;
    bool adjacent = false;
    for (uint16_t n : gs->regionAdj[size_t(region)]) if (gs->regionOwner[n] == player) adjacent = true;
    if (!adjacent) return false;
    std::vector<Amount> cost = rules_.regions.claimCost;
    int owned = 0;
    for (auto o : gs->regionOwner) if (o == player) ++owned;
    for (auto& c : cost) c.value = c.value + c.value * rules_.regions.claimCostGrowth * (owned - 1);
    if (costOut) *costOut = cost;
    return canAfford(sim, player, cost);
}

bool RtsModule::isVisible(Simulation& sim, uint8_t player, Vec3 p) const {
    auto* gs = sim.world().resource<GameState>();
    if (player == 0 || player >= gs->vision.size()) return true;
    int x = int(p.x.floorInt()), y = int(p.z.floorInt());
    if (!gs->grid.inside(x, y)) return false;
    return gs->vision[player][size_t(gs->grid.index(x, y))] == 2;
}

bool RtsModule::canPlace(Simulation& sim, uint8_t player, uint16_t type, Vec3 centre, Entity ignore, std::string* why) {
    const UnitType& t = rules_.types[type];
    auto fail = [&](const char* m) { if (why) *why = m; return false; };
    if (!t.structure) return fail("not a structure");
    World& w = sim.world();
    GameState& gs = state(sim);
    MapGrid& g = gs.grid;
    centre = snapToFootprint(t, centre);
    int x0, y0;
    footprintAt(t, centre, x0, y0);
    if (t.placeOn != kNoType) {
        // must sit exactly on a free node of the required type
        Entity node;
        w.each<Unit, Transform>([&](Entity e, Unit& u, Transform& tr) {
            if (u.type == t.placeOn && dist2D(tr.pos, centre) < kHalf) node = e;
        });
        if (!node.valid()) return fail("needs a resource spot");
    } else {
        if (!g.areaBuildable(x0, y0, t.footprintW, t.footprintH)) return fail("blocked");
    }
    for (int y = y0; y < y0 + t.footprintH; ++y)
        for (int x = x0; x < x0 + t.footprintW; ++x) {
            if (!g.inside(x, y)) return fail("outside the map");
            size_t i = size_t(g.index(x, y));
            if (t.terrainReq == TerrainReq::Rock && !(g.flags[i] & kCellRock)) return fail("needs rock");
            if (t.terrainReq == TerrainReq::Creep && !g.creep[i]) return fail("needs creep");
        }
    if (player != 0 && player < gs.players.size()) {
        if (rules_.buildRadius > 0) {
            bool hasBase = false, near = false;
            w.each<Unit, Owner, Transform>([&](Entity, Unit& u, Owner& o, Transform& tr) {
                if (o.player != player) return;
                const UnitType& ut = rules_.types[u.type];
                if (!ut.structure || !ut.extendsBase) return;
                hasBase = true;
                if (dist2D(tr.pos, centre) <= Fixed::fromInt(rules_.buildRadius) + ut.radius + t.radius) near = true;
            });
            if (hasBase && !near) return fail("too far from base");
        }
        if (rules_.regions.enabled && rules_.regions.buildOnlyOwned && gs.regionCount > 0) {
            int ri = regionAt(sim, centre);
            if (ri < 0 || gs.regionOwner[size_t(ri)] != player) return fail("region not owned");
        }
        if (rules_.territory.enabled && rules_.territory.buildOnlyOwned)
            for (int y = y0; y < y0 + t.footprintH; ++y)
                for (int x = x0; x < x0 + t.footprintW; ++x)
                    if (g.territory[size_t(g.index(x, y))] != player) return fail("territory not owned");
    }
    bool clear = true;
    w.each<Unit, Transform, Mobile>([&](Entity e, Unit& u, Transform& tr, Mobile&) {
        if (e == ignore || (u.flags & kUnitInHand) || !(rules_.types[u.type].layer & (kLayerGround | kLayerSandOnly))) return;
        int cx = int(tr.pos.x.floorInt()), cy = int(tr.pos.z.floorInt());
        if (cx >= x0 && cx < x0 + t.footprintW && cy >= y0 && cy < y0 + t.footprintH) {
            // own units step aside (they will be pushed); enemies block
            if (w.get<Owner>(e).player != player) clear = false;
        }
    });
    if (!clear) return fail("units in the way");
    return true;
}

Entity RtsModule::findNearestFieldCell(Simulation& sim, Vec3 p, int res, Vec3& out, int maxRadius) const {
    auto* gs = sim.world().resource<GameState>();
    const MapGrid& g = gs->grid;
    int cx = g.cellX(p.x), cy = g.cellY(p.z);
    for (int r = 0; r <= maxRadius; ++r)
        for (int dy = -r; dy <= r; ++dy)
            for (int dx = -r; dx <= r; ++dx) {
                if (std::abs(dx) != r && std::abs(dy) != r) continue;
                int x = cx + dx, y = cy + dy;
                if (!g.inside(x, y)) continue;
                size_t i = size_t(g.index(x, y));
                if (g.fieldAmt[i] >= kOne && (res < 0 || g.fieldRes[i] == res) && !g.blocked[i]) { // skip regrowth crumbs
                    out = g.cellCenter(x, y);
                    return Entity{uint32_t(i), 0}; // marker: valid() == true
                }
            }
    return kNullEntity;
}

Fixed RtsModule::contactDistance(Simulation& sim, Entity a, Entity b) {
    World& w = sim.world();
    auto* ta = w.tryGet<Transform>(a);
    auto* tb = w.tryGet<Transform>(b);
    if (!ta || !tb) return Fixed::fromInt(1 << 20);
    const UnitType& A = rules_.types[w.get<Unit>(a).type];
    const UnitType& B = rules_.types[w.get<Unit>(b).type];
    if (B.structure) {
        auto& f = w.get<Footprint>(b);
        Fixed px = fxClamp(ta->pos.x, Fixed::fromInt(f.x), Fixed::fromInt(f.x + f.w));
        Fixed pz = fxClamp(ta->pos.z, Fixed::fromInt(f.y), Fixed::fromInt(f.y + f.h));
        return fxMax(Vec2{ta->pos.x - px, ta->pos.z - pz}.length() - A.radius, Fixed{});
    }
    return fxMax(dist2D(ta->pos, tb->pos) - A.radius - B.radius, Fixed{});
}

Vec3 RtsModule::freeSpotNear(Simulation& sim, Entity s, uint8_t layer) {
    World& w = sim.world();
    GameState& gs = state(sim);
    Vec3 c = w.get<Transform>(s).pos;
    int x0 = int(c.x.floorInt()), y0 = int(c.z.floorInt());
    int fw = 1, fh = 1;
    if (auto* f = w.tryGet<Footprint>(s)) { x0 = f->x; y0 = f->y; fw = f->w; fh = f->h; }
    for (int ring = 0; ring < 10; ++ring)
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

Entity RtsModule::placeStructure(Simulation& sim, uint8_t player, uint16_t type, Vec3 centre, Entity builder, bool complete) {
    World& w = sim.world();
    const UnitType& t = rules_.types[type];
    centre = snapToFootprint(t, centre);
    // take over a resource node (refinery on a geyser): remember it, remove the node
    Fixed inherited{};
    int16_t inheritedRes = -1;
    uint16_t nodeType = kNoType;
    if (t.placeOn != kNoType) {
        Entity node;
        w.each<Unit, Transform>([&](Entity e, Unit& u, Transform& tr) { if (u.type == t.placeOn && dist2D(tr.pos, centre) < kHalf) node = e; });
        if (node.valid() && t.takesOverNode) {
            if (auto* rs = w.tryGet<ResourceStock>(node)) { inherited = rs->amount; inheritedRes = rs->res; }
            nodeType = w.get<Unit>(node).type;
            if (auto* f = w.tryGet<Footprint>(node)) state(sim).grid.block(f->x, f->y, f->w, f->h, -1);
            w.destroy(node);
        }
    }
    Entity s = spawn(sim, type, player, centre, !complete);
    if (inheritedRes >= 0) {
        w.add<ResourceStock>(s, ResourceStock{inheritedRes, inherited, inherited.raw < 0});
        w.add<Extractor>(s, Extractor{nodeType, inherited});
    }
    if (!complete) {
        auto& c = w.get<Construction>(s);
        c.builder = builder;
        c.paid = rules_.costModel == CostModel::Upfront ? kOne : Fixed{};
        if (builder.valid() && w.alive(builder)) {
            BuildMode bm = rules_.types[w.get<Unit>(builder).type].buildMode;
            c.selfBuild = bm != BuildMode::Walk;
        } else c.selfBuild = true;
    }
    sim.emit({complete ? kEvBuildDone : kEvBuildPlaced, player, s, builder, type, centre});
    return s;
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
    GameState& gs = state(sim);
    auto* pi = sim.player(cmd.player);
    if (!pi || !pi->alive) return;
    const bool queue = cmd.flags & kCmdQueue;
    if (cmd.type >= kCmdDig && cmd.type <= kCmdPlayerAbility) { applyDungeonCommand(sim, cmd); return; }
    if (cmd.type == kCmdBuildRoad || cmd.type == kCmdConnectRoad) { applySettlersCommand(sim, cmd); return; }

    if (cmd.type == kCmdSurrender) {
        pi->alive = false; // hashed with the player table; result() then reports the remaining team
        sim.emit({kEvPlayerDefeated, cmd.player, kNullEntity, kNullEntity, kDefeatSurrendered, {}});
        return;
    }

    if (cmd.type == kCmdClaim) {
        int ri = regionAt(sim, cmd.point);
        std::vector<Amount> cost;
        if (!canClaim(sim, cmd.player, ri, &cost)) return;
        if (!pay(sim, cmd.player, cost, {})) return;
        gs.regionOwner[size_t(ri)] = cmd.player;
        sim.emit({kEvRegionClaimed, cmd.player, kNullEntity, kNullEntity, ri, gs.regionCenter[size_t(ri)]});
        return;
    }

    if (cmd.type == kCmdAutoBuild) {
        uint16_t type = kNoType;
        if (cmd.param >= 0 && size_t(cmd.param) < rules_.types.size()) {
            if (std::find(rules_.autoBuildPool.begin(), rules_.autoBuildPool.end(), uint16_t(cmd.param)) != rules_.autoBuildPool.end()) type = uint16_t(cmd.param);
        } else {
            std::vector<uint16_t> options;
            for (uint16_t t : rules_.autoBuildPool) if (canTrain(sim, cmd.player, t)) options.push_back(t);
            if (!options.empty()) type = options[sim.rng().below(uint32_t(options.size()))];
        }
        if (type == kNoType || !canTrain(sim, cmd.player, type)) return;
        Vec3 spot;
        if (!findAutoSpot(sim, cmd.player, type, spot)) { sim.emit({kEvNotEnough, cmd.player, kNullEntity, kNullEntity, 254, {}}); return; }
        if (!pay(sim, cmd.player, autoBuildCost(sim, cmd.player, type), {})) return;
        gs.autoBuilt[cmd.player]++;
        placeStructure(sim, cmd.player, type, spot, kNullEntity, false);
        return;
    }

    std::vector<Entity> sel;
    for (Entity e : cmd.units) {
        auto* o = w.tryGet<Owner>(e);
        auto* u = w.tryGet<Unit>(e);
        if (!o || !u || o->player != cmd.player || u->hp.raw <= 0) continue;
        if (u->flags & (kUnitInHand | kUnitPossessed)) continue; // off the map / directly controlled
        if (auto* nd = w.tryGet<Needs>(e); nd && nd->leaving) continue;
        if ((u->flags & kUnitUnderConstruction) && cmd.type != kCmdCancelTrain) continue;
        if (std::find(sel.begin(), sel.end(), e) != sel.end()) continue;
        sel.push_back(e);
    }
    if (sel.empty()) return;
    int movers = 0;
    for (Entity e : sel) if (w.has<Mobile>(e)) ++movers;
    Vec3 target = cmd.point;
    if (w.alive(cmd.target)) if (auto* t = w.tryGet<Transform>(cmd.target)) target = t->pos;
    // attack-move of a group: spread into an arc facing the goal, flanks ahead (they wrap around the enemy)
    std::vector<std::pair<Entity, Vec3>> arc;
    if (cmd.type == kCmdAttack && !w.alive(cmd.target) && movers >= 3) {
        Vec3 c{};
        Fixed maxR{};
        std::vector<Entity> ms;
        for (Entity e : sel) if (w.has<Mobile>(e)) { ms.push_back(e); c += w.get<Transform>(e).pos; maxR = fxMax(maxR, rules_.types[w.get<Unit>(e).type].radius); }
        c = c / Fixed::fromInt(int64_t(ms.size()));
        Vec3 dir = flat(target - c);
        Fixed dl = dir.length();
        if (dl > Fixed::fromInt(2)) {
            dir = dir / dl;
            Vec3 perp{-dir.z, Fixed{}, dir.x};
            // order the units by their current lateral position so paths don't cross
            std::sort(ms.begin(), ms.end(), [&](Entity a, Entity b) {
                Fixed la = (w.get<Transform>(a).pos - c).dot(perp), lb = (w.get<Transform>(b).pos - c).dot(perp);
                return la != lb ? la < lb : a.index < b.index;
            });
            const Fixed spacing = maxR * 2 + Fixed::fromRatio(2, 5);
            const int n = int(ms.size());
            const Fixed R = fxMax(Fixed::fromInt(4), spacing * n / 2);
            for (int i = 0; i < n; ++i) {
                Fixed lat = spacing * (2 * i - (n - 1)) / 2;
                Vec3 p = target + perp * lat + dir * (lat * lat / (R * 2));
                arc.push_back({ms[size_t(i)], flat(p)});
            }
        }
    }

    for (Entity e : sel) {
        if (!w.alive(e)) continue;
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
            for (auto& [ae, ap] : arc) if (ae == e) { o.point = ap; o.param = 1; }
            if (t.weapons.empty()) { o.type = OrderType::Move; o.target = kNullEntity; }
            issue(sim, e, o, queue); break;
        case kCmdPatrol:
            o.type = OrderType::Patrol; o.point2 = w.get<Transform>(e).pos; o.target = kNullEntity;
            issue(sim, e, o, queue); break;
        case kCmdStop:
        case kCmdHold:
            if (auto* ord = w.tryGet<Orders>(e)) { ord->queue.clear(); stopMoving(sim, e); }
            if (auto* c = w.tryGet<Combat>(e)) c->target = kNullEntity;
            if (cmd.type == kCmdHold) w.get<Unit>(e).flags |= kUnitHold; else w.get<Unit>(e).flags &= uint8_t(~kUnitHold);
            break;
        case kCmdGather:
            if (t.gathers.empty()) break;
            if (o.target.valid() && w.has<ResourceStock>(o.target)) { o.type = OrderType::Gather; issue(sim, e, o, queue); }
            else if (o.target.valid()) { o.type = OrderType::Return; issue(sim, e, o, queue); }
            else { o.type = OrderType::Gather; o.target = kNullEntity; o.param = -1; issue(sim, e, o, queue); } // field at point
            break;
        case kCmdBuild: {
            if (cmd.param < 0 || size_t(cmd.param) >= rules_.types.size()) break;
            uint16_t bt = uint16_t(cmd.param);
            if (std::find(t.builds.begin(), t.builds.end(), bt) == t.builds.end()) break;
            if (!canTrain(sim, cmd.player, bt)) break;
            o.type = OrderType::Build; o.param = bt; o.target = kNullEntity; o.point = flat(cmd.point);
            issue(sim, e, o, queue);
            break;
        }
        case kCmdTrain: {
            auto* prod = w.tryGet<Production>(e);
            if (!prod || cmd.param < 0 || size_t(cmd.param) >= rules_.types.size()) break;
            uint16_t ut = uint16_t(cmd.param);
            if (std::find(t.produces.begin(), t.produces.end(), ut) == t.produces.end()) break;
            if (!canTrain(sim, cmd.player, ut) || int(prod->queue.size()) >= rules_.maxQueue) break;
            if (prod->ready != kNoType && rules_.types[ut].structure) break; // place the finished one first
            if (t.chargesMax > 0 && prod->charges <= 0) break;
            Entity recruit;
            if (rules_.types[ut].trainFrom != kNoType) {
                // made from an idle own unit standing near the producer (recruit + goods)
                Vec3 at = w.get<Transform>(e).pos;
                w.each<Unit, Owner, Transform>([&](Entity r, Unit& ru, Owner& ro, Transform& rt) {
                    if (recruit.valid() || ro.player != cmd.player || ru.type != rules_.types[ut].trainFrom || ru.hp.raw <= 0 || (ru.flags & (kUnitInHand | kUnitPossessed))) return;
                    if (auto* ord = w.tryGet<Orders>(r); ord && !ord->queue.empty()) return;
                    if (dist2D(rt.pos, at) <= Fixed::fromInt(15)) recruit = r;
                });
                if (!recruit.valid()) break;
            }
            if (rules_.costModel == CostModel::Upfront && !pay(sim, cmd.player, rules_.types[ut].cost, rules_.types[ut].supplyUsed)) break;
            if (recruit.valid()) { Unit& ru = w.get<Unit>(recruit); ru.hp = Fixed{}; ru.flags |= kUnitSilent; }
            if (rules_.costModel == CostModel::Progressive && !canAfford(sim, cmd.player, {}, rules_.types[ut].supplyUsed)) {
                sim.emit({kEvNotEnough, cmd.player, kNullEntity, kNullEntity, 255, {}});
                break;
            }
            if (t.chargesMax > 0) --prod->charges;
            prod->queue.push_back({int32_t(ut), Fixed{}, Fixed{}});
            gs.players[cmd.player].typeCountAll[ut]++;
            sim.emit({kEvTrainStarted, cmd.player, e, kNullEntity, ut, w.get<Transform>(e).pos});
            break;
        }
        case kCmdResearch: {
            auto* prod = w.tryGet<Production>(e);
            if (!prod || cmd.param < 0 || size_t(cmd.param) >= rules_.techs.size()) break;
            uint16_t k = uint16_t(cmd.param);
            if (std::find(t.researches.begin(), t.researches.end(), k) == t.researches.end()) break;
            auto& eco = gs.players[cmd.player];
            if (int(eco.researched[k]) >= rules_.techs[k].levels || eco.researching[k] || !requirementsMet(sim, cmd.player, rules_.techs[k].requires_)) break;
            if (int(prod->queue.size()) >= rules_.maxQueue) break;
            if (rules_.costModel == CostModel::Upfront && !pay(sim, cmd.player, techCost(sim, cmd.player, k), {})) break;
            eco.researching[k] = 1;
            prod->queue.push_back({~int32_t(k), Fixed{}, Fixed{}});
            break;
        }
        case kCmdCancelTrain: {
            if (auto* c = w.tryGet<Construction>(e)) {
                refund(sim, cmd.player, upfrontCost(t.cost), fxMin(kOne, c->paid));
                if (auto* inv = w.tryGet<Inventory>(e)) // delivered materials go back to the storehouses
                    for (size_t r = 0; r < inv->have.size(); ++r) if (inv->have[r].raw > 0) addResource(sim, cmd.player, uint16_t(r), inv->have[r]);
                w.get<Unit>(e).hp = Fixed{};
                w.get<Unit>(e).flags |= kUnitSilent;
                break;
            }
            auto* prod = w.tryGet<Production>(e);
            if (!prod || prod->queue.empty()) break;
            size_t slot = cmd.param < 0 || size_t(cmd.param) >= prod->queue.size() ? prod->queue.size() - 1 : size_t(cmd.param);
            ProdItem it = prod->queue[slot];
            const std::vector<Amount> cost = it.item >= 0 ? rules_.types[size_t(it.item)].cost : techCost(sim, cmd.player, uint16_t(~it.item));
            refund(sim, cmd.player, cost, rules_.costModel == CostModel::Upfront ? kOne : it.paid);
            if (it.item >= 0) gs.players[cmd.player].typeCountAll[size_t(it.item)]--;
            else gs.players[cmd.player].researching[size_t(~it.item)] = 0;
            if (t.chargesMax > 0) prod->charges = std::min(t.chargesMax, prod->charges + 1);
            prod->queue.erase(prod->queue.begin() + long(slot));
            break;
        }
        case kCmdUpgrade: {
            if (cmd.param < 0 || size_t(cmd.param) >= rules_.types.size()) break;
            uint16_t ut = uint16_t(cmd.param);
            if (std::find(t.upgrades.begin(), t.upgrades.end(), ut) == t.upgrades.end()) break;
            if (!requirementsMet(sim, cmd.player, rules_.types[ut].requires_)) break;
            if (!w.has<Production>(e)) w.add<Production>(e);
            auto& prod = w.get<Production>(e);
            bool busy = false;
            for (auto& it : prod.queue) if (it.upgrade) busy = true;
            if (busy || int(prod.queue.size()) >= rules_.maxQueue) break;
            std::vector<Amount> ucost = rules_.types[ut].cost;
            if (rules_.advancedEffects) { Fixed mult = stat(sim, e, Stat::UpgradeCost); for (auto& c : ucost) c.value = c.value * mult; }
            if (rules_.costModel == CostModel::Upfront && !pay(sim, cmd.player, ucost, {})) break;
            prod.queue.push_back({int32_t(ut), Fixed{}, Fixed{}, true});
            break;
        }
        case kCmdRally:
            if (auto* prod = w.tryGet<Production>(e)) { prod->rally = flat(target); prod->hasRally = true; prod->rallyTarget = o.target; }
            break;
        case kCmdPlace: {
            auto* prod = w.tryGet<Production>(e);
            if (!prod || prod->ready == kNoType) break;
            uint16_t st = prod->ready;
            if (!canPlace(sim, cmd.player, st, cmd.point)) break;
            prod->ready = kNoType;
            placeStructure(sim, cmd.player, st, cmd.point, kNullEntity, true);
            return; // one placement per command
        }
        case kCmdSell: {
            if (!t.structure || !t.sellable) break;
            refund(sim, cmd.player, t.cost, rules_.sellRefund * (w.get<Unit>(e).hp / fxMax(t.maxHp, kOne)));
            sim.emit({kEvSold, cmd.player, e, kNullEntity, w.get<Unit>(e).type, w.get<Transform>(e).pos});
            w.get<Unit>(e).hp = Fixed{};
            w.get<Unit>(e).flags |= kUnitSilent;
            break;
        }
        case kCmdDeploy: {
            if (t.deploysTo == kNoType) break;
            Vec3 at = w.get<Transform>(e).pos;
            if (!canPlace(sim, cmd.player, t.deploysTo, at, e)) break;
            w.get<Unit>(e).hp = Fixed{};
            w.get<Unit>(e).lastAttacker = 0;
            w.get<Unit>(e).flags |= kUnitSilent; // removed silently by sysDeath (no wreck/bounty)
            placeStructure(sim, cmd.player, t.deploysTo, at, kNullEntity, true);
            break;
        }
        case kCmdAbility: {
            if (cmd.param < 0 || size_t(cmd.param) >= t.abilities.size() || !canCast(sim, e, int(cmd.param))) break;
            const AbilityDef& a = t.abilities[size_t(cmd.param)];
            if (a.aura) break;
            if (a.target == TargetKind::None || a.target == TargetKind::Self) { castAbility(sim, e, int(cmd.param), e, w.get<Transform>(e).pos); break; }
            o.type = OrderType::Cast; o.param = int32_t(cmd.param);
            if (a.target == TargetKind::Point) o.target = kNullEntity;
            if (a.target == TargetKind::Unit && !o.target.valid()) break;
            if (w.has<Orders>(e)) issue(sim, e, o, queue);
            else if (!w.has<Mobile>(e)) castAbility(sim, e, int(cmd.param), o.target, o.point); // static casters
            break;
        }
        case kCmdReinforce: {
            auto* sq = w.tryGet<Squad>(e);
            if (!sq || sq->reinforcing || membersAlive(sim, e) >= t.members) break;
            // must be near an own structure that produces this type
            bool near = false;
            Vec3 p = w.get<Transform>(e).pos;
            w.each<Unit, Owner, Transform>([&](Entity, Unit& su, Owner& so, Transform& st) {
                if (so.player != cmd.player || !rules_.types[su.type].structure) return;
                if (dist2D(st.pos, p) < Fixed::fromInt(14)) near = true;
            });
            if (!near || !pay(sim, cmd.player, t.reinforceCost, {})) break;
            sq->reinforcing = true;
            sq->reinforceTimer = t.reinforceTime;
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
                if (auto* rs = w.tryGet<ResourceStock>(tg)) {
                    bool can = false;
                    for (auto& gd : t.gathers) if (gd.res == rs->res) can = true;
                    if (can && (!tt.ownerOnly || towner == cmd.player) && !w.has<Construction>(tg)) { o.type = OrderType::Gather; issue(sim, e, o, queue); break; }
                }
                if (cargo && cargo->amount.raw > 0 && towner == cmd.player && (tt.dropoffMask & (1u << cargo->res))) {
                    o.type = OrderType::Return; issue(sim, e, o, queue); break;
                }
                if (towner == cmd.player && !t.builds.empty()) {
                    if (w.has<Construction>(tg)) { o.type = OrderType::Build; o.param = -1; issue(sim, e, o, queue); break; }
                    if (tt.structure && w.get<Unit>(tg).hp < tt.maxHp) { o.type = OrderType::Repair; issue(sim, e, o, queue); break; }
                }
            } else {
                // right click on a resource field
                int cx = gs.grid.cellX(target.x), cy = gs.grid.cellY(target.z);
                if (gs.grid.inside(cx, cy)) {
                    int16_t fr = gs.grid.fieldRes[size_t(gs.grid.index(cx, cy))];
                    bool can = false;
                    for (auto& gd : t.gathers) if (gd.res == fr) can = true;
                    if (fr >= 0 && can) { o.type = OrderType::Gather; o.target = kNullEntity; o.param = fr; issue(sim, e, o, queue); break; }
                }
            }
            o.type = OrderType::Move; o.target = kNullEntity; o.param = movers;
            issue(sim, e, o, queue);
            break;
        }
        case kCmdSend: {
            auto* g = w.tryGet<Garrison>(e);
            if (!g || !w.alive(cmd.target) || cmd.target == e || !w.has<Garrison>(cmd.target) || t.spawnUnit == kNoType) break;
            if (rules_.advancedEffects && (statusFlags(sim, e) & kStLock)) break;   // locked: can't send
            int64_t pct = std::clamp<int64_t>(cmd.param, 1, 100);
            int n = int(int64_t(g->count) * pct / 100);
            if (n <= 0 && g->count > 0) n = 1;
            g->count -= n;
            g->pendingOut += n;
            g->pendingTarget = cmd.target;
            if (rules_.advancedEffects) {
                g->pendingGroup = gs.nextGroupId++;
                uint8_t to = w.get<Owner>(cmd.target).player;
                if (to < gs.players.size() && to != cmd.player && (gs.players[to].statusFlags & kStAlert))
                    sim.emit({kEvAlert, to, e, cmd.target, n, w.get<Transform>(cmd.target).pos});
            }
            break;
        }
        default: break;
        }
    }
}

// ================================================================= lane battler helpers

std::vector<Amount> RtsModule::autoBuildCost(Simulation& sim, uint8_t player, uint16_t type) const {
    std::vector<Amount> cost = rules_.autoBuildCost.empty() ? rules_.types[type].cost : rules_.autoBuildCost;
    auto* gs = sim.world().resource<GameState>();
    int built = player < gs->autoBuilt.size() ? gs->autoBuilt[player] : 0;
    for (auto& c : cost) c.value = c.value + c.value * rules_.autoBuildGrowth * built;
    return cost;
}

bool RtsModule::findAutoSpot(Simulation& sim, uint8_t player, uint16_t type, Vec3& out) {
    World& w = sim.world();
    // anchor: the player's key structure, else its first structure (lowest entity index)
    Entity anchor;
    bool anchorKey = false;
    w.each<Unit, Owner>([&](Entity e, Unit& u, Owner& o) {
        if (o.player != player || !rules_.types[u.type].structure) return;
        bool k = rules_.types[u.type].key;
        if (!anchor.valid() || (k && !anchorKey) || (k == anchorKey && e.index < anchor.index)) { anchor = e; anchorKey = k; }
    });
    if (!anchor.valid()) return false;
    Vec3 c = w.get<Transform>(anchor).pos;
    const UnitType& t = rules_.types[type];
    // prefer the side of the base facing away from the nearest enemy key/structure (keep the lane clear)
    Vec3 enemy = c;
    Fixed best = Fixed::fromInt(1 << 20);
    w.each<Unit, Owner, Transform>([&](Entity, Unit& u, Owner& o, Transform& tr) {
        if (!hostile(sim, player, o.player) || o.player == kHostileNeutral || !rules_.types[u.type].structure) return;
        Fixed d = dist2D(tr.pos, c);
        if (d < best) { best = d; enemy = tr.pos; }
    });
    Vec2 away = (c - enemy).xz();
    Fixed al = away.length();
    if (al.raw > 0) away = away / al;
    const int R = std::max(6, rules_.buildRadius > 0 ? rules_.buildRadius : 12);
    Fixed bestScore = Fixed::fromInt(1 << 20);
    bool found = false;
    int cx = int(c.x.floorInt()), cy = int(c.z.floorInt());
    for (int dy = -R; dy <= R; ++dy)
        for (int dx = -R; dx <= R; ++dx) {
            Vec3 p = snapToFootprint(t, {Fixed::fromInt(cx + dx) + kHalf, Fixed{}, Fixed::fromInt(cy + dy) + kHalf});
            int x0, y0;
            footprintAt(t, p, x0, y0);
            // keep a one-cell gap around structures so units can walk between them
            if (!state(sim).grid.areaBuildable(x0 - 1, y0 - 1, t.footprintW + 2, t.footprintH + 2) && t.placeOn == kNoType) continue;
            if (!canPlace(sim, player, type, p)) continue;
            Vec2 d = (p - c).xz();
            Fixed score = d.length() - d.dot(away) / 2;     // close to the core, behind it
            if (score < bestScore) { bestScore = score; out = p; found = true; }
        }
    return found;
}

uint32_t RtsModule::ticksUntilWave(Simulation& sim) const {
    auto* gs = sim.world().resource<GameState>();
    if (rules_.waveClock.raw <= 0) return 0;
    return gs->nextWaveTick > sim.tick() ? gs->nextWaveTick - sim.tick() : 0;
}

// ================================================================= hashing / result

int RtsModule::result(Simulation& sim) {
    auto* gs = sim.world().resource<GameState>();
    if (gs && gs->winnerTeam >= 0) return gs->winnerTeam == 0 ? -1 : gs->winnerTeam;
    if (gs && gs->victory.mode == VictoryMode::Survive) return 0; // decided by sysVictory
    int team = 0;
    for (auto& p : sim.players()) {
        if (!p.alive) continue;
        if (gs && std::find(gs->wavePlayers.begin(), gs->wavePlayers.end(), p.slot) != gs->wavePlayers.end()) continue;
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
        for (auto r : p.researched) h.u32(r);
    }
    for (auto o : gs.regionOwner) h.u32(o);
    for (auto t : gs.tickets) h.fx(t);
    for (auto t : gs.score) h.fx(t);
    for (auto& [k, v] : gs.trig.vars) { h.str(k); h.fx(v); }
    if (rules_.dungeon) hashDungeon(sim, h);
    if (rules_.settlers) hashSettlers(sim, h);
    if (rules_.advancedEffects) hashEffects(sim, h);
    if (rules_.lanes.enabled) {
        h.u32(gs.laneSignature); h.u32(uint32_t(gs.lanes.size()));
        for (auto& l : gs.lanes) { h.u32(uint32_t(l.points.size())); if (!l.points.empty()) h.v3(l.points[l.points.size() / 2]); }
        w.each<LaneWalker>([&](Entity e, LaneWalker& lw) { h.u64(e.packed()); h.i32(lw.lane); h.u32(lw.pos); h.u32(lw.reverse); });
        for (auto rr : gs.laneRoundRobin) h.u32(rr);
    }
    for (auto e : gs.trig.enabled) h.u32(e);
    h.u32(gs.nextWaveTick);
    if (auto* ss = w.resource<ScriptStore>()) ss->hash(h);   // only games with scripts
    if (gs.grid.hf) { // only heightfield games: every other game keeps its exact checksums
        h.u32(uint32_t(rules_.terrain.model)); h.u32(uint32_t(gs.grid.corner.size()));
        for (size_t i = 0; i < gs.grid.corner.size(); i += 13) h.i32(gs.grid.corner[i]);
    }
    for (size_t i = 0; i < gs.grid.fieldAmt.size(); i += 7) h.fx(gs.grid.fieldAmt[i]);
    h.u32(uint32_t(w.count<Unit>()));
    w.each<Unit, Owner, Transform>([&](Entity e, Unit& u, Owner& o, Transform& t) {
        h.u64(e.packed()); h.u32(u.type); h.fx(u.hp); h.u32(u.flags); h.u32(o.player); h.v3(t.pos); h.fx(u.energy);
        if (auto* ord = w.tryGet<Orders>(e)) { h.u32(uint32_t(ord->queue.size())); if (!ord->queue.empty()) { h.u32(uint32_t(ord->queue.front().type)); h.u32(ord->queue.front().state); } }
        if (auto* c = w.tryGet<Combat>(e)) { h.u64(c->target.packed()); h.i32(c->slot); h.u64(c->slotTarget.packed()); }
        if (auto* mb = w.tryGet<Mobile>(e)) { h.v3(mb->vel); h.u32(mb->yielded); h.u32(mb->yieldUntil); h.u32(mb->pathPos); h.u32(mb->moving); }
        if (auto* g = w.tryGet<Garrison>(e)) { h.i32(g->count); h.fx(g->growth); h.i32(g->pendingOut); }
        if (auto* p = w.tryGet<Production>(e)) { h.u32(uint32_t(p->queue.size())); if (!p->queue.empty()) h.fx(p->queue[0].progress); h.u32(p->ready); h.i32(p->charges); }
        if (auto* r = w.tryGet<ResourceStock>(e)) h.fx(r->amount);
        if (auto* c = w.tryGet<Cargo>(e)) { h.i32(c->res); h.fx(c->amount); }
        if (auto* hr = w.tryGet<Hero>(e)) { h.i32(hr->level); h.fx(hr->xp); }
        if (auto* cp = w.tryGet<Capture>(e)) { h.fx(cp->progress); h.u32(cp->team); }
    });
    h.u32(uint32_t(w.count<Projectile>()));
    w.each<Projectile, Transform>([&](Entity e, Projectile&, Transform& t) { h.u64(e.packed()); h.v3(t.pos); });
}

} // namespace rts::game
