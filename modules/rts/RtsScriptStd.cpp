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

} // namespace

void openStdApi(Vm& vm) {
    static const luaL_Reg funcs[] = {
        {"players", bind<rPlayers>}, {"alive", bind<rAlive>}, {"defeat", bind<rDefeat>}, {"team_of", bind<rTeamOf>},
        {"is_wave_player", bind<rIsWavePlayer>}, {"wave_players", bind<rWavePlayers>},
        {"presence", bind<rPresence>}, {"victory_rules", bind<rVictoryRules>}, {"tickets", bind<rTickets>}, {"score", bind<rScore>},
        {"territory", bind<rTerritory>}, {"territory_total", bind<rTerritoryTotal>}, {"winner", bind<rWinner>}, {"set_winner", bind<rSetWinner>},
        {nullptr, nullptr}};
    luaL_newlib(vm.L, funcs);
    lua_pushinteger(vm.L, kPresAny); lua_setfield(vm.L, -2, "PRES_ANY");
    lua_pushinteger(vm.L, kPresBuilt); lua_setfield(vm.L, -2, "PRES_BUILT");
    lua_pushinteger(vm.L, kPresUnit); lua_setfield(vm.L, -2, "PRES_UNIT");
    lua_pushinteger(vm.L, kPresKey); lua_setfield(vm.L, -2, "PRES_KEY");
    lua_pushinteger(vm.L, kPresStructure); lua_setfield(vm.L, -2, "PRES_STRUCTURE");
    lua_pushinteger(vm.L, kPresDeployer); lua_setfield(vm.L, -2, "PRES_DEPLOYER");
    lua_setglobal(vm.L, "rts");
}

} // namespace script
} // namespace rts::game
