// Agent operations for game data that has no dedicated op: any text data file of the game folder
// (lang/*.json, audio.json, tilesets, fx, ui/*.rml, ...) and the map's per-cell territory layer.
// Engine code is out of reach on purpose: paths are confined to the open game's folder.
#include "DataOps.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace rts::editor {

namespace {
Json str(const std::string& s) { return Json::string(s); }
Json okWith(const std::string& summary) { Json r = Json::object(); r.set("ok", Json::fromBool(true)); r.set("summary", str(summary)); return r; }

const char* kTextExt[] = {".json", ".rml", ".rcss", ".txt", ".md", ".csv", ".po", ".lua", ".js"};
constexpr size_t kMaxText = 2u << 20;

// "lang/ru.json" -> absolute path inside the game folder; throws for anything outside it.
fs::path gamePath(const Workspace& ws, const std::string& rel) {
    if (ws.project.dir.empty()) throw std::runtime_error("no game is open (open_game)");
    if (rel.empty()) throw std::runtime_error("'path' is required (relative to the game folder)");
    fs::path p = fs::path(rel).lexically_normal();
    if (p.is_absolute() || p.empty() || *p.begin() == "..") throw std::runtime_error("path must stay inside the game folder: " + rel);
    return fs::path(ws.project.dir) / p;
}

bool textFile(const fs::path& p) {
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return std::any_of(std::begin(kTextExt), std::end(kTextExt), [&](const char* e) { return ext == e; });
}

std::string readAll(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + p.string());
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}

int shapeInt(const Json& p, const char* k, int def = 0) { return p.has(k) ? int(std::lround(p[k].toDouble(def))) : def; }
} // namespace

