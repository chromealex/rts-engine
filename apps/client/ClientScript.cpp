// Lua for the client UI: one sandboxed Lua 5.4 state that RmlUi documents run their <script> and
// onclick="..." code in, plus the tables scripts drive the client with:
//   game.*     client: language, maps, match start / pause / menu, events (game.on in ui/lib/meta.lua)
//   storage.*  per-game JSON saves in the user folder (profile, progress)
//   store.*    in-app purchases (engine/services/Store.h), ads.* (Ads.h)
// engine/ui/lib/meta.lua builds the meta-game layer on top (screens, profile, shop); a game's own code
// goes into ui/*.lua (ui/main.lua runs once the documents are loaded; require('x') loads ui/x.lua).
// Reference: docs/ui-scripting.md.
//
// The state is created here until the engine-wide Lua module lands; then that shared state is passed in
// instead and the sandbox / API are added to it unchanged.
extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "GameClient.h"
#include "engine/services/Ads.h"
#include "engine/services/Store.h"
#include "engine/ui/UiLua.h"

namespace rts::client {

namespace {

GameClient& client(lua_State* L) { return *static_cast<GameClient*>(lua_touserdata(L, lua_upvalueindex(1))); }

bool readContent(const std::string& path, std::string& out) { return platform::readContentFile(path, out); }

bool safeName(const std::string& n) {
    if (n.empty() || n.size() > 64) return false;
    for (char c : n) if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')) return false;
    return true;
}

// game.json(path): only the game's own data under ui/ (catalogs, defaults), no ".."
bool safeDataPath(const std::string& p) {
    return p.rfind("ui/", 0) == 0 && p.size() > 8 && p.compare(p.size() - 5, 5, ".json") == 0 && p.find("..") == std::string::npos;
}

const char* purchaseState(store::PurchaseState s) {
    switch (s) {
    case store::PurchaseState::Purchased: return "purchased";
    case store::PurchaseState::Pending: return "pending";
    case store::PurchaseState::Cancelled: return "cancelled";
    default: return "failed";
    }
}

Json purchaseJson(const store::Purchase& p) {
    Json j = Json::object();
    j.set("product", Json::string(p.productId));
    j.set("state", Json::string(purchaseState(p.state)));
    j.set("token", Json::string(p.token));
    if (!p.orderId.empty()) j.set("order", Json::string(p.orderId));
    if (!p.error.empty()) j.set("error", Json::string(p.error));
    j.set("restored", Json::fromBool(p.restored));
    return j;
}

} // namespace

// Script-facing calls; a friend of GameClient so the C functions stay out of its header.
struct LuaApi {
    // A Lua callback kept across an async call (store / ads). Dropped silently if the state is gone.
    struct Callback {
        std::shared_ptr<bool> alive;
        lua_State* L = nullptr;
        int ref = LUA_NOREF;
        void call(const Json& a, const Json* b = nullptr) {
            if (!*alive || ref == LUA_NOREF) return;
            lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
            luaL_unref(L, LUA_REGISTRYINDEX, ref);
            ref = LUA_NOREF;
            ui::luaPushJson(L, a);
            if (b) ui::luaPushJson(L, *b);
            if (lua_pcall(L, b ? 2 : 1, 0, 0) != LUA_OK) {
                std::fprintf(stderr, "[lua] %s\n", lua_tostring(L, -1));
                lua_pop(L, 1);
            }
        }
    };
    static std::shared_ptr<Callback> keep(lua_State* L, int idx) {
        auto c = std::make_shared<Callback>();
        c->alive = client(L).luaAlive_;
        c->L = L;
        if (lua_isfunction(L, idx)) { lua_pushvalue(L, idx); c->ref = luaL_ref(L, LUA_REGISTRYINDEX); }
        return c;
    }

