// Agent operations for a game's own screens (meta menus: home, levels, shop, profile, settings, popups):
// ui/screens/<name>.rml documents driven by Lua (engine/assets/ui/lib/meta.lua, docs/ui-scripting.md).
//   ui_api            the reference an agent needs before writing screens
//   ui_screen_list    screens, screens.json, profile / shop data of the open game
//   ui_screen_create  a screen from a template (engine/ui/templates) + the data files it needs
//   ui_screen_delete  removes a screen
//   ui_screens_config start screen, screen after a match, popups (ui/screens.json)
//   ui_validate       Lua syntax, screen links, missing translations, JSON files
//   ui_preview        renders one screen (or the game's start) in the real client on a device size -> PNG
// Everything stays inside the open game's folder; engine files are only read.
#include "UiOps.h"
#include "BrandingOps.h"            // base64Encode
#include "apps/editor/Devices.h"
#include "tools/branding/BrandImage.h"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace rts::editor {

namespace {

Json str(const std::string& s) { return Json::string(s); }

std::string readText(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}

void writeText(const fs::path& p, const std::string& text) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << text;
    if (!f) throw std::runtime_error("cannot write " + p.string());
}

const fs::path& requireGame(const Workspace& ws, fs::path& out) {
    if (ws.project.dir.empty()) throw std::runtime_error("no game is open (open_game)");
    out = fs::path(ws.project.dir);
    return out;
}

bool screenName(const std::string& n) {
    if (n.empty() || n.size() > 48) return false;
    for (char c : n) if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')) return false;
    return true;
}

fs::path templatesDir(const Workspace& ws) { return fs::path(ws.engineDir) / "engine" / "ui" / "templates"; }

std::vector<std::string> templateNames(const Workspace& ws) {
    std::vector<std::string> out;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(templatesDir(ws), ec))
        if (e.path().extension() == ".rml") out.push_back(e.path().stem().string());
    std::sort(out.begin(), out.end());
    return out;
}

Json loadJsonOr(const fs::path& p, Json def) {
    if (!fs::exists(p)) return def;
    try { return Json::parse(readText(p)); } catch (const std::exception&) { return def; }
}

// Data files a template relies on; written only when the game has none yet.
const char* kProfileDefaults = R"({
  "name": "Player",
  "level": 1,
  "xp": 0,
  "gold": 100,
  "gems": 5,
  "stats": {"matches": 0, "wins": 0},
  "stars": {}
}
)";
const char* kShopDefaults = R"({
  "items": [
    {"id": "gold_pack", "name": "Gold pack", "description": "+300 gold", "price": {"gems": 3}, "give": {"gold": 300}},
    {"id": "banner", "name": "Banner", "description": "A banner for your castle", "price": {"gold": 100}, "give": {"cosmetics.banner": true}, "once": true}
  ],
  "products": [
    {"id": "gems_100", "name": "100 gems", "give": {"gems": 100}}
  ],
  "rewarded": [
    {"id": "free_gold", "name": "Free gold", "description": "+50 gold", "give": {"gold": 50}, "cooldown": 600}
  ]
}
)";

// ---- validation helpers
std::string luaSyntax(const std::string& code, const std::string& name) {
    lua_State* L = luaL_newstate();
    std::string err;
    if (luaL_loadbufferx(L, code.data(), code.size(), name.c_str(), "t") != LUA_OK) err = lua_tostring(L, -1);
    lua_close(L);
    return err;
}

// keys of a language file (nested objects -> dotted keys), engine or game
void collectKeys(const Json& j, const std::string& prefix, std::set<std::string>& out) {
    if (!j.isObject()) return;
    for (auto& [k, v] : j.members()) {
        if (!k.empty() && k[0] == '@') continue;
        const std::string key = prefix.empty() ? k : prefix + "." + k;
        // plural forms {"one": .., "other": ..} are one key
        if (v.isObject() && (v.has("other") || v.has("one"))) { out.insert(key); continue; }
        if (v.isObject()) collectKeys(v, key, out); else out.insert(key);
    }
}

std::set<std::string> languageKeys(const Workspace& ws, const std::string& lang) {
    std::set<std::string> keys;
    for (fs::path p : {fs::path(ws.engineDir) / "engine/assets/lang" / (lang + ".json"), fs::path(ws.project.dir) / "lang" / (lang + ".json")}) {
        if (!fs::exists(p)) continue;
        try { collectKeys(Json::parse(readText(p)), "", keys); } catch (const std::exception&) {}
    }
    return keys;
}