void registerDataOps(Workspace& ws) {
    ws.registerOp({"game_file_read",
        "Read a text data file of the open game (path relative to the game folder): lang/ru.json, audio.json, "
        "tilesets/desert.json, ui/hud.rml, fx/*.json, ... Use rules_get / map_get for rules.json and maps.",
        Json::parse(R"({"properties": {"path": {"type": "string"}}, "required": ["path"]})"),
        [&ws](const Json& p) {
            fs::path f = gamePath(ws, p.getStr("path"));
            if (!textFile(f)) throw std::runtime_error("not a text data file (json, rml, rcss, txt, md, csv): " + p.getStr("path"));
            if (!fs::exists(f)) throw std::runtime_error("no such file: " + p.getStr("path") + " (asset_list shows the files)");
            if (fs::file_size(f) > kMaxText) throw std::runtime_error("file is larger than 2 MB");
            Json r = Json::object();
            r.set("path", str(p.getStr("path")));
            r.set("text", str(readAll(f)));
            return r;
        }});
    ws.registerOp({"game_file_write",
        "Create or replace a text data file of the open game (lang/*.json, audio.json, tilesets/*.json, fx/*.json, "
        "ui/*.rml|rcss, campaign texts). JSON files are checked before saving. rules.json, game.json and maps have "
        "their own ops (rules_set, set_manifest, map_*), which validate against the engine.",
        Json::parse(R"({"properties": {"path": {"type": "string"}, "text": {"type": "string"}, "json": {}},
            "required": ["path"]})"),
        [&ws](const Json& p) {
            const std::string rel = p.getStr("path");
            fs::path f = gamePath(ws, rel);
            if (!textFile(f)) throw std::runtime_error("only text data files can be written (json, rml, rcss, txt, md, csv)");
            fs::path norm = fs::path(rel).lexically_normal();
            if (norm == "rules.json" || norm == "game.json" || norm == fs::path(ws.project.rulesPath()).lexically_relative(ws.project.dir))
                throw std::runtime_error("use rules_set / rules_merge / set_manifest for " + rel);
            if (!ws.map.path.empty() && fs::path(ws.map.path).lexically_normal() == f.lexically_normal())
                throw std::runtime_error("this is the open map: edit it with the map ops and map_save");
            std::string text;
            if (p.has("json")) text = p["json"].dump(2) + "\n";
            else if (p.has("text")) text = p.getStr("text");
            else throw std::runtime_error("give 'text' or 'json'");
            if (text.size() > kMaxText) throw std::runtime_error("text is larger than 2 MB");
            if (f.extension() == ".json") {
                try { (void)Json::parse(text); }
                catch (const std::exception& e) { throw std::runtime_error(std::string("invalid JSON, not saved: ") + e.what()); }
            }
            std::error_code ec;
            fs::create_directories(f.parent_path(), ec);
            bool existed = fs::exists(f);
            {
                std::ofstream o(f, std::ios::binary);
                if (!o) throw std::runtime_error("cannot write " + rel);
                o << text;
            }
            ws.note({Change::Project, 0, 0, -1, -1, -1, (existed ? "Изменён файл " : "Создан файл ") + rel});
            Json r = okWith((existed ? "Изменён " : "Создан ") + rel);
            r.set("bytes", Json::number(int64_t(text.size())));
            return r;
        }});
    ws.registerOp({"territory_paint",
        "Paint the map's starting territory layer (dungeon / territory-war games): cells owned by player 1..9, "
        "0 clears. Shape: rect [x0,y0,x1,y1] or circle [cx,cy,r]. Only ground cells count in the game.",
        Json::parse(R"({"properties": {"player": {"type": "integer"},
            "rect": {"type": "array", "items": {"type": "integer"}}, "circle": {"type": "array", "items": {"type": "integer"}}},
            "required": ["player"]})"),
        [&ws](const Json& p) {
            MapData& d = ws.map.data;
            int pl = shapeInt(p, "player");
            if (pl < 0 || pl > 9) throw std::runtime_error("player must be 0..9");
            bool rect = p.has("rect");
            const Json& sh = rect ? p["rect"] : p["circle"];
            if (!rect && !p.has("circle")) throw std::runtime_error("give 'rect' or 'circle'");
            if (sh.size() < (rect ? 4u : 3u)) throw std::runtime_error(rect ? "rect is [x0,y0,x1,y1]" : "circle is [cx,cy,r]");
            auto v = [&](size_t i) { return int(std::lround(sh[i].toDouble())); };
            ws.map.beginEdit();
            if (!d.extra["territory"].isObject()) d.extra.set("territory", Json::object());
            Json* t = d.extra.find("territory");
            std::vector<std::string> rows(size_t(d.h), std::string(size_t(d.w), '.'));
            const Json& old = (*t)["rows"];
            for (size_t y = 0; y < rows.size() && y < old.size(); ++y) {
                std::string r = old[y].str();
                for (size_t x = 0; x < rows[y].size() && x < r.size(); ++x) rows[y][x] = r[x];
            }
            char c = pl == 0 ? '.' : char('0' + pl);
            int n = 0, x0, y0, x1, y1;
            if (rect) { x0 = std::min(v(0), v(2)); y0 = std::min(v(1), v(3)); x1 = std::max(v(0), v(2)); y1 = std::max(v(1), v(3)); }
            else { x0 = v(0) - v(2); y0 = v(1) - v(2); x1 = v(0) + v(2); y1 = v(1) + v(2); }
            for (int y = std::max(0, y0); y <= std::min(d.h - 1, y1); ++y)
                for (int x = std::max(0, x0); x <= std::min(d.w - 1, x1); ++x) {
                    if (!rect && (x - v(0)) * (x - v(0)) + (y - v(1)) * (y - v(1)) > v(2) * v(2) + v(2)) continue;
                    rows[size_t(y)][size_t(x)] = c; ++n;
                }
            Json arr = Json::array();
            for (auto& r : rows) arr.push(str(r));
            t->set("rows", arr);
            ws.map.touched();
            ws.note({Change::Cells, x0, y0, x1, y1, -1, pl ? "Территория игрока " + std::to_string(pl) : "Территория снята"});
            Json r = okWith("Территория: " + std::to_string(n) + " клеток → " + (pl ? "игрок " + std::to_string(pl) : "ничья"));
            return r;
        }});
    ws.registerOp({"build_estimate",
        "Estimated build size for a platform before building: runs rtsbuild --estimate (the real content pack with the "
        "game's settings: atlases, sound profile, packaging.files rules, split), returns pack size, atlas stats and the "
        "heaviest files. Per-file settings live in game.json packaging.files [{match, exclude, level (0 = store), maxSize, "
        "scale}] (packaging.targets.<t>.files for one platform), set with set_manifest.",
        Json::parse(R"({"properties": {"target": {"type": "string", "enum": ["macos", "ios", "android", "web", "windows", "linux"]},
            "top": {"type": "integer"}}, "required": ["target"]})"),
        [&ws](const Json& p) {
            if (ws.project.dir.empty()) throw std::runtime_error("no game is open (open_game)");
            const std::string target = p.getStr("target");
            const fs::path out = fs::path(ws.project.dir) / "Builds" / "_estimate";
            std::string cmd = "\"" + ws.buildTool + "\" --project \"" + ws.project.dir + "\" --engine \"" + ws.engineDir + "\" --target " + target +
                              " --estimate --out \"" + out.string() + "\" 2>&1";
#if defined(_WIN32)
            FILE* pipe = _popen(cmd.c_str(), "r");
#else
            FILE* pipe = popen(cmd.c_str(), "r");
#endif
            if (!pipe) throw std::runtime_error("cannot start rtsbuild");
            std::string log; char buf[512];
            while (std::fgets(buf, sizeof buf, pipe)) log += buf;
#if defined(_WIN32)
            int rc = _pclose(pipe);
#else
            int rc = pclose(pipe);
#endif
            if (rc != 0) throw std::runtime_error("rtsbuild --estimate failed:\n" + log.substr(log.size() > 3000 ? log.size() - 3000 : 0));
            Json rep = Json::parseFile((out / target / "size-report.json").string());
            Json r = Json::object();
            r.set("target", Json::string(target));
            r.set("pack", rep["pack"]);
            r.set("atlas", rep["atlas"]);
            if (rep.has("split")) r.set("split", rep["split"]);
            r.set("fileRules", rep["fileRules"]);
            Json files = Json::array();
            std::vector<const Json*> all;
            for (auto& f : rep["files"].items()) all.push_back(&f);
            std::stable_sort(all.begin(), all.end(), [](const Json* a, const Json* b) { return (*a)["storedBytes"].integer() > (*b)["storedBytes"].integer(); });
            const size_t top = size_t(std::clamp<int64_t>(p["top"].integer(20), 1, 500));
            for (size_t i = 0; i < all.size() && i < top; ++i) files.push(*all[i]);
            r.set("heaviest", files);
            r.set("summary", str("Оценка " + target + ": пак " + std::to_string(rep["pack"]["packBytes"].integer() / 1024) + " КБ"));
            return r;
        }});
    // The contract between the game client and its RmlUi documents: an agent that rewrites the UI
    // (game_file_write ui/hud.rml ...) needs the names the client binds. Kept next to
    // apps/client/ClientUi.cpp — update both together.
    ws.registerOp({"ui_contract",
        "How to replace or restyle the game UI: documents the client loads (the game's ui/<name> overrides "
        "engine/ui/<name>, also for linked .rcss / images / fonts), the data model \"game\" (variables, item fields), "
        "event callbacks and required element ids. Write files with game_file_write, images with asset_import.",
        Json::parse(R"({"properties": {}})"),
        [&ws](const Json&) {
            Json r = Json::parse(R"J({
  "documents": {"menu.rml": "main menu, settings, map list", "lobby.rml": "network lobby", "hud.rml": "in-game HUD, pause, game over",
                "keyboard.rml": "on-screen keyboard (phones)", "theme.rcss": "shared engine styles", "skin.rcss": "per-game skin (colours, fonts, frames): the usual file to override"},
  "override": "templates/<id>/ui/<file> replaces engine/ui/<file>; a document may link its own .rcss. Fonts: fonts/*.ttf of the game, @font-face via font-family in rcss.",
  "dataModel": "game",
  "variables": {
    "screen": "menu | lobby | playing | paused | over", "gameName": "string", "error": "string", "platformInfo": "string",
    "maps": "[string] map names (index = argument of start)", "languages": "[string]", "lang": "string",
    "top": "[item] resource bar", "topActions": "[action]", "clock": "string", "notices": "[item]",
    "hasSelection": "bool", "ownSelection": "bool", "selTitle": "string", "emptyHint": "string", "selInfo": "[string]",
    "card": "[action] command card (sections: header + kind list|row|grid|queue)", "showCard": "bool",
    "overTitle": "string", "overTime": "string", "modeHint": "string", "hintStyle": "css", "boxStyle": "css", "minimapStyle": "css",
    "lines": "[string]", "touch": "bool",
    "difficulty": "int 0..2 (two-way)", "spectator": "bool (two-way)", "shadows": "int 0..3", "materials": "int 0..2", "particles": "int 0..3",
    "volume": "int 0..100", "music": "int 0..100",
    "lobby": "lobby state", "netStatus": "string", "netError": "string", "roomTitle": "string", "roomInfo": "string", "spectators": "[string]",
    "waitHint": "string", "queues": "[action]", "rooms": "[action]", "createMaps": "[action]", "slots": "[action]", "chat": "[string]",
    "isHost": "bool", "myReady": "bool", "canStart": "bool", "netMatch": "bool", "searching": "bool",
    "netAddress": "string (two-way)", "netName": "string (two-way)", "chatInput": "string (two-way)", "roomName": "string (two-way)"},
  "item": {"text": "string", "sub": "string", "style": "inline css (player colour)", "warn": "bool"},
  "action": {"id": "int: argument of act", "label": "", "cost": "", "hotkey": "", "tip": "", "enabled": "bool", "active": "bool",
             "progress": "float, < 0 = none", "kind": "list | row | grid | queue", "header": "bool: section title", "alt": "int: second button id (-1 none)", "altLabel": ""},
  "events": {"start(i)": "start map i", "act(id)": "press a command-card / top action", "pause()": "", "resume()": "", "restart()": "", "tomenu()": "",
             "net(name, arg?)": "lobby: connect, disconnect, open, back, leave, ready, start, spectate, refresh, unqueue, chatkey, id"},
  "transforms": {"tr": "{{ 'key' | tr }} — localized text from lang/*.json"},
  "elements": {"minimap-img": "img element the client draws the minimap into (keep it in hud.rml)"},
  "example": "<button data-for=\"a : card\" data-if=\"!a.header\" data-event-click=\"act(a.id)\" data-class-off=\"!a.enabled\">{{a.label}}</button>"
})J");
            r.set("gameUiFiles", Json::array());
            if (!ws.project.dir.empty()) {
                fs::path ui = fs::path(ws.project.dir) / "ui";
                Json files = Json::array();
                if (fs::is_directory(ui))
                    for (auto& e : fs::recursive_directory_iterator(ui))
                        if (e.is_regular_file()) files.push(str(e.path().lexically_relative(ws.project.dir).generic_string()));
                r.set("gameUiFiles", files);
            }
            r.set("summary", str("UI: menu/lobby/hud.rml + skin.rcss; переопределяются файлами ui/ игры"));
            return r;
        }});
}

} // namespace rts::editor
