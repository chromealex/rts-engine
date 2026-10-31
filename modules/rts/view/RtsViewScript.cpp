// View scripts: presentation-only Lua VM (see RtsViewScript.h).
#include "RtsViewScript.h"
#include <map>
#include "../RtsModule.h"
#include "../RtsScriptInternal.h"

namespace rts::game {
using namespace script;

namespace {
const char* eventName(uint16_t t) { return simEventName(t); }   // every GameEvent (RtsScript.cpp table)
struct Collector { std::vector<SimEvent> events; bool enabled = true; };
} // namespace

struct ViewScripts::Impl {
    ViewScripts& self;
    Vm vm;
    std::map<std::string, std::vector<int>> handlers;   // event -> function refs (LUA_NOREF: disabled)
    std::vector<int> frame;
    std::vector<ViewRequest> requests;
    std::vector<Command> commands;
    std::vector<UiRequest> ui;
    std::map<std::string, std::string> uiLast;          // key -> last value sent (unchanged values are not resent)
    std::map<std::string, std::vector<int>> uiHandlers;
    uint32_t stateTick = UINT32_MAX;                   // view.state(): copy of the simulation's `state`, once per tick
    uint64_t stateEpoch = 0;
    int stateRef = LUA_NOREF;
    std::shared_ptr<Collector> collector;
    uint8_t player = 0;
    explicit Impl(ViewScripts& s) : self(s) {}

