// The simulation script engine: loading, handlers (on / every / on_area / after), per-tick dispatch,
// calls from data (trigger conditions/actions, ability effects) and the persistent `state` table.
#include "RtsScriptInternal.h"
#include <algorithm>
#include "RtsInternal.h"
#include "RtsStdLua.h"

namespace rts::game {
using namespace script;
using namespace detail;

namespace {
enum Ev : uint8_t {
    EvStart, EvTick, EvUnitDied, EvSpawned, EvBuilt, EvTrained, EvResearched, EvCaptured, EvPlayerDefeated,
    EvAbility, EvDamage, EvLevelUp, EvUpgraded, EvEvery, EvEnterArea, EvLeaveArea, EvCount
};
const char* const kEvNames[EvCount] = {"start", "tick", "unit_died", "spawned", "built", "trained", "researched", "captured",
                                        "player_defeated", "ability", "damage", "level_up", "upgraded", "every", "enter_area", "leave_area"};
constexpr int kNamedEvents = EvUpgraded + 1;   // events registered with on(name, fn) / global on_<name>

int evFromSim(uint16_t t) {
    switch (t) {
        case kEvDied: return EvUnitDied;
        case kEvSpawned: return EvSpawned;
        case kEvBuildDone: return EvBuilt;
        case kEvTrainDone: return EvTrained;
        case kEvResearched: return EvResearched;
        case kEvCaptured: return EvCaptured;
        case kEvPlayerDefeated: return EvPlayerDefeated;
        case kEvAbility: return EvAbility;
        case kEvHit: return EvDamage;
        case kEvLevelUp: return EvLevelUp;
        case kEvUpgraded: return EvUpgraded;
        default: return -1;
    }
}

struct Handler { Ev ev = EvStart; int ref = LUA_NOREF; uint32_t period = 0; int area = -1; std::string where; };
struct PendingEffect { std::string fn; const Json* args = nullptr; EffectCtx ctx; std::vector<Entity> targets; };

void setInt(lua_State* L, const char* k, int64_t v) { lua_pushinteger(L, v); lua_setfield(L, -2, k); }
void setStr(lua_State* L, const char* k, const std::string& v) { lua_pushlstring(L, v.data(), v.size()); lua_setfield(L, -2, k); }
void setFx(lua_State* L, const char* k, Fixed v) { pushFx(L, v); lua_setfield(L, -2, k); }
void setEnt(lua_State* L, const char* k, Entity e) { if (e.valid()) { pushEntity(L, e); lua_setfield(L, -2, k); } }
} // namespace

struct ScriptEngine::Impl {
    ScriptEngine& self;
    RtsModule& m;
    Vm vm;
    std::vector<Handler> handlers;
    uint32_t evMask = 0;
    uint64_t serialCounter = 0, lastSerial = 0;
    uint32_t budgetTick = UINT32_MAX;
    std::vector<PendingEffect> effects;
    int stdRefs[size_t(StdFn::Count)];
    bool stdFailed[size_t(StdFn::Count)] = {};

    Impl(ScriptEngine& s, RtsModule& mod) : self(s), m(mod) { for (int& r : stdRefs) r = LUA_NOREF; }

    void logLine(bool error, const std::string& text) {
        ScriptLogEntry e{error, vm.sim ? vm.sim->tick() : 0, text};
        if (error) ++self.errors_;
        if (self.log_.size() > 500) self.log_.erase(self.log_.begin());
        self.log_.push_back(e);
        if (self.onLog) self.onLog(e);
    }
    ScriptStore& store(Simulation& sim) { return *sim.world().resource<ScriptStore>(); }

