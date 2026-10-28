// The `rts` table: low-level access for the engine's standard Lua library (modules/rts/lua/*.lua),
// which implements rule-level mechanics (victory, waves, triggers, ...) on top of the C++ core.
// Hot loops stay here as aggregate queries; Lua decides the rules. Everything mirrors the former
// C++ code exactly (same iteration order, same Fixed operations, same RNG calls), so matches are
// bit-identical to the C++ implementation. Available to game scripts too (advanced, unstable API).
#include "RtsScriptInternal.h"
#include <algorithm>
#include "RtsInternal.h"

namespace rts::game {
using namespace script;
using namespace detail;

namespace script {
namespace {

#define CTX RtsModule& m = *vm.module; Simulation& sim = *vm.sim; World& w = sim.world(); GameState& gs = m.state(sim); \
    const Rules& R = m.rules(); (void)m; (void)w; (void)gs; (void)R

int64_t intArg(lua_State* L, int idx) { return lua_isinteger(L, idx) ? lua_tointeger(L, idx) : 0; }
bool isWave(const GameState& gs, uint8_t slot) { return std::find(gs.wavePlayers.begin(), gs.wavePlayers.end(), slot) != gs.wavePlayers.end(); }

// ---------------------------------------------------------------- players / teams
int rPlayers(lua_State* L, Vm& vm) {
    auto& ps = vm.sim->players();
    lua_createtable(L, int(ps.size()), 0);
    int n = 0;
    for (auto& p : ps) {
        lua_createtable(L, 0, 3);
        lua_pushinteger(L, p.slot); lua_setfield(L, -2, "slot");
        lua_pushinteger(L, p.team); lua_setfield(L, -2, "team");
        lua_pushboolean(L, p.alive); lua_setfield(L, -2, "alive");
        lua_rawseti(L, -2, ++n);
    }
    return 1;
}
int rAlive(lua_State* L, Vm& vm) { auto* p = vm.sim->player(uint8_t(intArg(L, 1))); lua_pushboolean(L, p && p->alive); return 1; }
// Marks a player defeated with the standard event (value = reason, default 0 eliminated).
int rDefeat(lua_State* L, Vm& vm) {
    auto* p = vm.sim->player(uint8_t(intArg(L, 1)));
    if (!p) return 0;
    p->alive = false;
    vm.sim->emit({kEvPlayerDefeated, p->slot, kNullEntity, kNullEntity, intArg(L, 2), {}});
    return 0;
}
int rTeamOf(lua_State* L, Vm& vm) { lua_pushinteger(L, vm.module->teamOf(*vm.sim, uint8_t(intArg(L, 1)))); return 1; }
int rIsWavePlayer(lua_State* L, Vm& vm) { lua_pushboolean(L, isWave(vm.module->state(*vm.sim), uint8_t(intArg(L, 1)))); return 1; }
int rWavePlayers(lua_State* L, Vm& vm) {
    auto& wp = vm.module->state(*vm.sim).wavePlayers;
    lua_createtable(L, int(wp.size()), 0);
    for (size_t i = 0; i < wp.size(); ++i) { lua_pushinteger(L, wp[i]); lua_rawseti(L, -2, lua_Integer(i + 1)); }
    return 1;
}

// ---------------------------------------------------------------- victory
enum Presence : int { kPresAny = 1, kPresBuilt = 2, kPresUnit = 4, kPresKey = 8, kPresStructure = 16, kPresDeployer = 32 };
// What a player still owns among living units that count for victory, as flags (one pass in C++).
int rPresence(lua_State* L, Vm& vm) {
    CTX;
    const uint8_t slot = uint8_t(intArg(L, 1));
    int flags = 0;
    w.each<Unit, Owner>([&](Entity, Unit& u, Owner& o) {
        if (o.player != slot || u.hp.raw <= 0) return;
        const UnitType& t = R.types[u.type];
        if (!t.countsForVictory) return;
        flags |= kPresAny;
        if (t.structure) flags |= kPresStructure;
        if (t.structure && !(u.flags & kUnitUnderConstruction)) flags |= kPresBuilt;
        if (!t.structure) flags |= kPresUnit;
        if (t.key) flags |= kPresKey;
        if (t.deploysTo != kNoType) flags |= kPresDeployer;
    });
    lua_pushinteger(L, flags);
    return 1;
}
int rVictoryRules(lua_State* L, Vm& vm) {
    CTX;
    static const char* modes[] = {"noStructures", "noUnits", "noEntities", "keyUnits", "tickets", "survive", "score", "territory"};
    const VictoryRules& v = gs.victory;
    lua_createtable(L, 0, 8);
    lua_pushstring(L, modes[size_t(v.mode)]); lua_setfield(L, -2, "mode");
    lua_pushboolean(L, v.keyLoses); lua_setfield(L, -2, "key_loses");
    pushFx(L, v.share); lua_setfield(L, -2, "share");
    lua_pushinteger(L, (v.surviveSeconds * int64_t(sim.tickRate())).toInt()); lua_setfield(L, -2, "limit_ticks");
    bool anyKey = false;
    for (auto& t : R.types) anyKey |= t.key;
    lua_pushboolean(L, anyKey); lua_setfield(L, -2, "any_key_type");
    lua_pushboolean(L, R.territory.enabled); lua_setfield(L, -2, "territory_enabled");
    return 1;
}
int rTickets(lua_State* L, Vm& vm) {
    auto& t = vm.module->state(*vm.sim).tickets;
    int64_t k = intArg(L, 1);
    if (k < 0 || size_t(k) >= t.size()) lua_pushnil(L); else pushFx(L, t[size_t(k)]);
    return 1;
}
int rScore(lua_State* L, Vm& vm) {
    auto& s = vm.module->state(*vm.sim).score;
    int64_t k = intArg(L, 1);
    if (k < 0 || size_t(k) >= s.size()) lua_pushnil(L); else pushFx(L, s[size_t(k)]);
    return 1;
}
int rTerritory(lua_State* L, Vm& vm) { lua_pushinteger(L, vm.module->territoryOf(*vm.sim, uint8_t(intArg(L, 1)))); return 1; }
int rTerritoryTotal(lua_State* L, Vm& vm) { lua_pushinteger(L, vm.module->state(*vm.sim).territoryTotal); return 1; }
int rWinner(lua_State* L, Vm& vm) { lua_pushinteger(L, vm.module->state(*vm.sim).winnerTeam); return 1; }
int rSetWinner(lua_State* L, Vm& vm) { vm.module->state(*vm.sim).winnerTeam = int(intArg(L, 1)); return 0; }


// ---------------------------------------------------------------- generic primitives
Entity entArg(lua_State* L, int idx) { Entity e; argEntity(L, idx, e); return e; }
void pushV3(lua_State* L, Vec3 v) { pushFx(L, v.x); pushFx(L, v.y); pushFx(L, v.z); }
Fixed fxArg(lua_State* L, int idx) { Fixed* f = testFx(L, idx); return f ? *f : Fixed::fromInt(intArg(L, idx)); }
int rRngUnit(lua_State* L, Vm& vm) { pushFx(L, vm.sim->rng().unit()); return 1; }
int rRngBelow(lua_State* L, Vm& vm) { lua_pushinteger(L, vm.sim->rng().below(uint32_t(intArg(L, 1)))); return 1; }
// rts.emit(type, player, a, b, value, x, y, z): a SimEvent exactly like the C++ systems emit
int rEmit(lua_State* L, Vm& vm) {
    SimEvent e;
    e.type = uint16_t(intArg(L, 1)); e.player = uint8_t(intArg(L, 2));
    e.a = lua_isnoneornil(L, 3) ? kNullEntity : entArg(L, 3);
    e.b = lua_isnoneornil(L, 4) ? kNullEntity : entArg(L, 4);
    e.value = intArg(L, 5);
    if (!lua_isnoneornil(L, 6)) e.pos = {fxArg(L, 6), fxArg(L, 7), fxArg(L, 8)};
    vm.sim->emit(e);
    return 0;
}
// rts.spawn(typeIndex, owner, x, y, z) -> handle (module spawn, no checks)
int rSpawn(lua_State* L, Vm& vm) {
    const size_t t = size_t(intArg(L, 1));
    if (t >= vm.module->rules().types.size()) { lua_pushnil(L); return 1; }
    pushEntity(L, vm.module->spawn(*vm.sim, uint16_t(t), uint8_t(intArg(L, 2)), {fxArg(L, 3), fxArg(L, 4), fxArg(L, 5)}));
    return 1;
}
// rts.spawn_on_cell(typeIndex, owner, x, y, z, radius): nearest passable cell for the type's path layer within
// radius of the point's cell (planet-aware), spawned at its centre; nil when there is none
int rSpawnOnCell(lua_State* L, Vm& vm) {
    CTX;
    const size_t t = size_t(intArg(L, 1));
    if (t >= R.types.size()) { lua_pushnil(L); return 1; }
    int cx, cy;
    gs.grid.cellXY({fxArg(L, 3), fxArg(L, 4), fxArg(L, 5)}, cx, cy);
    if (!gs.grid.nearestPassable(cx, cy, R.types[t].pathLayer, int(intArg(L, 6)))) { lua_pushnil(L); return 1; }
    pushEntity(L, m.spawn(sim, uint16_t(t), uint8_t(intArg(L, 2)), gs.grid.cellCenter(cx, cy)));
    return 1;
}
// rts.free_spot(structure, typeIndex) -> x, y, z next to a structure for a unit of that type
int rFreeSpot(lua_State* L, Vm& vm) {
    CTX;
    Entity e = entArg(L, 1);
    const size_t t = size_t(intArg(L, 2));
    if (!w.alive(e) || !w.has<Transform>(e) || t >= R.types.size()) return 0;
    pushV3(L, m.freeSpotNear(sim, e, R.types[t].pathLayer));
    return 3;
}
// rts.order(unit, kind, x, z, param [, target]): an Order through RtsModule::issue (kind: OrderType index)
int rOrder(lua_State* L, Vm& vm) {
    CTX;
    Entity e = entArg(L, 1);
    if (!w.alive(e) || !w.has<Orders>(e)) { lua_pushboolean(L, 0); return 1; }
    Order o;
    o.type = OrderType(intArg(L, 2));
    o.point = {fxArg(L, 3), Fixed{}, fxArg(L, 4)};
    o.param = int32_t(intArg(L, 5));
    if (!lua_isnoneornil(L, 6)) o.target = entArg(L, 6);
    m.issue(sim, e, o, lua_toboolean(L, 7));
    lua_pushboolean(L, 1);
    return 1;
}
int rHasOrders(lua_State* L, Vm& vm) { Entity e = entArg(L, 1); lua_pushboolean(L, vm.sim->world().has<Orders>(e)); return 1; }
int rPos(lua_State* L, Vm& vm) {
    Entity e = entArg(L, 1);
    World& w = vm.sim->world();
    if (!w.alive(e) || !w.has<Transform>(e)) return 0;
    pushV3(L, w.get<Transform>(e).pos);
    return 3;
}
int rOwner(lua_State* L, Vm& vm) {
    Entity e = entArg(L, 1);
    World& w = vm.sim->world();
    if (!w.alive(e) || !w.has<Owner>(e)) { lua_pushnil(L); return 1; }
    lua_pushinteger(L, w.get<Owner>(e).player);
    return 1;
}

// ---------------------------------------------------------------- waves
// Static wave definitions of the map (GameState::waves): {player, tick, every, repeats, growth, spread, x, z, spawn = {{type, count}}}
int rWaves(lua_State* L, Vm& vm) {
    auto& waves = vm.module->state(*vm.sim).waves;
    lua_createtable(L, int(waves.size()), 0);
    for (size_t i = 0; i < waves.size(); ++i) {
        const WaveDef& wd = waves[i];
        lua_createtable(L, 0, 9);
        lua_pushinteger(L, wd.player); lua_setfield(L, -2, "player");
        lua_pushinteger(L, wd.tick); lua_setfield(L, -2, "tick");
        lua_pushinteger(L, wd.every); lua_setfield(L, -2, "every");
        lua_pushinteger(L, wd.repeats); lua_setfield(L, -2, "repeats");
        pushFx(L, wd.growth); lua_setfield(L, -2, "growth");
        pushFx(L, wd.spread); lua_setfield(L, -2, "spread");
        pushFx(L, wd.at.x); lua_setfield(L, -2, "x");
        pushFx(L, wd.at.y); lua_setfield(L, -2, "y");
        pushFx(L, wd.at.z); lua_setfield(L, -2, "z");
        lua_createtable(L, int(wd.spawn.size()), 0);
        for (size_t k = 0; k < wd.spawn.size(); ++k) {
            lua_createtable(L, 0, 2);
            lua_pushinteger(L, wd.spawn[k].first); lua_setfield(L, -2, "type");
            lua_pushinteger(L, wd.spawn[k].second); lua_setfield(L, -2, "count");
            lua_rawseti(L, -2, lua_Integer(k + 1));
        }
        lua_setfield(L, -2, "spawn");
        lua_rawseti(L, -2, lua_Integer(i + 1));
    }
    return 1;
}
int rWaveDone(lua_State* L, Vm& vm) {
    auto& waves = vm.module->state(*vm.sim).waves;
    const int64_t i = intArg(L, 1) - 1;
    if (i < 0 || size_t(i) >= waves.size()) return 0;
    if (!lua_isnoneornil(L, 2)) waves[size_t(i)].done = int(intArg(L, 2));
    lua_pushinteger(L, waves[size_t(i)].done);
    return 1;
}
// Lane waves: every structure with a wave spawner (not building / unpowered, owner alive) in ECS order:
// array of {from, type, count, owner}
int rLaneWaveSources(lua_State* L, Vm& vm) {
    CTX;
    lua_newtable(L);
    int n = 0;
    w.each<Unit, Owner>([&](Entity e, Unit& u, Owner& o) {
        const UnitType& t = R.types[u.type];
        if (t.waveUnit == kNoType || (u.flags & (kUnitUnderConstruction | kUnitDisabled)) || o.player == 0) return;
        auto* pi = sim.player(o.player);
        if (pi && !pi->alive) return;
        lua_createtable(L, 0, 4);
        pushEntity(L, e); lua_setfield(L, -2, "from");
        lua_pushinteger(L, t.waveUnit); lua_setfield(L, -2, "type");
        lua_pushinteger(L, t.waveCount); lua_setfield(L, -2, "count");
        lua_pushinteger(L, o.player); lua_setfield(L, -2, "owner");
        lua_rawseti(L, -2, ++n);
    });
    return 1;
}
int rWaveClock(lua_State* L, Vm& vm) {
    CTX;
    lua_pushinteger(L, R.waveClock.raw > 0 ? (R.waveClock * int64_t(sim.tickRate())).toInt() : -1);   // -1: no lane waves
    return 1;
}
int rNextWaveTick(lua_State* L, Vm& vm) {
    auto& gs = vm.module->state(*vm.sim);
    if (!lua_isnoneornil(L, 1)) gs.nextWaveTick = uint32_t(intArg(L, 1));
    lua_pushinteger(L, gs.nextWaveTick);
    return 1;
}
// Nearest hostile key structure (else any structure, else any unit) counting for victory: x, y, z or nothing
int rWaveObjective(lua_State* L, Vm& vm) {
    CTX;
    const uint8_t player = uint8_t(intArg(L, 1));
    const Vec3 from{fxArg(L, 2), fxArg(L, 3), fxArg(L, 4)};
    Vec3 best = from;
    Fixed bestD{};
    int bestRank = 9;
    bool found = false;
    w.each<Unit, Owner, Transform>([&](Entity, Unit& u, Owner& o, Transform& t) {
        if (!m.hostile(sim, player, o.player) || o.player == kHostileNeutral || u.hp.raw <= 0) return;
        const UnitType& ut = R.types[u.type];
        if (!ut.countsForVictory) return;
        int rank = ut.key ? 0 : ut.structure ? 1 : 2;
        Fixed d = dist2D(t.pos, from);
        if (!found || rank < bestRank || (rank == bestRank && d < bestD)) { best = t.pos; bestD = d; bestRank = rank; found = true; }
    });
    if (!found) return 0;
    pushV3(L, best);
    return 3;
}

// ---------------------------------------------------------------- triggers (lua/triggers.lua)
// Relations of TrigPlayer (rel: 0 triggering, 1 all, 2 enemies, 3 allies, 4 fixed slot, 5 hostile creeps).
bool playerMatches(Simulation& sim, RtsModule& m, const TrigPlayer& p, uint8_t slot, uint8_t triggering) {
    switch (p.rel) {
        case 0: return triggering == 0 || slot == triggering;
        case 1: return slot != 0;
        case 2: return triggering != 0 && m.hostile(sim, triggering, slot);
        case 3: return slot == triggering || (slot != 0 && slot != kHostileNeutral && triggering != 0 && triggering != kHostileNeutral && sim.allied(slot, triggering));
        case 4: return slot == uint8_t(p.slot);
        case 5: return slot == kHostileNeutral;
    }
    return false;
}
std::vector<uint8_t> resolvePlayers(Simulation& sim, RtsModule& m, const TrigPlayer& p, uint8_t triggering) {
    std::vector<uint8_t> out;
    if (p.rel == 4) { out.push_back(uint8_t(p.slot)); return out; }
    if (p.rel == 5) { out.push_back(kHostileNeutral); return out; }
    if (p.rel == 0) { if (triggering) out.push_back(triggering); return out; }
    for (auto& pi : sim.players()) if (playerMatches(sim, m, p, pi.slot, triggering)) out.push_back(pi.slot);
    return out;
}
const char* const kTrigEvents[] = {"start", "time", "every", "enterArea", "leaveArea", "unitDied", "built", "trained", "researched", "captured",
                                   "resource", "variable", "wave", "regionClaimed", "playerDefeated", "ability", "levelUp"};
const char* const kTrigCmp[] = {">=", ">", "<=", "<", "==", "!="};
const char* const kTrigCond[] = {"count", "resource", "variable", "researched", "random", "time", "alive", "regions", "script"};
const char* const kTrigAction[] = {"spawn", "give", "take", "setOwner", "kill", "remove", "damage", "heal", "buff", "message", "setVariable",
    "addVariable", "research", "enableType", "disableType", "victory", "defeat", "enableTrigger", "disableTrigger", "addScore",
    "setRegionOwner", "attackMove", "move", "setTickets", "castAbility", "script"};

void pushTrigPlayer(lua_State* L, const TrigPlayer& p) {
    lua_createtable(L, 0, 2);
    lua_pushinteger(L, p.rel); lua_setfield(L, -2, "rel");
    lua_pushinteger(L, uint8_t(p.slot)); lua_setfield(L, -2, "slot");
}
void pushTrigFilter(lua_State* L, const TrigFilter& f) {
    lua_createtable(L, 0, 4);
    if (f.type != kNoType) { lua_pushinteger(L, f.type); lua_setfield(L, -2, "type"); }
    lua_pushinteger(L, lua_Integer(f.tags)); lua_setfield(L, -2, "tags");
    lua_pushboolean(L, f.structures); lua_setfield(L, -2, "structures");
    lua_pushboolean(L, f.units); lua_setfield(L, -2, "units");
}
// The parsed triggers (rules + map) as Lua tables; indices are 1-based mirrors of TriggerSet::defs.
int rTriggerDefs(lua_State* L, Vm& vm) {
    const TriggerSet& ts = vm.module->triggers();
    lua_createtable(L, int(ts.defs.size()), 0);
    for (size_t i = 0; i < ts.defs.size(); ++i) {
        const TriggerDef& d = ts.defs[i];
        lua_createtable(L, 0, 16);
        lua_pushstring(L, d.id.c_str()); lua_setfield(L, -2, "id");
        lua_pushboolean(L, d.once); lua_setfield(L, -2, "once");
        lua_pushboolean(L, d.forEachPlayer); lua_setfield(L, -2, "for_each_player");
        lua_pushstring(L, kTrigEvents[size_t(d.event)]); lua_setfield(L, -2, "event");
        pushFx(L, d.seconds); lua_setfield(L, -2, "seconds");
        lua_pushinteger(L, d.area); lua_setfield(L, -2, "area");
        pushTrigFilter(L, d.filter); lua_setfield(L, -2, "filter");
        pushTrigPlayer(L, d.player); lua_setfield(L, -2, "player");
        lua_pushinteger(L, d.index); lua_setfield(L, -2, "index");
        lua_pushstring(L, d.name.c_str()); lua_setfield(L, -2, "name");
        lua_pushstring(L, kTrigCmp[size_t(d.cmp)]); lua_setfield(L, -2, "cmp");
        pushFx(L, d.value); lua_setfield(L, -2, "value");
        lua_createtable(L, int(d.conditions.size()), 0);
        for (size_t k = 0; k < d.conditions.size(); ++k) {
            const TrigCondition& c = d.conditions[k];
            lua_createtable(L, 0, 9);
            lua_pushstring(L, kTrigCond[size_t(c.kind)]); lua_setfield(L, -2, "kind");
            pushTrigPlayer(L, c.player); lua_setfield(L, -2, "player");
            lua_pushinteger(L, c.index); lua_setfield(L, -2, "index");
            lua_pushstring(L, c.name.c_str()); lua_setfield(L, -2, "name");
            lua_pushstring(L, kTrigCmp[size_t(c.cmp)]); lua_setfield(L, -2, "cmp");
            pushFx(L, c.value); lua_setfield(L, -2, "value");
            lua_pushboolean(L, c.negate); lua_setfield(L, -2, "negate");
            lua_rawseti(L, -2, lua_Integer(k + 1));
        }
        lua_setfield(L, -2, "conditions");
        lua_createtable(L, int(d.actions.size()), 0);
        for (size_t k = 0; k < d.actions.size(); ++k) {
            const TrigAction& a = d.actions[k];
            lua_createtable(L, 0, 20);
            lua_pushstring(L, kTrigAction[size_t(a.kind)]); lua_setfield(L, -2, "kind");
            pushTrigPlayer(L, a.player); lua_setfield(L, -2, "player");
            lua_pushinteger(L, a.area); lua_setfield(L, -2, "area");
            lua_pushboolean(L, a.triggeringUnit); lua_setfield(L, -2, "triggering_unit");
            if (a.type != kNoType) { lua_pushinteger(L, a.type); lua_setfield(L, -2, "type"); }
            lua_pushinteger(L, a.count); lua_setfield(L, -2, "count");
            lua_pushinteger(L, a.index); lua_setfield(L, -2, "index");
            pushFx(L, a.amount); lua_setfield(L, -2, "amount");
            pushFx(L, a.duration); lua_setfield(L, -2, "duration");
            lua_pushinteger(L, int(a.mod.stat)); lua_setfield(L, -2, "stat");
            pushFx(L, a.mod.add); lua_setfield(L, -2, "add");
            pushFx(L, a.mod.mul); lua_setfield(L, -2, "mul");
            lua_pushboolean(L, a.hasAt); lua_setfield(L, -2, "has_at");
            pushFx(L, a.at.x); lua_setfield(L, -2, "at_x");
            pushFx(L, a.at.z); lua_setfield(L, -2, "at_z");
            lua_pushboolean(L, a.attackMove); lua_setfield(L, -2, "attack_move");
            lua_pushstring(L, a.text.c_str()); lua_setfield(L, -2, "text");
            lua_pushstring(L, a.name.c_str()); lua_setfield(L, -2, "name");
            lua_pushinteger(L, a.kind == TrigAction::EnableTrigger || a.kind == TrigAction::DisableTrigger ? ts.findTrigger(a.name) : -1);
            lua_setfield(L, -2, "trigger");
            lua_rawseti(L, -2, lua_Integer(k + 1));
        }
        lua_setfield(L, -2, "actions");
        lua_rawseti(L, -2, lua_Integer(i + 1));
    }
    return 1;
}
int rTriggerAreas(lua_State* L, Vm& vm) {
    const TriggerSet& ts = vm.module->triggers();
    lua_createtable(L, int(ts.areas.size()), 0);
    for (size_t i = 0; i < ts.areas.size(); ++i) {
        const TrigArea& a = ts.areas[i];
        lua_createtable(L, 0, 4);
        pushFx(L, a.x); lua_setfield(L, -2, "x");
        pushFx(L, a.z); lua_setfield(L, -2, "z");
        pushFx(L, a.w); lua_setfield(L, -2, "w");
        pushFx(L, a.h); lua_setfield(L, -2, "h");
        lua_rawseti(L, -2, lua_Integer(i + 1));
    }
    return 1;
}
// Per-type flags for filters: {tags = {...}, structure = {...}} indexed by type index + 1
int rTypeFlags(lua_State* L, Vm& vm) {
    const Rules& R = vm.module->rules();
    lua_createtable(L, 0, 2);
    lua_createtable(L, int(R.types.size()), 0);
    for (size_t i = 0; i < R.types.size(); ++i) { lua_pushinteger(L, lua_Integer(R.types[i].tags)); lua_rawseti(L, -2, lua_Integer(i + 1)); }
    lua_setfield(L, -2, "tags");
    lua_createtable(L, int(R.types.size()), 0);
    for (size_t i = 0; i < R.types.size(); ++i) { lua_pushboolean(L, R.types[i].structure); lua_rawseti(L, -2, lua_Integer(i + 1)); }
    lua_setfield(L, -2, "structure");
    return 1;
}
// First call of a match: the runtime arrays of TriggerState sized for the defs.
int rTrigInit(lua_State*, Vm& vm) {
    TriggerState& ts = vm.module->state(*vm.sim).trig;
    const auto& defs = vm.module->triggers().defs;
    const size_t nd = defs.size(), slots = 256;
    if (ts.enabled.size() != nd) {
        ts.enabled.resize(nd);
        for (size_t i = 0; i < nd; ++i) ts.enabled[i] = defs[i].enabled;
        ts.fired.assign(nd, std::vector<uint8_t>(slots, 0));
        ts.inside.assign(nd, {});
        ts.edge.assign(nd, std::vector<uint8_t>(slots, 0));
    }
    return 0;
}
// get/set of TriggerState flags: rts.trig_flag("enabled"|"fired"|"edge", def [, player] [, value])
int rTrigEnabled(lua_State* L, Vm& vm) {
    auto& v = vm.module->state(*vm.sim).trig.enabled;
    const size_t di = size_t(intArg(L, 1) - 1);
    if (di >= v.size()) { lua_pushboolean(L, 0); return 1; }
    if (!lua_isnone(L, 2)) v[di] = lua_toboolean(L, 2);
    lua_pushboolean(L, v[di]);
    return 1;
}
int rTrigFired(lua_State* L, Vm& vm) {
    auto& v = vm.module->state(*vm.sim).trig.fired;
    const size_t di = size_t(intArg(L, 1) - 1), p = size_t(intArg(L, 2));
    if (di >= v.size() || p >= v[di].size()) { lua_pushboolean(L, 0); return 1; }
    if (!lua_isnone(L, 3)) v[di][p] = lua_toboolean(L, 3);
    lua_pushboolean(L, v[di][p]);
    return 1;
}
int rTrigEdge(lua_State* L, Vm& vm) {
    auto& v = vm.module->state(*vm.sim).trig.edge;
    const size_t di = size_t(intArg(L, 1) - 1), p = size_t(intArg(L, 2));
    if (di >= v.size() || p >= v[di].size()) { lua_pushboolean(L, 0); return 1; }
    if (!lua_isnone(L, 3)) v[di][p] = lua_toboolean(L, 3);
    lua_pushboolean(L, v[di][p]);
    return 1;
}
int rTrigStarted(lua_State* L, Vm& vm) {
    auto& ts = vm.module->state(*vm.sim).trig;
    if (!lua_isnone(L, 1)) ts.started = lua_toboolean(L, 1);
    lua_pushboolean(L, ts.started);
    return 1;
}
// Simulation events of this tick that triggers listen to: {type, player, a (handle or nil), value, x, y, z}
// (mask: bit per event type the caller listens to)
int rTrigEvents(lua_State* L, Vm& vm) {
    const uint64_t mask = uint64_t(intArg(L, 1));
    const std::vector<SimEvent> evs = vm.sim->events();
    lua_newtable(L);
    int n = 0;
    for (const SimEvent& ev : evs) {
        if (ev.type >= 64 || !((mask >> ev.type) & 1)) continue;
        lua_createtable(L, 0, 7);
        lua_pushinteger(L, ev.type); lua_setfield(L, -2, "type");
        lua_pushinteger(L, ev.player); lua_setfield(L, -2, "player");
        if (ev.a.valid()) { pushEntity(L, ev.a); lua_setfield(L, -2, "a"); }
        lua_pushinteger(L, ev.value); lua_setfield(L, -2, "value");
        pushFx(L, ev.pos.x); lua_setfield(L, -2, "x");
        pushFx(L, ev.pos.y); lua_setfield(L, -2, "y");
        pushFx(L, ev.pos.z); lua_setfield(L, -2, "z");
        lua_rawseti(L, -2, ++n);
    }
    return 1;
}
int rResolvePlayers(lua_State* L, Vm& vm) {   // (def, kind "c"|"a"|"e", index, triggering) -> slots
    const auto& d = vm.module->triggers().defs[size_t(intArg(L, 1) - 1)];
    const char* k = lua_tostring(L, 2);
    const size_t i = size_t(intArg(L, 3) - 1);
    const TrigPlayer& tp = *k == 'c' ? d.conditions[i].player : *k == 'a' ? d.actions[i].player : d.player;
    auto v = resolvePlayers(*vm.sim, *vm.module, tp, uint8_t(intArg(L, 4)));
    lua_createtable(L, int(v.size()), 0);
    for (size_t j = 0; j < v.size(); ++j) { lua_pushinteger(L, v[j]); lua_rawseti(L, -2, lua_Integer(j + 1)); }
    return 1;
}
int rPlayerMatches(lua_State* L, Vm& vm) {    // (def, slot, triggering): the event's player filter
    const auto& d = vm.module->triggers().defs[size_t(intArg(L, 1) - 1)];
    lua_pushboolean(L, playerMatches(*vm.sim, *vm.module, d.player, uint8_t(intArg(L, 2)), uint8_t(intArg(L, 3))));
    return 1;
}
// Condition "count": living units matching the condition's filter / player / area
int rTrigCount(lua_State* L, Vm& vm) {
    CTX;
    const auto& c = m.triggers().defs[size_t(intArg(L, 1) - 1)].conditions[size_t(intArg(L, 2) - 1)];
    const uint8_t player = uint8_t(intArg(L, 3));
    int n = 0;
    w.each<Unit, Owner, Transform>([&](Entity, Unit& u, Owner& o, Transform& t) {
        if (u.hp.raw <= 0 || !c.filter.matches(R, u.type) || !playerMatches(sim, m, c.player, o.player, player)) return;
        if (c.area >= 0 && !m.triggers().areas[size_t(c.area)].contains(t.pos)) return;
        ++n;
    });
    lua_pushinteger(L, n);
    return 1;
}
// Action targets in ECS order: the triggering unit, or living units matching filter / players / area
int rTrigTargets(lua_State* L, Vm& vm) {
    CTX;
    const auto& a = m.triggers().defs[size_t(intArg(L, 1) - 1)].actions[size_t(intArg(L, 2) - 1)];
    const uint8_t player = uint8_t(intArg(L, 3));
    Entity unit = lua_isnoneornil(L, 4) ? kNullEntity : entArg(L, 4);
    std::vector<Entity> out;
    if (a.triggeringUnit) { if (w.alive(unit) && w.has<Unit>(unit)) out.push_back(unit); }
    else {
        std::vector<uint8_t> players = resolvePlayers(sim, m, a.player, player);
        w.each<Unit, Owner, Transform>([&](Entity e, Unit& u, Owner& o, Transform& t) {
            if (u.hp.raw <= 0 || !a.filter.matches(R, u.type)) return;
            if (std::find(players.begin(), players.end(), o.player) == players.end()) return;
            if (a.area >= 0 && !m.triggers().areas[size_t(a.area)].contains(t.pos)) return;
            out.push_back(e);
        });
    }
    lua_createtable(L, int(out.size()), 0);
    for (size_t i = 0; i < out.size(); ++i) { pushEntity(L, out[i]); lua_rawseti(L, -2, lua_Integer(i + 1)); }
    return 1;
}
// setOwner without a triggering unit: every unit matching filter / area (any owner, any hp)
int rTrigOwnerTargets(lua_State* L, Vm& vm) {
    CTX;
    const auto& a = m.triggers().defs[size_t(intArg(L, 1) - 1)].actions[size_t(intArg(L, 2) - 1)];
    std::vector<Entity> out;
    w.each<Unit, Owner, Transform>([&](Entity e, Unit& u, Owner&, Transform& t) {
        if (a.filter.matches(R, u.type) && (a.area < 0 || m.triggers().areas[size_t(a.area)].contains(t.pos))) out.push_back(e);
    });
    lua_createtable(L, int(out.size()), 0);
    for (size_t i = 0; i < out.size(); ++i) { pushEntity(L, out[i]); lua_rawseti(L, -2, lua_Integer(i + 1)); }
    return 1;
}
// enterArea / leaveArea: updates the def's inside set; returns the entities to fire in order
// (enter: {e, owner at scan time}, leave: {e} of units that left and still exist)
int rTrigAreaUpdate(lua_State* L, Vm& vm) {
    CTX;
    const size_t di = size_t(intArg(L, 1) - 1);
    const TriggerDef& d = m.triggers().defs[di];
    const TrigArea& ar = m.triggers().areas[size_t(d.area)];
    std::vector<uint64_t> now;
    std::vector<std::pair<Entity, uint8_t>> entered;
    w.each<Unit, Owner, Transform>([&](Entity e, Unit& u, Owner& o, Transform& t) {
        if (u.hp.raw <= 0 || !d.filter.matches(R, u.type) || !playerMatches(sim, m, d.player, o.player, o.player)) return;
        if (!ar.contains(t.pos)) return;
        now.push_back(e.packed());
        entered.push_back({e, o.player});
    });
    std::sort(now.begin(), now.end());
    auto& prev = gs.trig.inside[di];
    lua_newtable(L);
    int n = 0;
    if (d.event == TrigEvent::EnterArea) {
        std::sort(entered.begin(), entered.end(), [](auto& x, auto& y) { return x.first.packed() < y.first.packed(); });
        for (auto& [e, owner] : entered)
            if (!std::binary_search(prev.begin(), prev.end(), e.packed())) {
                lua_createtable(L, 2, 0);
                pushEntity(L, e); lua_rawseti(L, -2, 1);
                lua_pushinteger(L, owner); lua_rawseti(L, -2, 2);
                lua_rawseti(L, -2, ++n);
            }
    } else {
        for (uint64_t pk : prev)
            if (!std::binary_search(now.begin(), now.end(), pk)) {
                lua_createtable(L, 1, 0);
                pushEntity(L, Entity::fromPacked(pk)); lua_rawseti(L, -2, 1);
                lua_rawseti(L, -2, ++n);
            }
    }
    prev.swap(now);
    return 1;
}
int rAliveEntity(lua_State* L, Vm& vm) { lua_pushboolean(L, vm.sim->world().alive(entArg(L, 1))); return 1; }
int rIsUnit(lua_State* L, Vm& vm) { Entity e = entArg(L, 1); lua_pushboolean(L, vm.sim->world().alive(e) && vm.sim->world().has<Unit>(e)); return 1; }
int rTrigScriptCondition(lua_State* L, Vm& vm) {
    CTX;
    const auto& c = m.triggers().defs[size_t(intArg(L, 1) - 1)].conditions[size_t(intArg(L, 2) - 1)];
    bool r = m.scripts()->callCondition(sim, c.name, c.args, uint8_t(intArg(L, 3)), kNullEntity, {});
    lua_sethook(L, nullptr, 0, 0);   // back in engine code: no budget
    lua_pushboolean(L, r);
    return 1;
}
int rTrigScriptAction(lua_State* L, Vm& vm) {
    CTX;
    const auto& d = m.triggers().defs[size_t(intArg(L, 1) - 1)];
    const auto& a = d.actions[size_t(intArg(L, 2) - 1)];
    Entity unit = lua_isnoneornil(L, 4) ? kNullEntity : entArg(L, 4);
    m.scripts()->callAction(sim, a.name, a.args, uint8_t(intArg(L, 3)), unit, {fxArg(L, 5), fxArg(L, 6), fxArg(L, 7)}, d.id);
    lua_sethook(L, nullptr, 0, 0);
    return 0;
}

// ---------------------------------------------------------------- state primitives
int rVar(lua_State* L, Vm& vm) {
    auto& vars = vm.module->state(*vm.sim).trig.vars;
    const char* k = lua_tostring(L, 1);
    if (!k) return 0;
    if (!lua_isnone(L, 2)) { vars[k] = fxArg(L, 2); pushFx(L, vars[k]); return 1; }
    auto it = vars.find(k);
    pushFx(L, it == vars.end() ? Fixed{} : it->second);
    return 1;
}
int rAddVar(lua_State* L, Vm& vm) { Fixed& x = vm.module->state(*vm.sim).trig.vars[lua_tostring(L, 1) ? lua_tostring(L, 1) : ""]; x += fxArg(L, 2); return 0; }
int rPlayerSlots(lua_State* L, Vm& vm) { lua_pushinteger(L, lua_Integer(vm.module->state(*vm.sim).players.size())); return 1; }
int rStock(lua_State* L, Vm& vm) {   // (player, res [, set]): raw stock (no caps)
    auto& gs = vm.module->state(*vm.sim);
    const size_t p = size_t(intArg(L, 1)), r = size_t(intArg(L, 2));
    if (p >= gs.players.size() || r >= gs.players[p].stock.size()) { pushFx(L, Fixed{}); return 1; }
    if (!lua_isnone(L, 3)) gs.players[p].stock[r] = fxArg(L, 3);
    pushFx(L, gs.players[p].stock[r]);
    return 1;
}
int rAddResource(lua_State* L, Vm& vm) { vm.module->addResource(*vm.sim, uint8_t(intArg(L, 1)), uint16_t(intArg(L, 2)), fxArg(L, 3)); return 0; }
int rResearched(lua_State* L, Vm& vm) {
    auto& gs = vm.module->state(*vm.sim);
    const size_t p = size_t(intArg(L, 1)), t = size_t(intArg(L, 2));
    lua_pushboolean(L, p < gs.players.size() && t < gs.players[p].researched.size() && gs.players[p].researched[t]);
    return 1;
}
int rResearch(lua_State* L, Vm& vm) {
    auto& gs = vm.module->state(*vm.sim);
    const size_t p = size_t(intArg(L, 1)), t = size_t(intArg(L, 2));
    if (p >= gs.players.size()) return 0;
    gs.players[p].researched[t] = 1;
    vm.module->recomputeMods(*vm.sim, uint8_t(p));
    return 0;
}
int rTypeDisabled(lua_State* L, Vm& vm) {
    auto& gs = vm.module->state(*vm.sim);
    const size_t p = size_t(intArg(L, 1)), t = size_t(intArg(L, 2));
    if (p < gs.players.size() && t < gs.players[p].typeDisabled.size()) gs.players[p].typeDisabled[t] = lua_toboolean(L, 3);
    return 0;
}
int rHp(lua_State* L, Vm& vm) {   // (e [, set])
    Entity e = entArg(L, 1);
    auto* u = vm.sim->world().tryGet<Unit>(e);
    if (!u) return 0;
    if (!lua_isnone(L, 2)) u->hp = fxArg(L, 2);
    pushFx(L, u->hp);
    return 1;
}
int rAddUnitFlags(lua_State* L, Vm& vm) { if (auto* u = vm.sim->world().tryGet<Unit>(entArg(L, 1))) u->flags |= uint8_t(intArg(L, 2)); return 0; }
int rStat(lua_State* L, Vm& vm) { pushFx(L, vm.module->stat(*vm.sim, entArg(L, 1), Stat(intArg(L, 2)))); return 1; }
int rDealDamage(lua_State* L, Vm& vm) {   // (e, amount, damageType, source|nil, sourceOwner)
    vm.module->dealDamage(*vm.sim, entArg(L, 1), fxArg(L, 2), uint16_t(intArg(L, 3)), lua_isnoneornil(L, 4) ? kNullEntity : entArg(L, 4), uint8_t(intArg(L, 5)));
    return 0;
}
int rAddBuff(lua_State* L, Vm& vm) {     // (e, stat, add, mul, until)
    if (auto* b = vm.sim->world().tryGet<Buffs>(entArg(L, 1)))
        b->list.push_back({Stat(intArg(L, 2)), fxArg(L, 3), fxArg(L, 4), uint32_t(intArg(L, 5)), kNoType});
    return 0;
}
int rSetOwner(lua_State* L, Vm& vm) { if (auto* o = vm.sim->world().tryGet<Owner>(entArg(L, 1))) o->player = uint8_t(intArg(L, 2)); return 0; }
int rMessageIndex(lua_State* L, Vm& vm) {
    auto& msgs = vm.module->state(*vm.sim).trig.messages;
    const std::string text = lua_tostring(L, 1) ? lua_tostring(L, 1) : "";
    auto it = std::find(msgs.begin(), msgs.end(), text);
    const int64_t idx = it - msgs.begin();
    if (it == msgs.end()) msgs.push_back(text);
    lua_pushinteger(L, idx);
    return 1;
}
int rAddScore(lua_State* L, Vm& vm) {     // (team, amount)
    auto& s = vm.module->state(*vm.sim).score;
    const size_t t = size_t(intArg(L, 1));
    if (t < s.size()) s[t] += fxArg(L, 2);
    return 0;
}
int rSetTickets(lua_State* L, Vm& vm) {
    auto& s = vm.module->state(*vm.sim).tickets;
    const size_t t = size_t(intArg(L, 1));
    if (t < s.size()) s[t] = fxArg(L, 2);
    return 0;
}
int rSetRegionOwnerByName(lua_State* L, Vm& vm) {
    auto& gs = vm.module->state(*vm.sim);
    const std::string name = lua_tostring(L, 1) ? lua_tostring(L, 1) : "";
    for (int i = 0; i < gs.regionCount; ++i)
        if (gs.regionNames[size_t(i)] == name) gs.regionOwner[size_t(i)] = uint8_t(intArg(L, 2));
    return 0;
}
int rRegionsOwned(lua_State* L, Vm& vm) {
    int n = 0;
    for (auto o : vm.module->state(*vm.sim).regionOwner) if (o == intArg(L, 1)) ++n;
    lua_pushinteger(L, n);
    return 1;
}
// Nearest hostile (non-creep) structure of any hp to (x, z), first in ECS order on ties: x, y, z or nothing
int rNearestEnemyStructure(lua_State* L, Vm& vm) {
    CTX;
    const uint8_t owner = uint8_t(intArg(L, 1));
    const Vec3 np{fxArg(L, 2), fxArg(L, 3), fxArg(L, 4)};
    Entity best; Fixed bestD{};
    w.each<Unit, Owner, Transform>([&](Entity e, Unit& u, Owner& o, Transform& t) {
        if (!m.hostile(sim, owner, o.player) || o.player == kHostileNeutral || !R.types[u.type].structure) return;
        Fixed dd = dist2D(t.pos, np);
        if (!best.valid() || dd < bestD) { best = e; bestD = dd; }
    });
    if (!best.valid()) return 0;
    pushV3(L, w.get<Transform>(best).pos);
    return 3;
}
// (player, ability index, x, y, z, unit|nil, free)
int rCastPlayerAbility(lua_State* L, Vm& vm) {
    CTX;
    const uint8_t p = uint8_t(intArg(L, 1));
    const int idx = int(intArg(L, 2));
    const Vec3 at{fxArg(L, 3), fxArg(L, 4), fxArg(L, 5)};
    Entity unit = lua_isnoneornil(L, 6) ? kNullEntity : entArg(L, 6);
    if (!lua_toboolean(L, 7)) { m.castPlayerAbility(sim, p, idx, at, unit); return 0; }
    EffectCtx ctx; ctx.owner = p; ctx.point = flat(at); ctx.origin = ctx.point; ctx.target = w.alive(unit) ? unit : kNullEntity;
    sim.emit({kEvPlayerAbility, p, kNullEntity, ctx.target, idx, ctx.point});
    m.applyPayload(sim, m.playerAbilityFor(sim, p, idx).payload, ctx);
    return 0;
}
int rHostile(lua_State* L, Vm& vm) { lua_pushboolean(L, vm.module->hostile(*vm.sim, uint8_t(intArg(L, 1)), uint8_t(intArg(L, 2)))); return 1; }
int rRatio(lua_State* L, Vm&) { pushFx(L, Fixed::fromRatio(intArg(L, 1), intArg(L, 2))); return 1; }

// ---------------------------------------------------------------- commands (lua/commands.lua)
void pushCost(lua_State* L, const std::vector<Amount>& cost) {
    lua_createtable(L, int(cost.size()), 0);
    for (size_t i = 0; i < cost.size(); ++i) {
        lua_createtable(L, 2, 0);
        lua_pushinteger(L, cost[i].res); lua_rawseti(L, -2, 1);
        pushFx(L, cost[i].value); lua_rawseti(L, -2, 2);
        lua_rawseti(L, -2, lua_Integer(i + 1));
    }
}
std::vector<Amount> readCost(lua_State* L, int idx) {
    std::vector<Amount> out;
    if (!lua_istable(L, idx)) return out;
    const lua_Integer n = lua_Integer(lua_rawlen(L, idx));
    for (lua_Integer i = 1; i <= n; ++i) {
        lua_rawgeti(L, idx, i);
        lua_rawgeti(L, -1, 1); lua_rawgeti(L, -2, 2);
        out.push_back({uint16_t(intArg(L, -2)), fxArg(L, -1)});
        lua_pop(L, 3);
    }
    return out;
}
int rAutoBuildRules(lua_State* L, Vm& vm) {
    const Rules& R = vm.module->rules();
    lua_createtable(L, 0, 3);
    lua_createtable(L, int(R.autoBuildPool.size()), 0);
    for (size_t i = 0; i < R.autoBuildPool.size(); ++i) { lua_pushinteger(L, R.autoBuildPool[i]); lua_rawseti(L, -2, lua_Integer(i + 1)); }
    lua_setfield(L, -2, "pool");
    if (!R.autoBuildCost.empty()) { pushCost(L, R.autoBuildCost); lua_setfield(L, -2, "cost"); }
    pushFx(L, R.autoBuildGrowth); lua_setfield(L, -2, "growth");
    return 1;
}
int rTypeCost(lua_State* L, Vm& vm) {
    const auto& types = vm.module->rules().types;
    const size_t t = size_t(intArg(L, 1));
    if (t >= types.size()) return 0;
    pushCost(L, types[t].cost);
    return 1;
}
int rAutoBuilt(lua_State* L, Vm& vm) {
    auto& v = vm.module->state(*vm.sim).autoBuilt;
    const size_t p = size_t(intArg(L, 1));
    if (p >= v.size()) { lua_pushinteger(L, 0); return 1; }
    if (!lua_isnone(L, 2)) v[p] = int(intArg(L, 2));
    lua_pushinteger(L, v[p]);
    return 1;
}
int rCanTrain(lua_State* L, Vm& vm) { lua_pushboolean(L, vm.module->canTrain(*vm.sim, uint8_t(intArg(L, 1)), uint16_t(intArg(L, 2)))); return 1; }
int rFindAutoSpot(lua_State* L, Vm& vm) {
    Vec3 spot;
    if (!vm.module->findAutoSpot(*vm.sim, uint8_t(intArg(L, 1)), uint16_t(intArg(L, 2)), spot)) return 0;
    pushV3(L, spot);
    return 3;
}
// (player, cost) -> paid; emits "not enough" on failure like every other purchase
int rPay(lua_State* L, Vm& vm) { lua_pushboolean(L, vm.module->pay(*vm.sim, uint8_t(intArg(L, 1)), readCost(L, 2), {})); return 1; }
int rRefund(lua_State* L, Vm& vm) { vm.module->refund(*vm.sim, uint8_t(intArg(L, 1)), readCost(L, 2), fxArg(L, 3)); return 0; }
int rPlaceStructure(lua_State* L, Vm& vm) {   // (player, type, x, y, z, complete) -> handle
    pushEntity(L, vm.module->placeStructure(*vm.sim, uint8_t(intArg(L, 1)), uint16_t(intArg(L, 2)), {fxArg(L, 3), fxArg(L, 4), fxArg(L, 5)},
                                            kNullEntity, lua_toboolean(L, 6)));
    return 1;
}
// Static per-type data for sell rules: {structure, sellable, max_hp} by type index + 1
int rSellRules(lua_State* L, Vm& vm) {
    const Rules& R = vm.module->rules();
    lua_createtable(L, 0, 4);
    pushFx(L, R.sellRefund); lua_setfield(L, -2, "refund");
    lua_createtable(L, int(R.types.size()), 0);
    for (size_t i = 0; i < R.types.size(); ++i) {
        lua_createtable(L, 0, 3);
        lua_pushboolean(L, R.types[i].structure && R.types[i].sellable); lua_setfield(L, -2, "sellable");
        pushFx(L, R.types[i].maxHp); lua_setfield(L, -2, "max_hp");
        lua_rawseti(L, -2, lua_Integer(i + 1));
    }
    lua_setfield(L, -2, "types");
    return 1;
}
int rUnitType(lua_State* L, Vm& vm) { auto* u = vm.sim->world().tryGet<Unit>(entArg(L, 1)); if (!u) return 0; lua_pushinteger(L, u->type); return 1; }

} // namespace

void openStdApi(Vm& vm) {
    static const luaL_Reg funcs[] = {
        {"players", bind<rPlayers>}, {"alive", bind<rAlive>}, {"defeat", bind<rDefeat>}, {"team_of", bind<rTeamOf>},
        {"is_wave_player", bind<rIsWavePlayer>}, {"wave_players", bind<rWavePlayers>},
        {"presence", bind<rPresence>}, {"victory_rules", bind<rVictoryRules>}, {"tickets", bind<rTickets>}, {"score", bind<rScore>},
        {"territory", bind<rTerritory>}, {"territory_total", bind<rTerritoryTotal>}, {"winner", bind<rWinner>}, {"set_winner", bind<rSetWinner>},
        {"rng_unit", bind<rRngUnit>}, {"rng_below", bind<rRngBelow>}, {"emit", bind<rEmit>}, {"spawn", bind<rSpawn>},
        {"spawn_on_cell", bind<rSpawnOnCell>}, {"free_spot", bind<rFreeSpot>}, {"order", bind<rOrder>}, {"has_orders", bind<rHasOrders>},
        {"pos", bind<rPos>}, {"owner", bind<rOwner>},
        {"waves", bind<rWaves>}, {"wave_done", bind<rWaveDone>}, {"lane_wave_sources", bind<rLaneWaveSources>},
        {"wave_clock_ticks", bind<rWaveClock>}, {"next_wave_tick", bind<rNextWaveTick>}, {"wave_objective", bind<rWaveObjective>},
        {"trigger_defs", bind<rTriggerDefs>}, {"trigger_areas", bind<rTriggerAreas>}, {"type_flags", bind<rTypeFlags>},
        {"trig_init", bind<rTrigInit>}, {"trig_enabled", bind<rTrigEnabled>}, {"trig_fired", bind<rTrigFired>}, {"trig_edge", bind<rTrigEdge>},
        {"trig_started", bind<rTrigStarted>}, {"trig_events", bind<rTrigEvents>}, {"trig_players", bind<rResolvePlayers>},
        {"trig_player_matches", bind<rPlayerMatches>}, {"trig_count", bind<rTrigCount>}, {"trig_targets", bind<rTrigTargets>},
        {"trig_owner_targets", bind<rTrigOwnerTargets>}, {"trig_area_update", bind<rTrigAreaUpdate>},
        {"trig_script_condition", bind<rTrigScriptCondition>}, {"trig_script_action", bind<rTrigScriptAction>},
        {"alive_entity", bind<rAliveEntity>}, {"is_unit", bind<rIsUnit>},
        {"var", bind<rVar>}, {"add_var", bind<rAddVar>}, {"player_slots", bind<rPlayerSlots>}, {"stock", bind<rStock>},
        {"add_resource", bind<rAddResource>}, {"researched", bind<rResearched>}, {"research", bind<rResearch>},
        {"set_type_disabled", bind<rTypeDisabled>}, {"hp", bind<rHp>}, {"add_unit_flags", bind<rAddUnitFlags>}, {"stat", bind<rStat>},
        {"deal_damage", bind<rDealDamage>}, {"add_buff", bind<rAddBuff>}, {"set_owner", bind<rSetOwner>},
        {"message_index", bind<rMessageIndex>}, {"add_score", bind<rAddScore>}, {"set_tickets", bind<rSetTickets>},
        {"set_region_owner_by_name", bind<rSetRegionOwnerByName>}, {"regions_owned", bind<rRegionsOwned>},
        {"nearest_enemy_structure", bind<rNearestEnemyStructure>}, {"cast_player_ability", bind<rCastPlayerAbility>},
        {"hostile", bind<rHostile>}, {"ratio", bind<rRatio>},
        {"auto_build_rules", bind<rAutoBuildRules>}, {"type_cost", bind<rTypeCost>}, {"auto_built", bind<rAutoBuilt>},
        {"can_train", bind<rCanTrain>}, {"find_auto_spot", bind<rFindAutoSpot>}, {"pay", bind<rPay>}, {"refund", bind<rRefund>},
        {"place_structure", bind<rPlaceStructure>}, {"sell_rules", bind<rSellRules>}, {"unit_type", bind<rUnitType>},
        {nullptr, nullptr}};
    luaL_newlib(vm.L, funcs);
    lua_pushinteger(vm.L, kPresAny); lua_setfield(vm.L, -2, "PRES_ANY");
    lua_pushinteger(vm.L, kPresBuilt); lua_setfield(vm.L, -2, "PRES_BUILT");
    lua_pushinteger(vm.L, kPresUnit); lua_setfield(vm.L, -2, "PRES_UNIT");
    lua_pushinteger(vm.L, kPresKey); lua_setfield(vm.L, -2, "PRES_KEY");
    lua_pushinteger(vm.L, kPresStructure); lua_setfield(vm.L, -2, "PRES_STRUCTURE");
    lua_pushinteger(vm.L, kPresDeployer); lua_setfield(vm.L, -2, "PRES_DEPLOYER");
    // event and order ids used by the library (RtsModule.h)
    static const std::pair<const char*, int> ids[] = {
        {"EV_WAVE", kEvWave}, {"EV_TRAIN_DONE", kEvTrainDone}, {"EV_PLAYER_DEFEATED", kEvPlayerDefeated}, {"EV_MESSAGE", kEvMessage},
        {"EV_PLAYER_ABILITY", kEvPlayerAbility}, {"EV_DIED", kEvDied}, {"EV_BUILD_DONE", kEvBuildDone}, {"EV_RESEARCHED", kEvResearched},
        {"EV_CAPTURED", kEvCaptured}, {"EV_REGION_CLAIMED", kEvRegionClaimed}, {"EV_ABILITY", kEvAbility}, {"EV_LEVEL_UP", kEvLevelUp},
        {"UNIT_SILENT", kUnitSilent}, {"EV_NOT_ENOUGH", kEvNotEnough}, {"EV_SOLD", kEvSold}, {"STAT_MAX_HP", int(Stat::MaxHp)}, {"HOSTILE_NEUTRAL", kHostileNeutral},
        {"ORDER_MOVE", int(OrderType::Move)}, {"ORDER_ATTACK_MOVE", int(OrderType::AttackMove)}, {"ORDER_ATTACK", int(OrderType::Attack)}};
    for (auto& [k, v] : ids) { lua_pushinteger(vm.L, v); lua_setfield(vm.L, -2, k); }
    lua_setglobal(vm.L, "rts");
}

} // namespace script
} // namespace rts::game
