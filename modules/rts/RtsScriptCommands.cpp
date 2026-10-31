// Script-defined player commands (G1): rules "commands" -> ScriptCommandDef, the Lua <-> wire encoding
// of their arguments (view.send, tests, MCP) and the validation the simulation applies to every
// kCmdScript before the game's handler sees it. The handler call itself is in RtsScriptEngine.cpp.
// Reference: docs/scripting.md "Script commands".
#include "RtsScriptInternal.h"
#include <algorithm>
#include "RtsInternal.h"

namespace rts::game {
using namespace script;

namespace {
bool parseKind(const std::string& t, ScriptArgDef::Kind& k) {
    static const std::pair<const char*, ScriptArgDef::Kind> kinds[] = {
        {"int", ScriptArgDef::Int}, {"enum", ScriptArgDef::Enum}, {"bool", ScriptArgDef::Bool},
        {"point", ScriptArgDef::Point}, {"unit", ScriptArgDef::Unit}, {"entity", ScriptArgDef::Entity}};
    for (auto& [n, v] : kinds) if (t == n) { k = v; return true; }
    return false;
}
} // namespace

std::vector<ScriptCommandDef> parseScriptCommands(const Json& rules, const Rules& R, std::vector<std::string>* errors) {
    std::vector<ScriptCommandDef> out;
    const Json& cs = rules["commands"];
    if (!cs.isObject()) return out;
    auto err = [&](const std::string& m) { if (errors) errors->push_back(m); };
    for (auto& [id, j] : cs.members()) {
        if (!j.isObject()) { err("commands." + id + ": object expected"); continue; }
        ScriptCommandDef d;
        d.id = id;
        d.label = j.getStr("label", id);
        d.icon = j.getStr("icon");
        d.hotkey = j.getStr("hotkey");
        d.tip = j.getStr("tip");
        d.fn = j.getStr("fn", "on_command");
        d.cooldown = j.getFx("cooldown");
        for (auto& [res, v] : j["cost"].members()) {
            int r = R.findResource(res);
            if (r < 0) { err("commands." + id + ".cost: unknown resource '" + res + "'"); continue; }
            d.cost.push_back({uint16_t(r), v.fx()});
        }
        const Json& tg = j["targets"];
        std::vector<std::string> names;
        if (tg.isString()) names.push_back(tg.str());
        for (auto& t : tg.items()) names.push_back(t.str());
        for (auto& n : names) {
            if (n == "any") { d.needUnits = true; continue; }
            if (n.rfind("tag:", 0) == 0) {
                uint64_t m = R.tagMask(n.substr(4));
                if (!m) err("commands." + id + ".targets: unknown tag '" + n.substr(4) + "'");
                d.tags |= m; d.needUnits = true;
                continue;
            }
            int t = R.findType(n);
            if (t < 0) { err("commands." + id + ".targets: unknown type '" + n + "'"); continue; }
            d.types.push_back(uint16_t(t)); d.needUnits = true;
        }
        // args: [{name, type, ...}] or {name: {type, ...}} (file order)
        std::vector<std::pair<std::string, const Json*>> as;
        if (j["args"].isArray()) for (auto& a : j["args"].items()) as.push_back({a.getStr("name"), &a});
        else for (auto& [n, a] : j["args"].members()) as.push_back({n, &a});
        for (auto& [n, a] : as) {
            ScriptArgDef ad;
            ad.name = n;
            if (n.empty() || n == "units") { err("commands." + id + ": argument without a name (or named 'units')"); continue; }
            if (!parseKind(a->getStr("type", "int"), ad.kind)) { err("commands." + id + "." + n + ": type int|enum|bool|point|unit|entity"); continue; }
            ad.min = (*a)["min"].integer(ad.min);
            ad.max = (*a)["max"].integer(ad.max);
            for (auto& v : (*a)["values"].items()) ad.values.push_back(v.str());
            if (ad.kind == ScriptArgDef::Enum && ad.values.empty()) { err("commands." + id + "." + n + ": enum without values"); continue; }
            const Json& dv = (*a)["default"];
            if (ad.kind == ScriptArgDef::Enum && dv.isString()) {
                auto it = std::find(ad.values.begin(), ad.values.end(), dv.str());
                ad.def = it == ad.values.end() ? 0 : it - ad.values.begin();
            } else if (ad.kind == ScriptArgDef::Bool) ad.def = dv.boolean(false) ? 1 : 0;
            else ad.def = std::clamp<int64_t>(dv.integer(0), ad.min, ad.max);
            ad.own = a->getBool("own");
            if (d.pick < 0 && (ad.kind == ScriptArgDef::Point || ad.kind == ScriptArgDef::Unit || ad.kind == ScriptArgDef::Entity))
                d.pick = int(d.args.size());
            d.args.push_back(std::move(ad));
        }
        out.push_back(std::move(d));
    }
    std::sort(out.begin(), out.end(), [](const ScriptCommandDef& a, const ScriptCommandDef& b) { return a.id < b.id; });
    return out;
}

namespace script {

int argSlots(const ScriptCommandDef& d) {
    int n = 0;
    for (auto& a : d.args) n += a.slots();
    return n;
}

// Lua args table (at idx) -> cmd.args; missing scalars take their defaults, missing points / units are
// an error. Used by view.send and game tests; the simulation validates again (validateScriptCommand).
bool encodeScriptArgs(lua_State* L, Vm& vm, int idx, const ScriptCommandDef& d, Command& c) {
    idx = lua_absindex(L, idx);
    c.args.clear();
    const int top = lua_gettop(L);
    auto field = [&](const char* k) { lua_settop(L, top); if (!lua_istable(L, idx)) return LUA_TNIL; lua_pushstring(L, k); return lua_rawget(L, idx); };
    for (auto& a : d.args) {
        const int t = field(a.name.c_str());
        int64_t v = a.def;
        switch (a.kind) {
        case ScriptArgDef::Int:
            if (t != LUA_TNIL && !argInt(L, vm, -1, v, a.name.c_str())) { lua_settop(L, top); return false; }
            c.args.push_back(v);
            break;
        case ScriptArgDef::Enum:
            if (t == LUA_TSTRING) {
                auto it = std::find(a.values.begin(), a.values.end(), std::string(lua_tostring(L, -1)));
                if (it == a.values.end()) { vm.err = d.id + "." + a.name + ": unknown value '" + lua_tostring(L, -1) + "'"; lua_settop(L, top); return false; }
                v = it - a.values.begin();
            } else if (t != LUA_TNIL && !argInt(L, vm, -1, v, a.name.c_str())) { lua_settop(L, top); return false; }
            c.args.push_back(v);
            break;
        case ScriptArgDef::Bool:
            c.args.push_back(t == LUA_TNIL ? a.def : lua_toboolean(L, -1) ? 1 : 0);
            break;
        case ScriptArgDef::Point: {
            Fixed x, z;
            if (t != LUA_TTABLE) { vm.err = d.id + "." + a.name + ": {x =, z =} expected"; lua_settop(L, top); return false; }
            const int pt = lua_gettop(L);
            lua_pushstring(L, "x"); lua_rawget(L, pt);
            lua_pushstring(L, "z"); lua_rawget(L, pt);
            if (!argFx(L, vm, -2, x, "x") || !argFx(L, vm, -1, z, "z")) { lua_settop(L, top); return false; }
            c.args.push_back(x.raw); c.args.push_back(z.raw);
            break;
        }
        case ScriptArgDef::Unit: case ScriptArgDef::Entity:
            if (t != LUA_TNUMBER || !lua_isinteger(L, -1)) { vm.err = d.id + "." + a.name + ": unit handle expected"; lua_settop(L, top); return false; }
            c.args.push_back(lua_tointeger(L, -1));
            break;
        }
    }
    // the selection: args.units = {h, ...}
    if (field("units") == LUA_TTABLE) {
        const lua_Integer n = lua_Integer(lua_rawlen(L, -1));
        for (lua_Integer i = 1; i <= n; ++i) {
            lua_rawgeti(L, -1, i);
            if (lua_isinteger(L, -1)) c.units.push_back(Entity::fromPacked(uint64_t(lua_tointeger(L, -1))));
            lua_pop(L, 1);
        }
    }
    lua_settop(L, top);
    return true;
}

// Checks a received kCmdScript against its definition: argument count / ranges / living handles; keeps
// only the issuing player's living selected units the command accepts. False: drop the command.
bool validateScriptCommand(RtsModule& m, Simulation& sim, const ScriptCommandDef& d, Command& c) {
    World& w = sim.world();
    const Rules& R = m.rules();
    if (int(c.args.size()) != argSlots(d)) return false;
    size_t k = 0;
    for (auto& a : d.args) {
        const int64_t v = c.args[k];
        switch (a.kind) {
        case ScriptArgDef::Int: if (v < a.min || v > a.max) return false; break;
        case ScriptArgDef::Enum: if (v < 0 || size_t(v) >= a.values.size()) return false; break;
        case ScriptArgDef::Bool: if (v != 0 && v != 1) return false; break;
        case ScriptArgDef::Point: {
            int cx, cy;
            if (!m.state(sim).grid.cellXY(Vec3{Fixed::fromRaw(v), Fixed{}, Fixed::fromRaw(c.args[k + 1])}, cx, cy)) return false;
            break;
        }
        case ScriptArgDef::Unit: case ScriptArgDef::Entity: {
            Entity e = Entity::fromPacked(uint64_t(v));
            if (!w.alive(e) || !w.has<Transform>(e)) return false;
            if (a.kind == ScriptArgDef::Unit && (!w.has<Unit>(e) || w.get<Unit>(e).hp.raw <= 0)) return false;
            if (a.own && (!w.has<Owner>(e) || w.get<Owner>(e).player != c.player)) return false;
            break;
        }
        }
        k += size_t(a.slots());
    }
    std::vector<Entity> keep;
    for (Entity e : c.units) {
        if (!w.alive(e) || !w.has<Unit>(e) || !w.has<Owner>(e) || w.get<Owner>(e).player != c.player || w.get<Unit>(e).hp.raw <= 0) continue;
        if (std::find(keep.begin(), keep.end(), e) != keep.end()) continue;
        const uint16_t t = w.get<Unit>(e).type;
        const bool typeOk = std::find(d.types.begin(), d.types.end(), t) != d.types.end() || (d.tags && (R.types[t].tags & d.tags));
        if ((!d.types.empty() || d.tags) && !typeOk) continue;   // "targets" lists types / tags
        keep.push_back(e);
    }
    std::sort(keep.begin(), keep.end(), [](Entity a, Entity b) { return a.index < b.index; });
    c.units.swap(keep);
    return !d.needUnits || !c.units.empty();
}

// cmd.args -> the handler's table {units = {...}, <arg> = value} (points {x, z}, enums by name)
void pushScriptArgs(lua_State* L, const ScriptCommandDef& d, const Command& c) {
    lua_createtable(L, 0, int(d.args.size()) + 1);
    size_t k = 0;
    for (auto& a : d.args) {
        const int64_t v = c.args[k];
        switch (a.kind) {
        case ScriptArgDef::Int: lua_pushinteger(L, v); break;
        case ScriptArgDef::Enum: lua_pushstring(L, a.values[size_t(v)].c_str()); break;
        case ScriptArgDef::Bool: lua_pushboolean(L, v != 0); break;
        case ScriptArgDef::Point:
            lua_createtable(L, 0, 2);
            pushFx(L, Fixed::fromRaw(v)); lua_setfield(L, -2, "x");
            pushFx(L, Fixed::fromRaw(c.args[k + 1])); lua_setfield(L, -2, "z");
            break;
        case ScriptArgDef::Unit: case ScriptArgDef::Entity: lua_pushinteger(L, v); break;
        }
        lua_setfield(L, -2, a.name.c_str());
        k += size_t(a.slots());
    }
    lua_createtable(L, int(c.units.size()), 0);
    for (size_t i = 0; i < c.units.size(); ++i) { lua_pushinteger(L, lua_Integer(c.units[i].packed())); lua_rawseti(L, -2, lua_Integer(i + 1)); }
    lua_setfield(L, -2, "units");
}

} // namespace script
} // namespace rts::game
