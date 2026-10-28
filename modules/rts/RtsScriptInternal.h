// Internals shared by the scripting files (RtsScript.cpp: VM, sandbox, fx, state; RtsScriptApi.cpp:
// the `game` table; view/RtsViewScript.cpp: presentation VM). Not a public header.
//
// Error discipline: Lua is compiled as C, so lua_error() is a longjmp that skips C++ destructors.
// Bindings are therefore written as `int impl(lua_State*, Vm&)` returning the number of results, or
// -1 after setting vm.err; the wrapper `bind<impl>` raises the error after impl has returned (all its
// C++ locals destroyed). Inside impl only non-raising Lua calls are used on user values (raw access,
// lua_to*, luaL_testudata); pushes can only fail on out-of-memory.
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include "RtsScript.h"
#include "../../engine/core/Fixed.h"

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

namespace rts { class Simulation; }

namespace rts::game::script {

enum class Mode : uint8_t { Sim, View };

struct Vm {
    lua_State* L = nullptr;
    RtsLuaCtx ctx{};
    Mode mode = Mode::Sim;
    RtsModule* module = nullptr;
    Simulation* sim = nullptr;              // set while scripts run
    std::string err;                        // pending binding error (see header comment)
    // instruction budget (count hook)
    int64_t budget = 1000000, used = 0;
    bool exceeded = false;
    size_t mem = 0, memLimit = 0;           // allocator accounting (crash guard, not a game rule)
    bool loading = false;                   // files are being run: handler registration allowed
    std::map<std::string, std::string> sources;   // path -> text (require)
    std::function<void(bool error, const std::string& text)> log;
    void* owner = nullptr;                  // ScriptEngine::Impl / view host

    ~Vm();
    bool open(Mode m, size_t memLimit);     // new state + sandbox
    void close();
};

Vm& vmOf(lua_State* L);

template <int (*F)(lua_State*, Vm&)>
int bind(lua_State* L) {
    Vm& vm = vmOf(L);
    int n = F(L, vm);
    if (n >= 0) return n;
    luaL_where(L, 1);
    lua_pushlstring(L, vm.err.data(), vm.err.size());
    vm.err.clear();
    lua_concat(L, 2);
    return lua_error(L);
}
inline int fail(Vm& vm, std::string msg) { vm.err = std::move(msg); return -1; }

// ---- fx numbers
void pushFx(lua_State* L, Fixed v);
Fixed* testFx(lua_State* L, int idx);
// Integer or fx at idx (View mode: any number). False with vm.err set otherwise.
bool argFx(lua_State* L, Vm& vm, int idx, Fixed& out, const char* what);
bool argInt(lua_State* L, Vm& vm, int idx, int64_t& out, const char* what);
bool optFx(lua_State* L, Vm& vm, int idx, Fixed& out, Fixed def, const char* what);
// A number for presentation: fx / integer / float as double (view scripts only).
double toDouble(lua_State* L, int idx);
std::string fxText(Fixed v);            // integer-only decimal formatting ("1.25", "-3")

// ---- values
void pushJson(lua_State* L, const Json& j);      // numbers: integer when whole, fx otherwise
// Canonical serialization of the value at idx (bool, integer, fx, string, nested tables with
// integer/string keys, sorted). False with `where` = path of the offending value.
bool serialize(lua_State* L, int idx, std::string& out, std::string& where, int depth = 0);
// Pushes the value; returns the read position (0 on malformed input, pushes nil).
size_t deserialize(lua_State* L, const std::string& in, size_t pos = 0, int depth = 0);
// Source scan for float literals (sim scripts): "" or "line N: ...".
std::string findFloatLiteral(const std::string& text);
// Load a chunk (text only) into the VM; pushes the function or returns false with message.
bool loadChunk(Vm& vm, const std::string& name, const std::string& text, std::string& error);
// lua_pcall with budget reset handling; on error logs "message" and returns false.
bool protectedCall(Vm& vm, int nargs, int nresults, std::string* error = nullptr);

// ---- API tables (RtsScriptApi.cpp)
void openGameApi(Vm& vm);           // `game` table: queries always, actions in Sim mode
void openStdApi(Vm& vm);            // `rts` table for the standard library (RtsScriptStd.cpp)
void openFieldApi(Vm& vm);          // `field` table: grid fields (RtsGridFieldsApi.cpp)
void pushEntity(lua_State* L, Entity e);
bool argEntity(lua_State* L, int idx, Entity& out); // integer handle (no liveness check)

} // namespace rts::game::script