std::vector<std::string> gameLanguages(const Workspace& ws) {
    std::vector<std::string> out;
    for (auto& l : ws.project.manifest["languages"].items()) out.push_back(l.str());
    if (out.empty()) out.push_back(ws.project.manifest.getStr("sourceLanguage", "en"));
    return out;
}

// directory that holds the engine assets as "engine/..." next to the client (the client reads them
// relative to its working directory when RTS_CONTENT points at a project folder)
fs::path engineAssetsRoot(const Workspace& ws, const std::string& exe) {
    // a folder with only engine/ in it: the client's own content folder also holds a sample game, whose
    // files would fill in for what the project lacks (audio.json, fx) through the relative-path fallback
    fs::path src = fs::path(ws.engineDir) / "engine" / "assets";
    const fs::path dir = fs::path(exe).parent_path();
    for (fs::path c : {dir / "content" / "engine", dir.parent_path() / "Resources" / "content" / "engine"}) {
        std::error_code ec;
        if (fs::exists(c / "ui" / "theme.rcss", ec)) { src = c; break; }   // the client's copy matches its code
    }
    const fs::path root = fs::temp_directory_path() / "rts-ui-preview";
    std::error_code ec;
    fs::create_directories(root, ec);
    fs::copy(src, root / "engine", fs::copy_options::recursive | fs::copy_options::update_existing, ec);
    return root;
}

bool readPpm(const fs::path& p, branding::Image& img) {
    std::ifstream f(p, std::ios::binary);
    std::string magic;
    int w = 0, h = 0, maxv = 0;
    if (!(f >> magic >> w >> h >> maxv) || magic != "P6" || w <= 0 || h <= 0) return false;
    f.get();
    std::vector<unsigned char> rgb(size_t(w) * size_t(h) * 3);
    if (!f.read(reinterpret_cast<char*>(rgb.data()), std::streamsize(rgb.size()))) return false;
    img.width = w; img.height = h;
    img.rgba.resize(size_t(w) * size_t(h) * 4);
    for (size_t i = 0, n = size_t(w) * size_t(h); i < n; ++i) {
        img.rgba[i * 4] = rgb[i * 3]; img.rgba[i * 4 + 1] = rgb[i * 3 + 1]; img.rgba[i * 4 + 2] = rgb[i * 3 + 2]; img.rgba[i * 4 + 3] = 255;
    }
    return true;
}

std::string shellArg(const std::string& s) {
#if defined(_WIN32)
    std::string out = "\"";
    for (char c : s) { if (c == '"') out += "\\\""; else out += c; }
    return out + "\"";
#else
    std::string out = "'";
    for (char c : s) { if (c == '\'') out += "'\\''"; else out += c; }
    return out + "'";
#endif
}

std::string runCapture(const std::string& cmd, int& rc) {
#if defined(_WIN32)
    FILE* pipe = _popen(cmd.c_str(), "r");
#else
    FILE* pipe = popen(cmd.c_str(), "r");
#endif
    if (!pipe) { rc = -1; return {}; }
    std::string out;
    char buf[512];
    while (std::fgets(buf, sizeof buf, pipe)) out += buf;
#if defined(_WIN32)
    rc = _pclose(pipe);
#else
    rc = pclose(pipe);
#endif
    return out;
}

