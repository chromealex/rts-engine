#include "UiLua.h"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <memory>

namespace rts::ui {

namespace {

struct RequireCtx {
    LuaReader read;
    std::vector<std::string> dirs;
};

const char* kRequireKey = "rts.require";
const char* kLoadedKey = "rts.loaded";

RequireCtx* requireCtx(lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, kRequireKey);
    auto* c = static_cast<RequireCtx*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return c;
}

int gcRequireCtx(lua_State* L) {
    static_cast<RequireCtx*>(lua_touserdata(L, 1))->~RequireCtx();
    return 0;
}

// require(name): game scripts only, from the content directories; cached like package.loaded.
int luaRequire(lua_State* L) {
    std::string name = luaL_checkstring(L, 1);
    for (char c : name)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '-')) return luaL_error(L, "require '%s': bad module name", name.c_str());
    lua_getfield(L, LUA_REGISTRYINDEX, kLoadedKey);
    lua_getfield(L, -1, name.c_str());
    if (!lua_isnil(L, -1)) return 1;
    lua_pop(L, 1);
    RequireCtx* ctx = requireCtx(L);
    std::string rel = name;
    for (char& c : rel) if (c == '.') c = '/';
    std::string text, path;
    bool found = false;
    for (auto& d : ctx->dirs)
        if (ctx->read && ctx->read(d + rel + ".lua", text)) { path = d + rel + ".lua"; found = true; break; }
    if (!found) return luaL_error(L, "require '%s': no %s.lua in the game's script folders", name.c_str(), rel.c_str());
    if (luaL_loadbufferx(L, text.data(), text.size(), ("@" + path).c_str(), "t") != LUA_OK) return lua_error(L);
    lua_pushstring(L, name.c_str());
    lua_call(L, 1, 1);
    if (lua_isnil(L, -1)) { lua_pop(L, 1); lua_pushboolean(L, 1); }
    lua_pushvalue(L, -1);
    lua_setfield(L, -3, name.c_str());   // loaded[name] = result
    return 1;
}

// load(chunk [, name [, mode [, env]]]) without binary chunks.
int luaLoadText(lua_State* L) {
    lua_settop(L, 4);
    lua_pushvalue(L, lua_upvalueindex(1));   // original load
    lua_pushvalue(L, 1);
    lua_pushvalue(L, 2);
    lua_pushliteral(L, "t");
    const bool hasEnv = !lua_isnil(L, 4);   // settop(4): a missing env is nil
    if (hasEnv) lua_pushvalue(L, 4);
    lua_call(L, hasEnv ? 4 : 3, LUA_MULTRET);
    return lua_gettop(L) - 4;
}

void removeField(lua_State* L, const char* table, const char* field) {
    lua_getglobal(L, table);
    if (lua_istable(L, -1)) { lua_pushnil(L); lua_setfield(L, -2, field); }
    lua_pop(L, 1);
}

} // namespace

void luaSandbox(lua_State* L, LuaReader read, std::vector<std::string> requireDirs) {
    const luaL_Reg libs[] = {
        {LUA_GNAME, luaopen_base}, {LUA_STRLIBNAME, luaopen_string}, {LUA_TABLIBNAME, luaopen_table},
        {LUA_MATHLIBNAME, luaopen_math}, {LUA_UTF8LIBNAME, luaopen_utf8}, {LUA_COLIBNAME, luaopen_coroutine},
        {LUA_OSLIBNAME, luaopen_os},
    };
    for (auto& l : libs) { luaL_requiref(L, l.name, l.func, 1); lua_pop(L, 1); }
    // os: clocks and dates only (daily rewards, timers)
    for (const char* f : {"execute", "exit", "getenv", "remove", "rename", "setlocale", "tmpname"}) removeField(L, LUA_OSLIBNAME, f);
    for (const char* f : {"dofile", "loadfile"}) { lua_pushnil(L); lua_setglobal(L, f); }
    removeField(L, LUA_STRLIBNAME, "dump");
    lua_getglobal(L, "load");
    lua_pushcclosure(L, luaLoadText, 1);
    lua_setglobal(L, "load");

    void* mem = lua_newuserdatauv(L, sizeof(RequireCtx), 0);
    new (mem) RequireCtx{std::move(read), std::move(requireDirs)};
    lua_newtable(L);
    lua_pushcfunction(L, gcRequireCtx);
    lua_setfield(L, -2, "__gc");
    lua_setmetatable(L, -2);
    lua_setfield(L, LUA_REGISTRYINDEX, kRequireKey);
    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, kLoadedKey);
    lua_pushcfunction(L, luaRequire);
    lua_setglobal(L, "require");
}

