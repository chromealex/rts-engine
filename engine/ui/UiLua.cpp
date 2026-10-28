#include "UiLua.h"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include <cctype>
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


} // namespace rts::ui