const char* kApiSummary = R"J({
  "files": {
    "ui/screens/<name>.rml": "one screen (RmlUi HTML/CSS); links ../theme.rcss, ../screens.rcss, ../skin.rcss; <div class=\"root\" data-model=\"meta\"> around the content",
    "ui/screens.json": "{\"start\": \"home\", \"afterMatch\": \"result\", \"screens\": {\"dialog\": {\"popup\": true}}}: start replaces the engine main menu",
    "ui/profile.json": "default player profile (any JSON): currencies, level, unlocks; saved per player",
    "ui/shop.json": "{items: [{id, name, description, icon, price: {gold: 100}, give: {gems: 10, \"skins.red\": true}, once}], products: [{id (game.json store.products), name, give}], rewarded: [{id, name, give, cooldown (s)}]}",
    "ui/main.lua": "optional: runs once at start (globals, event handlers); require('x') loads ui/x.lua",
    "lang/<language>.json": "texts: \"@key\" in documents (whole text node or attribute), game.tr('key', {n = 1}) in Lua"
  },
  "dataModel meta": {
    "profile": "the profile table: {{profile.gold}}, {{profile.stats.wins}}",
    "params": "parameters of the current screen (ui.open(name, params))", "screen": "current screen name",
    "shop": "rows of shop.list(): id, kind (item|product|rewarded), name, description, icon, priceText, owned, affordable, ready",
    "game": "{name, maps, language, touch}", "state": "free table; ui.set(key, value) adds any top-level key ({{key}})",
    "call(fn, ...)": "data-event-click=\"call('shop.buy', it.id)\": calls a global Lua function by dotted name",
    "open(name)": "data-event-click=\"open('shop')\"", "back()": "data-event-click=\"back()\""
  },
  "lua": {
    "ui": "open(name, params), back(), replace(name, params), reset(name, params), home(), popup(name, params), closePopup(name?), current(), root(), params(name?), document(name?), isOpen(name), set(key, value), get(key), refresh(key), screen(name, {open = fn(params, doc), close = fn(doc), back = fn() -> handled}), start(name)",
    "profile": "get(path, default), set(path, v), add(path, n), can(costs), spend(costs) -> ok, missing, give(gifts), merge(t), reset(), save(), data; paths are dotted ('stats.wins')",
    "shop": "list(kind?), buy(id) -> ok, reason, buyReal(productId, fn(ok, state)), watch(rewardedId, fn(ok)), restore(fn), cooldown(id)",
    "game": "tr(key, args), hasText(key), language(), languages(), setLanguage(tag), name(), maps(), start(i, {difficulty, spectate}), difficulty(n?), screen(), pause(), resume(), menu(), online(), howto(open?), engineMenu(on?), notice(text), lastResult(), touch(), platform(), preview(), time(), json('ui/x.json'), decode(text), encode(v), log(...), on(event, fn), off(event, fn)",
    "events": "screen (menu|lobby|playing|paused|over), open / close {screen}, matchEnd {won, seconds, map, mapTitle, difficulty, spectator, online}, language, purchase, profile, shop {id, ok, reason}, update (dt), back",
    "storage": "load(name) -> table, save(name, table), erase(name) (per game, user folder)",
    "store": "available(), provider(), products(fn(list, err)), buy(id, fn(p)), finish(token), restore(fn(list, err)) — shop.buyReal wraps them",
    "ads": "available(), ready(fmt), preload(fmt), show('rewarded'|'interstitial', fn({shown, rewarded, error}), placement), banner(show, top), enabled(on?)",
    "rmlui": "RmlUi Lua API: document:GetElementById(id).inner_rml = '...', element:SetClass(c, on), element.style.color, ...",
    "sandbox": "no io / os.execute / package / debug / dofile; os.time / os.date / os.clock are there"
  },
  "events in documents": "onclick=\"ui.open('shop')\" (Lua; element, event, document are set); data-event-click=\"call('fn', it.id)\" inside data-for",
  "css": "screens.rcss: .screen > .screen-header (.screen-title, button.back) + .screen-body + .screen-footer; .currencies .currency; .tabs; .cards .card (.card-title .card-text .card-icon); .list .list-row(.locked); .stats .stat (.stat-value .stat-label); .progress-bar > .progress-fill; .badge; .avatar; button.primary / .secondary / .big; .popup-bg > .popup.panel; .row .grow .center .muted .spacer. Portrait: @media (orientation: portrait). Phones: .touch on the root.",
  "rmlui limits": "flexbox and data bindings work; no box-shadow / filter / linear-gradient: decorator: vertical-gradient(#a #b) | horizontal-gradient; images: <img src=\"../../images/x.png\"/> (asset_import); fonts: fonts/ui.ttf, fonts/title.ttf (family title)",
  "workflow": "ui_screen_create {name, template} -> game_file_read / game_file_write ui/screens/<name>.rml -> ui_validate -> ui_preview {screen, device} (look at it) -> repeat"
})J";

} // namespace