    // ------------------------------------------------------------------ game.*
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
    static int hasText(lua_State* L) { lua_pushboolean(L, client(L).loc_.has(luaL_checkstring(L, 1))); return 1; }
    static int language(lua_State* L) { lua_pushstring(L, client(L).loc_.language().c_str()); return 1; }
    static int languages(lua_State* L) {
        auto& c = client(L);
        lua_createtable(L, int(c.langs_.available.size()), 0);
        for (size_t i = 0; i < c.langs_.available.size(); ++i) {
            lua_createtable(L, 0, 2);
            lua_pushstring(L, c.langs_.available[i].c_str()); lua_setfield(L, -2, "id");
            lua_pushstring(L, i < c.langNames_.size() ? c.langNames_[i].c_str() : c.langs_.available[i].c_str()); lua_setfield(L, -2, "name");
            lua_rawseti(L, -2, lua_Integer(i + 1));
        }
        return 1;
    }
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
        if (c.uiPreview_) return 0;   // previews never leave the screen
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
    static int difficulty(lua_State* L) {
        auto& c = client(L);
        if (lua_isinteger(L, 1)) c.difficulty_ = int(std::clamp<lua_Integer>(lua_tointeger(L, 1), 0, 2));
        lua_pushinteger(L, c.difficulty_);
        return 1;
    }
    static int screen(lua_State* L) { lua_pushstring(L, client(L).screenName()); return 1; }
    static int pause(lua_State* L) { auto& c = client(L); if (c.screen_ == GameClient::Screen::Playing) c.setScreen(GameClient::Screen::Paused); return 0; }
    static int resume(lua_State* L) { auto& c = client(L); if (c.screen_ == GameClient::Screen::Paused) c.setScreen(GameClient::Screen::Playing); return 0; }
    static int menu(lua_State* L) {
        auto& c = client(L);
        if (c.netMatch_) c.leaveNetMatch(); else c.toMenu();
        return 0;
    }
    static int online(lua_State* L) { auto& c = client(L); if (!c.uiPreview_) c.netAction("open", -1); return 0; }
    static int howto(lua_State* L) {
        auto& c = client(L);
        c.howtoOpen_ = lua_isnone(L, 1) ? !c.howtoOpen_ : bool(lua_toboolean(L, 1));
        return 0;
    }
    // game.engineMenu(false): the game's own screens replace the engine main menu (menu.rml)
    static int engineMenu(lua_State* L) {
        auto& c = client(L);
        if (!lua_isnone(L, 1)) c.engineMenu_ = lua_toboolean(L, 1);
        lua_pushboolean(L, c.engineMenu_);
        return 1;
    }
    static int notice(lua_State* L) { client(L).notice(luaL_checkstring(L, 1)); return 0; }
    static int lastResult(lua_State* L) { ui::luaPushJson(L, client(L).lastResult_); return 1; }
    // game.safeArea() -> {left, top, right, bottom} in dp: notch, home indicator, ad banner (RTS_UI_SAFE_AREA="l,t,r,b" for previews)
    static int safeArea(lua_State* L) {
        platform::Insets in = platform::safeAreaInsets();
        if (const char* e = std::getenv("RTS_UI_SAFE_AREA"); e && *e) std::sscanf(e, "%f,%f,%f,%f", &in.left, &in.top, &in.right, &in.bottom);
        lua_createtable(L, 0, 4);
        lua_pushnumber(L, in.left); lua_setfield(L, -2, "left");
        lua_pushnumber(L, in.top); lua_setfield(L, -2, "top");
        lua_pushnumber(L, in.right); lua_setfield(L, -2, "right");
        lua_pushnumber(L, in.bottom); lua_setfield(L, -2, "bottom");
        return 1;
    }
    static int touch(lua_State* L) { lua_pushboolean(L, platform::isTouchDevice()); return 1; }
    static int platformName(lua_State* L) { lua_pushstring(L, platform::name()); return 1; }
    static int preview(lua_State* L) { lua_pushboolean(L, client(L).uiPreview_); return 1; }
    static int time(lua_State* L) { lua_pushnumber(L, client(L).time_); return 1; }
    // game.json("ui/shop.json") -> table | nil, error: the game's own data files
    static int json(lua_State* L) {
        const std::string p = luaL_checkstring(L, 1);
        if (!safeDataPath(p)) return luaL_error(L, "game.json: only ui/*.json of the game ('%s')", p.c_str());
        std::string text;
        if (!platform::readContentFile(p, text)) { lua_pushnil(L); lua_pushstring(L, (p + ": not found").c_str()); return 2; }
        try { ui::luaPushJson(L, Json::parse(text)); return 1; }
        catch (const std::exception& e) { lua_pushnil(L); lua_pushstring(L, (p + ": " + e.what()).c_str()); return 2; }
    }
    // game.decode(text) -> value | nil, error; game.encode(value) -> JSON text
    static int decode(lua_State* L) {
        try { ui::luaPushJson(L, Json::parse(luaL_checkstring(L, 1))); return 1; }
        catch (const std::exception& e) { lua_pushnil(L); lua_pushstring(L, e.what()); return 2; }
    }
    static int encode(lua_State* L) { const std::string t = ui::luaToJson(L, 1).dump(-1); lua_pushlstring(L, t.data(), t.size()); return 1; }
    // game.log(...) -> stdout, like print but tagged
    static int log(lua_State* L) {
        std::string line;
        for (int i = 1; i <= lua_gettop(L); ++i) { if (i > 1) line += ' '; line += luaL_tolstring(L, i, nullptr); lua_pop(L, 1); }
        std::printf("[lua] %s\n", line.c_str());
        std::fflush(stdout);
        return 0;
    }

