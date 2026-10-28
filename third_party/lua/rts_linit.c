/*
** RTSEngine: luaL_openlibs without io / package / debug (linit.c is not built).
** Only referenced by the RmlUi Lua plugin for a state it creates itself; the engine always hands
** the plugin its own sandboxed state (engine/ui/UiLua.cpp), so this is a safe fallback.
*/
#define LUA_LIB
#include "lprefix.h"
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

LUALIB_API void luaL_openlibs(lua_State *L) {
  static const luaL_Reg libs[] = {
    {LUA_GNAME, luaopen_base}, {LUA_COLIBNAME, luaopen_coroutine}, {LUA_TABLIBNAME, luaopen_table},
    {LUA_STRLIBNAME, luaopen_string}, {LUA_MATHLIBNAME, luaopen_math}, {LUA_UTF8LIBNAME, luaopen_utf8},
    {NULL, NULL}
  };
  const luaL_Reg *lib;
  for (lib = libs; lib->func; lib++) {
    luaL_requiref(L, lib->name, lib->func, 1);
    lua_pop(L, 1);
  }
}
