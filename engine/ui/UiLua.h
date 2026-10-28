// Lua in UI documents: RmlUi's Lua plugin (<script>, <script src>, onclick="..." and the rmlui.* API)
// on a Lua 5.4 state owned by the caller, plus a sandbox for game content.
//
// The state is meant to be the engine's shared one; UI only adds to it:
//   lua_State* L = luaL_newstate();      // or the engine's shared state
//   ui::luaSandbox(L, readContent, {"ui/"});
//   ui_.attachLua(L);                    // after UiLayer::init, before loading documents
//   ... ui_.shutdown(); lua_close(L);    // the state outlives RmlUi
#pragma once
#include <functional>
#include <string>
#include <vector>

typedef struct lua_State lua_State;

namespace rts::ui {

using LuaReader = std::function<bool(const std::string& path, std::string& out)>;

// Opens the safe standard libraries only (base, string, table, math, utf8, coroutine, os.time/clock/date/difftime):
// no io, os.execute/remove/rename/exit/getenv, package, debug, dofile/loadfile, string.dump; load() and
// every chunk are text-only (bytecode can't be verified). require(name) loads <dir>/<name with . -> />.lua through
// `read` from the given directories (first hit wins), once per name.
void luaSandbox(lua_State* L, LuaReader read, std::vector<std::string> requireDirs = {"ui/"});

// Runs a Lua file through `read` (text mode). False (with the message in `error`) on a missing file,
// syntax or runtime error.
bool luaRunFile(lua_State* L, const LuaReader& read, const std::string& path, std::string* error = nullptr);
bool luaRunString(lua_State* L, const std::string& code, const std::string& name, std::string* error = nullptr);

} // namespace rts::ui