    // ------------------------------------------------------------------ storage.*
    static std::filesystem::path saveFile(GameClient& c, const std::string& name) {
        return std::filesystem::path(c.saveDir()) / (name + ".json");
    }
    // storage.load(name) -> table | nil
    static int load(lua_State* L) {
        const std::string n = luaL_checkstring(L, 1);
        if (!safeName(n)) return luaL_error(L, "storage: bad name '%s' (letters, digits, _ -)", n.c_str());
        std::ifstream f(saveFile(client(L), n), std::ios::binary);
        if (!f) { lua_pushnil(L); return 1; }
        std::stringstream ss; ss << f.rdbuf();
        try { ui::luaPushJson(L, Json::parse(ss.str())); }
        catch (const std::exception& e) { std::fprintf(stderr, "[lua] storage %s: %s\n", n.c_str(), e.what()); lua_pushnil(L); }
        return 1;
    }
    // storage.save(name, table) -> true | false, error
    static int save(lua_State* L) {
        GameClient& c = client(L);
        const std::string n = luaL_checkstring(L, 1);
        if (!safeName(n)) return luaL_error(L, "storage: bad name '%s' (letters, digits, _ -)", n.c_str());
        luaL_checktype(L, 2, LUA_TTABLE);
        const std::string text = ui::luaToJson(L, 2).dump(2);
        if (text.size() > (4u << 20)) return luaL_error(L, "storage.save: '%s' is larger than 4 MB", n.c_str());
        std::error_code ec;
        std::filesystem::create_directories(c.saveDir(), ec);
        const std::filesystem::path file = saveFile(c, n);
        const std::string tmp = file.string() + ".tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            f << text;
            if (!f) { lua_pushboolean(L, 0); lua_pushstring(L, "write failed"); return 2; }
        }
        std::filesystem::rename(tmp, file, ec);   // atomic: a crash never leaves half a save
        if (ec) { lua_pushboolean(L, 0); lua_pushstring(L, ec.message().c_str()); return 2; }
        platform::syncUserData();
        lua_pushboolean(L, 1);
        return 1;
    }
    static int erase(lua_State* L) {
        const std::string n = luaL_checkstring(L, 1);
        if (!safeName(n)) return luaL_error(L, "storage: bad name '%s'", n.c_str());
        std::error_code ec;
        std::filesystem::remove(saveFile(client(L), n), ec);
        return 0;
    }

    // ------------------------------------------------------------------ store.*
    static int storeAvailable(lua_State* L) { lua_pushboolean(L, store::available()); return 1; }
    static int storeProvider(lua_State* L) { lua_pushstring(L, store::provider().c_str()); return 1; }
    // store.products(fn(list, error)): list = {{id, title, description, price, currency, type}, ...}
    static int storeProducts(lua_State* L) {
        auto cb = keep(L, 1);
        store::products([cb](bool ok, const std::vector<store::Product>& list, const std::string& err) {
            Json a = Json::array();
            for (auto& p : list) {
                Json j = Json::object();
                j.set("id", Json::string(p.id)); j.set("title", Json::string(p.title)); j.set("description", Json::string(p.description));
                j.set("price", Json::string(p.price)); j.set("currency", Json::string(p.currency));
                j.set("type", Json::string(p.type == store::ProductType::Consumable ? "consumable" : p.type == store::ProductType::NonConsumable ? "nonConsumable" : "subscription"));
                a.push(j);
            }
            Json e = ok ? Json() : Json::string(err);
            cb->call(a, &e);
        });
        return 0;
    }
    // store.buy(product, fn(purchase)): purchase = {product, state, token, error, restored}; after granting
    // the item call store.finish(token). meta.lua's shop.buyReal does both and never grants a token twice.
    static int storeBuy(lua_State* L) {
        GameClient& c = client(L);
        const std::string id = luaL_checkstring(L, 1);
        auto cb = keep(L, 2);
        auto alive = c.luaAlive_;
        GameClient* self = &c;
        store::purchase(id, [cb, alive, self](const store::Purchase& p) {
            if (*alive && !p.token.empty()) self->purchases_[p.token] = p;
            cb->call(purchaseJson(p));
        });
        return 0;
    }
    static int storeFinish(lua_State* L) {
        GameClient& c = client(L);
        const std::string token = luaL_checkstring(L, 1);
        auto it = c.purchases_.find(token);
        if (it == c.purchases_.end()) { lua_pushboolean(L, 0); return 1; }
        store::finish(it->second);
        c.purchases_.erase(it);
        lua_pushboolean(L, 1);
        return 1;
    }
    static int storeRestore(lua_State* L) {
        GameClient& c = client(L);
        auto cb = keep(L, 1);
        auto alive = c.luaAlive_;
        GameClient* self = &c;
        store::restore([cb, alive, self](bool ok, const std::vector<store::Purchase>& list, const std::string& err) {
            Json a = Json::array();
            for (auto& p : list) { if (*alive && !p.token.empty()) self->purchases_[p.token] = p; a.push(purchaseJson(p)); }
            Json e = ok ? Json() : Json::string(err);
            cb->call(a, &e);
        });
        return 0;
    }

