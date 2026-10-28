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