static bool runLoaded(lua_State* L, int status, std::string* error) {
    if (status == LUA_OK) status = lua_pcall(L, 0, 0, 0);
    if (status != LUA_OK) {
        if (error) *error = lua_isstring(L, -1) ? lua_tostring(L, -1) : "lua error";
        lua_pop(L, 1);
        return false;
    }
    return true;
}

bool luaRunFile(lua_State* L, const LuaReader& read, const std::string& path, std::string* error) {
    std::string text;
    if (!read || !read(path, text)) { if (error) *error = path + ": not found"; return false; }
    return runLoaded(L, luaL_loadbufferx(L, text.data(), text.size(), ("@" + path).c_str(), "t"), error);
}

bool luaRunString(lua_State* L, const std::string& code, const std::string& name, std::string* error) {
    return runLoaded(L, luaL_loadbufferx(L, code.data(), code.size(), name.c_str(), "t"), error);
}


void luaPushJson(lua_State* L, const Json& j) {
    luaL_checkstack(L, 4, "json too deep");
    switch (j.type()) {
    case Json::Type::Null: lua_pushnil(L); break;
    case Json::Type::Bool: lua_pushboolean(L, j.boolean()); break;
    case Json::Type::String: { const std::string s = j.str(); lua_pushlstring(L, s.data(), s.size()); break; }
    case Json::Type::Number: {
        const std::string& t = j.numberLiteral();
        if (t.find_first_of(".eE") == std::string::npos) lua_pushinteger(L, lua_Integer(std::strtoll(t.c_str(), nullptr, 10)));
        else lua_pushnumber(L, std::strtod(t.c_str(), nullptr));
        break;
    }
    case Json::Type::Array: {
        lua_createtable(L, int(j.items().size()), 0);
        lua_Integer k = 1;
        for (auto& v : j.items()) { luaPushJson(L, v); lua_rawseti(L, -2, k++); }
        break;
    }
    case Json::Type::Object: {
        lua_createtable(L, 0, int(j.members().size()));
        for (auto& [k, v] : j.members()) { luaPushJson(L, v); lua_setfield(L, -2, k.c_str()); }
        break;
    }
    }
}

static Json toJson(lua_State* L, int idx, int depth) {
    idx = lua_absindex(L, idx);
    switch (lua_type(L, idx)) {
    case LUA_TBOOLEAN: return Json::fromBool(lua_toboolean(L, idx));
    case LUA_TSTRING: { size_t n; const char* s = lua_tolstring(L, idx, &n); return Json::string(std::string(s, n)); }
    case LUA_TNUMBER: {
        if (lua_isinteger(L, idx)) return Json::number(int64_t(lua_tointeger(L, idx)));
        char b[48];
        std::snprintf(b, sizeof b, "%.14g", double(lua_tonumber(L, idx)));
        std::string t = b;
        if (t.find_first_of(".eE") == std::string::npos) t += ".0";   // keep it a float on the way back
        if (t.find_first_of("ni") != std::string::npos) return Json();  // nan / inf
        return Json::numberText(t);
    }
    case LUA_TTABLE: {
        if (depth > 32) return Json();
        luaL_checkstack(L, 4, "table too deep");
        const lua_Integer n = lua_Integer(lua_rawlen(L, idx));
        bool array = true;
        std::vector<std::string> keys;
        lua_Integer count = 0;
        lua_pushnil(L);
        while (lua_next(L, idx)) {
            ++count;
            if (lua_type(L, -2) == LUA_TSTRING) { keys.push_back(lua_tostring(L, -2)); array = false; }
            else if (!lua_isinteger(L, -2) || lua_tointeger(L, -2) < 1 || lua_tointeger(L, -2) > n) array = false;
            lua_pop(L, 1);
        }
        if (array && count == n && n > 0) {
            Json a = Json::array();
            for (lua_Integer i = 1; i <= n; ++i) { lua_rawgeti(L, idx, i); a.push(toJson(L, -1, depth + 1)); lua_pop(L, 1); }
            return a;
        }
        std::sort(keys.begin(), keys.end());
        Json o = Json::object();
        for (auto& k : keys) {
            lua_getfield(L, idx, k.c_str());
            const int t = lua_type(L, -1);
            if (t != LUA_TFUNCTION && t != LUA_TUSERDATA && t != LUA_TLIGHTUSERDATA && t != LUA_TTHREAD) o.set(k, toJson(L, -1, depth + 1));
            lua_pop(L, 1);
        }
        return o;
    }
    default: return Json();
    }
}

Json luaToJson(lua_State* L, int index) { return toJson(L, index, 0); }

} // namespace rts::ui
