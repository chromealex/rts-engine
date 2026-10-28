// Lua VM core: sandbox, fx numbers, canonical state serialization, handlers and dispatch.
// Design and determinism rules: RtsScript.h / docs/scripting.md.
#include "RtsScriptInternal.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "RtsInternal.h"

namespace rts::game {
using namespace script;

namespace script {

// ================================================================= VM basics

Vm& vmOf(lua_State* L) {
    RtsLuaCtx* c = *static_cast<RtsLuaCtx**>(lua_getextraspace(L));
    return *static_cast<Vm*>(c->user);
}

namespace {
void* luaAlloc(void* ud, void* ptr, size_t osize, size_t nsize) {
    Vm* vm = static_cast<Vm*>(ud);
    const size_t old = ptr ? osize : 0;
    if (nsize == 0) { std::free(ptr); vm->mem -= old; return nullptr; }
    if (vm->memLimit && vm->mem - old + nsize > vm->memLimit) return nullptr;
    void* p = std::realloc(ptr, nsize);
    if (p) vm->mem = vm->mem - old + nsize;
    return p;
}

constexpr int kHookStep = 1000;

void countHook(lua_State* L, lua_Debug*) {
    Vm& vm = vmOf(L);
    vm.used += lua_gethookcount(L);
    if (vm.used <= vm.budget) return;
    if (!vm.exceeded) { vm.exceeded = true; lua_sethook(L, countHook, LUA_MASKCOUNT, 1); } // can't be caught by pcall loops
    luaL_error(L, "instruction budget exceeded (%d per tick): endless loop?", int(vm.budget));
}

const char* kFxMeta = "rts.fx";
} // namespace

Vm::~Vm() { close(); }
void Vm::close() { if (L) lua_close(L); L = nullptr; }

// ================================================================= fx

void pushFx(lua_State* L, Fixed v) {
    auto* p = static_cast<int64_t*>(lua_newuserdatauv(L, sizeof(int64_t), 0));
    *p = v.raw;
    luaL_setmetatable(L, kFxMeta);
}

Fixed* testFx(lua_State* L, int idx) {
    return static_cast<Fixed*>(luaL_testudata(L, idx, kFxMeta));
}

namespace {
constexpr int64_t kMaxFxInt = int64_t(1) << 46;
Fixed fromDouble(double d) { return Fixed::fromRaw(int64_t(d * double(Fixed::kOne))); } // view scripts only
}

bool argFx(lua_State* L, Vm& vm, int idx, Fixed& out, const char* what) {
    if (Fixed* f = testFx(L, idx)) { out = *f; return true; }
    if (lua_isinteger(L, idx)) {
        lua_Integer i = lua_tointeger(L, idx);
        if (i > kMaxFxInt || i < -kMaxFxInt) { vm.err = std::string(what) + ": number out of range"; return false; }
        out = Fixed::fromInt(i);
        return true;
    }
    if (lua_type(L, idx) == LUA_TNUMBER) {
        if (vm.mode == Mode::View) { out = fromDouble(lua_tonumber(L, idx)); return true; }
        vm.err = std::string(what) + ": got a float - use fx() or integers";
        return false;
    }
    vm.err = std::string(what) + ": number expected, got " + luaL_typename(L, idx);
    return false;
}

bool optFx(lua_State* L, Vm& vm, int idx, Fixed& out, Fixed def, const char* what) {
    if (lua_isnoneornil(L, idx)) { out = def; return true; }
    return argFx(L, vm, idx, out, what);
}

bool argInt(lua_State* L, Vm& vm, int idx, int64_t& out, const char* what) {
    if (lua_isinteger(L, idx)) { out = lua_tointeger(L, idx); return true; }
    if (Fixed* f = testFx(L, idx)) { out = f->floorInt(); return true; }
    if (lua_type(L, idx) == LUA_TNUMBER && vm.mode == Mode::View) { out = int64_t(lua_tonumber(L, idx)); return true; }
    vm.err = std::string(what) + (lua_type(L, idx) == LUA_TNUMBER ? ": got a float - use integers" : ": integer expected");
    return false;
}

double toDouble(lua_State* L, int idx) {
    if (Fixed* f = testFx(L, idx)) return f->toDouble();
    return lua_tonumber(L, idx);
}

std::string fxText(Fixed v) {
    // integer-only: identical text on every platform (no printf of doubles)
    const bool neg = v.raw < 0;
    uint64_t r = neg ? uint64_t(0) - uint64_t(v.raw) : uint64_t(v.raw);
    uint64_t ip = r >> Fixed::kFracBits;
    uint64_t frac = ((r & uint64_t(Fixed::kOne - 1)) * 10000 + uint64_t(Fixed::kOne / 2)) >> Fixed::kFracBits; // 4 decimals
    if (frac >= 10000) { ++ip; frac -= 10000; }
    std::string s = (neg && (ip || frac) ? "-" : "") + std::to_string(ip);
    if (frac) {
        char buf[8];
        std::snprintf(buf, sizeof buf, "%04u", unsigned(frac));
        std::string f = buf;
        while (!f.empty() && f.back() == '0') f.pop_back();
        s += "." + f;
    }
    return s;
}

namespace {
// Both operands of an fx metamethod (integers or fx; floats only in view scripts).
int fxPair(lua_State* L, Vm& vm, Fixed& a, Fixed& b) {
    if (!argFx(L, vm, 1, a, "fx arithmetic") || !argFx(L, vm, 2, b, "fx arithmetic")) return -1;
    return 0;
}
int fxAdd(lua_State* L, Vm& vm) { Fixed a, b; if (fxPair(L, vm, a, b)) return -1; pushFx(L, a + b); return 1; }
int fxSub(lua_State* L, Vm& vm) { Fixed a, b; if (fxPair(L, vm, a, b)) return -1; pushFx(L, a - b); return 1; }
int fxMul(lua_State* L, Vm& vm) { Fixed a, b; if (fxPair(L, vm, a, b)) return -1; pushFx(L, a * b); return 1; }
int fxDiv(lua_State* L, Vm& vm) {
    Fixed a, b;
    if (fxPair(L, vm, a, b)) return -1;
    if (b.raw == 0) return fail(vm, "fx division by zero");
    pushFx(L, a / b);
    return 1;
}
int fxIdiv(lua_State* L, Vm& vm) {
    Fixed a, b;
    if (fxPair(L, vm, a, b)) return -1;
    if (b.raw == 0) return fail(vm, "fx division by zero");
    pushFx(L, Fixed::fromInt((a / b).floorInt()));
    return 1;
}
int fxMod(lua_State* L, Vm& vm) {
    Fixed a, b;
    if (fxPair(L, vm, a, b)) return -1;
    if (b.raw == 0) return fail(vm, "fx modulo by zero");
    int64_t m = a.raw % b.raw;
    if (m != 0 && ((m < 0) != (b.raw < 0))) m += b.raw;
    pushFx(L, Fixed::fromRaw(m));
    return 1;
}
int fxUnm(lua_State* L, Vm& vm) { Fixed a; if (!argFx(L, vm, 1, a, "fx")) return -1; pushFx(L, -a); return 1; }
int fxEq(lua_State* L, Vm& vm) { Fixed a, b; if (fxPair(L, vm, a, b)) return -1; lua_pushboolean(L, a == b); return 1; }
int fxLt(lua_State* L, Vm& vm) { Fixed a, b; if (fxPair(L, vm, a, b)) return -1; lua_pushboolean(L, a < b); return 1; }
int fxLe(lua_State* L, Vm& vm) { Fixed a, b; if (fxPair(L, vm, a, b)) return -1; lua_pushboolean(L, a <= b); return 1; }
int fxToString(lua_State* L, Vm& vm) {
    Fixed a;
    if (!argFx(L, vm, 1, a, "fx")) return -1;
    std::string s = fxText(a);
    lua_pushlstring(L, s.data(), s.size());
    return 1;
}
int fxConcat(lua_State* L, Vm&) {
    for (int i = 1; i <= 2; ++i) {
        if (Fixed* f = testFx(L, i)) { std::string s = fxText(*f); lua_pushlstring(L, s.data(), s.size()); }
        else lua_pushvalue(L, i);
        if (!lua_isstring(L, -1)) { lua_pop(L, 1); lua_pushstring(L, luaL_typename(L, i)); }
    }
    lua_concat(L, 2);
    return 1;
}
// fx(3) / fx(3, 4) = 0.75 / fx("1.25") / fx(fxValue)
int fxNew(lua_State* L, Vm& vm) {
    if (lua_type(L, 1) == LUA_TSTRING) {
        bool ok = false;
        Fixed v = Fixed::parse(lua_tostring(L, 1), &ok);
        if (!ok) return fail(vm, std::string("fx: not a decimal number: '") + lua_tostring(L, 1) + "'");
        pushFx(L, v);
        return 1;
    }
    Fixed a;
    if (!argFx(L, vm, 1, a, "fx")) return -1;
    if (!lua_isnoneornil(L, 2)) {
        Fixed b;
        if (!argFx(L, vm, 2, b, "fx")) return -1;
        if (b.raw == 0) return fail(vm, "fx: division by zero");
        a = a / b;
    }
    pushFx(L, a);
    return 1;
}
int fxCall(lua_State* L, Vm& vm) { lua_remove(L, 1); return fxNew(L, vm); }   // fx(...) through the table's __call
int fxRaw(lua_State* L, Vm& vm) { int64_t r; if (!argInt(L, vm, 1, r, "fx.raw")) return -1; pushFx(L, Fixed::fromRaw(r)); return 1; }
int fxSqrtL(lua_State* L, Vm& vm) { Fixed a; if (!argFx(L, vm, 1, a, "fx.sqrt")) return -1; pushFx(L, fxSqrt(a)); return 1; }
int fxAbsL(lua_State* L, Vm& vm) { Fixed a; if (!argFx(L, vm, 1, a, "fx.abs")) return -1; pushFx(L, fxAbs(a)); return 1; }
int fxMinL(lua_State* L, Vm& vm) { Fixed a, b; if (fxPair(L, vm, a, b)) return -1; pushFx(L, fxMin(a, b)); return 1; }
int fxMaxL(lua_State* L, Vm& vm) { Fixed a, b; if (fxPair(L, vm, a, b)) return -1; pushFx(L, fxMax(a, b)); return 1; }
int fxFloorL(lua_State* L, Vm& vm) { Fixed a; if (!argFx(L, vm, 1, a, "fx.floor")) return -1; lua_pushinteger(L, a.floorInt()); return 1; }
int fxCeilL(lua_State* L, Vm& vm) { Fixed a; if (!argFx(L, vm, 1, a, "fx.ceil")) return -1; lua_pushinteger(L, -((-a).floorInt())); return 1; }
int fxRoundL(lua_State* L, Vm& vm) { Fixed a; if (!argFx(L, vm, 1, a, "fx.round")) return -1; lua_pushinteger(L, (a + Fixed::fromRatio(1, 2)).floorInt()); return 1; }
int fxTruncL(lua_State* L, Vm& vm) { Fixed a; if (!argFx(L, vm, 1, a, "fx.int")) return -1; lua_pushinteger(L, a.toInt()); return 1; }
int fxRawOf(lua_State* L, Vm& vm) { Fixed a; if (!argFx(L, vm, 1, a, "fx.rawof")) return -1; lua_pushinteger(L, a.raw); return 1; }
int fxIsL(lua_State* L, Vm&) { lua_pushboolean(L, testFx(L, 1) != nullptr); return 1; }
int fxNum(lua_State* L, Vm& vm) {
    if (vm.mode == Mode::Sim) return fail(vm, "fx.num: floats are only available in view scripts");
    lua_pushnumber(L, toDouble(L, 1));
    return 1;
}
int fxLerp(lua_State* L, Vm& vm) {
    Fixed a, b, t;
    if (!argFx(L, vm, 1, a, "fx.lerp") || !argFx(L, vm, 2, b, "fx.lerp") || !argFx(L, vm, 3, t, "fx.lerp")) return -1;
    pushFx(L, a + (b - a) * t);
    return 1;
}

void openFx(Vm& vm) {
    lua_State* L = vm.L;
    static const luaL_Reg funcs[] = {
        {"raw", bind<fxRaw>}, {"sqrt", bind<fxSqrtL>}, {"abs", bind<fxAbsL>}, {"min", bind<fxMinL>}, {"max", bind<fxMaxL>},
        {"floor", bind<fxFloorL>}, {"ceil", bind<fxCeilL>}, {"round", bind<fxRoundL>}, {"int", bind<fxTruncL>},
        {"rawof", bind<fxRawOf>}, {"is", bind<fxIsL>}, {"num", bind<fxNum>}, {"lerp", bind<fxLerp>}, {"new", bind<fxNew>},
        {nullptr, nullptr}};
    static const luaL_Reg meta[] = {
        {"__add", bind<fxAdd>}, {"__sub", bind<fxSub>}, {"__mul", bind<fxMul>}, {"__div", bind<fxDiv>}, {"__idiv", bind<fxIdiv>},
        {"__mod", bind<fxMod>}, {"__unm", bind<fxUnm>}, {"__eq", bind<fxEq>}, {"__lt", bind<fxLt>}, {"__le", bind<fxLe>},
        {"__tostring", bind<fxToString>}, {"__concat", bind<fxConcat>}, {nullptr, nullptr}};
    luaL_newlib(L, funcs);                        // fx
    lua_pushinteger(L, Fixed::kOne); lua_setfield(L, -2, "ONE_RAW");
    luaL_newmetatable(L, kFxMeta);                // fx, meta
    luaL_setfuncs(L, meta, 0);
    lua_pushvalue(L, -2); lua_setfield(L, -2, "__index");   // methods: v:floor(), v:int(), v:num()...
    lua_pushliteral(L, "fx"); lua_setfield(L, -2, "__name");
    lua_pop(L, 1);
    lua_createtable(L, 0, 1);                     // fx, mt-of-fx-table
    lua_pushcfunction(L, bind<fxCall>); lua_setfield(L, -2, "__call");
    lua_setmetatable(L, -2);
    lua_setglobal(L, "fx");
}

// ================================================================= sandbox

int sbPrint(lua_State* L, Vm& vm) {
    std::string line;
    const int n = lua_gettop(L);
    for (int i = 1; i <= n; ++i) {
        size_t len = 0;
        const char* s = nullptr;
        if (Fixed* f = testFx(L, i)) { line += (i > 1 ? "\t" : "") + fxText(*f); continue; }
        if (lua_type(L, i) == LUA_TSTRING || lua_type(L, i) == LUA_TNUMBER) s = lua_tolstring(L, i, &len);
        if (i > 1) line += '\t';
        if (s) line.append(s, len);
        else if (lua_type(L, i) == LUA_TBOOLEAN) line += lua_toboolean(L, i) ? "true" : "false";
        else line += luaL_typename(L, i);
    }
    if (vm.log) vm.log(false, line);
    return 0;
}

int sbSetMetatable(lua_State* L, Vm& vm) {
    if (lua_type(L, 1) != LUA_TTABLE) return fail(vm, "setmetatable: table expected");
    if (lua_type(L, 2) == LUA_TTABLE) {
        // weak tables and finalizers depend on when the garbage collector runs: not deterministic
        for (const char* k : {"__gc", "__mode"}) {
            lua_pushstring(L, k);
            bool has = lua_rawget(L, 2) != LUA_TNIL;
            lua_pop(L, 1);
            if (has && vm.mode == Mode::Sim) return fail(vm, std::string("setmetatable: ") + k + " is not allowed in simulation scripts");
        }
    } else if (!lua_isnil(L, 2)) return fail(vm, "setmetatable: nil or table expected");
    if (luaL_getmetafield(L, 1, "__metatable") != LUA_TNIL) { lua_pop(L, 1); return fail(vm, "cannot change a protected metatable"); }
    lua_settop(L, 2);
    lua_setmetatable(L, 1);
    return 1;
}

int sbToNumber(lua_State* L, Vm& vm) {
    if (Fixed* f = testFx(L, 1)) { pushFx(L, *f); return 1; }
    if (lua_isinteger(L, 1)) { lua_settop(L, 1); return 1; }
    if (lua_type(L, 1) == LUA_TSTRING && lua_isnoneornil(L, 2)) {
        size_t len = 0;
        const char* s = lua_tolstring(L, 1, &len);
        if (lua_stringtonumber(L, s) == 0) { lua_pushnil(L); return 1; }
        if (lua_isinteger(L, -1) || vm.mode == Mode::View) return 1;
        return fail(vm, std::string("tonumber('") + s + "') is a float: use fx('" + s + "')");
    }
    if (lua_type(L, 1) == LUA_TNUMBER && vm.mode == Mode::View) { lua_settop(L, 1); return 1; }
    if (!lua_isnoneornil(L, 2)) {   // tonumber(s, base): integers only
        lua_Integer base = lua_tointeger(L, 2);
        size_t len = 0;
        const char* s = lua_tolstring(L, 1, &len);
        if (!s || base < 2 || base > 36) { lua_pushnil(L); return 1; }
        lua_Integer v = 0;
        bool neg = false, any = false;
        size_t i = 0;
        while (i < len && (s[i] == ' ' || s[i] == '\t')) ++i;
        if (i < len && s[i] == '-') { neg = true; ++i; }
        for (; i < len; ++i) {
            int c = s[i], d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'z' ? c - 'a' + 10 : c >= 'A' && c <= 'Z' ? c - 'A' + 10 : 99;
            if (d >= base) break;
            v = lua_Integer(uint64_t(v) * uint64_t(base) + uint64_t(d));
            any = true;
        }
        while (i < len && (s[i] == ' ' || s[i] == '\t')) ++i;
        if (!any || i != len) { lua_pushnil(L); return 1; }
        lua_pushinteger(L, neg ? lua_Integer(0 - uint64_t(v)) : v);
        return 1;
    }
    if (lua_type(L, 1) == LUA_TNUMBER) return fail(vm, "tonumber: got a float - use fx() or integers");
    lua_pushnil(L);
    return 1;
}

// math for simulation scripts: integer functions, fx-aware, random from the simulation RNG
int mAbs(lua_State* L, Vm& vm) {
    if (Fixed* f = testFx(L, 1)) { pushFx(L, fxAbs(*f)); return 1; }
    int64_t v; if (!argInt(L, vm, 1, v, "math.abs")) return -1;
    lua_pushinteger(L, v < 0 ? lua_Integer(0 - uint64_t(v)) : v);
    return 1;
}
int mMinMax(lua_State* L, Vm& vm, bool isMax) {
    const int n = lua_gettop(L);
    if (n < 1) return fail(vm, "math.min/max: value expected");
    int best = 1;
    for (int i = 1; i <= n; ++i) {
        Fixed v, b;
        if (!argFx(L, vm, i, v, "math.min/max") || !argFx(L, vm, best, b, "math.min/max")) return -1;
        if (isMax ? b < v : v < b) best = i;
    }
    lua_pushvalue(L, best);
    return 1;
}
int mMin(lua_State* L, Vm& vm) { return mMinMax(L, vm, false); }
int mMax(lua_State* L, Vm& vm) { return mMinMax(L, vm, true); }
int mFloor(lua_State* L, Vm& vm) { int64_t v; if (!argInt(L, vm, 1, v, "math.floor")) return -1; lua_pushinteger(L, v); return 1; }
int mCeil(lua_State* L, Vm& vm) {
    if (Fixed* f = testFx(L, 1)) { lua_pushinteger(L, -((-*f).floorInt())); return 1; }
    int64_t v; if (!argInt(L, vm, 1, v, "math.ceil")) return -1; lua_pushinteger(L, v); return 1;
}
int mSqrt(lua_State* L, Vm& vm) { Fixed a; if (!argFx(L, vm, 1, a, "math.sqrt")) return -1; pushFx(L, fxSqrt(a)); return 1; }
int mToInteger(lua_State* L, Vm&) {
    if (lua_isinteger(L, 1)) lua_settop(L, 1);
    else if (Fixed* f = testFx(L, 1); f && (f->raw & (Fixed::kOne - 1)) == 0) lua_pushinteger(L, f->floorInt());
    else lua_pushnil(L);
    return 1;
}
int mType(lua_State* L, Vm&) {
    if (lua_isinteger(L, 1)) lua_pushliteral(L, "integer");
    else if (testFx(L, 1)) lua_pushliteral(L, "fx");
    else if (lua_type(L, 1) == LUA_TNUMBER) lua_pushliteral(L, "float");
    else lua_pushnil(L);
    return 1;
}
int mRandom(lua_State* L, Vm& vm) {
    if (!vm.sim) return fail(vm, "math.random: no simulation");
    Random& rng = vm.sim->rng();
    const int n = lua_gettop(L);
    if (n == 0) { pushFx(L, rng.unit()); return 1; }   // fx in [0, 1)
    int64_t lo = 1, hi;
    if (n == 1) { if (!argInt(L, vm, 1, hi, "math.random")) return -1; }
    else if (!argInt(L, vm, 1, lo, "math.random") || !argInt(L, vm, 2, hi, "math.random")) return -1;
    if (lo > hi) return fail(vm, "math.random: interval is empty");
    if (hi - lo >= int64_t(UINT32_MAX)) return fail(vm, "math.random: interval too large");
    lua_pushinteger(L, lo + int64_t(rng.below(uint32_t(hi - lo + 1))));
    return 1;
}

void openSimMath(Vm& vm) {
    lua_State* L = vm.L;
    static const luaL_Reg funcs[] = {
        {"abs", bind<mAbs>}, {"min", bind<mMin>}, {"max", bind<mMax>}, {"floor", bind<mFloor>}, {"ceil", bind<mCeil>},
        {"sqrt", bind<mSqrt>}, {"tointeger", bind<mToInteger>}, {"type", bind<mType>}, {"random", bind<mRandom>},
        {nullptr, nullptr}};
    luaL_newlib(L, funcs);
    lua_pushinteger(L, LUA_MAXINTEGER); lua_setfield(L, -2, "maxinteger");
    lua_pushinteger(L, LUA_MININTEGER); lua_setfield(L, -2, "mininteger");
    lua_setglobal(L, "math");
}

// require("lib.util") / require("scripts/lib/util.lua"): only scripts of the game, cached
int sbRequire(lua_State* L, Vm& vm) {
    size_t len = 0;
    const char* s = luaL_tolstring(L, 1, &len);
    std::string name(s, len);
    lua_pop(L, 1);
    std::string path = name;
    if (path.size() < 4 || path.compare(path.size() - 4, 4, ".lua") != 0) {
        std::replace(path.begin(), path.end(), '.', '/');
        path += ".lua";
    }
    if (path.rfind("scripts/", 0) != 0) path = "scripts/" + path;
    lua_getfield(L, LUA_REGISTRYINDEX, "rts.loaded");
    if (lua_getfield(L, -1, path.c_str()) != LUA_TNIL) return 1;
    lua_pop(L, 1);
    auto it = vm.sources.find(path);
    if (it == vm.sources.end()) return fail(vm, "require: '" + name + "' not found (" + path + ")");
    std::string err;
    if (!loadChunk(vm, path, it->second, err)) return fail(vm, "require: " + err);
    // run it unprotected: errors propagate to the caller of require (we are already inside a pcall);
    // no C++ objects of this frame are alive past this point except `err`/`path` - move them out first
    lua_pushstring(L, path.c_str());               // loaded, fn, path
    lua_insert(L, -2);                              // loaded, path, fn
    path.clear(); path.shrink_to_fit(); err.clear(); err.shrink_to_fit(); name.clear(); name.shrink_to_fit();
    lua_call(L, 0, 1);                              // loaded, path, result
    if (lua_isnil(L, -1)) { lua_pop(L, 1); lua_pushboolean(L, 1); }
    lua_pushvalue(L, -1);                           // loaded, path, result, result
    lua_insert(L, -3);                              // loaded, result, path, result
    lua_rawset(L, -4);                              // loaded, result
    return 1;
}

// Sorted pairs, address-free tostring / string.format, strict globals (installed after loading).
const char* kPrelude = R"LUA(
local rawnext, rawtype, sort, rawtostring, getmt, fmt, select, error = next, type, table.sort, tostring, getmetatable, string.format, select, error
local order = { boolean = 1, number = 2, string = 3 }
local function keyless(a, b)
  local ta, tb = rawtype(a), rawtype(b)
  if ta ~= tb then return order[ta] < order[tb] end
  if ta == "boolean" then return (not a) and b end
  return a < b
end
local function sorted_pairs(t)
  if rawtype(t) ~= "table" then error("pairs: table expected, got " .. rawtype(t), 2) end
  local keys, n = {}, 0
  for k in rawnext, t do
    local tk = rawtype(k)
    if not order[tk] then error("pairs: " .. tk .. " keys have no deterministic order (use integer or string keys)", 2) end
    n = n + 1
    keys[n] = k
  end
  sort(keys, keyless)
  local i = 0
  return function()
    i = i + 1
    local k = keys[i]
    if k ~= nil then return k, t[k] end
  end, t, nil
end
pairs = sorted_pairs
_G.sorted_pairs = sorted_pairs
local function safe_tostring(v)
  local t = rawtype(v)
  if t == "table" or t == "function" or t == "thread" or t == "userdata" then
    local mt = getmt(v)
    if rawtype(mt) ~= "table" or mt.__tostring == nil then return t end
  end
  return rawtostring(v)
end
tostring = safe_tostring
string.format = function(f, ...)
  if rawtype(f) == "string" and f:find("%%[-+ #0-9.]*p") then error("string.format: %p shows addresses", 2) end
  local n = select("#", ...)
  local args = { ... }
  for i = 1, n do
    local t = rawtype(args[i])
    if t == "table" or t == "function" or t == "thread" or t == "userdata" then args[i] = safe_tostring(args[i]) end
  end
  return fmt(f, table.unpack(args, 1, n))
end
string.dump = nil
)LUA";

} // namespace

bool Vm::open(Mode m, size_t limit) {
    close();
    mode = m;
    memLimit = limit;
    L = lua_newstate(luaAlloc, this);
    if (!L) return false;
    ctx.noFloats = m == Mode::Sim ? 1 : 0;
    ctx.user = this;
    *static_cast<RtsLuaCtx**>(lua_getextraspace(L)) = &ctx;
    static const luaL_Reg libs[] = {{LUA_GNAME, luaopen_base}, {LUA_TABLIBNAME, luaopen_table}, {LUA_STRLIBNAME, luaopen_string},
                                    {LUA_UTF8LIBNAME, luaopen_utf8}, {LUA_COLIBNAME, luaopen_coroutine}, {LUA_MATHLIBNAME, luaopen_math}};
    for (auto& l : libs) { luaL_requiref(L, l.name, l.func, 1); lua_pop(L, 1); }
    for (const char* k : {"dofile", "loadfile", "load", "collectgarbage", "next"}) { lua_pushnil(L); lua_setglobal(L, k); }
    lua_pushcfunction(L, bind<sbPrint>); lua_setglobal(L, "print");
    lua_pushcfunction(L, bind<sbSetMetatable>); lua_setglobal(L, "setmetatable");
    lua_pushcfunction(L, bind<sbRequire>); lua_setglobal(L, "require");
    lua_newtable(L); lua_setfield(L, LUA_REGISTRYINDEX, "rts.loaded");
    openFx(*this);
    if (m == Mode::Sim) {
        lua_pushcfunction(L, bind<sbToNumber>); lua_setglobal(L, "tonumber");
        openSimMath(*this);
    }
    // prelude (both modes: sorted pairs are also handy in view scripts); it keeps `next` as a local
    lua_pushcfunction(L, [](lua_State* S) -> int { lua_settop(S, 2); if (lua_next(S, 1)) return 2; lua_pushnil(S); return 1; });
    lua_setglobal(L, "next");
    if (luaL_loadbufferx(L, kPrelude, std::strlen(kPrelude), "=prelude", "t") != LUA_OK || lua_pcall(L, 0, 0, 0) != LUA_OK) {
        if (log) log(true, std::string("prelude: ") + lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    lua_pushnil(L); lua_setglobal(L, "next");
    if (m == Mode::View) { lua_pushnil(L); lua_setglobal(L, "collectgarbage"); }
    lua_sethook(L, countHook, LUA_MASKCOUNT, kHookStep);
    return true;
}

// ================================================================= values

void pushJson(lua_State* L, const Json& j) {
    switch (j.type()) {
    case Json::Type::Number: {
        Fixed v = j.fx();
        if ((v.raw & (Fixed::kOne - 1)) == 0) lua_pushinteger(L, v.floorInt());
        else pushFx(L, v);
        break;
    }
    case Json::Type::String: { const std::string s = j.str(); lua_pushlstring(L, s.data(), s.size()); break; }
    case Json::Type::Bool: lua_pushboolean(L, j.boolean()); break;
    case Json::Type::Array:
        lua_createtable(L, int(j.size()), 0);
        for (size_t i = 0; i < j.size(); ++i) { pushJson(L, j[i]); lua_rawseti(L, -2, lua_Integer(i + 1)); }
        break;
    case Json::Type::Object:
        lua_createtable(L, 0, int(j.size()));
        for (auto& [k, v] : j.members()) { lua_pushlstring(L, k.data(), k.size()); pushJson(L, v); lua_rawset(L, -3); }
        break;
    default: lua_pushnil(L);
    }
}

namespace {
void putU64(std::string& out, uint64_t v) { for (int i = 0; i < 8; ++i) out += char(uint8_t(v >> (8 * i))); }
uint64_t getU64(const std::string& in, size_t pos) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= uint64_t(uint8_t(in[pos + size_t(i)])) << (8 * i);
    return v;
}
struct Key { int kind; int64_t i; std::string s; };   // kind: 0 false/true (i), 1 integer, 2 string
}

bool serialize(lua_State* L, int idx, std::string& out, std::string& where, int depth) {
    idx = lua_absindex(L, idx);
    switch (lua_type(L, idx)) {
    case LUA_TBOOLEAN: out += lua_toboolean(L, idx) ? 't' : 'f'; return true;
    case LUA_TNUMBER:
        if (!lua_isinteger(L, idx)) { where = "a float"; return false; }
        out += 'i'; putU64(out, uint64_t(lua_tointeger(L, idx)));
        return true;
    case LUA_TSTRING: {
        size_t len = 0;
        const char* s = lua_tolstring(L, idx, &len);
        out += 's'; putU64(out, len); out.append(s, len);
        return true;
    }
    case LUA_TUSERDATA:
        if (Fixed* f = testFx(L, idx)) { out += 'x'; putU64(out, uint64_t(f->raw)); return true; }
        where = "userdata";
        return false;
    case LUA_TTABLE: {
        if (depth > 32) { where = "a table nested deeper than 32 levels (cycle?)"; return false; }
        std::vector<Key> keys;
        lua_pushnil(L);
        while (lua_next(L, idx)) {
            lua_pop(L, 1);
            int kt = lua_type(L, -1);
            if (kt == LUA_TBOOLEAN) keys.push_back({0, lua_toboolean(L, -1), {}});
            else if (kt == LUA_TNUMBER && lua_isinteger(L, -1)) keys.push_back({1, lua_tointeger(L, -1), {}});
            else if (kt == LUA_TSTRING) { size_t len = 0; const char* s = lua_tolstring(L, -1, &len); keys.push_back({2, 0, std::string(s, len)}); }
            else { lua_pop(L, 1); where = std::string("a key of type ") + lua_typename(L, kt); return false; }
        }
        std::sort(keys.begin(), keys.end(), [](const Key& a, const Key& b) {
            return a.kind != b.kind ? a.kind < b.kind : a.kind == 2 ? a.s < b.s : a.i < b.i;
        });
        out += '{'; putU64(out, keys.size());
        for (const Key& k : keys) {
            if (k.kind == 0) { out += k.i ? 't' : 'f'; lua_pushboolean(L, int(k.i)); }
            else if (k.kind == 1) { out += 'i'; putU64(out, uint64_t(k.i)); lua_pushinteger(L, k.i); }
            else { out += 's'; putU64(out, k.s.size()); out += k.s; lua_pushlstring(L, k.s.data(), k.s.size()); }
            lua_rawget(L, idx);
            bool ok = serialize(L, -1, out, where, depth + 1);
            lua_pop(L, 1);
            if (!ok) {
                where = "[" + (k.kind == 2 ? "\"" + k.s + "\"" : k.kind == 1 ? std::to_string(k.i) : std::string(k.i ? "true" : "false")) + "]" +
                        (where.empty() || where[0] == '[' ? "" : ": ") + where;
                return false;
            }
        }
        return true;
    }
    default:
        where = lua_typename(L, lua_type(L, idx));
        return false;
    }
}

size_t deserialize(lua_State* L, const std::string& in, size_t pos, int depth) {
    if (pos >= in.size() || depth > 40) { lua_pushnil(L); return 0; }
    char t = in[pos++];
    switch (t) {
    case 't': lua_pushboolean(L, 1); return pos;
    case 'f': lua_pushboolean(L, 0); return pos;
    case 'i': if (pos + 8 > in.size()) break; lua_pushinteger(L, lua_Integer(getU64(in, pos))); return pos + 8;
    case 'x': if (pos + 8 > in.size()) break; pushFx(L, Fixed::fromRaw(int64_t(getU64(in, pos)))); return pos + 8;
    case 's': {
        if (pos + 8 > in.size()) break;
        uint64_t n = getU64(in, pos);
        pos += 8;
        if (n > in.size() - pos) break;
        lua_pushlstring(L, in.data() + pos, size_t(n));
        return pos + size_t(n);
    }
    case '{': {
        if (pos + 8 > in.size()) break;
        uint64_t n = getU64(in, pos);
        pos += 8;
        lua_createtable(L, 0, 0);
        for (uint64_t i = 0; i < n; ++i) {
            pos = deserialize(L, in, pos, depth + 1);
            if (!pos) { lua_pop(L, 2); lua_pushnil(L); return 0; }
            pos = deserialize(L, in, pos, depth + 1);
            if (!pos) { lua_pop(L, 3); lua_pushnil(L); return 0; }
            lua_rawset(L, -3);
        }
        return pos;
    }
    default: break;
    }
    lua_pushnil(L);
    return 0;
}

// ================================================================= loading

std::string findFloatLiteral(const std::string& s) {
    const size_t n = s.size();
    int line = 1;
    auto isId = [](char c) { return std::isalnum(uint8_t(c)) || c == '_'; };
    auto longBracket = [&](size_t i, int& level) {   // at '[': returns true for [[ / [==[
        size_t j = i + 1;
        level = 0;
        while (j < n && s[j] == '=') { ++level; ++j; }
        return j < n && s[j] == '[';
    };
    auto skipLong = [&](size_t i, int level) {         // i after the opening bracket
        std::string close = "]" + std::string(size_t(level), '=') + "]";
        size_t e = s.find(close, i);
        if (e == std::string::npos) e = n;
        for (size_t k = i; k < e; ++k) if (s[k] == '\n') ++line;
        return e == n ? n : e + close.size();
    };
    size_t i = 0;
    while (i < n) {
        char c = s[i];
        if (c == '\n') { ++line; ++i; continue; }
        if (c == '-' && i + 1 < n && s[i + 1] == '-') {
            i += 2;
            int level = 0;
            if (i < n && s[i] == '[' && longBracket(i, level)) { i = skipLong(i + size_t(level) + 2, level); continue; }
            while (i < n && s[i] != '\n') ++i;
            continue;
        }
        if (c == '[') {
            int level = 0;
            if (longBracket(i, level)) { i = skipLong(i + size_t(level) + 2, level); continue; }
            ++i;
            continue;
        }
        if (c == '"' || c == '\'') {
            ++i;
            while (i < n && s[i] != c) {
                if (s[i] == '\\' && i + 1 < n) { if (s[i + 1] == '\n') ++line; i += 2; continue; }
                if (s[i] == '\n') ++line;
                ++i;
            }
            ++i;
            continue;
        }
        if (isId(c) && !std::isdigit(uint8_t(c))) { while (i < n && isId(s[i])) ++i; continue; }
        if (c == '.' && i + 1 < n && s[i + 1] == '.') { i += 2; if (i < n && s[i] == '.') ++i; continue; }
        if (std::isdigit(uint8_t(c)) || (c == '.' && i + 1 < n && std::isdigit(uint8_t(s[i + 1])))) {
            size_t b = i;
            bool hex = c == '0' && i + 1 < n && (s[i + 1] == 'x' || s[i + 1] == 'X');
            if (hex) i += 2;
            const char e1 = hex ? 'p' : 'e', e2 = hex ? 'P' : 'E';
            bool isFloat = false;
            while (i < n) {
                char d = s[i];
                if ((d == e1 || d == e2) && i + 1 < n && (s[i + 1] == '+' || s[i + 1] == '-')) { isFloat = true; i += 2; continue; }
                if (d == e1 || d == e2) isFloat = true;
                if (d == '.') isFloat = true;
                if (!(std::isxdigit(uint8_t(d)) || d == '.' || isId(d))) break;
                ++i;
            }
            if (isFloat) {
                std::string lit = s.substr(b, i - b);
                return "line " + std::to_string(line) + ": float literal " + lit + " - use fx(\"" + lit + "\") or an integer";
            }
            continue;
        }
        ++i;
    }
    return "";
}

bool loadChunk(Vm& vm, const std::string& name, const std::string& text, std::string& error) {
    if (vm.mode == Mode::Sim) {
        std::string f = findFloatLiteral(text);
        if (!f.empty()) {
            size_t sp = f.find(':');   // "line N: ..." -> "name:N: ..."
            error = name + ":" + f.substr(5, sp - 5) + ":" + f.substr(sp + 1);
            return false;
        }
    }
    const std::string chunk = "@" + name;
    if (luaL_loadbufferx(vm.L, text.data(), text.size(), chunk.c_str(), "t") != LUA_OK) {
        const char* m = lua_tostring(vm.L, -1);
        error = m ? m : "load error";
        if (error.find(name) == std::string::npos) error = name + ": " + error;   // constant-folding errors carry no position
        lua_pop(vm.L, 1);
        return false;
    }
    return true;
}

bool protectedCall(Vm& vm, int nargs, int nresults, std::string* error) {
    lua_State* L = vm.L;
    if (!vm.exceeded) lua_sethook(L, countHook, LUA_MASKCOUNT, kHookStep);   // resets the residual count: deterministic
    if (lua_pcall(L, nargs, nresults, 0) == LUA_OK) return true;
    std::string m;
    if (Fixed* f = testFx(L, -1)) m = fxText(*f);
    else if (lua_type(L, -1) == LUA_TSTRING || lua_type(L, -1) == LUA_TNUMBER) m = lua_tostring(L, -1);
    else m = std::string("error object is a ") + luaL_typename(L, -1);
    lua_pop(L, 1);
    if (error) *error = m;
    return false;
}

} // namespace script

// ================================================================= files

std::vector<std::string> scriptFiles(const Json& rules, const Json& map, bool view) {
    std::vector<std::string> out;
    const char* key = view ? "viewScripts" : "scripts";
    for (const Json* j : {&rules[key], &map[key]})
        for (auto& f : j->items())
            if (f.isString() && std::find(out.begin(), out.end(), f.str()) == out.end()) out.push_back(f.str());
    return out;
}

namespace {
// require("x") / require "x" / require('x') in a source (the files to embed with it)
std::vector<std::string> requiredFiles(const std::string& text) {
    std::vector<std::string> out;
    size_t p = 0;
    while ((p = text.find("require", p)) != std::string::npos) {
        p += 7;
        size_t q = p;
        while (q < text.size() && (text[q] == ' ' || text[q] == '(')) ++q;
        if (q >= text.size() || (text[q] != '"' && text[q] != '\'')) continue;
        char quote = text[q];
        size_t e = text.find(quote, q + 1);
        if (e == std::string::npos) break;
        std::string name = text.substr(q + 1, e - q - 1);
        std::string path = name;
        if (path.size() < 4 || path.compare(path.size() - 4, 4, ".lua") != 0) { std::replace(path.begin(), path.end(), '.', '/'); path += ".lua"; }
        if (path.rfind("scripts/", 0) != 0) path = "scripts/" + path;
        out.push_back(path);
    }
    return out;
}
} // namespace

bool embedScripts(Json& rules, const Json& map, const ScriptReader& read, std::string* err) {
    std::vector<std::string> todo = scriptFiles(rules, map, false);
    for (auto& v : scriptFiles(rules, map, true)) todo.push_back(v);
    if (todo.empty()) return true;
    const std::vector<std::string> listed = todo;
    std::string missing;
    Json src = Json::object();
    for (size_t i = 0; i < todo.size(); ++i) {
        const std::string path = todo[i];
        if (src.has(path)) continue;
        std::string text;
        if (!read(path, text)) {
            // listed files must exist; a missing required file is reported by require() when it runs
            if (std::find(listed.begin(), listed.end(), path) != listed.end() && missing.empty()) missing = path;
            continue;
        }
        for (auto& r : requiredFiles(text)) todo.push_back(r);
        src.set(path, Json::string(std::move(text)));
    }
    rules.set("$scriptSources", std::move(src));
    if (!missing.empty()) { if (err) *err = "script not found: " + missing; return false; }
    return true;
}

std::string checkScriptSyntax(const std::string& name, const std::string& text, bool sim) {
    Vm vm;
    if (!vm.open(sim ? Mode::Sim : Mode::View, 64u << 20)) return name + ": cannot create a Lua state";
    std::string err;
    if (!loadChunk(vm, name, text, err)) return err;
    return "";
}

} // namespace rts::game
