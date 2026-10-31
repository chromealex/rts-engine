// Script UI (G2): the bridge between the game's view scripts (modules/rts/view/RtsViewScript.cpp: ui.set /
// ui.open / ui.close / ui.on) and the client's UI Lua state, where engine/ui/lib/scriptui.lua keeps the
// RmlUi data model "script" and the documents ui/hud/<name>.rml. docs/scripting.md § Script UI.
extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <cstdio>

#include "GameClient.h"
#include "engine/ui/UiLua.h"

namespace rts::client {

namespace {
GameClient& host(lua_State* L) { return *static_cast<GameClient*>(lua_touserdata(L, lua_upvalueindex(1))); }

// scriptui.<fn>(args...) in the UI state; the arguments are already on the stack
void callScriptUi(lua_State* L, const char* fn, int nargs) {
    lua_getglobal(L, "scriptui");
    if (!lua_istable(L, -1)) { lua_pop(L, 1 + nargs); return; }
    lua_getfield(L, -1, fn);
    lua_remove(L, -2);
    if (!lua_isfunction(L, -1)) { lua_pop(L, 1 + nargs); return; }
    lua_insert(L, -nargs - 1);
    if (lua_pcall(L, nargs, 0, 0) != LUA_OK) { std::fprintf(stderr, "[lua] scriptui.%s: %s\n", fn, lua_tostring(L, -1)); lua_pop(L, 1); }
}
} // namespace

struct GameClient::ScriptUiApi {
    static int event(lua_State* L) {   // scripthost.event(name, args)
        GameClient& c = host(L);
        if (c.sim_ && lua_isstring(L, 1)) c.viewScripts_.uiEvent(*c.sim_, lua_tostring(L, 1), ui::luaToJson(L, 2));
        return 0;
    }
    static int send(lua_State* L) {    // scripthost.send(id, args): the selection is added when args.units is missing
        GameClient& c = host(L);
        if (!c.sim_ || !lua_isstring(L, 1) || c.spectator_) return 0;
        Json args = lua_istable(L, 2) ? ui::luaToJson(L, 2) : Json::object();
        if (!args.isObject()) args = Json::object();
        if (!args.has("units")) {
            Json u = Json::array();
            for (Entity e : c.selection_) u.push(Json::number(int64_t(e.packed())));
            args.set("units", u);
        }
        c.viewScripts_.send(*c.sim_, lua_tostring(L, 1), args);
        return 0;
    }
};

void GameClient::initScriptUi() {
    if (!lua_) return;
    lua_newtable(lua_);
    const luaL_Reg fns[] = {{"event", ScriptUiApi::event}, {"send", ScriptUiApi::send}};
    for (auto& f : fns) {
        lua_pushlightuserdata(lua_, this);
        lua_pushcclosure(lua_, f.func, 1);
        lua_setfield(lua_, -2, f.name);
    }
    lua_setglobal(lua_, "scripthost");
    std::string err;
    auto read = [](const std::string& p, std::string& out) { return platform::readContentFile(p, out); };
    if (!ui::luaRunFile(lua_, read, "engine/ui/lib/scriptui.lua", &err)) std::fprintf(stderr, "[lua] %s\n", err.c_str());
}

void GameClient::scriptUiReset() {
    if (lua_) callScriptUi(lua_, "_reset", 0);
}

// Per frame after the view scripts ran: their ui.* requests into the UI state.
void GameClient::scriptUiFrame() {
    if (!lua_) { viewScripts_.takeUiRequests(); return; }
    // values first: RmlUi binds a document's {{names}} when it loads, so the frame's ui.set must be in the
    // model before a ui.open of the same frame loads the document
    auto rq = viewScripts_.takeUiRequests();
    for (auto& r : rq) {
        if (r.kind != game::UiRequest::Set) continue;
        lua_pushstring(lua_, r.key.c_str());
        ui::luaPushJson(lua_, r.value);
        callScriptUi(lua_, "_set", 2);
    }
    for (auto& r : rq) {
        if (r.kind == game::UiRequest::Set) continue;
        lua_pushstring(lua_, r.key.c_str());
        if (r.kind == game::UiRequest::Open) { ui::luaPushJson(lua_, r.value); callScriptUi(lua_, "_open", 2); }
        else callScriptUi(lua_, "_close", 1);
    }
}

} // namespace rts::client
