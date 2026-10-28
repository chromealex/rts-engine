// The `game` table of the scripting API: queries (simulation and view scripts) and actions (simulation
// scripts only). Every entity is an integer handle (index | generation << 32) checked on each call: a
// dead or foreign handle gives nil / false, never a crash. Actions go through the module's own paths
// (spawn, dealDamage, addStatus, applyCommand...) so data rules, events and hashing stay consistent.
// Reference: docs/scripting.md (keep scriptApiReference() below in sync).
#include "RtsScriptInternal.h"
#include <algorithm>
#include "RtsInternal.h"

namespace rts::game {
using namespace script;
using namespace detail;
namespace detail { void changeUnitType(RtsModule& m, Simulation& sim, Entity e, uint16_t type); }   // RtsEffects.cpp

namespace script {

void pushEntity(lua_State* L, Entity e) { lua_pushinteger(L, lua_Integer(e.packed())); }
bool argEntity(lua_State* L, int idx, Entity& out) {
    if (!lua_isinteger(L, idx)) return false;
    out = Entity::fromPacked(uint64_t(lua_tointeger(L, idx)));
    return true;
}

namespace {

#define CTX RtsModule& m = *vm.module; Simulation& sim = *vm.sim; World& w = sim.world(); const Rules& R = m.rules(); \
    (void)m; (void)w; (void)R

// A living unit/structure handle, or false (no error: dead handles are normal).
bool unitArg(lua_State* L, Vm& vm, int idx, Entity& e) {
    World& w = vm.sim->world();
    return argEntity(L, idx, e) && w.alive(e) && w.has<Unit>(e) && w.has<Transform>(e);
}
bool playerArg(lua_State* L, Vm& vm, int idx, uint8_t& p, bool allowNeutral = false) {
    int64_t v;
    if (!argInt(L, vm, idx, v, "player")) return false;
    if (v == 0 && allowNeutral) { p = 0; return true; }
    if (!vm.sim->player(uint8_t(v)) || v < 1 || v > 254) { vm.err = "unknown player " + std::to_string(v); return false; }
    p = uint8_t(v);
    return true;
}
bool nameArg(lua_State* L, Vm& vm, int idx, std::string& out, const char* what) {
    if (lua_type(L, idx) != LUA_TSTRING) { vm.err = std::string(what) + ": string expected"; return false; }
    out = lua_tostring(L, idx);
    return true;
}
int typeArg(lua_State* L, Vm& vm, int idx) {
    std::string id;
    if (!nameArg(L, vm, idx, id, "type")) return -1;
    int t = vm.module->rules().findType(id);
    if (t < 0) vm.err = "unknown type '" + id + "'";
    return t;
}
int resArg(lua_State* L, Vm& vm, int idx) {
    std::string id;
    if (!nameArg(L, vm, idx, id, "resource")) return -1;
    int r = vm.module->rules().findResource(id);
    if (r < 0) vm.err = "unknown resource '" + id + "'";
    return r;
}
void setF(lua_State* L, const char* k, Fixed v) { pushFx(L, v); lua_setfield(L, -2, k); }
void setI(lua_State* L, const char* k, int64_t v) { lua_pushinteger(L, v); lua_setfield(L, -2, k); }
void setS(lua_State* L, const char* k, const std::string& v) { lua_pushlstring(L, v.data(), v.size()); lua_setfield(L, -2, k); }
void setB(lua_State* L, const char* k, bool v) { lua_pushboolean(L, v); lua_setfield(L, -2, k); }

// ---------------------------------------------------------------- unit filter
struct Filter {
    int owner = -1;
    int type = -1;
    uint64_t tags = 0;
    int kind = 0;                  // 1 units, 2 structures
    int area = -1;
    bool circle = false;
    Vec3 center{};
    Fixed radius{};
    int limit = 0;
    std::string has;               // entities whose game.data table has this key
    bool byDist = false;           // sort = "dist": nearest first (ties by handle), needs x/z
};
// fields are read raw (no metamethods): {owner=, type=, tag=, kind=, area=, x=, z=, radius=, limit=}
bool readFilter(lua_State* L, Vm& vm, int idx, Filter& f) {
    if (lua_isnoneornil(L, idx)) return true;
    if (!lua_istable(L, idx)) { vm.err = "filter table expected"; return false; }
    idx = lua_absindex(L, idx);
    const int top = lua_gettop(L);   // every field read is undone with settop (also on errors)
    auto field = [&](const char* k) { lua_pushstring(L, k); return lua_rawget(L, idx); };
    bool ok = true;
    if (field("owner") != LUA_TNIL) { int64_t v = -1; ok = ok && argInt(L, vm, -1, v, "filter.owner"); f.owner = int(v); }
    lua_settop(L, top);
    if (ok && field("type") != LUA_TNIL) { f.type = typeArg(L, vm, -1); ok = f.type >= 0; }
    lua_settop(L, top);
    if (ok && field("tag") != LUA_TNIL) {
        const char* t = lua_tostring(L, -1);
        f.tags = t ? vm.module->rules().tagMask(t) : 0;
        if (!f.tags) { vm.err = std::string("unknown tag '") + (t ? t : "?") + "'"; ok = false; }
    }
    lua_settop(L, top);
    if (ok && field("kind") != LUA_TNIL) {
        std::string k = lua_tostring(L, -1) ? lua_tostring(L, -1) : "";
        f.kind = k == "unit" ? 1 : k == "structure" ? 2 : -1;
        if (f.kind < 0) { vm.err = "filter.kind: \"unit\" or \"structure\""; ok = false; }
    }
    lua_settop(L, top);
    if (ok && field("area") != LUA_TNIL) {
        const char* a = lua_tostring(L, -1);
        f.area = a ? vm.module->triggers().findArea(a) : -1;
        if (f.area < 0) { vm.err = std::string("unknown area '") + (a ? a : "?") + "'"; ok = false; }
    }
    lua_settop(L, top);
    if (ok && field("radius") != LUA_TNIL) {
        f.circle = true;
        ok = argFx(L, vm, -1, f.radius, "filter.radius");
        lua_settop(L, top);
        if (ok) { field("x"); ok = argFx(L, vm, -1, f.center.x, "filter.x"); lua_settop(L, top); }
        if (ok) { field("z"); ok = argFx(L, vm, -1, f.center.z, "filter.z"); }
    }
    lua_settop(L, top);
    if (ok && field("limit") != LUA_TNIL) { int64_t v = 0; ok = argInt(L, vm, -1, v, "filter.limit"); f.limit = int(v); }
    lua_settop(L, top);
    if (ok && field("has") != LUA_TNIL) {
        const char* k = lua_tostring(L, -1);
        if (k) f.has = k; else { vm.err = "filter.has: data key (string) expected"; ok = false; }
    }
    lua_settop(L, top);
    if (ok && field("sort") != LUA_TNIL) {
        const char* k = lua_tostring(L, -1);
        f.byDist = k && std::string(k) == "dist";
        if (!f.byDist && !(k && std::string(k) == "id")) { vm.err = "filter.sort: \"id\" or \"dist\""; ok = false; }
        if (ok && f.byDist && !f.circle) {
            lua_settop(L, top); field("x"); ok = argFx(L, vm, -1, f.center.x, "filter.x");
            lua_settop(L, top); if (ok) { field("z"); ok = argFx(L, vm, -1, f.center.z, "filter.z"); }
        }
    }
    lua_settop(L, top);
    return ok;
}
// The radius-query grid: living on-map units in buckets of `cell` world units (positions outside the map
// go to the edge buckets). Only an accelerator: results are filtered exactly and sorted like the full scan.
void buildSpatial(Vm& vm) {
    CTX;
    const MapGrid& g = m.state(sim).grid;
    Vm::Spatial& sp = vm.spatial;
    sp.w = std::max(1, (g.w + sp.cell - 1) / sp.cell); sp.h = std::max(1, (g.h + sp.cell - 1) / sp.cell);
    if (sp.buckets.size() != size_t(sp.w * sp.h)) { sp.buckets.assign(size_t(sp.w * sp.h), {}); sp.used.clear(); }
    for (int32_t b : sp.used) sp.buckets[size_t(b)].clear();
    sp.used.clear();
    w.each<Unit, Owner, Transform>([&](Entity e, Unit& u, Owner&, Transform& t) {
        if (u.hp.raw <= 0 || (u.flags & kUnitOffMap)) return;
        int bx = std::clamp(int(t.pos.x.floorInt()) / sp.cell, 0, sp.w - 1), by = std::clamp(int(t.pos.z.floorInt()) / sp.cell, 0, sp.h - 1);
        auto& bk = sp.buckets[size_t(by * sp.w + bx)];
        if (bk.empty()) sp.used.push_back(by * sp.w + bx);
        bk.push_back(e);
    });
    vm.spatialOk = true;
}

// Matching living units sorted by entity index (deterministic and independent of ECS history).
std::vector<Entity> query(Vm& vm, const Filter& f) {
    CTX;
    std::vector<Entity> out;
    const TrigArea* ar = f.area >= 0 ? &m.triggers().areas[size_t(f.area)] : nullptr;
    auto test = [&](Entity e, Unit& u, Owner& o, Transform& t) {
        if (u.hp.raw <= 0 || (u.flags & kUnitOffMap)) return;
        if (f.owner >= 0 && o.player != f.owner) return;
        if (f.type >= 0 && u.type != f.type) return;
        const UnitType& ut = R.types[u.type];
        if (f.tags && !(ut.tags & f.tags)) return;
        if (f.kind == 1 && ut.structure) return;
        if (f.kind == 2 && !ut.structure) return;
        if (ar && !ar->contains(t.pos)) return;
        if (f.circle && dist2D(t.pos, f.center) > f.radius) return;
        out.push_back(e);
    };
    if (f.circle && !m.state(sim).surface && f.radius.raw >= 0) {   // bucket grid (flat maps)
        if (!vm.spatialOk) buildSpatial(vm);
        const Vm::Spatial& sp = vm.spatial;
        auto bucket = [&](Fixed v, int n) { return std::clamp(int(v.floorInt()) / sp.cell, 0, n - 1); };
        const int x0 = bucket(f.center.x - f.radius, sp.w), x1 = bucket(f.center.x + f.radius, sp.w);
        const int y0 = bucket(f.center.z - f.radius, sp.h), y1 = bucket(f.center.z + f.radius, sp.h);
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x)
                for (Entity e : sp.buckets[size_t(y * sp.w + x)]) test(e, w.get<Unit>(e), w.get<Owner>(e), w.get<Transform>(e));
    } else {
        w.each<Unit, Owner, Transform>(test);
    }
    std::sort(out.begin(), out.end(), [](Entity a, Entity b) { return a.index < b.index; });
    if (f.byDist) {
        std::vector<std::pair<Fixed, Entity>> d;
        d.reserve(out.size());
        for (Entity e : out) d.push_back({dist2D(w.get<Transform>(e).pos, f.center), e});
        std::stable_sort(d.begin(), d.end(), [](auto& a, auto& b) { return a.first < b.first; });
        for (size_t i = 0; i < d.size(); ++i) out[i] = d[i].second;
    }
    if (!f.has.empty()) {
        std::vector<Entity> keep;
        for (Entity e : out)
            if (vm.dataHas ? vm.dataHas(e, f.has.c_str()) : (w.has<ScriptData>(e) && blobHasKey(w.get<ScriptData>(e).blob, f.has.c_str()))) keep.push_back(e);
        out.swap(keep);
    }
    if (f.limit > 0 && out.size() > size_t(f.limit)) out.resize(size_t(f.limit));
    return out;
}

// ---------------------------------------------------------------- queries
int gTick(lua_State* L, Vm& vm) { lua_pushinteger(L, vm.sim->tick()); return 1; }
int gTime(lua_State* L, Vm& vm) { pushFx(L, Fixed::fromRatio(int64_t(vm.sim->tick()), int64_t(vm.sim->tickRate()))); return 1; }
int gTickRate(lua_State* L, Vm& vm) { lua_pushinteger(L, vm.sim->tickRate()); return 1; }
int gPlayers(lua_State* L, Vm& vm) {
    lua_newtable(L);
    int n = 0;
    for (auto& p : vm.sim->players()) { lua_pushinteger(L, p.slot); lua_rawseti(L, -2, ++n); }
    return 1;
}
int gPlayer(lua_State* L, Vm& vm) {
    CTX;
    int64_t s;
    if (!argInt(L, vm, 1, s, "player")) return -1;
    auto* pi = sim.player(uint8_t(s));
    if (!pi || s < 1) { lua_pushnil(L); return 1; }
    lua_createtable(L, 0, 6);
    setI(L, "slot", pi->slot); setI(L, "team", pi->team); setB(L, "alive", pi->alive); setS(L, "name", pi->name);
    GameState& gs = m.state(sim);
    if (pi->slot < gs.players.size()) {
        setS(L, "faction", gs.players[pi->slot].faction);
        setF(L, "supply", gs.players[pi->slot].supplyUsed); setF(L, "supply_cap", gs.players[pi->slot].supplyCap);
    }
    return 1;
}
int gAlivePlayer(lua_State* L, Vm& vm) {
    int64_t s;
    if (!argInt(L, vm, 1, s, "player")) return -1;
    auto* pi = vm.sim->player(uint8_t(s));
    lua_pushboolean(L, pi && s >= 1 && pi->alive);
    return 1;
}
int gTeam(lua_State* L, Vm& vm) { uint8_t p; if (!playerArg(L, vm, 1, p)) return -1; lua_pushinteger(L, vm.module->teamOf(*vm.sim, p)); return 1; }
int gHostile(lua_State* L, Vm& vm) {
    int64_t a, b;
    if (!argInt(L, vm, 1, a, "player") || !argInt(L, vm, 2, b, "player")) return -1;
    lua_pushboolean(L, vm.module->hostile(*vm.sim, uint8_t(a), uint8_t(b)));
    return 1;
}
int gResource(lua_State* L, Vm& vm) {
    CTX;
    uint8_t p; if (!playerArg(L, vm, 1, p)) return -1;
    int r = resArg(L, vm, 2); if (r < 0) return -1;
    GameState& gs = m.state(sim);
    pushFx(L, p < gs.players.size() ? gs.players[p].stock[size_t(r)] : Fixed{});
    return 1;
}
int gResources(lua_State* L, Vm& vm) {
    CTX;
    uint8_t p; if (!playerArg(L, vm, 1, p)) return -1;
    GameState& gs = m.state(sim);
    lua_newtable(L);
    for (size_t r = 0; r < R.resources.size(); ++r) setF(L, R.resources[r].id.c_str(), p < gs.players.size() ? gs.players[p].stock[r] : Fixed{});
    return 1;
}
int gUnits(lua_State* L, Vm& vm) {
    Filter f;
    if (!readFilter(L, vm, 1, f)) return -1;
    std::vector<Entity> v = query(vm, f);
    lua_createtable(L, int(v.size()), 0);
    for (size_t i = 0; i < v.size(); ++i) { pushEntity(L, v[i]); lua_rawseti(L, -2, lua_Integer(i + 1)); }
    return 1;
}
int gCount(lua_State* L, Vm& vm) {
    Filter f;
    if (!readFilter(L, vm, 1, f)) return -1;
    lua_pushinteger(L, lua_Integer(query(vm, f).size()));
    return 1;
}
int gNearest(lua_State* L, Vm& vm) {
    CTX;
    Vec3 p{};
    if (!argFx(L, vm, 1, p.x, "x") || !argFx(L, vm, 2, p.z, "z")) return -1;
    Filter f;
    if (!readFilter(L, vm, 3, f)) return -1;
    Entity best;
    Fixed bd{};
    for (Entity e : query(vm, f)) {
        Fixed d = dist2D(w.get<Transform>(e).pos, p);
        if (!best.valid() || d < bd) { best = e; bd = d; }
    }
    if (best.valid()) pushEntity(L, best); else lua_pushnil(L);
    return 1;
}
int gUnit(lua_State* L, Vm& vm) {
    CTX;
    Entity e;
    if (!unitArg(L, vm, 1, e)) { lua_pushnil(L); return 1; }
    const Unit& u = w.get<Unit>(e);
    const UnitType& t = R.types[u.type];
    const Vec3 pos = w.get<Transform>(e).pos;
    lua_createtable(L, 0, 16);
    pushEntity(L, e); lua_setfield(L, -2, "id");
    setS(L, "type", t.id); setS(L, "name", t.name);
    setI(L, "owner", w.get<Owner>(e).player);
    setF(L, "x", pos.x); setF(L, "y", pos.y); setF(L, "z", pos.z);
    setF(L, "hp", u.hp); setF(L, "max_hp", m.stat(sim, e, Stat::MaxHp)); setF(L, "energy", u.energy);
    setB(L, "structure", t.structure);
    setB(L, "under_construction", u.flags & kUnitUnderConstruction);
    setB(L, "alive", u.hp.raw > 0);
    if (auto* h = w.tryGet<Hero>(e)) setI(L, "level", h->level);
    if (auto* g = w.tryGet<Garrison>(e)) setI(L, "garrison", g->count);
    lua_newtable(L);
    int n = 0;
    for (size_t i = 0; i < R.tagNames.size() && i < 64; ++i)
        if (t.tags & (uint64_t(1) << i)) { lua_pushstring(L, R.tagNames[i].c_str()); lua_rawseti(L, -2, ++n); }
    lua_setfield(L, -2, "tags");
    lua_newtable(L);
    n = 0;
    if (auto* st = w.tryGet<Statuses>(e))
        for (auto& in : st->list)
            if (in.def < R.statuses.size()) { lua_pushstring(L, R.statuses[in.def].id.c_str()); lua_rawseti(L, -2, ++n); }
    lua_setfield(L, -2, "statuses");
    return 1;
}
int gAlive(lua_State* L, Vm& vm) { Entity e; lua_pushboolean(L, unitArg(L, vm, 1, e) && vm.sim->world().get<Unit>(e).hp.raw > 0); return 1; }
int gOwner(lua_State* L, Vm& vm) { Entity e; if (unitArg(L, vm, 1, e)) lua_pushinteger(L, vm.sim->world().get<Owner>(e).player); else lua_pushnil(L); return 1; }
int gTypeOf(lua_State* L, Vm& vm) {
    Entity e;
    if (unitArg(L, vm, 1, e)) lua_pushstring(L, vm.module->rules().types[vm.sim->world().get<Unit>(e).type].id.c_str()); else lua_pushnil(L);
    return 1;
}
int gPos(lua_State* L, Vm& vm) {
    Entity e;
    if (!unitArg(L, vm, 1, e)) { lua_pushnil(L); return 1; }
    Vec3 p = vm.sim->world().get<Transform>(e).pos;
    pushFx(L, p.x); pushFx(L, p.z);
    return 2;
}
int gHp(lua_State* L, Vm& vm) {
    Entity e;
    if (!unitArg(L, vm, 1, e)) { lua_pushnil(L); return 1; }
    pushFx(L, vm.sim->world().get<Unit>(e).hp);
    pushFx(L, vm.module->stat(*vm.sim, e, Stat::MaxHp));
    return 2;
}
int gStat(lua_State* L, Vm& vm) {
    Entity e;
    std::string s;
    if (!nameArg(L, vm, 2, s, "stat")) return -1;
    bool ok = false;
    Stat st = parseStat(s, &ok);
    if (!ok) return fail(vm, "unknown stat '" + s + "'");
    if (!unitArg(L, vm, 1, e)) { lua_pushnil(L); return 1; }
    pushFx(L, vm.module->stat(*vm.sim, e, st));
    return 1;
}
int gHasStatus(lua_State* L, Vm& vm) {
    CTX;
    std::string s;
    if (!nameArg(L, vm, 2, s, "status")) return -1;
    int d = R.findStatus(s);
    if (d < 0) return fail(vm, "unknown status '" + s + "'");
    Entity e;
    bool has = false;
    if (unitArg(L, vm, 1, e))
        if (auto* st = w.tryGet<Statuses>(e)) for (auto& in : st->list) has |= in.def == d;
    lua_pushboolean(L, has);
    return 1;
}
int gTypeInfo(lua_State* L, Vm& vm) {
    CTX;
    std::string id;
    if (!nameArg(L, vm, 1, id, "type")) return -1;
    int k = R.findType(id);
    if (k < 0) { lua_pushnil(L); return 1; }
    const UnitType& t = R.types[size_t(k)];
    lua_createtable(L, 0, 8);
    setS(L, "id", t.id); setS(L, "name", t.name); setB(L, "structure", t.structure);
    setF(L, "max_hp", t.maxHp); setF(L, "speed", t.speed); setF(L, "armor", t.armor);
    lua_newtable(L);
    for (auto& c : t.cost) if (c.res < R.resources.size()) setF(L, R.resources[c.res].id.c_str(), c.value);
    lua_setfield(L, -2, "cost");
    lua_newtable(L);
    int n = 0;
    for (size_t i = 0; i < R.tagNames.size() && i < 64; ++i)
        if (t.tags & (uint64_t(1) << i)) { lua_pushstring(L, R.tagNames[i].c_str()); lua_rawseti(L, -2, ++n); }
    lua_setfield(L, -2, "tags");
    return 1;
}
int gTypes(lua_State* L, Vm& vm) {
    const Rules& R = vm.module->rules();
    lua_createtable(L, int(R.types.size()), 0);
    for (size_t i = 0; i < R.types.size(); ++i) { lua_pushstring(L, R.types[i].id.c_str()); lua_rawseti(L, -2, lua_Integer(i + 1)); }
    return 1;
}
int gMapSize(lua_State* L, Vm& vm) {
    const MapGrid& g = vm.module->state(*vm.sim).grid;
    lua_pushinteger(L, g.w); lua_pushinteger(L, g.h);
    return 2;
}
bool cellArgs(lua_State* L, Vm& vm, int& cx, int& cy, Vec3& p) {
    if (!argFx(L, vm, 1, p.x, "x") || !argFx(L, vm, 2, p.z, "z")) return false;
    const MapGrid& g = vm.module->state(*vm.sim).grid;
    cx = g.cellX(p.x); cy = g.cellY(p.z);
    return true;
}
int gPassable(lua_State* L, Vm& vm) {
    int cx, cy; Vec3 p{};
    if (!cellArgs(L, vm, cx, cy, p)) return -1;
    std::string layer = lua_isstring(L, 3) ? lua_tostring(L, 3) : "ground";
    uint8_t lay = layer == "air" ? kLayerAir : layer == "naval" ? kLayerNaval : kLayerGround;
    const MapGrid& g = vm.module->state(*vm.sim).grid;
    lua_pushboolean(L, g.inside(cx, cy) && g.passable(cx, cy, lay));
    return 1;
}
int gLevel(lua_State* L, Vm& vm) {
    int cx, cy; Vec3 p{};
    if (!cellArgs(L, vm, cx, cy, p)) return -1;
    lua_pushinteger(L, vm.module->levelAt(*vm.sim, p));
    return 1;
}
int gHeight(lua_State* L, Vm& vm) {
    int cx, cy; Vec3 p{};
    if (!cellArgs(L, vm, cx, cy, p)) return -1;
    pushFx(L, vm.module->groundHeight(*vm.sim, p));
    return 1;
}
int gArea(lua_State* L, Vm& vm) {
    std::string n;
    if (!nameArg(L, vm, 1, n, "area")) return -1;
    const TriggerSet& ts = vm.module->triggers();
    int a = ts.findArea(n);
    if (a < 0) { lua_pushnil(L); return 1; }
    const TrigArea& ar = ts.areas[size_t(a)];
    lua_createtable(L, 0, 4);
    setF(L, "x", ar.x); setF(L, "z", ar.z); setF(L, "w", ar.w); setF(L, "h", ar.h);
    return 1;
}
int gVar(lua_State* L, Vm& vm) {
    std::string n;
    if (!nameArg(L, vm, 1, n, "variable")) return -1;
    auto& vars = vm.module->state(*vm.sim).trig.vars;
    auto it = vars.find(n);
    pushFx(L, it == vars.end() ? Fixed{} : it->second);
    return 1;
}
// game.data(h): the entity's own Lua table (simulation: persistent, saved and hashed; view: a read-only copy)
int gData(lua_State* L, Vm& vm) {
    Entity e;
    if (!unitArg(L, vm, 1, e)) { lua_pushnil(L); return 1; }
    if (vm.pushData) { vm.pushData(e); return 1; }
    auto* sd = vm.sim->world().tryGet<ScriptData>(e);
    if (!sd || sd->blob.empty()) lua_newtable(L);
    else deserialize(L, sd->blob);
    return 1;
}
int gHasData(lua_State* L, Vm& vm) {
    Entity e;
    std::string k;
    if (!nameArg(L, vm, 2, k, "key")) return -1;
    World& w = vm.sim->world();
    bool has = unitArg(L, vm, 1, e) && (vm.dataHas ? vm.dataHas(e, k.c_str()) : (w.has<ScriptData>(e) && blobHasKey(w.get<ScriptData>(e).blob, k.c_str())));
    lua_pushboolean(L, has);
    return 1;
}
int gLog(lua_State* L, Vm& vm) {
    std::string s;
    for (int i = 1; i <= lua_gettop(L); ++i) {
        if (Fixed* f = testFx(L, i)) s += fxText(*f);
        else if (const char* c = lua_tostring(L, i)) s += c;
        else s += luaL_typename(L, i);
        if (i < lua_gettop(L)) s += ' ';
    }
    if (vm.log) vm.log(false, s);
    return 0;
}

// ---------------------------------------------------------------- actions (simulation scripts)
int gGive(lua_State* L, Vm& vm) {
    CTX;
    uint8_t p; if (!playerArg(L, vm, 1, p)) return -1;
    int r = resArg(L, vm, 2); if (r < 0) return -1;
    Fixed a; if (!argFx(L, vm, 3, a, "amount")) return -1;
    m.addResource(sim, p, uint16_t(r), a);
    return 0;
}
int gTake(lua_State* L, Vm& vm) {
    CTX;
    uint8_t p; if (!playerArg(L, vm, 1, p)) return -1;
    int r = resArg(L, vm, 2); if (r < 0) return -1;
    Fixed a; if (!argFx(L, vm, 3, a, "amount")) return -1;
    GameState& gs = m.state(sim);
    Fixed& s = gs.players[p].stock[size_t(r)];
    Fixed taken = fxMax(Fixed{}, fxMin(s, a));
    s -= taken;
    pushFx(L, taken);
    return 1;
}
int gSpawn(lua_State* L, Vm& vm) {
    CTX;
    int t = typeArg(L, vm, 1); if (t < 0) return -1;
    int64_t owner; if (!argInt(L, vm, 2, owner, "owner")) return -1;
    if (owner != 0 && owner != kHostileNeutral && !sim.player(uint8_t(owner))) return fail(vm, "spawn: unknown owner " + std::to_string(owner));
    Vec3 p{};
    if (!argFx(L, vm, 3, p.x, "x") || !argFx(L, vm, 4, p.z, "z")) return -1;
    GameState& gs = m.state(sim);
    const UnitType& ut = R.types[size_t(t)];
    int cx, cy;
    if (!gs.grid.cellXY(p, cx, cy)) { lua_pushnil(L); return 1; }
    if (!ut.structure) {
        const int ox = cx, oy = cy;
        if (!gs.grid.nearestPassable(cx, cy, ut.pathLayer, 6)) { lua_pushnil(L); return 1; }
        if (cx != ox || cy != oy) p = gs.grid.cellCenter(cx, cy);
    }
    pushEntity(L, m.spawn(sim, uint16_t(t), uint8_t(owner), p));
    return 1;
}
int gKill(lua_State* L, Vm& vm) {
    Entity e;
    if (!unitArg(L, vm, 1, e)) { lua_pushboolean(L, 0); return 1; }
    Unit& u = vm.sim->world().get<Unit>(e);
    if (lua_toboolean(L, 2)) u.flags |= kUnitSilent;   // remove without death effects
    u.hp = Fixed{};
    lua_pushboolean(L, 1);
    return 1;
}
int gDamage(lua_State* L, Vm& vm) {
    CTX;
    Fixed a; if (!argFx(L, vm, 2, a, "amount")) return -1;
    Entity e, src;
    if (!unitArg(L, vm, 1, e)) { lua_pushboolean(L, 0); return 1; }
    uint8_t so = 0;
    if (!lua_isnoneornil(L, 3) && unitArg(L, vm, 3, src)) so = w.get<Owner>(src).player;
    std::string dt = lua_isstring(L, 4) ? lua_tostring(L, 4) : "";
    uint16_t dti = 0;
    for (size_t i = 0; i < R.damageTypes.size(); ++i) if (R.damageTypes[i] == dt) dti = uint16_t(i);
    m.dealDamage(sim, e, a, dti, src, so);
    lua_pushboolean(L, 1);
    return 1;
}
int gHeal(lua_State* L, Vm& vm) {
    CTX;
    Fixed a; if (!argFx(L, vm, 2, a, "amount")) return -1;
    Entity e;
    if (!unitArg(L, vm, 1, e)) { lua_pushboolean(L, 0); return 1; }
    Unit& u = w.get<Unit>(e);
    if (u.hp.raw > 0) u.hp = fxMin(m.stat(sim, e, Stat::MaxHp), u.hp + a);
    lua_pushboolean(L, 1);
    return 1;
}
int gSetOwner(lua_State* L, Vm& vm) {
    CTX;
    uint8_t p; if (!playerArg(L, vm, 2, p, true)) return -1;
    Entity e;
    if (!unitArg(L, vm, 1, e)) { lua_pushboolean(L, 0); return 1; }
    Owner& o = w.get<Owner>(e);
    const uint8_t old = o.player;
    if (old == p) { lua_pushboolean(L, 1); return 1; }
    GameState& gs = m.state(sim);
    const uint16_t ty = w.get<Unit>(e).type;
    if (old < gs.players.size() && !gs.players[old].typeCountAll.empty() && gs.players[old].typeCountAll[ty] > 0) gs.players[old].typeCountAll[ty]--;
    if (p < gs.players.size() && !gs.players[p].typeCountAll.empty()) gs.players[p].typeCountAll[ty]++;
    o.player = p;
    if (auto* pr = w.tryGet<Production>(e)) pr->queue.clear();   // the old owner's queue (paid by them)
    if (auto* ord = w.tryGet<Orders>(e)) ord->queue.clear();
    if (auto* c = w.tryGet<Combat>(e)) c->target = kNullEntity;
    m.stopMoving(sim, e);
    sim.emit({kEvCaptured, p, e, kNullEntity, old, w.get<Transform>(e).pos});
    lua_pushboolean(L, 1);
    return 1;
}
int gAddStatus(lua_State* L, Vm& vm) {
    CTX;
    std::string s;
    if (!nameArg(L, vm, 2, s, "status")) return -1;
    int d = R.findStatus(s);
    if (d < 0) return fail(vm, "unknown status '" + s + "'");
    Fixed dur; if (!optFx(L, vm, 3, dur, Fixed{}, "seconds")) return -1;
    if (!R.advancedEffects) return fail(vm, "add_status: the game has no statuses");
    Entity e, caster;
    if (!unitArg(L, vm, 1, e)) { lua_pushboolean(L, 0); return 1; }
    EffectCtx ctx;
    if (!lua_isnoneornil(L, 4) && unitArg(L, vm, 4, caster)) { ctx.caster = caster; ctx.owner = w.get<Owner>(caster).player; ctx.sourceType = w.get<Unit>(caster).type; }
    else ctx.owner = w.get<Owner>(e).player;
    ctx.target = e; ctx.point = w.get<Transform>(e).pos;
    m.addStatus(sim, e, d, dur, ctx);
    lua_pushboolean(L, 1);
    return 1;
}
int gBuff(lua_State* L, Vm& vm) {
    CTX;
    std::string s;
    if (!nameArg(L, vm, 2, s, "stat")) return -1;
    bool ok = false;
    Stat st = parseStat(s, &ok);
    if (!ok) return fail(vm, "unknown stat '" + s + "'");
    Fixed add, mul, dur;
    if (!optFx(L, vm, 3, add, Fixed{}, "add") || !optFx(L, vm, 4, mul, Fixed::fromInt(1), "mul") || !optFx(L, vm, 5, dur, Fixed{}, "seconds")) return -1;
    Entity e;
    if (!unitArg(L, vm, 1, e)) { lua_pushboolean(L, 0); return 1; }
    uint32_t until = dur.raw > 0 ? sim.tick() + uint32_t((dur * int64_t(sim.tickRate())).toInt()) : UINT32_MAX;
    if (auto* b = w.tryGet<Buffs>(e)) b->list.push_back({st, add, mul, until, kNoType});
    lua_pushboolean(L, 1);
    return 1;
}
// Units argument: a handle or an array of handles; living ones grouped by owner.
bool unitsArg(lua_State* L, Vm& vm, int idx, std::vector<Entity>& out) {
    Entity e;
    if (lua_isinteger(L, idx)) { if (unitArg(L, vm, idx, e)) out.push_back(e); return true; }
    if (!lua_istable(L, idx)) { vm.err = "unit handle or list of handles expected"; return false; }
    const lua_Integer n = lua_Integer(lua_rawlen(L, idx));
    for (lua_Integer i = 1; i <= n; ++i) {
        lua_rawgeti(L, idx, i);
        if (unitArg(L, vm, -1, e)) out.push_back(e);
        lua_pop(L, 1);
    }
    return true;
}
const std::pair<const char*, uint16_t> kCommands[] = {
    {"move", kCmdMove}, {"attack", kCmdAttack}, {"stop", kCmdStop}, {"hold", kCmdHold}, {"patrol", kCmdPatrol},
    {"gather", kCmdGather}, {"build", kCmdBuild}, {"train", kCmdTrain}, {"cancel", kCmdCancelTrain}, {"rally", kCmdRally},
    {"smart", kCmdSmart}, {"send", kCmdSend}, {"research", kCmdResearch}, {"place", kCmdPlace}, {"sell", kCmdSell},
    {"deploy", kCmdDeploy}, {"ability", kCmdAbility}, {"reinforce", kCmdReinforce}, {"claim", kCmdClaim},
    {"auto_build", kCmdAutoBuild}, {"upgrade", kCmdUpgrade}, {"surrender", kCmdSurrender}, {"player_ability", kCmdPlayerAbility},
    {"dig", kCmdDig}, {"paint_room", kCmdPaintRoom}, {"pick_up", kCmdPickUp}, {"drop", kCmdDrop}, {"slap", kCmdSlap},
    {"possess", kCmdPossess}, {"direct", kCmdDirect}, {"build_road", kCmdBuildRoad}, {"connect_road", kCmdConnectRoad},
    {"transit", kCmdTransit}, {"stance", kCmdStance}, {"retreat", kCmdRetreat}, {"load", kCmdLoad}, {"unload", kCmdUnload},
    {"guard", kCmdGuard}, {"follow", kCmdFollow}, {"formation", kCmdFormation}, {"build_line", kCmdBuildLine},
    {"merge", kCmdMerge}, {"split", kCmdSplit}, {"trade", kCmdTrade}, {"trade_route", kCmdTradeRoute}, {"tribute", kCmdTribute},
    {"diplomacy", kCmdDiplomacy}, {"buy_item", kCmdBuyItem}, {"use_item", kCmdUseItem}, {"drop_item", kCmdDropItem},
    {"pick_item", kCmdPickItem}, {"learn", kCmdLearn}, {"bounty", kCmdBounty}, {"rotate", kCmdRotate}, {"lock", kCmdLock},
    {"end_turn", kCmdEndTurn},
};
const std::pair<const char*, int> kStances[] = {{"aggressive", 0}, {"defensive", 1}, {"hold", 2}, {"passive", 3}};
const std::pair<const char*, int> kFormations[] = {{"line", 0}, {"box", 1}, {"wedge", 2}, {"loose", 3}, {"none", -1}};
// Packed params from named fields (RtsModule.h CommandType comments); `param` given as a number wins.
bool commandParam(lua_State* L, Vm& vm, int t, Command& c) {
    const Rules& R = vm.module->rules();
    const int top = lua_gettop(L);
    auto field = [&](const char* k) { lua_settop(L, top); lua_pushstring(L, k); return lua_rawget(L, t) != LUA_TNIL; };
    auto num = [&](const char* k, int64_t& v, int64_t def) { v = def; return !field(k) || argInt(L, vm, -1, v, k); };
    auto res = [&](int64_t& r) {
        r = 0;
        if (!field("resource")) return true;
        if (lua_type(L, -1) != LUA_TSTRING) return argInt(L, vm, -1, r, "resource");
        r = R.findResource(lua_tostring(L, -1));
        if (r < 0) vm.err = std::string("unknown resource '") + lua_tostring(L, -1) + "'";
        return r >= 0;
    };
    bool ok = true;
    int64_t a = 0, b = 0, d = 0;
    switch (c.type) {
    case kCmdDig: case kCmdPaintRoom: case kCmdBuildRoad:
        ok = num("w", a, 1) && num("h", b, 1) && num("extra", d, 0);
        if (ok && !field("param")) c.param = rectParam(int(a), int(b), int(d));
        break;
    case kCmdBuildLine:
        ok = num("end_x", a, 0) && num("end_z", b, 0);
        if (ok) c.param = (c.param & 0xFFFF) | (a & 0xFFFF) << 16 | (b & 0xFFFF) << 32;
        break;
    case kCmdTrade:
        ok = res(a);
        if (ok && field("sell")) c.param = a | (lua_toboolean(L, -1) ? 1 << 8 : 0);
        else if (ok && !field("param")) c.param = a;
        break;
    case kCmdTribute:
        ok = res(a) && num("to", b, 0) && num("amount", d, 0);
        if (ok && !field("param")) c.param = a | b << 8 | d << 16;
        break;
    case kCmdDiplomacy:
        ok = num("other", a, 0);
        if (ok && !field("param")) { field("ally"); c.param = a | (lua_toboolean(L, -1) ? 1 << 8 : 0); }
        break;
    case kCmdBounty:
        ok = res(a) && num("reward", b, 0);
        if (ok && !field("param")) c.param = a | b << 8;
        break;
    case kCmdStance: case kCmdFormation:
        if (field("param") && lua_type(L, -1) == LUA_TSTRING) {
            std::string n = lua_tostring(L, -1);
            ok = false;
            if (c.type == kCmdStance) { for (auto& [k, v] : kStances) if (n == k) { c.param = v; ok = true; } }
            else for (auto& [k, v] : kFormations) if (n == k) { c.param = v; ok = true; }
            if (!ok) vm.err = "command: unknown " + std::string(c.type == kCmdStance ? "stance" : "formation") + " '" + n + "'";
        }
        break;
    default: break;
    }
    lua_settop(L, top);
    return ok;
}
// game.command(player, {type=, units=, x=, z=, target=, param=, queue=}): any player command, applied now
// with the normal validation (invalid commands are ignored, like commands from the network).
int gCommand(lua_State* L, Vm& vm) {
    CTX;
    uint8_t p; if (!playerArg(L, vm, 1, p)) return -1;
    if (!lua_istable(L, 2)) return fail(vm, "command: table expected");
    Command c;
    c.player = p;
    c.tick = sim.tick();
    lua_pushstring(L, "type"); lua_rawget(L, 2);
    std::string type = lua_tostring(L, -1) ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);
    bool known = false;
    for (auto& [n, v] : kCommands) if (type == n) { c.type = v; known = true; }
    if (!known) return fail(vm, "command: unknown type '" + type + "'");
    lua_pushstring(L, "units");
    if (lua_rawget(L, 2) != LUA_TNIL && !unitsArg(L, vm, lua_gettop(L), c.units)) return -1;
    lua_pop(L, 1);
    lua_pushstring(L, "target");
    if (lua_rawget(L, 2) != LUA_TNIL) { Entity t; if (unitArg(L, vm, -1, t)) c.target = t; }
    lua_pop(L, 1);
    lua_pushstring(L, "x");
    if (lua_rawget(L, 2) != LUA_TNIL && !argFx(L, vm, -1, c.point.x, "command.x")) return -1;
    lua_pop(L, 1);
    lua_pushstring(L, "z");
    if (lua_rawget(L, 2) != LUA_TNIL && !argFx(L, vm, -1, c.point.z, "command.z")) return -1;
    lua_pop(L, 1);
    lua_pushstring(L, "queue");
    if (lua_rawget(L, 2) == LUA_TBOOLEAN && lua_toboolean(L, -1)) c.flags |= kCmdQueue;
    lua_pop(L, 1);
    lua_pushstring(L, "param");
    int pt = lua_rawget(L, 2);
    if (pt == LUA_TSTRING && (c.type == kCmdStance || c.type == kCmdFormation)) {
        // named below (commandParam)
    } else if (pt == LUA_TSTRING) {   // names: types (train/build/upgrade/build_line), techs (research), player abilities
        std::string n = lua_tostring(L, -1);
        int k = c.type == kCmdResearch ? R.findTech(n) : c.type == kCmdPlayerAbility ? R.findPlayerAbility(n) : R.findType(n);
        if (k < 0) { lua_pop(L, 1); return fail(vm, "command: unknown '" + n + "'"); }
        c.param = k;
    } else if (pt != LUA_TNIL) {
        int64_t v;
        if (!argInt(L, vm, -1, v, "command.param")) { lua_pop(L, 1); return -1; }
        c.param = v;
    }
    lua_pop(L, 1);
    if (!commandParam(L, vm, 2, c)) return -1;
    m.applyCommand(sim, c);
    return 0;
}
// game.order(units, "move"|"attack"|"patrol"|"stop"|"hold", x, z) / game.order(units, "attack", target)
int gOrder(lua_State* L, Vm& vm) {
    CTX;
    std::vector<Entity> us;
    if (!unitsArg(L, vm, 1, us)) return -1;
    std::string kind;
    if (!nameArg(L, vm, 2, kind, "order")) return -1;
    Command c;
    c.tick = sim.tick();
    bool known = false;
    for (auto& [n, v] : kCommands) if (kind == n && v <= kCmdPatrol) { c.type = v; known = true; }
    if (!known) return fail(vm, "order: move, attack, patrol, stop or hold");
    Entity t;
    if (lua_isinteger(L, 3) && lua_isnoneornil(L, 4)) { if (!unitArg(L, vm, 3, t)) return 0; c.target = t; c.point = w.get<Transform>(t).pos; }
    else if (!lua_isnoneornil(L, 3) && (!argFx(L, vm, 3, c.point.x, "x") || !argFx(L, vm, 4, c.point.z, "z"))) return -1;
    std::sort(us.begin(), us.end(), [&](Entity a, Entity b) {
        uint8_t oa = w.get<Owner>(a).player, ob = w.get<Owner>(b).player;
        return oa != ob ? oa < ob : a.index < b.index;
    });
    for (size_t i = 0; i < us.size();) {   // one command per owner
        Command ci = c;
        ci.player = w.get<Owner>(us[i]).player;
        while (i < us.size() && w.get<Owner>(us[i]).player == ci.player) ci.units.push_back(us[i++]);
        if (ci.player >= 1 && ci.player != kHostileNeutral) m.applyCommand(sim, ci);
        else for (Entity e : ci.units) {   // neutral / creeps: orders directly
            if (!w.has<Orders>(e)) continue;
            Order o;
            o.type = c.type == kCmdAttack ? (c.target.valid() ? OrderType::Attack : OrderType::AttackMove) : c.type == kCmdPatrol ? OrderType::Patrol : OrderType::Move;
            o.target = c.target; o.point = flat(c.point);
            if (c.type == kCmdStop || c.type == kCmdHold) { w.get<Orders>(e).queue.clear(); m.stopMoving(sim, e); }
            else m.issue(sim, e, o, false);
        }
    }
    return 0;
}
// game.cast(unit, ability (index from 0 or id), target | x, z): orders the unit to use its ability
int gCast(lua_State* L, Vm& vm) {
    CTX;
    Entity e;
    if (!unitArg(L, vm, 1, e)) { lua_pushboolean(L, 0); return 1; }
    const UnitType& t = R.types[w.get<Unit>(e).type];
    int idx = -1;
    if (lua_type(L, 2) == LUA_TSTRING) {
        std::string id = lua_tostring(L, 2);
        for (size_t i = 0; i < t.abilities.size(); ++i) if (t.abilities[i].id == id) idx = int(i);
    } else { int64_t v; if (!argInt(L, vm, 2, v, "ability")) return -1; idx = int(v); }
    if (idx < 0 || size_t(idx) >= t.abilities.size() || !m.canCast(sim, e, idx)) { lua_pushboolean(L, 0); return 1; }
    Command c;
    c.tick = sim.tick(); c.player = w.get<Owner>(e).player; c.type = kCmdAbility; c.param = idx; c.units = {e};
    Entity tg;
    if (lua_isinteger(L, 3) && lua_isnoneornil(L, 4)) { if (unitArg(L, vm, 3, tg)) { c.target = tg; c.point = w.get<Transform>(tg).pos; } }
    else if (!lua_isnoneornil(L, 3) && (!argFx(L, vm, 3, c.point.x, "x") || !argFx(L, vm, 4, c.point.z, "z"))) return -1;
    if (c.player == 0 || c.player == kHostileNeutral) m.castAbility(sim, e, idx, c.target, c.point);
    else m.applyCommand(sim, c);
    lua_pushboolean(L, 1);
    return 1;
}
// game.player_ability(player, id, x, z [, free]): free casts skip cost / cooldown / loadout
int gPlayerAbility(lua_State* L, Vm& vm) {
    CTX;
    uint8_t p; if (!playerArg(L, vm, 1, p)) return -1;
    std::string id;
    if (!nameArg(L, vm, 2, id, "ability")) return -1;
    int k = R.findPlayerAbility(id);
    if (k < 0) return fail(vm, "unknown player ability '" + id + "'");
    Vec3 pt{};
    if (!argFx(L, vm, 3, pt.x, "x") || !argFx(L, vm, 4, pt.z, "z")) return -1;
    if (!lua_toboolean(L, 5)) { lua_pushboolean(L, m.castPlayerAbility(sim, p, k, pt, kNullEntity)); return 1; }
    EffectCtx ctx; ctx.owner = p; ctx.point = pt; ctx.origin = pt;
    sim.emit({kEvPlayerAbility, p, kNullEntity, kNullEntity, k, pt});
    m.applyPayload(sim, m.playerAbilityFor(sim, p, k).payload, ctx);
    lua_pushboolean(L, 1);
    return 1;
}
int gSetVar(lua_State* L, Vm& vm) {
    std::string n;
    Fixed v;
    if (!nameArg(L, vm, 1, n, "variable") || !argFx(L, vm, 2, v, "value")) return -1;
    vm.module->state(*vm.sim).trig.vars[n] = v;
    return 0;
}
int gAddVar(lua_State* L, Vm& vm) {
    std::string n;
    Fixed v;
    if (!nameArg(L, vm, 1, n, "variable") || !argFx(L, vm, 2, v, "value")) return -1;
    Fixed& x = vm.module->state(*vm.sim).trig.vars[n];
    x += v;
    pushFx(L, x);
    return 1;
}
// game.message(player (0 = everyone), text [, x, z])
int gMessage(lua_State* L, Vm& vm) {
    CTX;
    int64_t p;
    if (!argInt(L, vm, 1, p, "player")) return -1;
    size_t len = 0;
    const char* s = luaL_tolstring(L, 2, &len);   // strings / numbers / fx (__tostring)
    std::string text(s, len);
    lua_pop(L, 1);
    Vec3 at{};
    if (!lua_isnoneornil(L, 3) && (!argFx(L, vm, 3, at.x, "x") || !argFx(L, vm, 4, at.z, "z"))) return -1;
    GameState& gs = m.state(sim);
    auto it = std::find(gs.trig.messages.begin(), gs.trig.messages.end(), text);
    int64_t idx = it - gs.trig.messages.begin();
    if (it == gs.trig.messages.end()) gs.trig.messages.push_back(text);
    sim.emit({kEvMessage, uint8_t(p), kNullEntity, kNullEntity, idx, at});
    return 0;
}
int gVictory(lua_State* L, Vm& vm) {
    uint8_t p; if (!playerArg(L, vm, 1, p)) return -1;
    vm.module->state(*vm.sim).winnerTeam = vm.module->teamOf(*vm.sim, p);
    return 0;
}
int gDraw(lua_State*, Vm& vm) { vm.module->state(*vm.sim).winnerTeam = 0; return 0; }
int gDefeat(lua_State* L, Vm& vm) {
    uint8_t p; if (!playerArg(L, vm, 1, p)) return -1;
    if (auto* pi = vm.sim->player(p); pi && pi->alive) { pi->alive = false; vm.sim->emit({kEvPlayerDefeated, p, kNullEntity, kNullEntity, 0, {}}); }
    return 0;
}
int gResearch(lua_State* L, Vm& vm) {
    CTX;
    uint8_t p; if (!playerArg(L, vm, 1, p)) return -1;
    std::string id;
    if (!nameArg(L, vm, 2, id, "tech")) return -1;
    int k = R.findTech(id);
    if (k < 0) return fail(vm, "unknown tech '" + id + "'");
    m.state(sim).players[p].researched[size_t(k)] = 1;
    m.recomputeMods(sim, p);
    return 0;
}
int gEnableType(lua_State* L, Vm& vm) {
    CTX;
    uint8_t p; if (!playerArg(L, vm, 1, p)) return -1;
    int t = typeArg(L, vm, 2); if (t < 0) return -1;
    m.state(sim).players[p].typeDisabled[size_t(t)] = !lua_toboolean(L, 3);
    return 0;
}
// ---------------------------------------------------------------- direct unit control and state queries (G4)
const char* const kOrderNames[] = {"move", "attack_move", "attack", "patrol", "gather", "return", "build", "repair", "enter", "cast",
                                   "dig", "claim_cell", "need", "haul", "board", "unload", "follow", "rearm", "trade", "pick_item", "merge"};
void pushOrder(lua_State* L, const Order& o) {
    lua_createtable(L, 0, 6);
    const size_t k = size_t(o.type);
    setS(L, "order", k < std::size(kOrderNames) ? kOrderNames[k] : "other");
    setF(L, "x", o.point.x); setF(L, "z", o.point.z);
    if (o.target.valid()) { pushEntity(L, o.target); lua_setfield(L, -2, "target"); }
    setI(L, "param", o.param);
    setI(L, "state", o.state);
}
// game.orders(h): the order queue, current first: {order=, x=, z=, target=, param=, state=}
int gOrders(lua_State* L, Vm& vm) {
    CTX;
    Entity e;
    if (!unitArg(L, vm, 1, e)) { lua_pushnil(L); return 1; }
    auto* o = w.tryGet<Orders>(e);
    lua_createtable(L, o ? int(o->queue.size()) : 0, 0);
    if (o) for (size_t i = 0; i < o->queue.size(); ++i) { pushOrder(L, o->queue[i]); lua_rawseti(L, -2, lua_Integer(i + 1)); }
    return 1;
}
int gCurrentOrder(lua_State* L, Vm& vm) {
    CTX;
    Entity e;
    auto* o = unitArg(L, vm, 1, e) ? w.tryGet<Orders>(e) : nullptr;
    if (!o || o->queue.empty()) { lua_pushnil(L); return 1; }
    pushOrder(L, o->queue.front());
    return 1;
}
// game.target(h): what the unit is fighting (weapon target), else the current order's target
int gTarget(lua_State* L, Vm& vm) {
    CTX;
    Entity e;
    if (!unitArg(L, vm, 1, e)) { lua_pushnil(L); return 1; }
    Entity t;
    if (auto* c = w.tryGet<Combat>(e); c && w.alive(c->target)) t = c->target;
    else if (auto* o = w.tryGet<Orders>(e); o && !o->queue.empty() && w.alive(o->queue.front().target)) t = o->queue.front().target;
    if (t.valid()) pushEntity(L, t); else lua_pushnil(L);
    return 1;
}
// game.idle(h): no orders, not moving, not fighting, nothing in production
int gIdle(lua_State* L, Vm& vm) {
    CTX;
    Entity e;
    if (!unitArg(L, vm, 1, e)) { lua_pushnil(L); return 1; }
    bool busy = false;
    if (auto* o = w.tryGet<Orders>(e)) busy |= !o->queue.empty();
    if (auto* mb = w.tryGet<Mobile>(e)) busy |= mb->moving;
    if (auto* c = w.tryGet<Combat>(e)) busy |= w.alive(c->target);
    if (auto* p = w.tryGet<Production>(e)) busy |= !p->queue.empty();
    lua_pushboolean(L, !busy);
    return 1;
}
int gFacing(lua_State* L, Vm& vm) {
    Entity e;
    if (!unitArg(L, vm, 1, e)) { lua_pushnil(L); return 1; }
    const Vec3 f = vm.sim->world().get<Unit>(e).facing;
    pushFx(L, f.x); pushFx(L, f.z);
    return 2;
}
int gVelocity(lua_State* L, Vm& vm) {
    Entity e;
    if (!unitArg(L, vm, 1, e)) { lua_pushnil(L); return 1; }
    auto* mb = vm.sim->world().tryGet<Mobile>(e);
    pushFx(L, mb ? mb->vel.x : Fixed{}); pushFx(L, mb ? mb->vel.z : Fixed{});
    return 2;
}
// game.path(h): the remaining waypoints {{x=, z=}, ...} ending at the goal; empty when not moving
int gPath(lua_State* L, Vm& vm) {
    Entity e;
    if (!unitArg(L, vm, 1, e)) { lua_pushnil(L); return 1; }
    auto* mb = vm.sim->world().tryGet<Mobile>(e);
    lua_newtable(L);
    if (!mb || !mb->moving) return 1;
    int n = 0;
    auto add = [&](Vec3 p) { lua_createtable(L, 0, 2); setF(L, "x", p.x); setF(L, "z", p.z); lua_rawseti(L, -2, ++n); };
    for (size_t i = mb->pathPos; i < mb->path.size(); ++i) add(mb->path[i]);
    if (mb->path.empty() || mb->path.back().x != mb->goal.x || mb->path.back().z != mb->goal.z) add(mb->goal);
    return 1;
}
int gEnergy(lua_State* L, Vm& vm) {
    Entity e;
    if (!unitArg(L, vm, 1, e)) { lua_pushnil(L); return 1; }
    pushFx(L, vm.sim->world().get<Unit>(e).energy);
    pushFx(L, vm.module->rules().types[vm.sim->world().get<Unit>(e).type].maxEnergy);
    return 2;
}

// game.set_pos(h, x, z [, exact]): teleports a mobile unit (nearest passable cell unless exact or flying);
// structures don't move. False when it can't be placed.
int gSetPos(lua_State* L, Vm& vm) {
    CTX;
    Vec3 p{};
    if (!argFx(L, vm, 2, p.x, "x") || !argFx(L, vm, 3, p.z, "z")) return -1;
    Entity e;
    if (!unitArg(L, vm, 1, e) || !w.has<Mobile>(e)) { lua_pushboolean(L, 0); return 1; }
    GameState& gs = m.state(sim);
    const UnitType& ut = R.types[w.get<Unit>(e).type];
    int cx, cy;
    if (!gs.grid.cellXY(p, cx, cy)) { lua_pushboolean(L, 0); return 1; }
    if (!(ut.layer & kLayerAir) && !gs.grid.passable(cx, cy, ut.layer)) {
        if (lua_toboolean(L, 4) || !gs.grid.nearestPassable(cx, cy, ut.layer, 6)) { lua_pushboolean(L, 0); return 1; }
        p = gs.grid.cellCenter(cx, cy);
    }
    Transform& tr = w.get<Transform>(e);
    tr.pos = flat(p); tr.prevPos = tr.pos;
    m.stopMoving(sim, e);
    if (auto* lw = w.tryGet<LaneWalker>(e); lw && lw->lane >= 0 && size_t(lw->lane) < gs.lanes.size()) {
        const auto& pts = gs.lanes[size_t(lw->lane)].points;   // continue from the nearest point of the own lane
        Fixed best{}; uint16_t bi = lw->pos;
        for (size_t i = 0; i < pts.size(); ++i) {
            Fixed d = dist2D(pts[lw->reverse ? pts.size() - 1 - i : i], tr.pos);
            if (i == 0 || d < best) { best = d; bi = uint16_t(i); }
        }
        lw->pos = bi;
    }
    lua_pushboolean(L, 1);
    return 1;
}
// game.set_facing(h, dx, dz): direction (any length > 0)
int gSetFacing(lua_State* L, Vm& vm) {
    Vec3 d{};
    if (!argFx(L, vm, 2, d.x, "dx") || !argFx(L, vm, 3, d.z, "dz")) return -1;
    Entity e;
    Fixed len = d.length();
    if (!unitArg(L, vm, 1, e) || len.raw <= 0) { lua_pushboolean(L, 0); return 1; }
    vm.sim->world().get<Unit>(e).facing = d / len;
    lua_pushboolean(L, 1);
    return 1;
}
// Slide a mobile unit by `d` in 8 steps, stopping at the first impassable step (ground units).
bool pushUnit(Vm& vm, Entity e, Vec3 d) {
    CTX;
    if (!w.has<Mobile>(e)) return false;
    Transform& tr = w.get<Transform>(e);
    const UnitType& ut = R.types[w.get<Unit>(e).type];
    const MapGrid& g = m.state(sim).grid;
    for (int k = 0; k < 8; ++k) {
        Vec3 np = tr.pos + d * Fixed::fromRatio(1, 8);
        if (!(ut.layer & kLayerAir) && (!g.inside(g.cellX(np.x), g.cellY(np.z)) || !g.passableAt(np, ut.layer))) break;
        tr.pos = np;
    }
    m.stopMoving(sim, e);
    return true;
}
// game.impulse(h, dx, dz): pushes the unit by (dx, dz) world units, blocked by terrain
int gImpulse(lua_State* L, Vm& vm) {
    Vec3 d{};
    if (!argFx(L, vm, 2, d.x, "dx") || !argFx(L, vm, 3, d.z, "dz")) return -1;
    Entity e;
    lua_pushboolean(L, unitArg(L, vm, 1, e) && pushUnit(vm, e, d));
    return 1;
}
// game.knockback(h, x, z, distance): pushes the unit away from (x, z) (negative distance: towards it)
int gKnockback(lua_State* L, Vm& vm) {
    Vec3 c{};
    Fixed dist;
    if (!argFx(L, vm, 2, c.x, "x") || !argFx(L, vm, 3, c.z, "z") || !argFx(L, vm, 4, dist, "distance")) return -1;
    Entity e;
    if (!unitArg(L, vm, 1, e)) { lua_pushboolean(L, 0); return 1; }
    Vec3 d = flat(vm.sim->world().get<Transform>(e).pos - c);
    d.y = Fixed{};
    Fixed dl = d.length();
    if (dl.raw == 0) { d = {kOne, Fixed{}, Fixed{}}; dl = kOne; }
    if (dist.raw < 0) dist = -fxMin(-dist, dl);
    lua_pushboolean(L, pushUnit(vm, e, d / dl * dist));
    return 1;
}
// game.set_hp(h, hp): clamped to [0, max hp]; 0 kills the unit (normal death)
int gSetHp(lua_State* L, Vm& vm) {
    CTX;
    Fixed v;
    if (!argFx(L, vm, 2, v, "hp")) return -1;
    Entity e;
    if (!unitArg(L, vm, 1, e)) { lua_pushboolean(L, 0); return 1; }
    w.get<Unit>(e).hp = fxClamp(v, Fixed{}, m.stat(sim, e, Stat::MaxHp));
    lua_pushboolean(L, 1);
    return 1;
}
int gSetEnergy(lua_State* L, Vm& vm) {
    CTX;
    Fixed v;
    if (!argFx(L, vm, 2, v, "energy")) return -1;
    Entity e;
    if (!unitArg(L, vm, 1, e)) { lua_pushboolean(L, 0); return 1; }
    Unit& u = w.get<Unit>(e);
    const Fixed mx = R.types[u.type].maxEnergy;
    u.energy = fxClamp(v, Fixed{}, mx.raw > 0 ? mx : v);
    lua_pushboolean(L, 1);
    return 1;
}
// game.set_stat_base(h, stat, value | nil): replaces the type's base value of a stat for this unit (techs,
// levels and buffs still apply on top); nil restores the type's value
int gSetStatBase(lua_State* L, Vm& vm) {
    CTX;
    std::string s;
    if (!nameArg(L, vm, 2, s, "stat")) return -1;
    bool ok = false;
    Stat st = parseStat(s, &ok);
    if (!ok) return fail(vm, "unknown stat '" + s + "'");
    Fixed v;
    const bool clear = lua_isnoneornil(L, 3);
    if (!clear && !argFx(L, vm, 3, v, "value")) return -1;
    Entity e;
    if (!unitArg(L, vm, 1, e)) { lua_pushboolean(L, 0); return 1; }
    Unit& u = w.get<Unit>(e);
    const Fixed oldMax = m.stat(sim, e, Stat::MaxHp);
    auto* ss = w.tryGet<ScriptStats>(e);
    if (!ss && clear) { lua_pushboolean(L, 1); return 1; }
    if (!ss) ss = &w.add<ScriptStats>(e);
    auto it = std::find_if(ss->list.begin(), ss->list.end(), [&](const ScriptStats::Entry& x) { return x.stat == uint8_t(st); });
    if (clear) { if (it != ss->list.end()) ss->list.erase(it); }
    else if (it != ss->list.end()) it->base = v;
    else ss->list.push_back({uint8_t(st), v});
    if (ss->list.empty()) w.remove<ScriptStats>(e);
    if (st == Stat::MaxHp && oldMax.raw > 0 && u.hp.raw > 0)   // keep the hp fraction
        u.hp = fxMax(Fixed::fromRaw(1), u.hp * m.stat(sim, e, Stat::MaxHp) / oldMax);
    lua_pushboolean(L, 1);
    return 1;
}
// game.transform(h, type): changes the unit's type in place (same handle, data, orders; hp keeps its
// fraction); units stay units and structures stay structures
int gTransform(lua_State* L, Vm& vm) {
    CTX;
    int t = typeArg(L, vm, 2); if (t < 0) return -1;
    Entity e;
    if (!unitArg(L, vm, 1, e)) { lua_pushboolean(L, 0); return 1; }
    if (R.types[size_t(t)].structure != R.types[w.get<Unit>(e).type].structure) return fail(vm, "transform: a unit can't become a structure (or back)");
    game::detail::changeUnitType(m, sim, e, uint16_t(t));
    lua_pushboolean(L, 1);
    return 1;
}

int gAddScore(lua_State* L, Vm& vm) {
    CTX;
    uint8_t p; if (!playerArg(L, vm, 1, p)) return -1;
    Fixed a; if (!argFx(L, vm, 2, a, "amount")) return -1;
    GameState& gs = m.state(sim);
    uint8_t t = m.teamOf(sim, p);
    if (t < gs.score.size()) gs.score[t] += a;
    return 0;
}
int gClearData(lua_State* L, Vm& vm) {
    Entity e;
    if (unitArg(L, vm, 1, e)) vm.sim->world().remove<ScriptData>(e);
    return 0;
}
int gEnableTrigger(lua_State* L, Vm& vm) {
    CTX;
    std::string id;
    if (!nameArg(L, vm, 1, id, "trigger")) return -1;
    int k = m.triggers().findTrigger(id);
    if (k < 0) return fail(vm, "unknown trigger '" + id + "'");
    auto& en = m.state(sim).trig.enabled;
    if (size_t(k) < en.size()) en[size_t(k)] = lua_toboolean(L, 2);
    return 0;
}

} // namespace