void registerUiOps(Workspace& ws) {
    ws.registerOp({"ui_api",
        "Reference for building the game's own screens (meta menus: home, levels, shop, profile, settings, rewards, "
        "popups): files, the Lua API (ui / profile / shop / game / store / ads), the data model \"meta\" bound in "
        "documents, CSS classes of screens.rcss, events and templates. Read it before ui_screen_create / game_file_write.",
        Json::parse(R"({"properties": {"full": {"type": "boolean", "description": "also return docs/ui-scripting.md"}}})"),
        [&ws](const Json& p) {
            Json r = Json::parse(kApiSummary);
            Json t = Json::array();
            for (auto& n : templateNames(ws)) t.push(str(n));
            r.set("templates", t);
            if (p.getBool("full")) r.set("doc", str(readText(fs::path(ws.engineDir) / "docs" / "ui-scripting.md")));
            r.set("summary", str("UI API: экраны ui/screens/*.rml + Lua (ui, profile, shop, game, store, ads)"));
            return r;
        }});

    ws.registerOp({"ui_screen_list",
        "The game's own screens: ui/screens/*.rml (name, title, size), ui/screens.json (start, afterMatch, popups), "
        "whether ui/profile.json, ui/shop.json, ui/main.lua exist, and the templates ui_screen_create can use.",
        Json::parse(R"({"properties": {}})"),
        [&ws](const Json&) {
            fs::path g; requireGame(ws, g);
            Json screens = Json::array();
            std::error_code ec;
            std::vector<fs::path> files;
            for (auto& e : fs::directory_iterator(g / "ui" / "screens", ec))
                if (e.path().extension() == ".rml") files.push_back(e.path());
            std::sort(files.begin(), files.end());
            for (auto& f : files) {
                const std::string text = readText(f);
                std::smatch m;
                Json s = Json::object();
                s.set("name", str(f.stem().string()));
                s.set("path", str("ui/screens/" + f.filename().string()));
                if (std::regex_search(text, m, std::regex("<title>([^<]*)</title>"))) s.set("title", str(m[1].str()));
                s.set("bytes", Json::number(int64_t(text.size())));
                screens.push(s);
            }
            Json r = Json::object();
            r.set("screens", screens);
            r.set("config", loadJsonOr(g / "ui" / "screens.json", Json::object()));
            Json data = Json::object();
            for (const char* f : {"ui/profile.json", "ui/shop.json", "ui/main.lua"}) data.set(f, Json::fromBool(fs::exists(g / f)));
            r.set("files", data);
            Json t = Json::array();
            for (auto& n : templateNames(ws)) t.push(str(n));
            r.set("templates", t);
            r.set("summary", str("Экранов: " + std::to_string(screens.items().size())));
            return r;
        }});

    ws.registerOp({"ui_screen_create",
        "Create a screen ui/screens/<name>.rml from a template (blank, home, levels, shop, profile, settings, result, "
        "dialog — ui_api lists them). Also writes what it needs when the game has none yet: ui/screens.json (the first "
        "screen or start:true becomes the start screen, replacing the engine main menu; template result becomes "
        "afterMatch; popup:true or template dialog registers a popup), ui/profile.json, ui/shop.json. Then edit the "
        "file with game_file_write and check it with ui_preview.",
        Json::parse(R"({"properties": {"name": {"type": "string", "description": "letters, digits, _ -"},
            "template": {"type": "string"}, "title": {"type": "string", "description": "header text or @lang.key"},
            "start": {"type": "boolean"}, "popup": {"type": "boolean"}, "overwrite": {"type": "boolean"}}, "required": ["name"]})"),
        [&ws](const Json& p) {
            fs::path g; requireGame(ws, g);
            const std::string name = p.getStr("name"), tpl = p.getStr("template", "blank");
            if (!screenName(name)) throw std::runtime_error("screen name: letters, digits, '_' or '-' (got '" + name + "')");
            const fs::path src = templatesDir(ws) / (tpl + ".rml");
            if (!fs::exists(src)) throw std::runtime_error("unknown template '" + tpl + "' (ui_api lists them)");
            const fs::path dst = g / "ui" / "screens" / (name + ".rml");
            if (fs::exists(dst) && !p.getBool("overwrite")) throw std::runtime_error("ui/screens/" + name + ".rml exists (overwrite: true replaces it)");
            std::string text = readText(src);
            std::string title = p.getStr("title");
            if (title.empty()) {
                static const std::map<std::string, std::string> kTitles = {{"home", "@meta.play"}, {"levels", "@meta.play"}, {"shop", "@meta.shop"},
                                                                           {"profile", "@meta.profile"}, {"settings", "@meta.settings"}};
                auto it = kTitles.find(tpl);
                title = it != kTitles.end() ? it->second : name;
            }
            for (auto [from, to] : {std::pair<std::string, std::string>{"$NAME", name}, {"$TITLE", title}})
                for (size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size())) text.replace(at, from.size(), to);
            writeText(dst, text);
            Json created = Json::array();
            created.push(str("ui/screens/" + name + ".rml"));

            Json cfg = loadJsonOr(g / "ui" / "screens.json", Json::object());
            bool cfgChanged = !fs::exists(g / "ui" / "screens.json");
            const bool popup = p.getBool("popup", tpl == "dialog");
            if (popup) { cfg.member("screens").member(name).set("popup", Json::fromBool(true)); cfgChanged = true; }
            if (!popup && tpl != "result" && (p.getBool("start") || !cfg.has("start"))) { cfg.set("start", str(name)); cfgChanged = true; }
            if (tpl == "result" && !cfg.has("afterMatch")) { cfg.set("afterMatch", str(name)); cfgChanged = true; }
            if (cfgChanged) { writeText(g / "ui" / "screens.json", cfg.dump(2) + "\n"); created.push(str("ui/screens.json")); }
            if (tpl != "blank" && tpl != "dialog" && !fs::exists(g / "ui" / "profile.json")) { writeText(g / "ui" / "profile.json", kProfileDefaults); created.push(str("ui/profile.json")); }
            if (tpl == "shop" && !fs::exists(g / "ui" / "shop.json")) { writeText(g / "ui" / "shop.json", kShopDefaults); created.push(str("ui/shop.json")); }
            ws.log("mcp", "Экран " + name + " (" + tpl + ")");
            Json r = Json::object();
            r.set("ok", Json::fromBool(true));
            r.set("created", created);
            r.set("config", cfg);
            r.set("summary", str("Создан экран " + name + " из шаблона " + tpl));
            return r;
        }});

    ws.registerOp({"ui_screen_delete",
        "Delete ui/screens/<name>.rml (and its entries in ui/screens.json).",
        Json::parse(R"({"properties": {"name": {"type": "string"}}, "required": ["name"]})"),
        [&ws](const Json& p) {
            fs::path g; requireGame(ws, g);
            const std::string name = p.getStr("name");
            if (!screenName(name)) throw std::runtime_error("bad screen name");
            const fs::path f = g / "ui" / "screens" / (name + ".rml");
            if (!fs::exists(f)) throw std::runtime_error("no screen '" + name + "'");
            fs::remove(f);
            if (fs::exists(g / "ui" / "screens.json")) {
                Json cfg = loadJsonOr(g / "ui" / "screens.json", Json::object());
                if (cfg.getStr("start") == name) cfg.erase("start");
                if (cfg.getStr("afterMatch") == name) cfg.erase("afterMatch");
                if (cfg.has("screens")) cfg.member("screens").erase(name);
                writeText(g / "ui" / "screens.json", cfg.dump(2) + "\n");
            }
            Json r = Json::object();
            r.set("ok", Json::fromBool(true));
            r.set("summary", str("Удалён экран " + name));
            return r;
        }});

    ws.registerOp({"ui_screens_config",
        "Edit ui/screens.json: start (first screen; replaces the engine main menu; \"\" removes it and brings the "
        "engine menu back), afterMatch (screen opened after every match with params.result), popup screens.",
        Json::parse(R"({"properties": {"start": {"type": "string"}, "afterMatch": {"type": "string"},
            "popups": {"type": "array", "items": {"type": "string"}}}})"),
        [&ws](const Json& p) {
            fs::path g; requireGame(ws, g);
            Json cfg = loadJsonOr(g / "ui" / "screens.json", Json::object());
            auto exists = [&](const std::string& n) { return fs::exists(g / "ui" / "screens" / (n + ".rml")); };
            for (const char* k : {"start", "afterMatch"}) {
                if (!p.has(k)) continue;
                const std::string v = p.getStr(k);
                if (v.empty()) { cfg.erase(k); continue; }
                if (!exists(v)) throw std::runtime_error(std::string(k) + ": no screen '" + v + "'");
                cfg.set(k, str(v));
            }
            if (p.has("popups")) {
                Json& sc = cfg.member("screens");
                for (auto& [n, v] : sc.membersMut()) if (v.isObject()) v.erase("popup");
                for (auto& n : p["popups"].items()) {
                    if (!exists(n.str())) throw std::runtime_error("popups: no screen '" + n.str() + "'");
                    sc.member(n.str()).set("popup", Json::fromBool(true));
                }
            }
            writeText(g / "ui" / "screens.json", cfg.dump(2) + "\n");
            Json r = Json::object();
            r.set("config", cfg);
            r.set("summary", str("ui/screens.json обновлён"));
            return r;
        }});

    ws.registerOp({"ui_validate",
        "Check the game's screens and UI scripts: Lua syntax of ui/**/*.lua and every <script> in ui/**/*.rml, screens "
        "named in ui.open / popup / replace / reset / start and screens.json exist, \"@key\" and game.tr('key') texts exist "
        "in every language of the game (engine keys count), JSON data files parse. Returns issues (empty = fine).",
        Json::parse(R"({"properties": {}})"),
        [&ws](const Json&) {
            fs::path g; requireGame(ws, g);
            Json issues = Json::array();
            auto issue = [&](const std::string& file, const std::string& text) {
                Json i = Json::object(); i.set("file", str(file)); i.set("issue", str(text)); issues.push(i);
            };
            std::set<std::string> screens, usedScreens, usedKeys;
            std::map<std::string, std::string> keyFile;
            std::error_code ec;
            for (auto& e : fs::directory_iterator(g / "ui" / "screens", ec))
                if (e.path().extension() == ".rml") screens.insert(e.path().stem().string());
            const std::regex reOpen(R"(ui\.(?:open|popup|replace|reset|start|preview)\s*\(\s*['"]([A-Za-z0-9_-]+)['"])");
            const std::regex reDataOpen(R"(open\(\s*'([A-Za-z0-9_-]+)')");
            const std::regex reTr(R"(game\.tr\(\s*['"]([A-Za-z0-9_.-]+)['"])");
            const std::regex reAt(R"([>"']\s*@([A-Za-z][A-Za-z0-9_.-]*)\s*[<"'])");
            const std::regex reScript(R"(<script>([\s\S]*?)</script>)");
            const std::regex reEvent(R"#(\son[a-z]+="([^"]*)")#");
            auto scan = [&](const std::string& rel, const std::string& text) {
                for (std::sregex_iterator it(text.begin(), text.end(), reOpen), end; it != end; ++it) usedScreens.insert((*it)[1]);
                for (std::sregex_iterator it(text.begin(), text.end(), reDataOpen), end; it != end; ++it) usedScreens.insert((*it)[1]);
                for (auto* re : {&reTr, &reAt})
                    for (std::sregex_iterator it(text.begin(), text.end(), *re), end; it != end; ++it) { usedKeys.insert((*it)[1]); keyFile.emplace((*it)[1], rel); }
            };
            std::vector<fs::path> files;
            for (auto& e : fs::recursive_directory_iterator(g / "ui", ec)) if (e.is_regular_file()) files.push_back(e.path());
            std::sort(files.begin(), files.end());
            for (auto& f : files) {
                const std::string rel = f.lexically_relative(g).generic_string(), ext = f.extension().string(), text = readText(f);
                if (ext == ".lua") {
                    if (auto err = luaSyntax(text, "@" + rel); !err.empty()) issue(rel, "Lua: " + err);
                    scan(rel, text);
                } else if (ext == ".rml") {
                    int n = 0;
                    for (std::sregex_iterator it(text.begin(), text.end(), reScript), end; it != end; ++it, ++n) {
                        // line of the script for readable errors
                        const int line = int(std::count(text.begin(), text.begin() + it->position(1), '\n'));
                        if (auto err = luaSyntax(std::string(size_t(line), '\n') + (*it)[1].str(), "@" + rel); !err.empty()) issue(rel, "Lua <script>: " + err);
                    }
                    for (std::sregex_iterator it(text.begin(), text.end(), reEvent), end; it != end; ++it) {
                        std::string code = (*it)[1];
                        for (auto [a, b] : {std::pair<std::string, std::string>{"&quot;", "\""}, {"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}})
                            for (size_t at = code.find(a); at != std::string::npos; at = code.find(a, at + b.size())) code.replace(at, a.size(), b);
                        if (auto err = luaSyntax(code, "=" + rel + " " + (*it)[0].str().substr(1, (*it)[0].str().find('=') - 1)); !err.empty()) issue(rel, "Lua event: " + err);
                    }
                    if (text.find("data-model=") == std::string::npos && rel.rfind("ui/screens/", 0) == 0)
                        issue(rel, "no data-model: put <div class=\"root\" data-model=\"meta\"> around the content");
                    scan(rel, text);
                } else if (ext == ".json") {
                    try { Json::parse(text); } catch (const std::exception& e) { issue(rel, std::string("JSON: ") + e.what()); }
                }
            }
            Json cfg = loadJsonOr(g / "ui" / "screens.json", Json::object());
            for (const char* k : {"start", "afterMatch"}) if (cfg.has(k)) usedScreens.insert(cfg.getStr(k));
            for (auto& s : usedScreens) if (!screens.count(s)) issue("ui", "screen '" + s + "' is used but ui/screens/" + s + ".rml does not exist");
            for (auto& lang : gameLanguages(ws)) {
                const auto keys = languageKeys(ws, lang);
                for (auto& k : usedKeys) if (!keys.count(k)) issue(keyFile[k], "text '" + k + "' is missing in lang/" + lang + ".json");
            }
            Json r = Json::object();
            r.set("issues", issues);
            r.set("screens", Json::number(int64_t(screens.size())));
            r.set("summary", str(issues.items().empty() ? "UI: ошибок нет" : "UI: замечаний " + std::to_string(issues.items().size())));
            return r;
        }});

    ws.registerOp({"ui_preview",
        "Render a screen of the game in the real game client and return a PNG: the game's start screen, or "
        "'screen' (popups open over the start screen) with 'params' (ui.open parameters) and 'profile' (fields merged "
        "into a scratch profile: the player's save is never touched). Size: 'device' (ui_api / device_list ids, e.g. "
        "iphone-x, pixel-7) + 'landscape' (the device's safe area is applied and its cut-out drawn), or width / height; 'lang'. 'lua' runs after the screen opens (switch a tab, "
        "fill a list). Returns the image and the client log (Lua errors, RmlUi warnings). Needs a display (desktop).",
        Json::parse(R"({"properties": {"screen": {"type": "string"}, "params": {"type": "object"}, "profile": {"type": "object"},
            "device": {"type": "string"}, "landscape": {"type": "boolean"}, "width": {"type": "integer"}, "height": {"type": "integer"},
            "lang": {"type": "string"}, "lua": {"type": "string"}, "frames": {"type": "integer"},
            "frame": {"type": "boolean", "description": "draw the device cut-out and home indicator, default true"}}})"),
        [&ws](const Json& p) {
            fs::path g; requireGame(ws, g);
            if (ws.gameExe.empty() || !fs::exists(ws.gameExe)) throw std::runtime_error("game client (RTSGame) not found next to the editor / rtsmcp");
            int w = int(p["width"].integer(0)), h = int(p["height"].integer(0));
            std::string devName;
            if (p.has("device")) {
                const DeviceProfile* d = findDevice(p.getStr("device"));
                if (!d || d->w <= 0) throw std::runtime_error("unknown device '" + p.getStr("device") + "' (device_list)");
                w = d->w; h = d->h; devName = d->name;
                const bool portraitNative = d->w < d->h;
                if (p.getBool("landscape") == portraitNative) std::swap(w, h);
            }
            if (w <= 0 || h <= 0) {
                const bool portrait = ws.project.manifest.getStr("orientation") == "portrait";
                w = portrait ? 390 : 1280; h = portrait ? 844 : 720;
            }
            w = std::clamp(w, 200, 3000); h = std::clamp(h, 200, 3000);
            const fs::path tmp = fs::temp_directory_path() / ("rts-ui-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            fs::create_directories(tmp / "save");
            const fs::path shot = tmp / "shot.ppm";
            std::string args = " --size " + std::to_string(w) + "x" + std::to_string(h);
            if (p.has("lang")) args += " --lang " + shellArg(p.getStr("lang"));
            if (p.has("screen")) {
                if (!screenName(p.getStr("screen"))) throw std::runtime_error("bad screen name");
                if (!fs::exists(g / "ui" / "screens" / (p.getStr("screen") + ".rml"))) throw std::runtime_error("no screen '" + p.getStr("screen") + "' (ui_screen_list)");
                args += " --ui-screen " + shellArg(p.getStr("screen"));
                if (p.has("params")) args += " --ui-params " + shellArg(p["params"].dump(-1));
                if (p.has("profile")) args += " --ui-profile " + shellArg(p["profile"].dump(-1));
                if (p.has("lua")) args += " --ui-lua " + shellArg(p.getStr("lua"));
            }
            // the device's safe area (notch, home indicator) reaches the screens as game.safeArea()
            Insets4 safe{};
            const DeviceProfile* dev = p.has("device") ? findDevice(p.getStr("device")) : nullptr;
            if (dev) safe = (w > h) == (dev->w < dev->h) ? dev->landscape : dev->portrait;
            char safeEnv[96];
            std::snprintf(safeEnv, sizeof safeEnv, "%g,%g,%g,%g", safe.left, safe.top, safe.right, safe.bottom);
            const int frames = int(std::clamp<int64_t>(p["frames"].integer(30), 5, 600));
            const fs::path cwd = engineAssetsRoot(ws, ws.gameExe);
            std::string cmd;
#if defined(_WIN32)
            _putenv_s("RTS_CONTENT", g.string().c_str());
            _putenv_s("RTS_UI_SAVE_DIR", (tmp / "save").string().c_str());
            _putenv_s("RTS_SCREENSHOT", shot.string().c_str());
            _putenv_s("RTS_EXIT_AFTER_FRAMES", std::to_string(frames).c_str());
            _putenv_s("RTS_SPLASH", "off");
            _putenv_s("RTS_AUDIO", "off");
            _putenv_s("RTS_UI_SAFE_AREA", safeEnv);
            cmd = "cd /d " + shellArg(cwd.string()) + " && " + shellArg(ws.gameExe) + args + " 2>&1";
#else
            cmd = "cd " + shellArg(cwd.string()) + " && RTS_CONTENT=" + shellArg(g.string()) + " RTS_UI_SAVE_DIR=" + shellArg((tmp / "save").string()) +
                  " RTS_SCREENSHOT=" + shellArg(shot.string()) + " RTS_EXIT_AFTER_FRAMES=" + std::to_string(frames) +
                  " RTS_SPLASH=off RTS_AUDIO=off RTS_UI_SAFE_AREA=" + shellArg(safeEnv) + " " + shellArg(ws.gameExe) + args + " 2>&1";
#endif
            int rc = 0;
            const std::string out = runCapture(cmd, rc);
            Json log = Json::array();
            std::istringstream ls(out);
            for (std::string line; std::getline(ls, line);)
                if (line.find("[lua]") != std::string::npos || line.find("[ui warning]") != std::string::npos || line.find("[ui error]") != std::string::npos ||
                    line.rfind("error", 0) == 0 || line.rfind("Error", 0) == 0)
                    if (log.items().size() < 60) log.push(str(line));
            branding::Image img;
            const bool ok = readPpm(shot, img);
            std::error_code ec;
            fs::remove_all(tmp, ec);
            if (!ok) throw std::runtime_error("the client did not produce a screenshot (exit " + std::to_string(rc) + "):\n" + out.substr(out.size() > 3000 ? out.size() - 3000 : 0));
            if (dev && p.getBool("frame", true)) {   // the cut-out and home indicator over the picture: what covers the UI
                const splash::Color black{0, 0, 0, 255}, bar{235, 235, 235, 255};
                const bool landscape = w > h;
                const float k = float(img.width) / float(w);   // points -> screenshot pixels (Retina: 2)
                auto rect = [&](float x, float y, float rw, float rh, splash::Color c) {
                    branding::fillRect(img, int(x * k), int(y * k), std::max(1, int(rw * k)), std::max(1, int(rh * k)), c);
                };
                if (dev->cutout != Cutout::None && dev->cutW > 0) {
                    const float cw = dev->cutW, ch = std::max(dev->cutH, 8.0f), cy = dev->cutY;
                    if (!landscape) rect((float(w) - cw) / 2, cy, cw, ch, black);
                    else rect(cy, (float(h) - cw) / 2, ch, cw, black);
                }
                if (dev->touch && (landscape ? dev->landscape.bottom : dev->portrait.bottom) > 0) {
                    const float bw = std::min(134.0f, float(w) / 3);
                    rect((float(w) - bw) / 2, float(h) - 9, bw, 5, bar);
                }
            }
            if (std::max(img.width, img.height) > 1600) {   // a reasonable MCP payload
                const float k = 1600.0f / float(std::max(img.width, img.height));
                img = branding::resize(img, std::max(1, int(img.width * k)), std::max(1, int(img.height * k)));
            }
            Json r = Json::object();
            r.set("png_base64", str(base64Encode(branding::encodePng(img))));
            r.set("width", Json::number(int64_t(w)));
            r.set("height", Json::number(int64_t(h)));
            r.set("log", log);
            r.set("summary", str("Превью " + (p.has("screen") ? "экрана " + p.getStr("screen") : std::string("стартового экрана")) +
                                 (devName.empty() ? "" : " на " + devName) + " " + std::to_string(w) + "×" + std::to_string(h)));
            return r;
        }});
}

} // namespace rts::editor
