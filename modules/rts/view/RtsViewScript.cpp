// View scripts: presentation-only Lua VM (see RtsViewScript.h).
#include "RtsViewScript.h"
#include <map>
#include "../RtsModule.h"
#include "../RtsScriptInternal.h"

namespace rts::game {
using namespace script;

namespace {
const char* eventName(uint16_t t) {
    switch (t) {
        case kEvDied: return "unit_died";
        case kEvSpawned: return "spawned";
        case kEvBuildDone: return "built";
        case kEvTrainDone: return "trained";
        case kEvResearched: return "researched";
        case kEvCaptured: return "captured";
        case kEvPlayerDefeated: return "player_defeated";
        case kEvAbility: return "ability";
        case kEvHit: return "damage";
        case kEvLevelUp: return "level_up";
        case kEvUpgraded: return "upgraded";
        case kEvMessage: return "message";
        default: return nullptr;
    }
}
struct Collector { std::vector<SimEvent> events; bool enabled = true; };
} // namespace

struct ViewScripts::Impl {
    ViewScripts& self;
    Vm vm;
    std::map<std::string, std::vector<int>> handlers;   // event -> function refs (LUA_NOREF: disabled)
    std::vector<int> frame;
    std::vector<ViewRequest> requests;
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
} // namespace

ViewScripts::ViewScripts() : impl_(std::make_unique<Impl>(*this)) {}
ViewScripts::~ViewScripts() { detach(); }
bool ViewScripts::active() const { return impl_->vm.L != nullptr; }

bool ViewScripts::load(Simulation& sim, const Json& rules, const Json& map, uint8_t localPlayer) {
    Impl& im = *impl_;
    detach();
    im.vm.close();
    im.handlers.clear(); im.frame.clear(); im.requests.clear();
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
                                     {"effect", bind<vEffect>}, {"sound", bind<vSound>}, {"player", bind<vPlayer>}, {nullptr, nullptr}};
    luaL_newlib(vm.L, funcs);
    lua_setglobal(vm.L, "view");
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

} // namespace rts::game