    // ---- persistent state
    void restoreState(const std::string& blob) {
        lua_State* L = vm.L;
        lua_getfield(L, LUA_REGISTRYINDEX, "rts.state");       // the table `state` was created with
        lua_pushnil(L);                                        // clear in place: scripts may hold `state` in locals
        while (lua_next(L, -2)) { lua_pop(L, 1); lua_pushvalue(L, -1); lua_pushnil(L); lua_rawset(L, -4); }
        if (blob.empty()) lua_pushnil(L);
        else deserialize(L, blob);
        if (lua_istable(L, -1)) {
            lua_pushnil(L);
            while (lua_next(L, -2)) { lua_pushvalue(L, -2); lua_insert(L, -2); lua_rawset(L, -5); }
        }
        lua_pop(L, 2);
    }
    void commit(Simulation& sim) {
        lua_State* L = vm.L;
        ScriptStore& st = store(sim);
        lua_getfield(L, LUA_REGISTRYINDEX, "rts.state");
        std::string blob, where;
        bool ok = serialize(L, -1, blob, where);
        lua_pop(L, 1);
        if (!ok) {
            logLine(true, "state" + (!where.empty() && where[0] == '[' ? where : ": " + where) + " can't be kept in state (integers, fx, strings, booleans, "
                          "tables with integer/string keys only); state reverted to the last tick");
            restoreState(st.state);
            return;
        }
        st.state.swap(blob);
        st.serial = ++serialCounter;
        lastSerial = st.serial;
    }
    // Before any script code runs in a tick: rollback check and the per-tick budget.
    void enter(Simulation& sim) {
        vm.sim = &sim;
        ScriptStore& st = store(sim);
        if (st.serial != lastSerial) { restoreState(st.state); lastSerial = st.serial; }
        if (budgetTick != sim.tick()) { budgetTick = sim.tick(); vm.used = 0; vm.exceeded = false; }
    }

