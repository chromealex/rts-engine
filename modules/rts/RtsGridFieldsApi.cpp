// The `field` table of the scripting API (grid fields, RtsGridFields.h). Queries in simulation and view
// scripts, writes in simulation scripts only. Values are integers ("int" fields) or fx ("fixed" fields);
// positions are world coordinates (x, z), *_cell variants take field cells. Per-player fields take the player
// slot as the last argument (queries without it return the highest value of any player).
// Reference: docs/fields.md (keep scriptApiReference() in RtsScriptApi.cpp in sync).
#include "RtsScriptInternal.h"
#include <algorithm>
#include "RtsInternal.h"

namespace rts::game::script {

namespace {

struct FieldArg { GridFields* gf = nullptr; int index = -1; const GridFieldDef* def = nullptr; };

bool fieldArg(lua_State* L, Vm& vm, int idx, FieldArg& out) {
    GridFields* gf = vm.module ? vm.module->gridFields() : nullptr;
    if (lua_type(L, idx) != LUA_TSTRING) { vm.err = "field id (string) expected"; return false; }
    const std::string id = lua_tostring(L, idx);
    const int i = gf ? gf->find(id) : -1;
    if (i < 0) { vm.err = "unknown field '" + id + "' (rules \"fields\" / map \"gridFields\")"; return false; }
    out.gf = gf; out.index = i; out.def = &gf->defs()[size_t(i)];
    return true;
}
void pushValue(lua_State* L, const GridFieldDef& d, int64_t v) {
    if (d.fixed) pushFx(L, Fixed::fromRaw(v));
    else lua_pushinteger(L, lua_Integer(v));
}
bool valueArg(lua_State* L, Vm& vm, int idx, const GridFieldDef& d, int32_t& out) {
    Fixed f;
    if (!argFx(L, vm, idx, f, "value")) return false;
    out = d.raw(f);
    return true;
}
// layer from an optional player argument: -2 = not given (queries: max of all players), -1 invalid
bool layerArg(lua_State* L, Vm& vm, int idx, const FieldArg& fa, int& layer, bool write) {
    if (!fa.def->perPlayer) { layer = 0; return true; }
    if (lua_isnoneornil(L, idx)) {
        if (write) { vm.err = "field '" + fa.def->id + "' is per player: pass the player slot"; return false; }
        layer = -2;
        return true;
    }
    int64_t p;
    if (!argInt(L, vm, idx, p, "player")) return false;
    if (p < 1 || p >= fa.gf->layers(fa.index)) { vm.err = "player " + std::to_string(p) + " has no layer in field '" + fa.def->id + "'"; return false; }
    layer = int(p);
    return true;
}
int32_t valueAt(Vm& vm, const FieldArg& fa, int cx, int cz, int layer) {
    if (layer >= 0) return fa.gf->get(*vm.sim, fa.index, cx, cz, layer);
    int32_t best = INT32_MIN;
    for (int l = 1; l < fa.gf->layers(fa.index); ++l) best = std::max(best, fa.gf->get(*vm.sim, fa.index, cx, cz, l));
    return best == INT32_MIN ? fa.def->def : best;
}
bool posArg(lua_State* L, Vm& vm, int idx, const FieldArg& fa, int& cx, int& cz) {
    Fixed x, z;
    if (!argFx(L, vm, idx, x, "x") || !argFx(L, vm, idx + 1, z, "z")) return false;
    if (!fa.gf->cellOf(fa.index, x, z, cx, cz)) { cx = -1; cz = -1; }
    return true;
}
bool cellArg(lua_State* L, Vm& vm, int idx, int& cx, int& cz) {
    int64_t a, b;
    if (!argInt(L, vm, idx, a, "cx") || !argInt(L, vm, idx + 1, b, "cz")) return false;
    cx = int(std::clamp<int64_t>(a, -1, 1 << 20)); cz = int(std::clamp<int64_t>(b, -1, 1 << 20));
    return true;
}
// world rect (x0, z0, x1, z1) -> clamped field cell rect; false: empty
bool rectArg(lua_State* L, Vm& vm, int idx, const FieldArg& fa, int& x0, int& z0, int& x1, int& z1, bool& ok) {
    Fixed a, b, c, d;
    ok = false;
    if (!argFx(L, vm, idx, a, "x0") || !argFx(L, vm, idx + 1, b, "z0") || !argFx(L, vm, idx + 2, c, "x1") || !argFx(L, vm, idx + 3, d, "z1")) return false;
    const int res = fa.def->resolution;
    const GridFieldStore* st = fa.gf->store(*vm.sim);
    const auto& f = st->fields[size_t(fa.index)];
    x0 = int(std::max<int64_t>(0, fxMin(a, c).floorInt() / res)); x1 = int(std::min<int64_t>(f.w - 1, fxMax(a, c).floorInt() / res));
    z0 = int(std::max<int64_t>(0, fxMin(b, d).floorInt() / res)); z1 = int(std::min<int64_t>(f.h - 1, fxMax(b, d).floorInt() / res));
    ok = x0 <= x1 && z0 <= z1 && fxMax(a, c).raw >= 0 && fxMax(b, d).raw >= 0;
    return true;
}
int modeArg(lua_State* L, Vm& vm, int idx) {
    if (lua_isnoneornil(L, idx)) return 0;
    const char* s = lua_tostring(L, idx);
    const std::string m = s ? s : "";
    if (m == "set") return 0;
    if (m == "add") return 1;
    if (m == "max") return 2;
    vm.err = "mode: \"set\", \"add\" or \"max\"";
    return -1;
}

// ---- queries
int fIds(lua_State* L, Vm& vm) {
    GridFields* gf = vm.module ? vm.module->gridFields() : nullptr;
    lua_newtable(L);
    if (gf) for (size_t i = 0; i < gf->defs().size(); ++i) { lua_pushstring(L, gf->defs()[i].id.c_str()); lua_rawseti(L, -2, lua_Integer(i + 1)); }
    return 1;
}
int fInfo(lua_State* L, Vm& vm) {
    FieldArg fa; if (!fieldArg(L, vm, 1, fa)) return -1;
    const auto& f = fa.gf->store(*vm.sim)->fields[size_t(fa.index)];
    lua_createtable(L, 0, 10);
    lua_pushstring(L, fa.def->id.c_str()); lua_setfield(L, -2, "id");
    lua_pushstring(L, fa.def->fixed ? "fixed" : "int"); lua_setfield(L, -2, "type");
    lua_pushinteger(L, f.w); lua_setfield(L, -2, "w");
    lua_pushinteger(L, f.h); lua_setfield(L, -2, "h");
    lua_pushinteger(L, fa.def->resolution); lua_setfield(L, -2, "resolution");
    pushValue(L, *fa.def, fa.def->def); lua_setfield(L, -2, "default");
    pushValue(L, *fa.def, fa.def->min); lua_setfield(L, -2, "min");
    pushValue(L, *fa.def, fa.def->max); lua_setfield(L, -2, "max");
    lua_pushboolean(L, fa.def->perPlayer); lua_setfield(L, -2, "per_player");
    lua_pushinteger(L, f.layers); lua_setfield(L, -2, "layers");
    return 1;
}
int fGet(lua_State* L, Vm& vm) {
    FieldArg fa; int cx, cz, layer;
    if (!fieldArg(L, vm, 1, fa) || !posArg(L, vm, 2, fa, cx, cz) || !layerArg(L, vm, 4, fa, layer, false)) return -1;
    pushValue(L, *fa.def, valueAt(vm, fa, cx, cz, layer));
    return 1;
}
int fGetCell(lua_State* L, Vm& vm) {
    FieldArg fa; int cx, cz, layer;
    if (!fieldArg(L, vm, 1, fa) || !cellArg(L, vm, 2, cx, cz) || !layerArg(L, vm, 4, fa, layer, false)) return -1;
    pushValue(L, *fa.def, valueAt(vm, fa, cx, cz, layer));
    return 1;
}
int fCell(lua_State* L, Vm& vm) {
    FieldArg fa; int cx, cz;
    if (!fieldArg(L, vm, 1, fa) || !posArg(L, vm, 2, fa, cx, cz)) return -1;
    if (cx < 0) { lua_pushnil(L); return 1; }
    lua_pushinteger(L, cx); lua_pushinteger(L, cz);
    return 2;
}
int fCenter(lua_State* L, Vm& vm) {
    FieldArg fa; int cx, cz;
    if (!fieldArg(L, vm, 1, fa) || !cellArg(L, vm, 2, cx, cz)) return -1;
    const Vec3 c = fa.gf->centerOf(fa.index, cx, cz);
    pushFx(L, c.x); pushFx(L, c.z);
    return 2;
}
// sum / max / count over a world rect
int aggregate(lua_State* L, Vm& vm, int kind) {
    FieldArg fa; int x0, z0, x1, z1, layer; bool ok;
    if (!fieldArg(L, vm, 1, fa) || !rectArg(L, vm, 2, fa, x0, z0, x1, z1, ok)) return -1;
    int32_t above = 0;
    const int layerIdx = kind == 2 ? 7 : 6;
    if (kind == 2 && !valueArg(L, vm, 6, *fa.def, above)) return -1;
    if (!layerArg(L, vm, layerIdx, fa, layer, false)) return -1;
    int64_t sum = 0, n = 0; int32_t best = INT32_MIN; int bx = -1, bz = -1;
    if (ok)
        for (int z = z0; z <= z1; ++z)
            for (int x = x0; x <= x1; ++x) {
                const int32_t v = valueAt(vm, fa, x, z, layer);
                sum += v;
                if (v >= above) ++n;
                if (v > best) { best = v; bx = x; bz = z; }
            }
    if (kind == 0) { pushValue(L, *fa.def, sum); return 1; }
    if (kind == 2) { lua_pushinteger(L, n); return 1; }
    if (bx < 0) { lua_pushnil(L); return 1; }
    pushValue(L, *fa.def, best);
    const Vec3 c = fa.gf->centerOf(fa.index, bx, bz);
    pushFx(L, c.x); pushFx(L, c.z);
    return 3;
}
int fSum(lua_State* L, Vm& vm) { return aggregate(L, vm, 0); }
int fMax(lua_State* L, Vm& vm) { return aggregate(L, vm, 1); }
int fCount(lua_State* L, Vm& vm) { return aggregate(L, vm, 2); }

// field.find(id, pred [, opts]): pred = {above, below} or function(v, x, z) -> bool; opts / pred table also:
// x, z, radius (search disc, nearest rows first), limit (default 64, at most 4096), player.
// Returns {{x =, z =, v =, cx =, cz =}, ...} in row-major cell order; at most 65536 cells are examined.
int fFind(lua_State* L, Vm& vm) {
    FieldArg fa;
    if (!fieldArg(L, vm, 1, fa)) return -1;
    const bool fn = lua_isfunction(L, 2);
    const int opt = fn ? 3 : 2;
    if (!fn && !lua_istable(L, 2)) return fail(vm, "find: predicate table {above, below} or function(v, x, z) expected");
    if (!lua_isnoneornil(L, opt) && !lua_istable(L, opt)) return fail(vm, "find: options table expected");
    const int top = lua_gettop(L);
    auto field = [&](const char* k) { if (!lua_istable(L, opt)) { lua_pushnil(L); return LUA_TNIL; } lua_pushstring(L, k); return lua_rawget(L, opt); };
    bool hasAbove = false, hasBelow = false, circle = false;
    int32_t above = 0, below = 0; Fixed cx{}, cz{}, radius{}; int64_t limit = 64; int layer = fa.def->perPlayer ? -2 : 0;
    if (field("above") != LUA_TNIL) { hasAbove = true; if (!valueArg(L, vm, -1, *fa.def, above)) return -1; }
    lua_settop(L, top);
    if (field("below") != LUA_TNIL) { hasBelow = true; if (!valueArg(L, vm, -1, *fa.def, below)) return -1; }
    lua_settop(L, top);
    if (field("radius") != LUA_TNIL) {
        circle = true;
        if (!argFx(L, vm, -1, radius, "find.radius")) return -1;
        lua_settop(L, top); field("x"); if (!argFx(L, vm, -1, cx, "find.x")) return -1;
        lua_settop(L, top); field("z"); if (!argFx(L, vm, -1, cz, "find.z")) return -1;
    }
    lua_settop(L, top);
    if (field("limit") != LUA_TNIL && !argInt(L, vm, -1, limit, "find.limit")) return -1;
    lua_settop(L, top);
    if (field("player") != LUA_TNIL && !layerArg(L, vm, lua_gettop(L), fa, layer, false)) return -1;
    lua_settop(L, top);
    limit = std::clamp<int64_t>(limit, 1, 4096);
    const auto& f = fa.gf->store(*vm.sim)->fields[size_t(fa.index)];
    const int res = fa.def->resolution;
    int x0 = 0, z0 = 0, x1 = f.w - 1, z1 = f.h - 1;
    if (circle) {
        x0 = std::max(0, int((cx - radius).floorInt()) / res); x1 = std::min(f.w - 1, int((cx + radius).floorInt()) / res);
        z0 = std::max(0, int((cz - radius).floorInt()) / res); z1 = std::min(f.h - 1, int((cz + radius).floorInt()) / res);
    }
    lua_newtable(L);
    const int out = lua_gettop(L);
    int64_t found = 0, examined = 0;
    const Fixed r2 = radius * radius;
    for (int z = z0; z <= z1 && found < limit && examined < 65536; ++z)
        for (int x = x0; x <= x1 && found < limit && examined < 65536; ++x) {
            const Vec3 c = fa.gf->centerOf(fa.index, x, z);
            if (circle && (c.x - cx) * (c.x - cx) + (c.z - cz) * (c.z - cz) > r2) continue;
            ++examined;
            const int32_t v = valueAt(vm, fa, x, z, layer);
            if (hasAbove && v < above) continue;
            if (hasBelow && v >= below) continue;
            if (fn) {
                if (!hasAbove && !hasBelow && v == fa.def->def) continue;   // functions see non-default cells only
                lua_pushvalue(L, 2);
                pushValue(L, *fa.def, v); pushFx(L, c.x); pushFx(L, c.z);
                std::string err;
                if (!protectedCall(vm, 3, 1, &err)) return fail(vm, "find predicate: " + err);
                const bool keep = lua_toboolean(L, -1);
                lua_pop(L, 1);
                if (!keep) continue;
            }
            lua_createtable(L, 0, 5);
            pushFx(L, c.x); lua_setfield(L, -2, "x");
            pushFx(L, c.z); lua_setfield(L, -2, "z");
            pushValue(L, *fa.def, v); lua_setfield(L, -2, "v");
            lua_pushinteger(L, x); lua_setfield(L, -2, "cx");
            lua_pushinteger(L, z); lua_setfield(L, -2, "cz");
            lua_rawseti(L, out, ++found);
        }
    return 1;
}

// ---- writes (simulation scripts)
int writeOne(lua_State* L, Vm& vm, bool cell, int mode) {
    FieldArg fa; int cx, cz, layer; int32_t v;
    if (!fieldArg(L, vm, 1, fa)) return -1;
    if (cell ? !cellArg(L, vm, 2, cx, cz) : !posArg(L, vm, 2, fa, cx, cz)) return -1;
    if (!valueArg(L, vm, 4, *fa.def, v) || !layerArg(L, vm, 5, fa, layer, true)) return -1;
    lua_pushboolean(L, cx >= 0 && fa.gf->write(*vm.sim, fa.index, cx, cz, layer, v, mode));
    return 1;
}
int fSet(lua_State* L, Vm& vm) { return writeOne(L, vm, false, 0); }
int fAdd(lua_State* L, Vm& vm) { return writeOne(L, vm, false, 1); }
int fSetCell(lua_State* L, Vm& vm) { return writeOne(L, vm, true, 0); }
int fAddCell(lua_State* L, Vm& vm) { return writeOne(L, vm, true, 1); }
// field.fill_rect(id, x0, z0, x1, z1, v [, mode [, player]]) -> cells changed
int fFillRect(lua_State* L, Vm& vm) {
    FieldArg fa; int x0, z0, x1, z1, layer; bool ok; int32_t v;
    if (!fieldArg(L, vm, 1, fa) || !rectArg(L, vm, 2, fa, x0, z0, x1, z1, ok) || !valueArg(L, vm, 6, *fa.def, v)) return -1;
    const int mode = modeArg(L, vm, 7);
    if (mode < 0 || !layerArg(L, vm, 8, fa, layer, true)) return -1;
    lua_pushinteger(L, ok ? fa.gf->fillRect(*vm.sim, fa.index, x0, z0, x1, z1, layer, v, mode) : 0);
    return 1;
}
// field.fill_circle(id, x, z, radius, v [, mode [, player]]) -> cells changed
int fFillCircle(lua_State* L, Vm& vm) {
    FieldArg fa; Fixed x, z, r; int layer; int32_t v;
    if (!fieldArg(L, vm, 1, fa) || !argFx(L, vm, 2, x, "x") || !argFx(L, vm, 3, z, "z") || !argFx(L, vm, 4, r, "radius") || !valueArg(L, vm, 5, *fa.def, v)) return -1;
    const int mode = modeArg(L, vm, 6);
    if (mode < 0 || !layerArg(L, vm, 7, fa, layer, true)) return -1;
    if (r.raw > Fixed::fromInt(256).raw) r = Fixed::fromInt(256);
    lua_pushinteger(L, fa.gf->fillCircle(*vm.sim, fa.index, x, z, r, layer, v, mode));
    return 1;
}

} // namespace

void openFieldApi(Vm& vm) {
    lua_State* L = vm.L;
    static const luaL_Reg queries[] = {
        {"ids", bind<fIds>}, {"info", bind<fInfo>}, {"get", bind<fGet>}, {"get_cell", bind<fGetCell>}, {"cell", bind<fCell>},
        {"center", bind<fCenter>}, {"sum", bind<fSum>}, {"max", bind<fMax>}, {"count", bind<fCount>}, {"find", bind<fFind>},
        {nullptr, nullptr}};
    static const luaL_Reg writes[] = {
        {"set", bind<fSet>}, {"add", bind<fAdd>}, {"set_cell", bind<fSetCell>}, {"add_cell", bind<fAddCell>},
        {"fill_rect", bind<fFillRect>}, {"fill_circle", bind<fFillCircle>}, {nullptr, nullptr}};
    luaL_newlib(L, queries);
    if (vm.mode == Mode::Sim) luaL_setfuncs(L, writes, 0);
    lua_setglobal(L, "field");
}

} // namespace rts::game::script
