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
// rts.spawn_on_cell(typeIndex, owner, x, z, radius): nearest passable cell for the type's path layer within
// radius of (x, z), spawned at its centre; nil when there is none
int rSpawnOnCell(lua_State* L, Vm& vm) {
    CTX;
    const size_t t = size_t(intArg(L, 1));
    if (t >= R.types.size()) { lua_pushnil(L); return 1; }
    int cx = gs.grid.cellX(fxArg(L, 3)), cy = gs.grid.cellY(fxArg(L, 4));
    if (!gs.grid.nearestPassable(cx, cy, R.types[t].pathLayer, int(intArg(L, 5)))) { lua_pushnil(L); return 1; }
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
        {"EV_PLAYER_ABILITY", kEvPlayerAbility},
        {"ORDER_MOVE", int(OrderType::Move)}, {"ORDER_ATTACK_MOVE", int(OrderType::AttackMove)}, {"ORDER_ATTACK", int(OrderType::Attack)}};
    for (auto& [k, v] : ids) { lua_pushinteger(vm.L, v); lua_setfield(vm.L, -2, k); }
    lua_setglobal(vm.L, "rts");
}

} // namespace script
} // namespace rts::game