    // ---- calls
    bool callHandler(Simulation& sim, size_t hi, int nargs) {
        // stack: args; function is pushed under them
        if (vm.exceeded) { lua_pop(vm.L, nargs); return false; }   // budget spent this tick: skip, don't blame
        lua_rawgeti(vm.L, LUA_REGISTRYINDEX, handlers[hi].ref);
        lua_insert(vm.L, -nargs - 1);
        std::string err;
        if (protectedCall(vm, nargs, 0, &err)) return true;
        store(sim).disabled[hi] = 1;
        logLine(true, err + " [" + kEvNames[handlers[hi].ev] + " handler " + handlers[hi].where + " disabled]");
        return false;
    }
    bool fnDisabled(Simulation& sim, const std::string& fn) {
        auto& d = store(sim).disabledFns;
        return std::binary_search(d.begin(), d.end(), fn);
    }
    // Calls global function `fn` with the table on top of the stack; leaves `nres` results on success.
    bool callGlobal(Simulation& sim, const std::string& fn, int nres) {
        lua_State* L = vm.L;
        if (fnDisabled(sim, fn) || vm.exceeded) { lua_pop(L, 1); return false; }
        lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);
        lua_pushlstring(L, fn.data(), fn.size());
        lua_rawget(L, -2);                                // no strict-globals metamethod
        lua_remove(L, -2);
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 2);
            auto& d = store(sim).disabledFns;
            d.insert(std::upper_bound(d.begin(), d.end(), fn), fn);
            logLine(true, "script function '" + fn + "' is not defined");
            return false;
        }
        lua_insert(L, -2);
        std::string err;
        if (protectedCall(vm, 1, nres, &err)) return true;
        auto& d = store(sim).disabledFns;
        d.insert(std::upper_bound(d.begin(), d.end(), fn), fn);
        logLine(true, err + " [function " + fn + " disabled]");
        return false;
    }

    void pushEvent(Simulation& sim, const SimEvent& ev, int kind) {
        lua_State* L = vm.L;
        const Rules& r = m.rules();
        lua_createtable(L, 0, 8);
        setStr(L, "event", kEvNames[kind]);
        setInt(L, "player", ev.player);
        setInt(L, "tick", sim.tick());
        setFx(L, "x", ev.pos.x);
        setFx(L, "z", ev.pos.z);
        auto typeName = [&](int64_t t) { return t >= 0 && size_t(t) < r.types.size() ? r.types[size_t(t)].id : std::string(); };
        switch (kind) {
        case EvUnitDied: case EvSpawned: case EvBuilt: case EvTrained: case EvUpgraded:
            setEnt(L, "unit", ev.a); setInt(L, "owner", ev.player); setStr(L, "unit_type", typeName(ev.value));
            if (kind == EvTrained) setEnt(L, "producer", ev.b);
            break;
        case EvResearched:
            setEnt(L, "producer", ev.a);
            if (ev.value >= 0 && size_t(ev.value) < r.techs.size()) setStr(L, "tech", r.techs[size_t(ev.value)].id);
            break;
        case EvCaptured: setEnt(L, "unit", ev.a); setEnt(L, "by", ev.b); setInt(L, "owner", ev.player); setInt(L, "old_owner", ev.value); break;
        case EvPlayerDefeated: lua_pushboolean(L, ev.value == kDefeatSurrendered); lua_setfield(L, -2, "surrendered"); break;
        case EvAbility:
            setEnt(L, "caster", ev.a); setEnt(L, "target", ev.b); setInt(L, "index", ev.value);
            if (sim.world().alive(ev.a) && sim.world().has<Unit>(ev.a)) setStr(L, "ability", m.abilityFor(sim, ev.a, int(ev.value)).id);
            break;
        case EvDamage: setEnt(L, "unit", ev.a); setEnt(L, "source", ev.b); setFx(L, "amount", Fixed::fromRaw(ev.value)); break;
        case EvLevelUp: setEnt(L, "unit", ev.a); setInt(L, "level", ev.value); break;
        default: break;
        }
    }

    void tickAreas(Simulation& sim) {
        World& w = sim.world();
        ScriptStore& st = store(sim);
        const TriggerSet& ts = m.triggers();
        for (size_t hi = 0; hi < handlers.size(); ++hi) {
            const Handler& h = handlers[hi];
            if ((h.ev != EvEnterArea && h.ev != EvLeaveArea) || st.disabled[hi]) continue;
            const TrigArea& ar = ts.areas[size_t(h.area)];
            std::vector<uint64_t> now;
            w.each<Unit, Transform>([&](Entity e, Unit& u, Transform& t) { if (u.hp.raw > 0 && ar.contains(t.pos)) now.push_back(e.packed()); });
            std::sort(now.begin(), now.end());
            std::vector<uint64_t> fire;
            const std::vector<uint64_t>& prev = st.inside[hi];
            const auto& a = h.ev == EvEnterArea ? now : prev;
            const auto& b = h.ev == EvEnterArea ? prev : now;
            std::set_difference(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(fire));
            st.inside[hi].swap(now);
            for (uint64_t pk : fire) {
                Entity e = Entity::fromPacked(pk);
                if (!w.alive(e) || !w.has<Unit>(e) || store(sim).disabled[hi]) continue;
                lua_createtable(vm.L, 0, 6);
                setStr(vm.L, "event", kEvNames[h.ev]);
                setEnt(vm.L, "unit", e);
                setInt(vm.L, "owner", w.get<Owner>(e).player);
                setStr(vm.L, "area", ts.areaNames[size_t(h.area)]);
                setFx(vm.L, "x", w.get<Transform>(e).pos.x);
                setFx(vm.L, "z", w.get<Transform>(e).pos.z);
                callHandler(sim, hi, 1);
            }
        }
    }

    void pushCtx(Simulation& sim, uint8_t player, Entity unit, Vec3 at, const Json* args) {
        lua_State* L = vm.L;
        lua_createtable(L, 0, 6);
        setInt(L, "player", player);
        if (sim.world().alive(unit)) setEnt(L, "unit", unit);
        setFx(L, "x", at.x);
        setFx(L, "z", at.z);
        setInt(L, "tick", sim.tick());
        if (args) { pushJson(L, *args); lua_setfield(L, -2, "args"); }
    }
};

// ================================================================= registration bindings