void openGameApi(Vm& vm) {
    lua_State* L = vm.L;
    static const luaL_Reg queries[] = {
        {"tick", bindQ<gTick>}, {"time", bindQ<gTime>}, {"tick_rate", bindQ<gTickRate>}, {"players", bindQ<gPlayers>},
        {"player", bindQ<gPlayer>}, {"player_alive", bindQ<gAlivePlayer>}, {"team", bindQ<gTeam>}, {"hostile", bindQ<gHostile>},
        {"resource", bindQ<gResource>}, {"resources", bindQ<gResources>}, {"units", bindQ<gUnits>}, {"count", bindQ<gCount>},
        {"nearest", bindQ<gNearest>}, {"unit", bindQ<gUnit>}, {"alive", bindQ<gAlive>}, {"owner", bindQ<gOwner>},
        {"type_of", bindQ<gTypeOf>}, {"pos", bindQ<gPos>}, {"hp", bindQ<gHp>}, {"stat", bindQ<gStat>},
        {"has_status", bindQ<gHasStatus>}, {"type_info", bindQ<gTypeInfo>}, {"types", bindQ<gTypes>}, {"map_size", bindQ<gMapSize>},
        {"passable", bindQ<gPassable>}, {"level", bindQ<gLevel>}, {"height", bindQ<gHeight>}, {"area", bindQ<gArea>},
        {"var", bindQ<gVar>}, {"log", bindQ<gLog>}, {"data", bindQ<gData>}, {"has_data", bindQ<gHasData>},
        {"orders", bindQ<gOrders>}, {"current_order", bindQ<gCurrentOrder>}, {"target", bindQ<gTarget>}, {"idle", bindQ<gIdle>},
        {"facing", bindQ<gFacing>}, {"velocity", bindQ<gVelocity>}, {"path", bindQ<gPath>}, {"energy", bindQ<gEnergy>}, {nullptr, nullptr}};
    static const luaL_Reg actions[] = {
        {"give", bind<gGive>}, {"take", bind<gTake>}, {"spawn", bind<gSpawn>}, {"kill", bind<gKill>}, {"damage", bind<gDamage>},
        {"heal", bind<gHeal>}, {"set_owner", bind<gSetOwner>}, {"add_status", bind<gAddStatus>}, {"buff", bind<gBuff>},
        {"command", bind<gCommand>}, {"order", bind<gOrder>}, {"cast", bind<gCast>}, {"player_ability", bind<gPlayerAbility>},
        {"set_var", bind<gSetVar>}, {"add_var", bind<gAddVar>}, {"message", bind<gMessage>}, {"victory", bind<gVictory>},
        {"draw", bind<gDraw>}, {"defeat", bind<gDefeat>}, {"research", bind<gResearch>}, {"enable_type", bind<gEnableType>},
        {"add_score", bind<gAddScore>}, {"enable_trigger", bind<gEnableTrigger>}, {"clear_data", bind<gClearData>},
        {"set_pos", bind<gSetPos>}, {"teleport", bind<gSetPos>}, {"set_facing", bind<gSetFacing>}, {"impulse", bind<gImpulse>},
        {"knockback", bind<gKnockback>}, {"set_hp", bind<gSetHp>}, {"set_energy", bind<gSetEnergy>},
        {"set_stat_base", bind<gSetStatBase>}, {"transform", bind<gTransform>}, {nullptr, nullptr}};
    luaL_newlib(L, queries);
    if (vm.mode == Mode::Sim) {
        luaL_setfuncs(L, actions, 0);
        lua_getglobal(L, "math");
        lua_getfield(L, -1, "random");
        lua_setfield(L, -3, "random");
        lua_pop(L, 1);
    }
    lua_setglobal(L, "game");
}

} // namespace script

