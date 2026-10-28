// Agent operations for Lua scripts of the open game (docs/scripting.md): list / read / write (syntax
// checked before saving) / check (compile + a short headless bot match) / api (the reference as JSON).
// Work without the GUI (rtsmcp). Paths are confined to scripts/ of the game folder.
#include "ScriptOps.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include "modules/rts/RtsAI.h"
#include "modules/rts/RtsModule.h"
#include "modules/rts/RtsScript.h"

namespace fs = std::filesystem;

namespace rts::editor {

using game::ScriptReader;

namespace {
Json str(const std::string& s) { return Json::string(s); }

ScriptReader folderReader(const std::string& dir) {
    return [dir](const std::string& p, std::string& out) {
        std::ifstream f(fs::path(dir) / p, std::ios::binary);
        if (!f) return false;
        std::stringstream ss; ss << f.rdbuf();
        out = ss.str();
        return true;
    };
}

// "scripts/x.lua" checked: relative, inside scripts/, .lua
fs::path scriptPath(const Workspace& ws, const std::string& rel) {
    if (ws.project.dir.empty()) throw std::runtime_error("no game is open (open_game)");
    fs::path p = fs::path(rel).lexically_normal();
    if (p.is_absolute() || p.empty() || *p.begin() != "scripts" || p.extension() != ".lua")
        throw std::runtime_error("script paths are scripts/<name>.lua inside the game folder: " + rel);
    return fs::path(ws.project.dir) / p;
}

bool isViewScript(const Workspace& ws, const std::string& rel) {
    const Json map = ws.map.toJson();
    auto v = game::scriptFiles(ws.project.rulesJson, map, true);
    return std::find(v.begin(), v.end(), rel) != v.end() || rel.rfind("scripts/view/", 0) == 0;
}
} // namespace

void registerScriptOps(Workspace& ws) {
    ws.registerOp({"script_list",
        "Lua scripts of the open game: every scripts/**/*.lua with whether rules/map list it as a simulation script "
        "(\"scripts\") or a view script (\"viewScripts\"). Reference: script_api.",
        Json::parse(R"({"properties": {}})"),
        [&ws](const Json&) {
            if (ws.project.dir.empty()) throw std::runtime_error("no game is open (open_game)");
            const Json map = ws.map.toJson();
            auto simList = game::scriptFiles(ws.project.rulesJson, map, false);
            auto viewList = game::scriptFiles(ws.project.rulesJson, map, true);
            Json files = Json::array();
            std::vector<std::string> found;
            std::error_code ec;
            fs::path root = fs::path(ws.project.dir) / "scripts";
            if (fs::exists(root, ec))
                for (auto& e : fs::recursive_directory_iterator(root, ec))
                    if (e.is_regular_file() && e.path().extension() == ".lua")
                        found.push_back(fs::relative(e.path(), ws.project.dir, ec).generic_string());
            std::sort(found.begin(), found.end());
            for (auto& f : found) {
                Json j = Json::object();
                j.set("path", str(f));
                const bool s = std::find(simList.begin(), simList.end(), f) != simList.end();
                const bool v = std::find(viewList.begin(), viewList.end(), f) != viewList.end();
                j.set("role", str(s ? "simulation" : v ? "view" : "module (require) or unused"));
                files.push(j);
            }
            Json r = Json::object();
            r.set("files", files);
            Json missing = Json::array();
            for (auto& f : simList) if (std::find(found.begin(), found.end(), f) == found.end()) missing.push(str(f));
            for (auto& f : viewList) if (std::find(found.begin(), found.end(), f) == found.end()) missing.push(str(f));
            if (missing.size()) r.set("missing", missing);
            return r;
        }});
    ws.registerOp({"script_read", "Read a Lua script of the open game (path scripts/....lua).",
        Json::parse(R"({"properties": {"path": {"type": "string"}}, "required": ["path"]})"),
        [&ws](const Json& p) {
            fs::path f = scriptPath(ws, p.getStr("path"));
            std::string text;
            if (!folderReader(ws.project.dir)(fs::relative(f, ws.project.dir).generic_string(), text)) throw std::runtime_error("no such script: " + p.getStr("path"));
            Json r = Json::object();
            r.set("path", str(p.getStr("path")));
            r.set("text", str(text));
            return r;
        }});
    ws.registerOp({"script_write",
        "Create or replace a Lua script (scripts/....lua). It is compiled first and NOT saved when it has a syntax error "
        "(the error is returned with file:line). Simulation scripts must not use float literals (use fx(\"1.5\"), fx(3, 2) or "
        "integers). register=true adds it to rules.json \"scripts\" (or \"viewScripts\" for scripts/view/...). "
        "A running play-test restarts with the new scripts.",
        Json::parse(R"({"properties": {"path": {"type": "string"}, "text": {"type": "string"}, "register": {"type": "boolean"}},
            "required": ["path", "text"]})"),
        [&ws](const Json& p) {
            const std::string rel = fs::path(p.getStr("path")).lexically_normal().generic_string();
            fs::path f = scriptPath(ws, rel);
            const std::string text = p.getStr("text");
            if (text.size() > (2u << 20)) throw std::runtime_error("script is larger than 2 MB");
            const bool view = isViewScript(ws, rel);
            std::string err = game::checkScriptSyntax(rel, text, !view);
            if (!err.empty()) throw std::runtime_error("not saved: " + err);
            std::error_code ec;
            fs::create_directories(f.parent_path(), ec);
            const bool existed = fs::exists(f);
            {
                std::ofstream o(f, std::ios::binary);
                if (!o) throw std::runtime_error("cannot write " + rel);
                o << text;
            }
            Json r = Json::object();
            r.set("ok", Json::fromBool(true));
            r.set("summary", str((existed ? "Изменён " : "Создан ") + rel));
            if (p.getBool("register", false)) {
                const char* key = view ? "viewScripts" : "scripts";
                Json& list = ws.project.rulesJson.member(key);
                if (!list.isArray()) list = Json::array();
                bool listed = false;
                for (auto& x : list.items()) listed |= x.str() == rel;
                if (!listed) {
                    list.push(str(rel));
                    std::string serr;
                    ws.project.rulesDirty = true;
                    if (!ws.project.saveRules(&serr)) throw std::runtime_error("saved the script but not rules.json: " + serr);
                    ws.project.reloadRules();
                    ws.note({Change::Rules, 0, 0, -1, -1, -1, std::string("rules.json: ") + key + " += " + rel});
                    r.set("registered", Json::fromBool(true));
                }
            }
            ws.note({Change::Project, 0, 0, -1, -1, -1, (existed ? "Изменён скрипт " : "Создан скрипт ") + rel});
            return r;
        }});
    ws.registerOp({"script_check",
        "Compile every script of the open game and run a headless bot match on the open map for `seconds` (default 60): "
        "returns syntax errors, runtime errors (file:line) and the script log. Run it after script_write.",
        Json::parse(R"({"properties": {"seconds": {"type": "number"}, "seed": {"type": "integer"}}})"),
        [&ws](const Json& p) {
            if (ws.project.dir.empty()) throw std::runtime_error("no game is open (open_game)");
            Json r = Json::object();
            Json errors = Json::array(), log = Json::array();
            const Json map = ws.map.toJson();
            for (bool view : {false, true})
                for (auto& f : game::scriptFiles(ws.project.rulesJson, map, view)) {
                    std::string text;
                    if (!folderReader(ws.project.dir)(f, text)) { errors.push(str(f + ": file not found")); continue; }
                    std::string e = game::checkScriptSyntax(f, text, !view);
                    if (!e.empty()) errors.push(str(e));
                }
            const double seconds = std::clamp(p["seconds"].toDouble(60), 1.0, 3600.0);
            SimConfig cfg;
            cfg.seed = uint64_t(std::max<int64_t>(0, p.getInt("seed", 1)));
            try {
                Simulation sim(cfg, std::make_unique<game::RtsModule>());
                sim.setup(ws.project.simRules(map), map);
                auto& m = static_cast<game::RtsModule&>(sim.module());
                std::vector<game::RtsAI> bots;
                for (auto& pl : sim.players()) bots.emplace_back(pl.slot, cfg.seed);
                const uint32_t ticks = uint32_t(seconds * sim.tickRate());
                while (sim.tick() < ticks && sim.result() == 0) { for (auto& b : bots) b.update(sim); sim.step(); }
                r.set("ticks", Json::number(int64_t(sim.tick())));
                r.set("result", Json::number(int64_t(sim.result())));
                if (auto* se = m.scripts())
                    for (auto& e : se->log()) {
                        std::string line = "t=" + std::to_string(e.tick) + " " + e.text;
                        if (e.error) errors.push(str(line));
                        else if (log.size() < 200) log.push(str(line));
                    }
                else r.set("note", str("the game has no simulation scripts (rules/map \"scripts\")"));
            } catch (const std::exception& e) {
                errors.push(str(std::string("setup: ") + e.what()));
            }
            r.set("ok", Json::fromBool(errors.size() == 0));
            r.set("errors", errors);
            r.set("log", log);
            return r;
        }});
    ws.registerOp({"script_api",
        "Reference of the Lua scripting API (events, game.* queries and actions, fx numbers, data hooks, view scripts) as JSON. "
        "Read it before writing scripts; the full guide is docs/scripting.md.",
        Json::parse(R"({"properties": {}})"),
        [](const Json&) { return game::scriptApiReference(); }});
}

Json Project::simRules(const Json& map) const {
    Json r = rulesJson;
    if (!dir.empty()) game::embedScripts(r, map, folderReader(dir), nullptr);   // missing files: reported by the engine
    return r;
}

} // namespace rts::editor
