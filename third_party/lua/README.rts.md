# Lua 5.4.7 (vendored)

Source: https://github.com/lua/lua tag `v5.4.7` (commit 1ab3208a1fceb12fca8f24ba57d6e13c5bff15e3),
MIT license (`LICENSE`). Used by the scripting system (`modules/rts/RtsScript*`, `docs/scripting.md`).

Not compiled: `lua.c`, `luac.c`, `onelua.c`, `ltests.*` (not copied) and `linit.c`, `liolib.c`,
`loslib.c`, `loadlib.c`, `ldblib.c` (copied unchanged but left out of the build: no files, processes,
dynamic loading or debug library on any platform — works the same on web, iOS and Android).
Compiled as C (errors use setjmp/longjmp, which emscripten supports without C++ exceptions).

Local changes (keep them when updating Lua):
- `luaconf.h`: one line in the "Local configuration" section includes `rts_luaconf.h`.
- `rts_luaconf.h` (new): constant string-hash seed and table.sort pivot (deterministic across runs and
  platforms), float-arithmetic guard macros.
- `rts_lua.c` (new): the guard (`rts_lua_floatop`), raising an error in simulation states.
- `lvm.c`: `floatforloop` takes `lua_State *L` (the guard macros need it).