    // ------------------------------------------------------------------ ads.*
    static ads::Format format(lua_State* L, int idx) {
        const std::string f = luaL_optstring(L, idx, "rewarded");
        return f == "interstitial" ? ads::Format::Interstitial : f == "banner" ? ads::Format::Banner : ads::Format::Rewarded;
    }
    static int adsAvailable(lua_State* L) { lua_pushboolean(L, ads::available()); return 1; }
    static int adsReady(lua_State* L) { lua_pushboolean(L, ads::ready(format(L, 1))); return 1; }
    static int adsPreload(lua_State* L) { ads::preload(format(L, 1)); return 0; }
    // ads.show("rewarded" | "interstitial", fn({shown, rewarded, error}) [, placement])
    static int adsShow(lua_State* L) {
        const ads::Format f = format(L, 1);
        auto cb = keep(L, 2);
        const std::string placement = luaL_optstring(L, 3, "");
        ads::show(f, [cb](const ads::ShowResult& r) {
            Json j = Json::object();
            j.set("shown", Json::fromBool(r.shown)); j.set("rewarded", Json::fromBool(r.rewarded));
            if (!r.error.empty()) j.set("error", Json::string(r.error));
            cb->call(j);
        }, placement);
        return 0;
    }
    static int adsBanner(lua_State* L) {
        if (lua_toboolean(L, 1)) ads::showBanner(lua_toboolean(L, 2)); else ads::hideBanner();
        return 0;
    }
    static int adsEnabled(lua_State* L) {
        if (!lua_isnone(L, 1)) ads::setEnabled(lua_toboolean(L, 1));
        lua_pushboolean(L, ads::enabled());
        return 1;
    }
};

const char* GameClient::screenName() const {
    switch (screen_) {
    case Screen::Menu: return "menu";
    case Screen::Lobby: return "lobby";
    case Screen::Playing: return "playing";
    case Screen::Paused: return "paused";
    default: return "over";
    }
}

std::string GameClient::saveDir() const {
    if (const char* d = std::getenv("RTS_UI_SAVE_DIR"); d && *d) return d;   // previews / tests: never the real save
    std::string id = manifest_.getStr("id", "game");
    for (char& ch : id) if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '.' || ch == '-' || ch == '_')) ch = '_';
    return platform::userDataDir() + "/" + id;
}