namespace {
ScriptEngine::Impl& implOf(Vm& vm) { return *static_cast<ScriptEngine::Impl*>(vm.owner); }

std::string whereOf(lua_State* L, int idx) {
    lua_Debug ar;
    lua_pushvalue(L, idx);
    if (!lua_getinfo(L, ">S", &ar)) return "?";
    return std::string(ar.short_src) + ":" + std::to_string(ar.linedefined);
}

int addHandler(lua_State* L, Vm& vm, Ev ev, int fnIdx, uint32_t period, int area) {
    if (!vm.loading) return fail(vm, "handlers are registered while the script files load (at the top level of a file)");
    if (!lua_isfunction(L, fnIdx)) return fail(vm, "function expected");
    auto& im = implOf(vm);
    Handler h;
    h.ev = ev; h.period = period; h.area = area;
    h.where = whereOf(L, fnIdx);
    lua_pushvalue(L, fnIdx);
    h.ref = luaL_ref(L, LUA_REGISTRYINDEX);
    im.handlers.push_back(std::move(h));
    im.evMask |= 1u << ev;
    return 0;
}

// on("unit_died", function(e) ... end)
int lOn(lua_State* L, Vm& vm) {
    const char* name = lua_tostring(L, 1);
    if (!name) return fail(vm, "on: event name expected");
    for (int k = 0; k < kNamedEvents; ++k)
        if (std::string(name) == kEvNames[k]) return addHandler(L, vm, Ev(k), 2, 0, -1);
    std::string known;
    for (int k = 0; k < kNamedEvents; ++k) known += std::string(k ? ", " : "") + kEvNames[k];
    return fail(vm, std::string("on: unknown event '") + name + "' (" + known + ")");
}
// every(seconds, function(tick) ... end)
int lEvery(lua_State* L, Vm& vm) {
    Fixed s;
    if (!argFx(L, vm, 1, s, "every: seconds")) return -1;
    if (s.raw <= 0) return fail(vm, "every: seconds must be > 0");
    uint32_t period = uint32_t(std::max<int64_t>(1, (s * int64_t(vm.sim->tickRate())).toInt()));
    return addHandler(L, vm, EvEvery, 2, period, -1);
}
// on_area("gate", "enter" | "leave", function(e) ... end)
int lOnArea(lua_State* L, Vm& vm) {
    const char* area = lua_tostring(L, 1);
    const char* kind = lua_tostring(L, 2);
    if (!area || !kind) return fail(vm, "on_area(area, \"enter\"|\"leave\", fn)");
    int a = implOf(vm).m.triggers().findArea(area);
    if (a < 0) return fail(vm, std::string("on_area: unknown area '") + area + "' (map \"areas\")");
    std::string k = kind;
    if (k != "enter" && k != "leave") return fail(vm, "on_area: \"enter\" or \"leave\"");
    return addHandler(L, vm, k == "enter" ? EvEnterArea : EvLeaveArea, 3, 0, a);
}
// game.after(seconds, "function_name", arg): a timer kept in the simulation state
int lAfter(lua_State* L, Vm& vm) {
    Fixed s;
    if (!argFx(L, vm, 1, s, "after: seconds")) return -1;
    const char* fn = lua_tostring(L, 2);
    if (!fn) return fail(vm, "after: global function name expected (a string: timers must survive save/rollback)");
    std::string arg, where;
    if (!lua_isnoneornil(L, 3) && !serialize(L, 3, arg, where)) return fail(vm, "after: argument can't be stored: " + where);
    ScriptStore& st = implOf(vm).store(*vm.sim);
    ScriptStore::Timer t;
    t.tick = vm.sim->tick() + uint32_t(std::max<int64_t>(1, (s * int64_t(vm.sim->tickRate())).toInt()));
    t.seq = st.timerSeq++;
    t.fn = fn;
    t.arg = std::move(arg);
    auto it = std::upper_bound(st.timers.begin(), st.timers.end(), t, [](const ScriptStore::Timer& a, const ScriptStore::Timer& b) {
        return a.tick != b.tick ? a.tick < b.tick : a.seq < b.seq;
    });
    st.timers.insert(it, std::move(t));
    return 0;
}
int lStrictIndex(lua_State* L, Vm& vm) {
    const char* k = lua_tostring(L, 2);
    return fail(vm, std::string("undefined global '") + (k ? k : "?") + "'");
}
int lStrictNewIndex(lua_State* L, Vm& vm) {
    const char* k = lua_tostring(L, 2);
    return fail(vm, std::string("can't create global '") + (k ? k : "?") + "' while the game runs: keep data in state." +
                    (k ? std::string(k) : std::string("x")) + " (globals are read-only after loading)");
}
} // namespace

// ================================================================= engine

ScriptEngine::ScriptEngine(RtsModule& module) : impl_(std::make_unique<Impl>(*this, module)) {}
ScriptEngine::~ScriptEngine() = default;

