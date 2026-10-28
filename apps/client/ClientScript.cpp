// Lua for the client UI: one sandboxed Lua 5.4 state that RmlUi documents run their <script> and
// onclick="..." code in, plus a `game` table that lets scripts drive the client. A game's own UI logic
// goes into ui/*.lua (ui/main.lua runs once the documents are loaded; require('x') loads ui/x.lua).
//
// The state is created here until the engine-wide Lua module lands; then that shared state is passed in
// instead and the sandbox / `game` API are added to it unchanged.
extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include <algorithm>
#include <cstdio>

#include "GameClient.h"
#include "engine/ui/UiLua.h"

namespace rts::client {

namespace {

GameClient& client(lua_State* L) { return *static_cast<GameClient*>(lua_touserdata(L, lua_upvalueindex(1))); }

bool readContent(const std::string& path, std::string& out) { return platform::readContentFile(path, out); }

} // namespace

// Script-facing calls; a friend of GameClient so the C functions stay out of its header.
struct LuaApi {
    // game.tr(key [, {name = value, ...}]) -> localized text
    static int tr(lua_State* L) {
        GameClient& c = client(L);
        const char* key = luaL_checkstring(L, 1);
        i18n::Args a;
        if (lua_istable(L, 2)) {
            lua_pushnil(L);
            while (lua_next(L, 2)) {
                if (lua_type(L, -2) == LUA_TSTRING) {
                    const std::string k = lua_tostring(L, -2);
                    if (lua_isinteger(L, -1)) a.num(k, lua_tointeger(L, -1));
                    else if (lua_type(L, -1) == LUA_TNUMBER) a.num(k, int64_t(lua_tonumber(L, -1)));
                    else if (const char* v = lua_tostring(L, -1)) a.str(k, v);
                }
                lua_pop(L, 1);
            }
        }
        const std::string s = c.tr(key, a);
        lua_pushlstring(L, s.data(), s.size());
        return 1;
    }
    static int language(lua_State* L) { lua_pushstring(L, client(L).loc_.language().c_str()); return 1; }
    static int setLanguage(lua_State* L) { client(L).applyLanguage(luaL_checkstring(L, 1), true); return 0; }
    static int name(lua_State* L) { lua_pushstring(L, client(L).gameName_.c_str()); return 1; }
    // game.maps() -> {"title", ...} (1-based like Lua)
    static int maps(lua_State* L) {
        auto& m = client(L).maps_;
        lua_createtable(L, int(m.size()), 0);
        for (size_t i = 0; i < m.size(); ++i) { lua_pushstring(L, m[i].title.c_str()); lua_rawseti(L, -2, lua_Integer(i + 1)); }
        return 1;
    }
    // game.start(map [, {difficulty = 0..2, spectate = bool}])
    static int start(lua_State* L) {
        GameClient& c = client(L);
        const lua_Integer i = luaL_checkinteger(L, 1);
        if (i < 1 || size_t(i) > c.maps_.size()) return luaL_error(L, "game.start: map %d of %d", int(i), int(c.maps_.size()));
        if (lua_istable(L, 2)) {
            lua_getfield(L, 2, "difficulty");
            if (lua_isinteger(L, -1)) c.difficulty_ = int(std::clamp<lua_Integer>(lua_tointeger(L, -1), 0, 2));
            lua_getfield(L, 2, "spectate");
            if (!lua_isnil(L, -1)) c.spectator_ = lua_toboolean(L, -1);
            lua_pop(L, 2);
        }
        c.startMatch(size_t(i - 1));
        return 0;
    }
    static int screen(lua_State* L) {
        using S = GameClient::Screen;
        const S s = client(L).screen_;
        lua_pushstring(L, s == S::Menu ? "menu" : s == S::Lobby ? "lobby" : s == S::Playing ? "playing" : s == S::Paused ? "paused" : "over");
        return 1;
    }
    static int pause(lua_State* L) { auto& c = client(L); if (c.screen_ == GameClient::Screen::Playing) c.setScreen(GameClient::Screen::Paused); return 0; }
    static int resume(lua_State* L) { auto& c = client(L); if (c.screen_ == GameClient::Screen::Paused) c.setScreen(GameClient::Screen::Playing); return 0; }
    static int menu(lua_State* L) {
        auto& c = client(L);
        if (c.netMatch_) c.leaveNetMatch(); else c.toMenu();
        return 0;
    }
    static int howto(lua_State* L) {
        auto& c = client(L);
        c.howtoOpen_ = lua_isnone(L, 1) ? !c.howtoOpen_ : bool(lua_toboolean(L, 1));
        return 0;
    }
    static int touch(lua_State* L) { lua_pushboolean(L, platform::isTouchDevice()); return 1; }
    static int platformName(lua_State* L) { lua_pushstring(L, platform::name()); return 1; }
    // game.log(...) -> stdout, like print but tagged
    static int log(lua_State* L) {
        std::string line;
        for (int i = 1; i <= lua_gettop(L); ++i) { if (i > 1) line += ' '; line += luaL_tolstring(L, i, nullptr); lua_pop(L, 1); }
        std::printf("[lua] %s\n", line.c_str());
        std::fflush(stdout);
        return 0;
    }
};

void GameClient::initLua() {
    lua_ = luaL_newstate();
    if (!lua_) return;
    ui::luaSandbox(lua_, readContent, {"ui/"});
    const luaL_Reg api[] = {
        {"tr", LuaApi::tr}, {"language", LuaApi::language}, {"setLanguage", LuaApi::setLanguage}, {"name", LuaApi::name},
        {"maps", LuaApi::maps}, {"start", LuaApi::start}, {"screen", LuaApi::screen}, {"pause", LuaApi::pause},
        {"resume", LuaApi::resume}, {"menu", LuaApi::menu}, {"howto", LuaApi::howto}, {"touch", LuaApi::touch},
        {"platform", LuaApi::platformName}, {"log", LuaApi::log}, {nullptr, nullptr},
    };
    lua_newtable(lua_);
    lua_pushlightuserdata(lua_, this);
    luaL_setfuncs(lua_, api, 1);
    lua_setglobal(lua_, "game");
    lua_pushlightuserdata(lua_, this);
    lua_pushcclosure(lua_, LuaApi::log, 1);
    lua_setglobal(lua_, "print");   // print -> the client log
    ui_.attachLua(lua_);
}

void GameClient::runGameScript() {
    std::string text, err;
    if (!lua_ || !platform::readContentFile("ui/main.lua", text)) return;
    if (!ui::luaRunString(lua_, text, "@ui/main.lua", &err)) std::fprintf(stderr, "[lua] %s\n", err.c_str());
}

void GameClient::shutdownLua() {   // after the UI: RmlUi's documents hold references into the state
    if (lua_) lua_close(lua_);
    lua_ = nullptr;
}

} // namespace rts::client