void GameClient::initLua() {
    lua_ = luaL_newstate();
    if (!lua_) return;
    luaAlive_ = std::make_shared<bool>(true);
    ui::luaSandbox(lua_, readContent, {"ui/", "engine/ui/lib/"});
    auto table = [&](const char* global, std::initializer_list<luaL_Reg> fns) {
        lua_newtable(lua_);
        for (auto& f : fns) {
            lua_pushlightuserdata(lua_, this);
            lua_pushcclosure(lua_, f.func, 1);
            lua_setfield(lua_, -2, f.name);
        }
        lua_setglobal(lua_, global);
    };
    table("game", {
        {"tr", LuaApi::tr}, {"hasText", LuaApi::hasText}, {"language", LuaApi::language}, {"languages", LuaApi::languages},
        {"setLanguage", LuaApi::setLanguage}, {"name", LuaApi::name}, {"maps", LuaApi::maps}, {"start", LuaApi::start},
        {"difficulty", LuaApi::difficulty}, {"screen", LuaApi::screen}, {"pause", LuaApi::pause}, {"resume", LuaApi::resume},
        {"menu", LuaApi::menu}, {"online", LuaApi::online}, {"howto", LuaApi::howto}, {"engineMenu", LuaApi::engineMenu},
        {"notice", LuaApi::notice}, {"safeArea", LuaApi::safeArea}, {"lastResult", LuaApi::lastResult}, {"touch", LuaApi::touch},
        {"platform", LuaApi::platformName}, {"preview", LuaApi::preview}, {"time", LuaApi::time}, {"json", LuaApi::json}, {"decode", LuaApi::decode}, {"encode", LuaApi::encode},
        {"log", LuaApi::log}});
    table("storage", {{"load", LuaApi::load}, {"save", LuaApi::save}, {"erase", LuaApi::erase}});
    table("store", {{"available", LuaApi::storeAvailable}, {"provider", LuaApi::storeProvider}, {"products", LuaApi::storeProducts},
                    {"buy", LuaApi::storeBuy}, {"finish", LuaApi::storeFinish}, {"restore", LuaApi::storeRestore}});
    table("ads", {{"available", LuaApi::adsAvailable}, {"ready", LuaApi::adsReady}, {"preload", LuaApi::adsPreload},
                  {"show", LuaApi::adsShow}, {"banner", LuaApi::adsBanner}, {"enabled", LuaApi::adsEnabled}});
    lua_pushlightuserdata(lua_, this);
    lua_pushcclosure(lua_, LuaApi::log, 1);
    lua_setglobal(lua_, "print");   // print -> the client log
    ui_.attachLua(lua_);
    // purchases that complete outside store.buy (pending -> purchased, after a restart)
    auto alive = luaAlive_;
    store::onPurchase([this, alive](const store::Purchase& p) {
        if (!*alive) return;
        if (!p.token.empty()) purchases_[p.token] = p;
        luaEmit("purchase", purchaseJson(p));
    });
}

// Calls game._emit(name, data) (ui/lib/meta.lua dispatches to game.on handlers).
void GameClient::luaEmit(const char* name, const Json& data) {
    if (!lua_) return;
    lua_getglobal(lua_, "game");
    if (lua_istable(lua_, -1)) {
        lua_getfield(lua_, -1, "_emit");
        if (lua_isfunction(lua_, -1)) {
            lua_pushstring(lua_, name);
            ui::luaPushJson(lua_, data);
            if (lua_pcall(lua_, 2, 0, 0) != LUA_OK) { std::fprintf(stderr, "[lua] %s: %s\n", name, lua_tostring(lua_, -1)); lua_pop(lua_, 1); }
        } else lua_pop(lua_, 1);
    }
    lua_pop(lua_, 1);
}

void GameClient::luaTick(double dt) {
    if (!lua_) return;
    lua_getglobal(lua_, "game");
    if (lua_istable(lua_, -1)) {
        lua_getfield(lua_, -1, "_tick");
        if (lua_isfunction(lua_, -1)) {
            lua_pushnumber(lua_, dt);
            if (lua_pcall(lua_, 1, 0, 0) != LUA_OK) { std::fprintf(stderr, "[lua] tick: %s\n", lua_tostring(lua_, -1)); lua_pop(lua_, 1); }
        } else lua_pop(lua_, 1);
    }
    lua_pop(lua_, 1);
}

void GameClient::runGameScript() {
    if (!lua_) return;
    std::string err;
    if (!ui::luaRunFile(lua_, readContent, "engine/ui/lib/meta.lua", &err)) std::fprintf(stderr, "[lua] %s\n", err.c_str());
    initScriptUi();   // ClientScriptUi.cpp: data model "script" for view scripts (before ui/main.lua: games may extend it)
    std::string text;
    if (platform::readContentFile("ui/main.lua", text) && !ui::luaRunString(lua_, text, "@ui/main.lua", &err))
        std::fprintf(stderr, "[lua] %s\n", err.c_str());
    // ui/screens.json {"start": "home"}: the game's screens replace the engine menu (meta.lua ui.boot)
    std::string boot = "if ui and ui.boot then ui.boot(" + std::string(uiPreview_ ? "true" : "false") + ") end";
    if (!ui::luaRunString(lua_, boot, "=boot", &err)) std::fprintf(stderr, "[lua] %s\n", err.c_str());
    if (!uiPreviewLua_.empty() && !ui::luaRunString(lua_, uiPreviewLua_, "=preview", &err)) std::fprintf(stderr, "[lua] preview: %s\n", err.c_str());
}

void GameClient::shutdownLua() {   // after the UI: RmlUi's documents hold references into the state
    if (luaAlive_) *luaAlive_ = false;
    purchases_.clear();
    if (lua_) lua_close(lua_);
    lua_ = nullptr;
}

} // namespace rts::client