void ScriptEngine::load(Simulation& sim, const Json& rules, const Json& map) {
    Impl& im = *impl_;
    Vm& vm = im.vm;
    vm.log = [&im](bool e, const std::string& t) { im.logLine(e, t); };
    vm.owner = &im;
    vm.module = &im.m;
    vm.sim = &sim;
    ScriptStore& st = sim.world().setResource<ScriptStore>();
    if (!vm.open(Mode::Sim, size_t(256) << 20)) { im.logLine(true, "cannot create the Lua state"); return; }
    lua_State* L = vm.L;
    for (auto& [path, text] : rules["$scriptSources"].members()) vm.sources[path] = text.str();
    vm.budget = std::max<int64_t>(10000, rules.getInt("scriptBudget", 1000000));

    openGameApi(vm);
    openStdApi(vm);
    lua_newtable(L);
    lua_setglobal(L, "std");
    lua_getglobal(L, "game");
    lua_pushcfunction(L, bind<lAfter>); lua_setfield(L, -2, "after");
    lua_pop(L, 1);
    lua_pushcfunction(L, bind<lOn>); lua_setglobal(L, "on");
    lua_pushcfunction(L, bind<lEvery>); lua_setglobal(L, "every");
    lua_pushcfunction(L, bind<lOnArea>); lua_setglobal(L, "on_area");
    lua_newtable(L);
    lua_pushvalue(L, -1); lua_setfield(L, LUA_REGISTRYINDEX, "rts.state");
    lua_setglobal(L, "state");

    // the standard library first (engine code: no budget); scripts/std/<name>.lua of the game replaces a module
    vm.loading = true;
    lua_sethook(L, nullptr, 0, 0);
    for (const stdlua::File* f = stdlua::kFiles; f->name; ++f) {
        const std::string over = std::string("scripts/std/") + f->name + ".lua";
        auto it = vm.sources.find(over);
        const bool own = it != vm.sources.end();
        const std::string name = own ? over : std::string("std/") + f->name + ".lua";
        const std::string text = own ? it->second : std::string(reinterpret_cast<const char*>(f->data), f->size);
        std::string err;
        if (!loadChunk(vm, name, text, err) || lua_pcall(L, 0, 0, 0) != LUA_OK) {
            if (err.empty()) { err = lua_tostring(L, -1) ? lua_tostring(L, -1) : "error"; lua_pop(L, 1); }
            im.logLine(true, err);
        }
    }
    static const char* const kStdNames[] = {"victory"};
    static_assert(sizeof(kStdNames) / sizeof(kStdNames[0]) == size_t(StdFn::Count));
    lua_getglobal(L, "std");
    for (size_t k = 0; k < size_t(StdFn::Count); ++k) {
        if (lua_getfield(L, -1, kStdNames[k]) == LUA_TFUNCTION) im.stdRefs[k] = luaL_ref(L, LUA_REGISTRYINDEX);
        else { lua_pop(L, 1); im.logLine(true, std::string("standard library: std.") + kStdNames[k] + " is missing"); }
    }
    lua_pop(L, 1);
    // the game's files (a larger budget: setup work)
    const int64_t tickBudget = vm.budget;
    vm.budget = tickBudget * 20;
    vm.used = 0;
    for (const std::string& f : scriptFiles(rules, map)) {
        if (f.rfind("scripts/std/", 0) == 0) continue;   // standard library overrides: loaded above
        userScripts_ = true;
        auto it = vm.sources.find(f);
        if (it == vm.sources.end()) { im.logLine(true, f + ": script source not loaded (the loader must call embedScripts)"); continue; }
        std::string err;
        if (!loadChunk(vm, f, it->second, err)) { im.logLine(true, err); continue; }
        if (!protectedCall(vm, 0, 0, &err)) im.logLine(true, err);
    }
    // global on_<event> functions are handlers too (unless also registered with on())
    for (int k = 0; k < kNamedEvents; ++k) {
        const std::string g = std::string("on_") + kEvNames[k];
        lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);
        lua_getfield(L, -1, g.c_str());
        bool dup = false;
        if (lua_isfunction(L, -1))
            for (auto& h : im.handlers) {
                lua_rawgeti(L, LUA_REGISTRYINDEX, h.ref);
                dup |= lua_rawequal(L, -1, -2) != 0;
                lua_pop(L, 1);
            }
        if (lua_isfunction(L, -1) && !dup) addHandler(L, vm, Ev(k), lua_gettop(L), 0, -1);
        lua_pop(L, 2);
    }
    vm.loading = false;
    vm.budget = tickBudget;
    // strict, read-only globals from now on
    lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);
    lua_createtable(L, 0, 2);
    lua_pushcfunction(L, bind<lStrictIndex>); lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, bind<lStrictNewIndex>); lua_setfield(L, -2, "__newindex");
    lua_pushboolean(L, 1); lua_setfield(L, -2, "__metatable");
    lua_setmetatable(L, -2);
    lua_pop(L, 1);

    // references from data
    const Rules& r = im.m.rules();
    for (auto& list : r.payloads)
        for (auto& e : list)
            if (e.kind == EffectKind::Script && !hasFunction(e.scriptFn)) im.logLine(true, "effect 'script': function '" + e.scriptFn + "' is not defined");
    for (auto& d : im.m.triggers().defs) {
        for (auto& c : d.conditions)
            if (c.kind == TrigCondition::Script && !hasFunction(c.name)) im.logLine(true, "trigger '" + d.id + "': script function '" + c.name + "' is not defined");
        for (auto& a : d.actions)
            if (a.kind == TrigAction::Script && !hasFunction(a.name)) im.logLine(true, "trigger '" + d.id + "': script function '" + a.name + "' is not defined");
    }
    st.userScripts = userScripts_;
    st.disabled.assign(im.handlers.size(), 0);
    st.inside.assign(im.handlers.size(), {});
    im.commit(sim);
}

