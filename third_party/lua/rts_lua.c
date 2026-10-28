/*
** RTSEngine: implementation of the float guard declared in rts_luaconf.h.
*/
#define LUA_CORE
#include "lprefix.h"
#include "lua.h"
#include "lstate.h"
#include "ldebug.h"

int rts_lua_floatop(lua_State *L) {
  RtsLuaCtx *c;
  if (L == NULL) return 0;
  c = *(RtsLuaCtx **)lua_getextraspace(L);
  if (c != NULL && c->noFloats) {
    /* arithmetic fast paths don't save the state: do what 'savestate' does so the error unwinds
       cleanly (the reported line is the last saved one, usually the current line) */
    if (isLua(L->ci)) L->top.p = L->ci->top.p;
    luaG_runerror(L, "float arithmetic is not allowed in simulation scripts "
                     "(use fx() numbers, or // for integer division)");
  }
  return 0;
}

/*
** RTSEngine: luaL_openlibs without linit.c. The engine builds Lua without the io / package / debug
** libraries, so the stock linit.c cannot be linked; RmlUi's Lua plugin calls luaL_openlibs when it
** creates its own state. Opens only the safe libraries (os is opened by engine/ui/UiLua.cpp itself,
** with the dangerous functions removed).
*/
#include "lauxlib.h"
#include "lualib.h"

LUALIB_API void luaL_openlibs (lua_State *L) {
  static const luaL_Reg libs[] = {
    {LUA_GNAME, luaopen_base},
    {LUA_COLIBNAME, luaopen_coroutine},
    {LUA_TABLIBNAME, luaopen_table},
    {LUA_STRLIBNAME, luaopen_string},
    {LUA_MATHLIBNAME, luaopen_math},
    {LUA_UTF8LIBNAME, luaopen_utf8},
    {NULL, NULL}
  };
  const luaL_Reg *lib;
  for (lib = libs; lib->func; lib++) {
    luaL_requiref(L, lib->name, lib->func, 1);
    lua_pop(L, 1);
  }
}
