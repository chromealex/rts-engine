/*
** RTSEngine additions to luaconf.h (included from its "Local configuration" section).
**
** 1. Deterministic hashing: the string hash seed and table.sort's pivot randomization normally
**    come from time() / ASLR. Simulation scripts must behave identically on every machine and
**    every run (lockstep, replays), so both are constants.
** 2. Float guard: every float arithmetic primitive calls rts_lua_floatop(L) first. For a state
**    whose RtsLuaCtx has noFloats set (simulation scripts) it raises a Lua error, so float math
**    can never influence the simulation. Presentation scripts (noFloats = 0) are unaffected.
*/
#ifndef RTS_LUACONF_H
#define RTS_LUACONF_H

#define luai_makeseed(L)	((unsigned int)0x52545345u)
#define l_randomizePivot()	(0u)

/* Per-state context stored in lua_getextraspace(L) (a pointer to it). */
typedef struct RtsLuaCtx {
  int noFloats;		/* 1: float arithmetic raises an error */
  void *user;		/* owner (engine side) */
} RtsLuaCtx;

#if defined(LUA_CORE)
struct lua_State;
int rts_lua_floatop(struct lua_State *L);

#define luai_numdiv(L,a,b)	((void)rts_lua_floatop(L), (a)/(b))
#define luai_numidiv(L,a,b)	((void)rts_lua_floatop(L), l_floor((a)/(b)))
#define luai_nummod(L,a,b,m)  \
  { (void)rts_lua_floatop(L); (m) = l_mathop(fmod)(a,b); \
    if (((m) > 0) ? (b) < 0 : ((m) < 0 && (b) > 0)) (m) += (b); }
#define luai_numpow(L,a,b)  \
  ((void)rts_lua_floatop(L), (b == 2) ? (a)*(a) : l_mathop(pow)(a,b))
#define luai_numadd(L,a,b)	((void)rts_lua_floatop(L), (a)+(b))
#define luai_numsub(L,a,b)	((void)rts_lua_floatop(L), (a)-(b))
#define luai_nummul(L,a,b)	((void)rts_lua_floatop(L), (a)*(b))
#define luai_numunm(L,a)	((void)rts_lua_floatop(L), -(a))
/* llimits.h defines these in the same block as luai_numadd: unchanged */
#define luai_numeq(a,b)		((a)==(b))
#define luai_numlt(a,b)		((a)<(b))
#define luai_numle(a,b)		((a)<=(b))
#define luai_numgt(a,b)		((a)>(b))
#define luai_numge(a,b)		((a)>=(b))
#define luai_numisnan(a)	(!luai_numeq((a), (a)))
#endif

#endif