bool ScriptEngine::hasFunction(const std::string& name) const {
    lua_State* L = impl_->vm.L;
    if (!L) return false;
    lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);
    lua_pushlstring(L, name.data(), name.size());
    lua_rawget(L, -2);
    bool ok = lua_isfunction(L, -1);
    lua_pop(L, 2);
    return ok;
}

void ScriptEngine::tick(Simulation& sim) {
    Impl& im = *impl_;
    if (!im.vm.L) return;
    lua_State* L = im.vm.L;
    im.enter(sim);
    ScriptStore& st = im.store(sim);
    const uint32_t tick = sim.tick();
    auto active = [&](size_t hi, Ev ev) { return im.handlers[hi].ev == ev && !im.store(sim).disabled[hi]; };

    if (!st.started) {
        st.started = true;
        for (size_t hi = 0; hi < im.handlers.size(); ++hi) if (active(hi, EvStart)) im.callHandler(sim, hi, 0);
    }
    // timers (handlers may add more: re-read the store each time)
    while (!im.store(sim).timers.empty() && im.store(sim).timers.front().tick <= tick) {
        ScriptStore::Timer t = std::move(im.store(sim).timers.front());
        im.store(sim).timers.erase(im.store(sim).timers.begin());
        if (t.arg.empty()) lua_pushnil(L); else deserialize(L, t.arg);
        im.callGlobal(sim, t.fn, 0);
    }
    if (im.evMask & (1u << EvEvery))
        for (size_t hi = 0; hi < im.handlers.size(); ++hi)
            if (active(hi, EvEvery) && tick > 0 && tick % im.handlers[hi].period == 0) { lua_pushinteger(L, tick); im.callHandler(sim, hi, 1); }
    if (im.evMask & (1u << EvTick))
        for (size_t hi = 0; hi < im.handlers.size(); ++hi)
            if (active(hi, EvTick)) { lua_pushinteger(L, tick); im.callHandler(sim, hi, 1); }
    // simulation events of this tick
    const uint32_t eventBits = im.evMask & ~((1u << EvStart) | (1u << EvTick) | (1u << EvEvery) | (1u << EvEnterArea) | (1u << EvLeaveArea));
    if (eventBits) {
        const std::vector<SimEvent> evs = sim.events();
        for (const SimEvent& ev : evs) {
            int k = evFromSim(ev.type);
            if (k < 0 || !(eventBits & (1u << k))) continue;
            for (size_t hi = 0; hi < im.handlers.size(); ++hi) {
                if (!active(hi, Ev(k))) continue;
                im.pushEvent(sim, ev, k);
                im.callHandler(sim, hi, 1);
            }
        }
    }
    if ((im.evMask & ((1u << EvEnterArea) | (1u << EvLeaveArea))) && tick % 5 == 0) im.tickAreas(sim);
    // ability effects queued during this tick
    std::vector<PendingEffect> fx;
    fx.swap(im.effects);
    for (auto& p : fx) {
        World& w = sim.world();
        lua_createtable(L, 0, 10);
        setEnt(L, "caster", w.alive(p.ctx.caster) ? p.ctx.caster : kNullEntity);
        setInt(L, "owner", p.ctx.owner);
        setEnt(L, "target", w.alive(p.ctx.target) ? p.ctx.target : kNullEntity);
        setFx(L, "x", p.ctx.point.x);
        setFx(L, "z", p.ctx.point.z);
        setFx(L, "scale", p.ctx.scale);
        if (p.ctx.sourceType < im.m.rules().types.size()) setStr(L, "source_type", im.m.rules().types[p.ctx.sourceType].id);
        lua_createtable(L, int(p.targets.size()), 0);
        int n = 0;
        for (Entity e : p.targets) if (w.alive(e)) { pushEntity(L, e); lua_rawseti(L, -2, ++n); }
        lua_setfield(L, -2, "targets");
        if (p.args) { pushJson(L, *p.args); lua_setfield(L, -2, "args"); }
        im.callGlobal(sim, p.fn, 0);
    }
    im.commit(sim);
}