    void logLine(bool error, const std::string& t) {
        if (self.log_.size() > 300) self.log_.erase(self.log_.begin());
        self.log_.push_back({error, vm.sim ? vm.sim->tick() : 0, t});
    }
    void call(int& ref, int nargs) {
        lua_rawgeti(vm.L, LUA_REGISTRYINDEX, ref);
        lua_insert(vm.L, -nargs - 1);
        std::string err;
        if (protectedCall(vm, nargs, 0, &err)) return;
        logLine(true, err + " [view handler disabled]");
        luaL_unref(vm.L, LUA_REGISTRYINDEX, ref);
        ref = LUA_NOREF;
    }
};

namespace {
ViewScripts::Impl& viewOf(Vm& vm) { return *static_cast<ViewScripts::Impl*>(vm.owner); }

int vOn(lua_State* L, Vm& vm) {
    const char* n = lua_tostring(L, 1);
    if (!n || !lua_isfunction(L, 2)) return fail(vm, "view.on(event, fn)");
    lua_pushvalue(L, 2);
    viewOf(vm).handlers[n].push_back(luaL_ref(L, LUA_REGISTRYINDEX));
    return 0;
}
int vOnFrame(lua_State* L, Vm& vm) {
    if (!lua_isfunction(L, 1)) return fail(vm, "view.on_frame(fn)");
    lua_pushvalue(L, 1);
    viewOf(vm).frame.push_back(luaL_ref(L, LUA_REGISTRYINDEX));
    return 0;
}
int vRequest(lua_State* L, Vm& vm, ViewRequest::Kind k) {
    size_t len = 0;
    const char* s = luaL_tolstring(L, 1, &len);
    ViewRequest r;
    r.kind = k;
    r.text.assign(s, len);
    lua_pop(L, 1);
    if (!lua_isnoneornil(L, 2)) { r.x = float(toDouble(L, 2)); r.z = float(toDouble(L, 3)); r.positioned = true; }
    auto& q = viewOf(vm).requests;
    if (q.size() < 256) q.push_back(std::move(r));
    return 0;
}
int vNotice(lua_State* L, Vm& vm) { return vRequest(L, vm, ViewRequest::Notice); }
int vEffect(lua_State* L, Vm& vm) { return vRequest(L, vm, ViewRequest::Effect); }
int vSound(lua_State* L, Vm& vm) { return vRequest(L, vm, ViewRequest::Sound); }
int vPlayer(lua_State* L, Vm& vm) { lua_pushinteger(L, viewOf(vm).player); return 1; }

// ---- Lua <-> Json for the UI (view values: numbers, fx, strings, booleans, tables)
Json toUiJson(lua_State* L, int idx, int depth = 0) {
    idx = lua_absindex(L, idx);
    if (Fixed* f = testFx(L, idx)) return Json::number(f->toDouble(), 4);
    switch (lua_type(L, idx)) {
    case LUA_TBOOLEAN: return Json::fromBool(lua_toboolean(L, idx));
    case LUA_TSTRING: { size_t n; const char* c = lua_tolstring(L, idx, &n); return Json::string(std::string(c, n)); }
    case LUA_TNUMBER: return lua_isinteger(L, idx) ? Json::number(int64_t(lua_tointeger(L, idx))) : Json::number(double(lua_tonumber(L, idx)), 4);
    case LUA_TTABLE: {
        if (depth > 16) return Json();
        const lua_Integer n = lua_Integer(lua_rawlen(L, idx));
        if (n > 0) {   // array part
            Json a = Json::array();
            for (lua_Integer i = 1; i <= n; ++i) { lua_rawgeti(L, idx, i); a.push(toUiJson(L, -1, depth + 1)); lua_pop(L, 1); }
            return a;
        }
        Json o = Json::object();   // string keys (lua_next order: the host sees a map, order doesn't matter)
        lua_pushnil(L);
        while (lua_next(L, idx)) {
            if (lua_type(L, -2) == LUA_TSTRING) o.set(lua_tostring(L, -2), toUiJson(L, -1, depth + 1));
            lua_pop(L, 1);
        }
        return o;
    }
    default: return Json();
    }
}
void pushUiJson(lua_State* L, const Json& j, int depth = 0) {
    luaL_checkstack(L, 4, "json too deep");
    switch (j.type()) {
    case Json::Type::Bool: lua_pushboolean(L, j.boolean()); break;
    case Json::Type::String: lua_pushstring(L, j.str().c_str()); break;
    case Json::Type::Number: {
        const Fixed f = j.fx();
        if (f.raw % Fixed::fromInt(1).raw == 0) lua_pushinteger(L, lua_Integer(f.toInt())); else lua_pushnumber(L, f.toDouble());
        break;
    }
    case Json::Type::Array: {
        lua_createtable(L, int(j.items().size()), 0);
        lua_Integer k = 1;
        for (auto& v : j.items()) { pushUiJson(L, v, depth + 1); lua_rawseti(L, -2, k++); }
        break;
    }
    case Json::Type::Object:
        lua_createtable(L, 0, int(j.members().size()));
        for (auto& [k, v] : j.members()) { pushUiJson(L, v, depth + 1); lua_setfield(L, -2, k.c_str()); }
        break;
    default: lua_pushnil(L); break;
    }
}
// ui.set(key, value): a value of data model "script" ({{key}} / {{key.field}} in ui/hud documents)
int uSet(lua_State* L, Vm& vm) {
    const char* k = lua_tostring(L, 1);
    if (!k || !*k) return fail(vm, "ui.set(key, value)");
    auto& im = viewOf(vm);
    Json v = toUiJson(L, 2);
    std::string text = v.dump(0);
    auto it = im.uiLast.find(k);
    if (it != im.uiLast.end() && it->second == text) return 0;
    im.uiLast[k] = std::move(text);
    if (im.ui.size() < 4096) im.ui.push_back({UiRequest::Set, k, std::move(v)});
    return 0;
}
int uOpen(lua_State* L, Vm& vm) {
    const char* n = lua_tostring(L, 1);
    if (!n || !*n) return fail(vm, "ui.open(document [, params])");
    viewOf(vm).ui.push_back({UiRequest::Open, n, lua_isnoneornil(L, 2) ? Json::object() : toUiJson(L, 2)});
    return 0;
}
int uClose(lua_State* L, Vm& vm) {
    const char* n = lua_tostring(L, 1);
    if (!n || !*n) return fail(vm, "ui.close(document)");
    viewOf(vm).ui.push_back({UiRequest::Close, n, Json()});
    return 0;
}
int uOn(lua_State* L, Vm& vm) {
    const char* n = lua_tostring(L, 1);
    if (!n || !lua_isfunction(L, 2)) return fail(vm, "ui.on(event, fn)");
    lua_pushvalue(L, 2);
    viewOf(vm).uiHandlers[n].push_back(luaL_ref(L, LUA_REGISTRYINDEX));
    return 0;
}
// view.state(): read-only copy of the simulation scripts' `state` (refreshed once per tick)
int vState(lua_State* L, Vm& vm) {
    auto& im = viewOf(vm);
    Simulation& sim = *vm.sim;
    auto* st = sim.world().resource<ScriptStore>();
    if (!st || !st->userScripts) { lua_newtable(L); return 1; }
    if (im.stateRef == LUA_NOREF || im.stateTick != sim.tick() || im.stateEpoch != st->serial) {
        if (ScriptEngine* se = vm.module->scripts()) se->flush(sim);   // what a checksum would write now (no effect on outcomes)
        if (st->state.empty()) lua_newtable(L); else deserialize(L, st->state);
        if (!lua_istable(L, -1)) { lua_pop(L, 1); lua_newtable(L); }
        if (im.stateRef != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, im.stateRef);
        im.stateRef = luaL_ref(L, LUA_REGISTRYINDEX);
        im.stateTick = sim.tick();
        im.stateEpoch = st->serial;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, im.stateRef);
    return 1;
}
// view.send(id, args): a rules "commands" command from the local player (args as in on_command; the
// selection goes in args.units). The simulation validates it again when it arrives.
int vSend(lua_State* L, Vm& vm) {
    const char* id = lua_tostring(L, 1);
    ScriptEngine* se = vm.module ? vm.module->scripts() : nullptr;
    const int k = id && se ? se->findCommand(id) : -1;
    if (k < 0) return fail(vm, std::string("view.send: unknown command '") + (id ? id : "?") + "'");
    Command c;
    c.type = kCmdScript;
    c.param = k;
    if (!encodeScriptArgs(L, vm, 2, se->commands()[size_t(k)], c)) return -1;
    auto& q = viewOf(vm).commands;
    if (q.size() < 64) q.push_back(std::move(c));
    lua_pushboolean(L, 1);
    return 1;
}
} // namespace

ViewScripts::ViewScripts() : impl_(std::make_unique<Impl>(*this)) {}
ViewScripts::~ViewScripts() { detach(); }
bool ViewScripts::active() const { return impl_->vm.L != nullptr; }

bool ViewScripts::load(Simulation& sim, const Json& rules, const Json& map, uint8_t localPlayer) {
    Impl& im = *impl_;
    detach();
    im.vm.close();
    im.handlers.clear(); im.frame.clear(); im.requests.clear(); im.commands.clear();
    im.ui.clear(); im.uiLast.clear(); im.uiHandlers.clear(); im.stateRef = LUA_NOREF; im.stateTick = UINT32_MAX;
    log_.clear();
    const auto files = scriptFiles(rules, map, true);
    if (files.empty() || sim.module().name() != "rts") return false;
    Vm& vm = im.vm;
    vm.owner = &im;
    vm.module = static_cast<RtsModule*>(&sim.module());
    vm.sim = &sim; vm.spatialOk = false;
    vm.log = [&im](bool e, const std::string& t) { im.logLine(e, t); };
    if (!vm.open(Mode::View, size_t(64) << 20)) return false;
    vm.budget = 5000000;
    im.player = localPlayer;
    for (auto& [p, t] : rules["$scriptSources"].members()) vm.sources[p] = t.str();
    openGameApi(vm);
    static const luaL_Reg funcs[] = {{"on", bind<vOn>}, {"on_frame", bind<vOnFrame>}, {"notice", bind<vNotice>},
                                     {"effect", bind<vEffect>}, {"sound", bind<vSound>}, {"player", bind<vPlayer>},
                                     {"send", bind<vSend>}, {"state", bind<vState>}, {nullptr, nullptr}};
    luaL_newlib(vm.L, funcs);
    lua_setglobal(vm.L, "view");
    static const luaL_Reg uiFuncs[] = {{"set", bind<uSet>}, {"open", bind<uOpen>}, {"close", bind<uClose>}, {"on", bind<uOn>}, {nullptr, nullptr}};
    luaL_newlib(vm.L, uiFuncs);
    lua_setglobal(vm.L, "ui");
    for (auto& f : files) {
        auto it = vm.sources.find(f);
        if (it == vm.sources.end()) { im.logLine(true, f + ": view script not loaded"); continue; }
        std::string err;
        vm.used = 0; vm.exceeded = false;
        if (!loadChunk(vm, f, it->second, err) || !protectedCall(vm, 0, 0, &err)) im.logLine(true, err);
    }
    return true;
}

void ViewScripts::attach(Simulation& sim) {
    detach();
    impl_->collector = std::make_shared<Collector>();
    std::weak_ptr<Collector> weak = impl_->collector;
    // runs last in every tick and only copies events out
    sim.addSystem("viewscripts.collect", [weak](Simulation& s) {
        if (auto c = weak.lock(); c && c->enabled && c->events.size() < 20000) c->events.insert(c->events.end(), s.events().begin(), s.events().end());
    });
}

void ViewScripts::detach() {
    if (impl_->collector) impl_->collector->enabled = false;
    impl_->collector.reset();
}

void ViewScripts::update(Simulation& sim, float dt) {
    Impl& im = *impl_;
    if (!im.vm.L) return;
    lua_State* L = im.vm.L;
    im.vm.sim = &sim; im.vm.spatialOk = false;
    im.vm.used = 0;
    im.vm.exceeded = false;
    if (im.collector) {
        std::vector<SimEvent> evs;
        evs.swap(im.collector->events);
        const Rules& r = im.vm.module->rules();
        for (const SimEvent& ev : evs) {
            const char* n = eventName(ev.type);
            if (!n) continue;
            auto it = im.handlers.find(n);
            if (it == im.handlers.end()) continue;
            for (int& ref : it->second) {
                if (ref == LUA_NOREF) continue;
                lua_createtable(L, 0, 8);
                lua_pushstring(L, n); lua_setfield(L, -2, "event");
                lua_pushinteger(L, ev.player); lua_setfield(L, -2, "player");
                lua_pushinteger(L, ev.player); lua_setfield(L, -2, "owner");
                if (ev.a.valid()) { pushEntity(L, ev.a); lua_setfield(L, -2, "unit"); }
                if (ev.b.valid()) { pushEntity(L, ev.b); lua_setfield(L, -2, "other"); }
                if (ev.b.valid() && ev.type == kEvDied) { pushEntity(L, ev.b); lua_setfield(L, -2, "killer"); }
                lua_pushinteger(L, ev.value); lua_setfield(L, -2, "value");
                lua_pushnumber(L, ev.pos.x.toDouble()); lua_setfield(L, -2, "x");
                lua_pushnumber(L, ev.pos.z.toDouble()); lua_setfield(L, -2, "z");
                const bool typed = ev.type == kEvDied || ev.type == kEvSpawned || ev.type == kEvBuildDone || ev.type == kEvTrainDone || ev.type == kEvUpgraded;
                if (typed && ev.value >= 0 && size_t(ev.value) < r.types.size()) { lua_pushstring(L, r.types[size_t(ev.value)].id.c_str()); lua_setfield(L, -2, "unit_type"); }
                if (ev.type == kEvHit) { lua_pushnumber(L, Fixed::fromRaw(ev.value).toDouble()); lua_setfield(L, -2, "amount"); }
                if (ev.type == kEvMessage) {
                    auto& msgs = im.vm.module->state(sim).trig.messages;
                    if (ev.value >= 0 && size_t(ev.value) < msgs.size()) { lua_pushstring(L, msgs[size_t(ev.value)].c_str()); lua_setfield(L, -2, "text"); }
                }
                im.call(ref, 1);
            }
        }
    }
    for (int& ref : im.frame) {
        if (ref == LUA_NOREF) continue;
        lua_pushnumber(L, dt);
        im.call(ref, 1);
    }
}

std::vector<ViewRequest> ViewScripts::takeRequests() {
    std::vector<ViewRequest> out;
    out.swap(impl_->requests);
    return out;
}

std::vector<Command> ViewScripts::takeCommands() {
    std::vector<Command> out;
    out.swap(impl_->commands);
    return out;
}

std::vector<UiRequest> ViewScripts::takeUiRequests() {
    std::vector<UiRequest> out;
    out.swap(impl_->ui);
    return out;
}

void ViewScripts::uiEvent(Simulation& sim, const std::string& name, const Json& args) {
    Impl& im = *impl_;
    if (!im.vm.L) return;
    auto it = im.uiHandlers.find(name);
    if (it == im.uiHandlers.end()) { im.logLine(true, "ui event '" + name + "': no ui.on handler"); return; }
    im.vm.sim = &sim; im.vm.spatialOk = false;
    im.vm.used = 0; im.vm.exceeded = false;
    for (int& ref : it->second) {
        if (ref == LUA_NOREF) continue;
        pushUiJson(im.vm.L, args);
        im.call(ref, 1);
    }
}

bool ViewScripts::send(Simulation& sim, const std::string& id, const Json& args) {
    Impl& im = *impl_;
    if (!im.vm.L) return false;
    ScriptEngine* se = im.vm.module->scripts();
    const int k = se ? se->findCommand(id) : -1;
    if (k < 0) { im.logLine(true, "ui send: unknown command '" + id + "'"); return false; }
    im.vm.sim = &sim; im.vm.spatialOk = false;
    lua_State* L = im.vm.L;
    pushUiJson(L, args);
    Command c;
    c.type = kCmdScript;
    c.param = k;
    const bool ok = encodeScriptArgs(L, im.vm, -1, se->commands()[size_t(k)], c);
    lua_pop(L, 1);
    if (!ok) { im.logLine(true, "ui send '" + id + "': " + im.vm.err); im.vm.err.clear(); return false; }
    if (im.commands.size() < 64) im.commands.push_back(std::move(c));
    return true;
}

} // namespace rts::game