// ================================================================= reference

Json scriptApiReference() {
    auto fn = [](const char* sig, const char* doc) {
        Json j = Json::object();
        j.set("signature", Json::string(sig));
        j.set("doc", Json::string(doc));
        return j;
    };
    Json r = Json::object();
    r.set("overview", Json::string(
        "Lua 5.4 scripts in the game folder. Simulation scripts (rules/map \"scripts\": [\"scripts/x.lua\"]) run inside the deterministic "
        "tick: numbers are integers or fx (fixed point); float literals, '/' on plain numbers and float math raise errors - use "
        "fx(\"1.5\"), fx(3, 2), fx(a) / b, or // for integer division. Persistent data lives in the global table `state` "
        "(integers, fx, strings, booleans, nested tables with integer/string keys); globals are read-only after loading and "
        "file-level locals must not be changed by handlers. pairs() iterates in sorted key order. Entities are integer handles; "
        "dead handles give nil/false. Errors are logged with file:line and disable that handler. View scripts (\"viewScripts\") "
        "are presentation-only: floats allowed, read-only game queries, view.notice/effect/sound requests."));
    Json ev = Json::array();
    for (const char* e : {"on(\"start\", fn())", "on(\"tick\", fn(tick))", "every(seconds, fn(tick))", "on(\"unit_died\", fn(e)) e: unit, owner, unit_type, x, z, killer, killer_owner",
                          "on(\"spawned\"|\"built\"|\"trained\"|\"upgraded\", fn(e)) e: unit, owner, unit_type (trained: producer)",
                          "on(\"researched\", fn(e)) e: player, tech, producer", "on(\"captured\", fn(e)) e: unit, owner, old_owner, by",
                          "on(\"player_defeated\", fn(e)) e: player, surrendered", "on(\"ability\", fn(e)) e: caster, player, ability, index, target, x, z",
                          "on(\"damage\", fn(e)) e: unit, source, player (source owner), amount (fx)", "on(\"level_up\", fn(e)) e: unit, player, level",
                          "on_area(name, \"enter\"|\"leave\", fn(e)) e: unit, owner, area, x, z (checked every 5 ticks)",
                          "global functions named on_start, on_tick, on_unit_died, ... are registered too",
                          "game.after(seconds, \"global_fn\", arg): one-shot timer kept in the simulation state"})
        ev.push(Json::string(e));
    r.set("events", ev);
    Json q = Json::array();
    q.push(fn("game.tick() / game.time() / game.tick_rate()", "tick number / seconds as fx / ticks per second"));
    q.push(fn("game.players() / game.player(p) / game.player_alive(p) / game.team(p) / game.hostile(a, b)", "player slots; player(p) = {slot, team, alive, name, faction, supply, supply_cap}"));
    q.push(fn("game.resource(p, res) / game.resources(p)", "stock as fx / {res = fx}"));
    q.push(fn("game.units(filter) / game.count(filter) / game.nearest(x, z, filter)", "living units sorted by handle; filter = {owner, type, tag, kind=\"unit\"|\"structure\", area, x, z, radius, has (game.data key), sort=\"id\"|\"dist\", limit}; radius queries use a bucket grid"));
    q.push(fn("game.unit(h)", "{id, type, name, owner, x, y, z, hp, max_hp, energy, structure, under_construction, level, garrison, tags, statuses} or nil"));
    q.push(fn("game.alive(h) / game.owner(h) / game.type_of(h) / game.pos(h) -> x, z / game.hp(h) -> hp, max / game.stat(h, stat) / game.has_status(h, id)", "unit fields (nil for dead handles)"));
    q.push(fn("game.type_info(id) / game.types()", "{id, name, structure, max_hp, speed, armor, cost, tags} / all type ids"));
    q.push(fn("game.map_size() -> w, h / game.passable(x, z [, \"ground\"|\"air\"|\"naval\"]) / game.level(x, z) / game.height(x, z) / game.area(name)", "map queries; area = {x, z, w, h}"));
    q.push(fn("game.var(name)", "trigger variable (fx)"));
    q.push(fn("game.data(h) / game.has_data(h, key)", "the entity's own persistent Lua table (created on first use, saved, hashed, gone with the entity; view: read-only copy) / key test"));
    q.push(fn("game.orders(h) / game.current_order(h)", "order queue / first order: {order, x, z, target, param, state}"));
    q.push(fn("game.target(h) / game.idle(h)", "weapon target, else the order's target / no orders, not moving, not fighting, no production"));
    q.push(fn("game.facing(h) -> dx, dz / game.velocity(h) -> vx, vz / game.path(h) / game.energy(h) -> e, max", "movement state; path = remaining waypoints {{x, z}, ...}"));
    q.push(fn("game.log(...) / print(...)", "to the script log (editor Console)"));
    r.set("queries", q);
    Json a = Json::array();
    a.push(fn("game.give(p, res, amount) / game.take(p, res, amount) -> taken", "resources"));
    a.push(fn("game.spawn(type, owner, x, z) -> handle|nil", "create a unit / structure (owner 0 neutral, 255 hostile creeps)"));
    a.push(fn("game.kill(h [, silent]) / game.damage(h, amount [, source [, damageType]]) / game.heal(h, amount)", "unit changes"));
    a.push(fn("game.set_owner(h, p)", "ownership change (type counts, clears production / orders, fires captured)"));
    a.push(fn("game.set_pos(h, x, z [, exact]) / game.teleport(...)", "move a mobile unit (nearest passable cell unless exact / flying)"));
    a.push(fn("game.set_facing(h, dx, dz) / game.impulse(h, dx, dz) / game.knockback(h, x, z, distance)", "turn / push by a vector / away from a point (terrain stops it)"));
    a.push(fn("game.set_hp(h, hp) / game.set_energy(h, e)", "clamped to [0, max]; hp 0 kills"));
    a.push(fn("game.set_stat_base(h, stat, value | nil)", "per-unit base stat (techs, buffs apply on top); nil restores the type's"));
    a.push(fn("game.transform(h, type)", "change the type in place (same handle and data, hp fraction kept)"));
    a.push(fn("game.add_status(h, status [, seconds [, caster]]) / game.buff(h, stat, add, mul [, seconds])", "statuses (rules \"statuses\") and stat modifiers"));
    a.push(fn("game.order(units, \"move\"|\"attack\"|\"patrol\"|\"stop\"|\"hold\", x, z | target)", "orders (through the normal command path)"));
    a.push(fn("game.command(p, {type, units, x, z, target, param, queue})", "any player command: move attack stop hold patrol gather build train cancel rally smart send research place sell deploy ability reinforce claim auto_build upgrade surrender player_ability dig paint_room pick_up drop slap possess direct build_road connect_road transit stance retreat load unload guard follow formation build_line merge split trade trade_route tribute diplomacy buy_item use_item drop_item pick_item learn bounty rotate lock end_turn; param may be a type / tech / player ability / stance / formation name; packed params by field: w h extra, end_x end_z, resource sell to amount other ally reward"));
    a.push(fn("game.cast(h, ability, target | x, z) / game.player_ability(p, id, x, z [, free])", "abilities"));
    a.push(fn("game.set_var(name, v) / game.add_var(name, v)", "trigger variables (shared with rules triggers)"));
    a.push(fn("game.message(p, text [, x, z])", "notice for player p (0 = everyone)"));
    a.push(fn("game.victory(p) / game.defeat(p) / game.draw()", "end conditions"));
    a.push(fn("game.clear_data(h)", "forget the entity's game.data table"));
    a.push(fn("game.research(p, tech) / game.enable_type(p, type, on) / game.add_score(p, amount) / game.enable_trigger(id, on)", "rules state"));
    a.push(fn("game.random(n) / game.random(a, b) / math.random()", "simulation RNG: integers, or fx in [0,1)"));
    r.set("actions", a);
    Json n = Json::array();
    n.push(fn("fx(x) / fx(num, den) / fx(\"1.25\")", "fixed-point number (Q47.16); + - * / // % comparisons and .. with integers and fx"));
    n.push(fn("fx.sqrt fx.abs fx.min fx.max fx.lerp(a, b, t)", "fx math"));
    n.push(fn("fx.floor(v) fx.ceil(v) fx.round(v) fx.int(v) / v:floor() ...", "to integers"));
    n.push(fn("fx.is(v) / math.type(v) == \"fx\" / fx.num(v) (view only)", "type checks / float for presentation"));
    n.push(fn("NOTE", "fx(1) == 1 is false (Lua compares different types as unequal): compare with fx(1) == fx(1), or use < <= >= >"));
    r.set("numbers", n);
    Json d = Json::array();
    d.push(fn("ability effect {\"type\": \"script\", \"fn\": \"name\", ...}", "fn(ctx): ctx = {caster, owner, target, x, z, scale, source_type, targets, args}; runs at the end of the tick"));
    d.push(fn("trigger action {\"type\": \"script\", \"fn\": \"name\", ...}", "fn(ctx): ctx = {player, unit, x, z, tick, trigger, args}"));
    d.push(fn("trigger condition {\"type\": \"script\", \"fn\": \"name\", ...}", "fn(ctx) -> boolean"));
    d.push(fn("rules \"scriptBudget\": 1000000", "instructions per tick for all simulation scripts"));
    r.set("data", d);
    Json v = Json::array();
    v.push(fn("view.on(event, fn(e)) / view.on_frame(fn(dt))", "presentation events (same names as on()) / every frame"));
    v.push(fn("view.notice(text) / view.effect(name, x, z) / view.sound(name, x, z)", "requests to the host (UI notice, particle effect, sound)"));
    v.push(fn("view.player()", "the local player slot"));
    r.set("view", v);
    return r;
}

} // namespace rts::game