void ScriptEngine::callStd(Simulation& sim, StdFn fn) {
    Impl& im = *impl_;
    const int ref = im.stdRefs[size_t(fn)];
    if (ref == LUA_NOREF) return;
    lua_State* L = im.vm.L;
    im.enter(sim);
    lua_sethook(L, nullptr, 0, 0);   // engine code: no instruction budget (user calls set it again)
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    lua_pushinteger(L, sim.tick());
    if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
        if (!im.stdFailed[size_t(fn)]) {
            im.stdFailed[size_t(fn)] = true;
            im.logLine(true, std::string("standard library: ") + (lua_tostring(L, -1) ? lua_tostring(L, -1) : "error"));
        }
        lua_pop(L, 1);
    }
}

bool ScriptEngine::callCondition(Simulation& sim, const std::string& fn, const Json& args, uint8_t player, Entity unit, Vec3 at) {
    Impl& im = *impl_;
    if (!im.vm.L) return false;
    im.enter(sim);
    im.pushCtx(sim, player, unit, at, &args);
    if (!im.callGlobal(sim, fn, 1)) return false;
    bool r = lua_toboolean(im.vm.L, -1);
    lua_pop(im.vm.L, 1);
    return r;
}

void ScriptEngine::callAction(Simulation& sim, const std::string& fn, const Json& args, uint8_t player, Entity unit, Vec3 at, const std::string& trigger) {
    Impl& im = *impl_;
    if (!im.vm.L) return;
    im.enter(sim);
    im.pushCtx(sim, player, unit, at, &args);
    setStr(im.vm.L, "trigger", trigger);
    im.callGlobal(sim, fn, 0);
}

void ScriptEngine::queueEffect(const EffectDef& e, const EffectCtx& ctx, std::vector<Entity> targets) {
    impl_->effects.push_back({e.scriptFn, &e.scriptArgs, ctx, std::move(targets)});
}

void ScriptStore::hash(Hasher& h) const {
    if (!userScripts) return;   // only the standard library: its state lives in GameState (hashed there)
    h.str(state);
    h.u32(started);
    for (auto d : disabled) h.u32(d);
    for (auto& f : disabledFns) h.str(f);
    for (auto& in : inside) { h.u32(uint32_t(in.size())); for (auto e : in) h.u64(e); }
    h.u32(uint32_t(timers.size()));
    for (auto& t : timers) { h.u32(t.tick); h.u32(t.seq); h.str(t.fn); h.str(t.arg); }
    h.u32(timerSeq);
}

// The module owns the engine (unique_ptr of an incomplete type in RtsModule.h).
RtsModule::RtsModule() = default;
RtsModule::~RtsModule() = default;

} // namespace rts::game
