#include "Workspace.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include "engine/sim/Simulation.h"
#include "modules/rts/RtsAI.h"
#include "modules/rts/RtsModule.h"

namespace fs = std::filesystem;

namespace rts::editor {

// ================================================================= json paths

namespace {
struct PathPart { std::string key; long index = -1; };
std::vector<PathPart> splitPath(const std::string& path) {
    std::vector<PathPart> out;
    std::string cur;
    for (size_t i = 0; i <= path.size(); ++i) {
        char c = i < path.size() ? path[i] : '.';
        if (c == '.' || c == '/') { if (!cur.empty()) out.push_back({cur}); cur.clear(); continue; }
        if (c == '[') {
            if (!cur.empty()) out.push_back({cur});
            cur.clear();
            size_t e = path.find(']', i);
            if (e == std::string::npos) break;
            out.push_back({"", std::atol(path.substr(i + 1, e - i - 1).c_str())});
            i = e;
            continue;
        }
        cur += c;
    }
    return out;
}
} // namespace

Json* jsonPath(Json& j, const std::string& path, bool create) {
    Json* cur = &j;
    for (auto& p : splitPath(path)) {
        if (p.index >= 0) {
            if (!cur->isArray()) { if (!create) return nullptr; *cur = Json::array(); }
            while (create && cur->size() <= size_t(p.index)) cur->push(Json());
            if (size_t(p.index) >= cur->size()) return nullptr;
            cur = &cur->at(size_t(p.index));
        } else {
            if (!cur->isObject()) { if (!create) return nullptr; *cur = Json::object(); }
            Json* n = cur->find(p.key);
            if (!n) { if (!create) return nullptr; n = &cur->set(p.key, Json()); }
            cur = n;
        }
    }
    return cur;
}
const Json* jsonPath(const Json& j, const std::string& path) { return jsonPath(const_cast<Json&>(j), path, false); }

namespace {
bool erasePath(Json& j, const std::string& path) {
    auto parts = splitPath(path);
    if (parts.empty()) return false;
    PathPart last = parts.back();
    parts.pop_back();
    Json* parent = &j;
    for (auto& p : parts) {
        parent = p.index >= 0 ? (parent->isArray() && size_t(p.index) < parent->size() ? &parent->at(size_t(p.index)) : nullptr) : parent->find(p.key);
        if (!parent) return false;
    }
    if (last.index >= 0) { if (!parent->isArray() || size_t(last.index) >= parent->size()) return false; parent->eraseAt(size_t(last.index)); return true; }
    return parent->erase(last.key);
}
std::string readText(const std::string& f) {
    std::ifstream in(f, std::ios::binary);
    std::stringstream ss; ss << in.rdbuf();
    return ss.str();
}
bool writeText(const std::string& f, const std::string& text, std::string* err) {
    std::ofstream out(f, std::ios::binary);
    if (!out) { if (err) *err = "cannot write " + f; return false; }
    out << text;
    return bool(out);
}
Json ok(Json extra = Json::object()) { extra.set("ok", Json::fromBool(true)); return extra; }
Json str(const std::string& s) { return Json::string(s); }
Json num(double v) { return Json::number(v, 3); }
Json schema(const char* text) { return Json::parse(text); }
float pf(const Json& p, const char* k, float def = 0) { return p.has(k) ? float(p[k].toDouble(def)) : def; }
int pi(const Json& p, const char* k, int def = 0) { return p.has(k) ? int(std::lround(p[k].toDouble(def))) : def; }
std::string ps(const Json& p, const char* k, const std::string& def = "") { return p.getStr(k, def); }

char cellChar(const std::string& kind) {
    if (kind == "ground" || kind == ".") return '.';
    if (kind == "water" || kind == "~") return '~';
    if (kind == "blocked" || kind == "cliff" || kind == "#") return '#';
    if (kind == "nobuild" || kind == ",") return ',';
    if (kind == "shallow" || kind == "=") return '=';
    if (kind == "ramp" || kind == "/") return '/';
    if (kind == "rock" || kind == "R") return 'R';
    throw std::runtime_error("unknown terrain kind '" + kind + "' (ground, water, blocked, nobuild, shallow, ramp, rock)");
}
// shape: {"rect": [x0,y0,x1,y1]} or {"circle": [cx,cy,r]}
struct Shape { bool rect = true; int x0 = 0, y0 = 0, x1 = 0, y1 = 0, cx = 0, cy = 0, r = 0; };
Shape readShape(const Json& p) {
    Shape s;
    if (p.has("circle")) {
        s.rect = false;
        s.cx = int(p["circle"][0].toDouble()); s.cy = int(p["circle"][1].toDouble()); s.r = int(p["circle"][2].toDouble(1));
        s.x0 = s.cx - s.r; s.y0 = s.cy - s.r; s.x1 = s.cx + s.r; s.y1 = s.cy + s.r;
    } else if (p.has("rect")) {
        s.x0 = int(p["rect"][0].toDouble()); s.y0 = int(p["rect"][1].toDouble()); s.x1 = int(p["rect"][2].toDouble()); s.y1 = int(p["rect"][3].toDouble());
        if (s.x0 > s.x1) std::swap(s.x0, s.x1);
        if (s.y0 > s.y1) std::swap(s.y0, s.y1);
    } else throw std::runtime_error("expected \"rect\": [x0,y0,x1,y1] or \"circle\": [cx,cy,r]");
    return s;
}
const char* kShapeSchema = R"("rect": {"type": "array", "items": {"type": "number"}, "description": "[x0, y0, x1, y1] inclusive cells"},
                               "circle": {"type": "array", "items": {"type": "number"}, "description": "[cx, cy, radius] in cells"})";
} // namespace

// ================================================================= project

std::string Project::rulesPath() const { return dir + "/" + manifest.getStr("rules", "rules.json"); }

bool Project::load(const std::string& d, std::string* err) {
    try {
        Json m = Json::parseFile(d + "/game.json");
        Json r = Json::parseFile(d + "/" + m.getStr("rules", "rules.json"));
        game::Rules parsed = game::loadRules(r);
        dir = d; manifest = std::move(m); rulesJson = std::move(r); rules = std::move(parsed);
        rulesError.clear();
        rulesDirty = manifestDirty = false;
        return true;
    } catch (const std::exception& e) {
        if (err) *err = e.what();
        return false;
    }
}
bool Project::reloadRules() {
    try { rules = game::loadRules(rulesJson); rulesError.clear(); return true; }
    catch (const std::exception& e) { rulesError = e.what(); return false; }
}
bool Project::saveRules(std::string* err) {
    if (!writeText(rulesPath(), rulesJson.dump() + "\n", err)) return false;
    rulesDirty = false;
    return true;
}
bool Project::saveManifest(std::string* err) {
    if (!writeText(dir + "/game.json", manifest.dump() + "\n", err)) return false;
    manifestDirty = false;
    return true;
}

// ================================================================= workspace

Workspace::Workspace() { registerCoreOps(); }

void Workspace::registerOp(Op op) {
    for (auto& o : ops_) if (o.name == op.name) { o = std::move(op); return; }
    ops_.push_back(std::move(op));
}

void Workspace::log(const std::string& source, const std::string& text, bool error) {
    activity.push_back({clock(), source, text, error});
    if (activity.size() > 500) activity.erase(activity.begin(), activity.begin() + 100);
}

std::string Workspace::relative(const std::string& file) const {
    std::error_code ec;
    fs::path rel = fs::relative(fs::path(file), fs::path(project.dir), ec);
    return ec || rel.empty() ? file : rel.generic_string();
}

Json Workspace::toolList() const {
    Json tools = Json::array();
    for (auto& o : ops_) {
        Json t = Json::object();
        t.set("name", str(o.name));
        t.set("description", str(o.description));
        Json s = o.schema.isObject() ? o.schema : Json::object();
        if (!s.has("type")) s.set("type", str("object"));
        if (!s.has("properties")) s.set("properties", Json::object());
        t.set("inputSchema", s);
        tools.push(t);
    }
    return tools;
}

Json Workspace::call(const std::string& name, const Json& params, const std::string& source) {
    for (auto& o : ops_) {
        if (o.name != name) continue;
        try {
            Json r = o.fn(params.isObject() ? params : Json::object());
            if (source != "editor" && r.has("summary")) log(source, r.getStr("summary"));
            return r;
        } catch (const std::exception& e) {
            if (source != "editor") log(source, name + ": " + e.what(), true);
            Json err = Json::object();
            err.set("error", str(e.what()));
            return err;
        }
    }
    Json err = Json::object();
    err.set("error", str("unknown operation '" + name + "'"));
    return err;
}

std::vector<GameInfo> Workspace::listGames() const {
    std::vector<GameInfo> out;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(gamesDir(), ec)) {
        if (!e.is_directory() || !fs::exists(e.path() / "game.json")) continue;
        GameInfo g;
        g.id = e.path().filename().string();
        g.dir = e.path().string();
        try {
            Json m = Json::parseFile((e.path() / "game.json").string());
            g.name = m.getStr("name", g.id);
            g.maps = int(m["maps"].size());
        } catch (...) { g.name = g.id + " (ошибка game.json)"; }
        out.push_back(g);
    }
    std::sort(out.begin(), out.end(), [](const GameInfo& a, const GameInfo& b) { return a.id < b.id; });
    return out;
}

bool Workspace::openGame(const std::string& idOrDir, std::string* err) {
    std::string dir = idOrDir;
    if (!fs::exists(fs::path(dir) / "game.json")) dir = gamesDir() + "/" + idOrDir;
    if (!fs::exists(fs::path(dir) / "game.json")) { if (err) *err = "no game.json in " + idOrDir; return false; }
    std::error_code ec;
    dir = fs::weakly_canonical(dir, ec).string();
    if (!project.load(dir, err)) return false;
    // open the first map of the game (or a fresh one)
    std::string first = project.manifest["maps"][0].getStr("file");
    std::string merr;
    if (first.empty() || !map.load(dir + "/" + first, &merr)) map.reset(48, 32);
    note({Change::Project, 0, 0, -1, -1, -1, "Открыта игра " + project.manifest.getStr("name", idOrDir)});
    return true;
}

// ================================================================= operations

void Workspace::registerCoreOps() {
    auto add = [this](const char* name, const char* desc, const char* sch, OpFn fn) {
        registerOp({name, desc, schema(sch), std::move(fn)});
    };
    auto needProject = [this] { if (project.dir.empty()) throw std::runtime_error("no game is open: call open_game first"); };
    auto entityRef = [this](const Json& p) {
        int i = -1;
        if (p.has("id")) i = map.findEntity(ps(p, "id"));
        else if (p.has("index")) i = pi(p, "index", -1);
        if (i < 0 || i >= int(map.data.entities.size())) throw std::runtime_error("entity not found (pass \"id\" or \"index\")");
        return i;
    };
    auto entityJson = [this](int i) {
        const auto& e = map.data.entities[size_t(i)];
        Json j = Json::object();
        j.set("index", Json::number(int64_t(i)));
        if (!e.id.empty()) j.set("id", str(e.id));
        j.set("type", str(e.type));
        j.set("owner", Json::number(int64_t(e.owner)));
        j.set("x", num(e.x)); j.set("z", num(e.z));
        if (e.garrison >= 0) j.set("garrison", Json::number(int64_t(e.garrison)));
        if (e.amount >= 0) j.set("amount", num(e.amount));
        return j;
    };
    auto rulesChanged = [this](const std::string& what) {
        project.rulesDirty = true;
        bool good = project.reloadRules();
        note({Change::Rules, 0, 0, -1, -1, -1, what});
        if (!good) throw std::runtime_error("rules rejected by the loader: " + project.rulesError);
    };

    // ---------------------------------------------------------------- games
    add("list_games", "List the game projects in the engine's games/ folder.", "{}", [this](const Json&) {
        Json arr = Json::array();
        for (auto& g : listGames()) {
            Json j = Json::object();
            j.set("id", str(g.id)); j.set("name", str(g.name)); j.set("dir", str(g.dir)); j.set("maps", Json::number(int64_t(g.maps)));
            j.set("open", Json::fromBool(g.dir == project.dir));
            arr.push(j);
        }
        Json r = Json::object();
        r.set("games", arr);
        return r;
    });
    add("open_game", "Open a game project by id (folder name in games/) or path; opens its first map.",
        R"({"properties": {"game": {"type": "string"}}, "required": ["game"]})", [this](const Json& p) {
            std::string err;
            if (!openGame(ps(p, "game"), &err)) throw std::runtime_error(err);
            Json r = ok();
            r.set("summary", str("Открыта игра " + project.manifest.getStr("name")));
            return r;
        });
    add("create_game", "Create a new game project in games/<id> from a template game (default: copy of 'skirmish', or 'empty').",
        R"({"properties": {"id": {"type": "string", "description": "folder name, latin letters/digits/-"}, "name": {"type": "string"},
            "template": {"type": "string", "description": "id of a game to copy, or 'empty'"}}, "required": ["id", "name"]})",
        [this](const Json& p) {
            std::string id = ps(p, "id"), name = ps(p, "name"), tpl = ps(p, "template", "skirmish");
            if (id.empty() || id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-_") != std::string::npos) throw std::runtime_error("id: use lowercase latin letters, digits, - or _");
            fs::path dst = fs::path(gamesDir()) / id;
            if (fs::exists(dst)) throw std::runtime_error("games/" + id + " already exists");
            fs::create_directories(dst / "maps");
            if (tpl != "empty") {
                fs::path src = fs::path(gamesDir()) / tpl;
                if (!fs::exists(src / "game.json")) throw std::runtime_error("template game '" + tpl + "' not found");
                fs::copy(src, dst, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
            } else {
                writeText((dst / "rules.json").string(), R"({
  "resources": {"gold": {"name": "Золото", "start": 500}},
  "types": {
    "hq": {"name": "Штаб", "key": true, "structure": {"footprint": [3, 3]}, "health": {"max": 1500}, "produces": ["worker"], "dropoff": ["gold"], "supply": 10},
    "worker": {"name": "Рабочий", "move": {"speed": 2.5}, "health": {"max": 40}, "cost": {"gold": 50, "time": 8, "supply": 1}, "gather": {"gold": {"carry": 8, "time": 2}}, "builds": []},
    "mine": {"name": "Рудник", "structure": {"footprint": [2, 2]}, "invulnerable": true, "resourceNode": {"resource": "gold", "amount": 5000}}
  }
}
)", nullptr);
                writeText((dst / "maps/start.json").string(), R"({"name": "Start", "size": [48, 32], "players": [{"name": "Blue", "team": 1}, {"name": "Red", "team": 2}],
 "terrain": {"rows": []}, "entities": [{"type": "hq", "owner": 1, "pos": [8.5, 16.5]}, {"type": "mine", "owner": 0, "pos": [4, 10]},
 {"type": "hq", "owner": 2, "pos": [39.5, 16.5]}, {"type": "mine", "owner": 0, "pos": [44, 22]}]}
)", nullptr);
                writeText((dst / "game.json").string(), "{}", nullptr);
            }
            Json m = Json::parseFile((dst / "game.json").string());
            m.set("name", str(name));
            m.set("id", str("com.rtsengine." + id));
            m.set("version", str("0.1.0"));
            if (!m.has("module")) m.set("module", str("rts"));
            if (!m.has("rules")) m.set("rules", str("rules.json"));
            if (!m.has("maps") || tpl == "empty") {
                Json maps = Json::array();
                Json e = Json::object(); e.set("file", str("maps/start.json")); e.set("title", str("Start"));
                maps.push(e);
                m.set("maps", maps);
            }
            writeText((dst / "game.json").string(), m.dump() + "\n", nullptr);
            std::string err;
            if (!openGame(dst.string(), &err)) throw std::runtime_error(err);
            Json r = ok();
            r.set("dir", str(dst.string()));
            r.set("summary", str("Создана игра «" + name + "» (games/" + id + ")"));
            return r;
        });
    add("game_info", "Manifest (game.json), rules summary and maps of the open game.", "{}", [this, needProject](const Json&) {
        needProject();
        Json r = Json::object();
        r.set("dir", str(project.dir));
        r.set("manifest", project.manifest);
        Json s = Json::object();
        s.set("resources", Json::number(int64_t(project.rules.resources.size())));
        s.set("types", Json::number(int64_t(project.rules.types.size())));
        s.set("techs", Json::number(int64_t(project.rules.techs.size())));
        s.set("factions", Json::number(int64_t(project.rules.factions.size())));
        r.set("rulesSummary", s);
        if (!project.rulesError.empty()) r.set("rulesError", str(project.rulesError));
        return r;
    });
    add("set_manifest", "Set a value in game.json (name, version, orientation, icon, maps, input, ...). Saved immediately.",
        R"({"properties": {"path": {"type": "string"}, "value": {}}, "required": ["path", "value"]})", [this, needProject](const Json& p) {
            needProject();
            *jsonPath(project.manifest, ps(p, "path"), true) = p["value"];
            std::string err;
            if (!project.saveManifest(&err)) throw std::runtime_error(err);
            Json r = ok();
            r.set("summary", str("game.json: " + ps(p, "path")));
            return r;
        });

    // ---------------------------------------------------------------- rules
    add("rules_schema", "Reference of the game data format: every rules/map block and field, with examples. Read this before editing rules.", "{}",
        [this](const Json&) {
            std::string text = readText(engineDir + "/docs/game-format.md");
            if (text.empty()) text = "docs/game-format.md not found in " + engineDir;
            Json r = Json::object();
            r.set("text", str(text));
            return r;
        });
    add("rules_get", "Read rules.json or a part of it by path (e.g. 'types.footman', 'resources', 'triggers[0]'). No path = whole file.",
        R"({"properties": {"path": {"type": "string"}}})", [this, needProject](const Json& p) {
            needProject();
            const Json* v = p.has("path") ? jsonPath(project.rulesJson, ps(p, "path")) : &project.rulesJson;
            if (!v) throw std::runtime_error("path not found: " + ps(p, "path"));
            Json r = Json::object();
            r.set("value", *v);
            return r;
        });
    add("rules_list", "Compact list of resources, types (id, name, kind, cost) and techs of the open game.", "{}", [this, needProject](const Json&) {
        needProject();
        const auto& R = project.rules;
        Json types = Json::array();
        for (auto& t : R.types) {
            Json j = Json::object();
            j.set("id", str(t.id)); j.set("name", str(t.name));
            j.set("kind", str(t.structure ? "structure" : t.resourceNode >= 0 ? "resource" : t.speed.raw > 0 ? "unit" : "object"));
            std::string cost;
            for (auto& c : t.cost) cost += std::to_string(c.value.toInt()) + " " + R.resources[c.res].id + " ";
            if (!cost.empty()) j.set("cost", str(cost));
            types.push(j);
        }
        Json res = Json::array();
        for (auto& rd : R.resources) res.push(str(rd.id));
        Json techs = Json::array();
        for (auto& k : R.techs) techs.push(str(k.id));
        Json r = Json::object();
        r.set("resources", res); r.set("types", types); r.set("techs", techs);
        return r;
    });
    add("rules_set", "Set a value in rules.json by path and save. Examples: path 'types.archer' value {...whole type...}; "
        "'types.archer.weapons[0].damage' value 12; 'resources.oil' value {\"start\": 100}. The loader validates the result; "
        "invalid data is rolled back and the error is returned.",
        R"({"properties": {"path": {"type": "string"}, "value": {}}, "required": ["path", "value"]})", [this, needProject, rulesChanged](const Json& p) {
            needProject();
            Json backup = project.rulesJson;
            *jsonPath(project.rulesJson, ps(p, "path"), true) = p["value"];
            try { rulesChanged("Правила: " + ps(p, "path")); }
            catch (...) { project.rulesJson = backup; project.reloadRules(); throw; }
            std::string err;
            if (!project.saveRules(&err)) throw std::runtime_error(err);
            Json r = ok();
            r.set("summary", str("Правила: " + ps(p, "path")));
            return r;
        });
    add("rules_merge", "Deep-merge an object into rules.json at path (keeps other fields), validate and save.",
        R"({"properties": {"path": {"type": "string"}, "value": {"type": "object"}}, "required": ["path", "value"]})", [this, needProject, rulesChanged](const Json& p) {
            needProject();
            Json backup = project.rulesJson;
            std::function<void(Json&, const Json&)> merge = [&](Json& dst, const Json& src) {
                if (!src.isObject() || !dst.isObject()) { dst = src; return; }
                for (auto& [k, v] : src.members()) { Json* d = dst.find(k); if (d) merge(*d, v); else dst.set(k, v); }
            };
            merge(*jsonPath(project.rulesJson, ps(p, "path"), true), p["value"]);
            try { rulesChanged("Правила: " + ps(p, "path")); }
            catch (...) { project.rulesJson = backup; project.reloadRules(); throw; }
            std::string err;
            if (!project.saveRules(&err)) throw std::runtime_error(err);
            Json r = ok();
            r.set("summary", str("Правила (слияние): " + ps(p, "path")));
            return r;
        });
    add("rules_delete", "Delete a value from rules.json by path (e.g. 'types.archer'), validate and save.",
        R"({"properties": {"path": {"type": "string"}}, "required": ["path"]})", [this, needProject, rulesChanged](const Json& p) {
            needProject();
            Json backup = project.rulesJson;
            if (!erasePath(project.rulesJson, ps(p, "path"))) throw std::runtime_error("path not found");
            try { rulesChanged("Правила: удалено " + ps(p, "path")); }
            catch (...) { project.rulesJson = backup; project.reloadRules(); throw; }
            std::string err;
            if (!project.saveRules(&err)) throw std::runtime_error(err);
            Json r = ok();
            r.set("summary", str("Правила: удалено " + ps(p, "path")));
            return r;
        });

    // ---------------------------------------------------------------- maps
    add("map_list", "Maps of the open game (files in maps/ and entries of game.json).", "{}", [this, needProject](const Json&) {
        needProject();
        Json files = Json::array();
        std::error_code ec;
        std::vector<std::string> names;
        for (auto& e : fs::directory_iterator(mapsDir(), ec)) if (e.path().extension() == ".json") names.push_back("maps/" + e.path().filename().string());
        std::sort(names.begin(), names.end());
        for (auto& n : names) files.push(str(n));
        Json r = Json::object();
        r.set("files", files);
        r.set("inGame", project.manifest["maps"]);
        r.set("open", str(map.path.empty() ? "" : relative(map.path)));
        return r;
    });
    add("map_open", "Open a map file of the game (path relative to the game folder, e.g. 'maps/valley.json').",
        R"({"properties": {"file": {"type": "string"}}, "required": ["file"]})", [this, needProject](const Json& p) {
            needProject();
            std::string f = ps(p, "file");
            if (!fs::path(f).is_absolute()) f = project.dir + "/" + f;
            std::string err;
            if (!map.load(f, &err)) throw std::runtime_error(err);
            note({Change::Map, 0, 0, map.data.w - 1, map.data.h - 1, -1, "Открыта карта " + relative(f)});
            Json r = ok();
            r.set("summary", str("Открыта карта " + relative(f)));
            return r;
        });
    add("map_new", "Start a new empty map (not saved until map_save).",
        R"({"properties": {"width": {"type": "integer"}, "height": {"type": "integer"}, "name": {"type": "string"}}, "required": ["width", "height"]})",
        [this](const Json& p) {
            map.reset(std::clamp(pi(p, "width", 64), 8, 1024), std::clamp(pi(p, "height", 48), 8, 1024));
            if (p.has("name")) map.data.name = ps(p, "name");
            note({Change::Map, 0, 0, map.data.w - 1, map.data.h - 1, -1, "Новая карта"});
            Json r = ok();
            r.set("summary", str("Новая карта " + std::to_string(map.data.w) + "x" + std::to_string(map.data.h)));
            return r;
        });
    add("map_save", "Save the open map (to its file, or to 'file' relative to the game folder). 'addToGame': also list it in game.json.",
        R"({"properties": {"file": {"type": "string"}, "addToGame": {"type": "boolean"}, "title": {"type": "string"}}})", [this, needProject](const Json& p) {
            needProject();
            std::string f = p.has("file") ? ps(p, "file") : map.path;
            if (f.empty()) throw std::runtime_error("map has no file yet: pass \"file\", e.g. maps/new.json");
            if (!fs::path(f).is_absolute()) f = project.dir + "/" + f;
            std::error_code ec;
            fs::create_directories(fs::path(f).parent_path(), ec);
            std::string err;
            if (!map.save(f, &err)) throw std::runtime_error(err);
            if (p.getBool("addToGame", false)) {
                std::string rel = relative(f);
                bool listed = false;
                for (auto& m : project.manifest["maps"].items()) if (m.getStr("file") == rel) listed = true;
                if (!listed) {
                    Json e = Json::object();
                    e.set("file", str(rel)); e.set("title", str(ps(p, "title", map.data.name)));
                    project.manifest.member("maps");
                    if (!project.manifest["maps"].isArray()) project.manifest.set("maps", Json::array());
                    project.manifest.find("maps")->push(e);
                    project.saveManifest(&err);
                }
            }
            Json r = ok();
            r.set("summary", str("Сохранена карта " + relative(f)));
            return r;
        });
    add("map_info", "Size, players, object counts and extra blocks (areas, waves, triggers, victory) of the open map.", "{}", [this](const Json&) {
        const auto& d = map.data;
        Json r = Json::object();
        r.set("name", str(d.name)); r.set("file", str(map.path.empty() ? "" : relative(map.path)));
        r.set("width", Json::number(int64_t(d.w))); r.set("height", Json::number(int64_t(d.h)));
        r.set("dirty", Json::fromBool(map.dirty));
        Json players = map.toJson()["players"];
        r.set("players", players);
        r.set("entities", Json::number(int64_t(d.entities.size())));
        r.set("roads", Json::number(int64_t(d.roads.size())));
        Json keys = Json::array();
        for (auto& [k, v] : d.extra.members()) keys.push(str(k));
        r.set("extraBlocks", keys);
        return r;
    });
    add("map_ascii", "Text picture of the open map (terrain, levels, fields, objects) — the fastest way to 'see' a map.",
        R"({"properties": {"maxWidth": {"type": "integer"}}})", [this](const Json& p) {
            Json r = Json::object();
            r.set("text", str(map.ascii(pi(p, "maxWidth", 128))));
            return r;
        });
    add("map_get", "Read the open map as JSON (or a part by path: 'players[0]', 'waves', 'triggers', 'areas', 'victory').",
        R"({"properties": {"path": {"type": "string"}, "includeTerrain": {"type": "boolean"}}})", [this](const Json& p) {
            Json j = map.toJson();
            if (!p.getBool("includeTerrain", false) && !p.has("path")) { j.erase("terrain"); j.erase("fields"); j.member("regions").erase("rows"); }
            const Json* v = p.has("path") ? jsonPath(j, ps(p, "path")) : &j;
            if (!v) throw std::runtime_error("path not found");
            Json r = Json::object();
            r.set("value", *v);
            return r;
        });
    add("map_set", "Set any value of the map JSON by path (players, waves, triggers, areas, victory, roadsOnly, name...). "
        "For terrain and objects prefer the dedicated tools.",
        R"({"properties": {"path": {"type": "string"}, "value": {}}, "required": ["path", "value"]})", [this](const Json& p) {
            Json j = map.toJson();
            *jsonPath(j, ps(p, "path"), true) = p["value"];
            MapDoc probe;
            std::string err;
            if (!probe.loadJson(j, &err)) throw std::runtime_error(err);
            std::string file = map.path;
            map.beginEdit();
            auto undoSafe = map.data; (void)undoSafe;
            map.data = probe.data;
            map.path = file;
            map.touched();
            note({Change::Map, 0, 0, -1, -1, -1, "Карта: " + ps(p, "path")});
            Json r = ok();
            r.set("summary", str("Карта: " + ps(p, "path")));
            return r;
        });
    add("map_resize", "Resize the map (keeps the top-left part).",
        R"({"properties": {"width": {"type": "integer"}, "height": {"type": "integer"}}, "required": ["width", "height"]})", [this](const Json& p) {
            map.resize(pi(p, "width"), pi(p, "height"));
            note({Change::Map, 0, 0, map.data.w - 1, map.data.h - 1, -1, "Размер карты"});
            Json r = ok();
            r.set("summary", str("Размер карты " + std::to_string(map.data.w) + "x" + std::to_string(map.data.h)));
            return r;
        });
    add("map_mirror", "Make the map symmetric: copy the left half to the right ('horizontal') or rotate it 180° ('point'); owners 1 and 2 swap.",
        R"({"properties": {"mode": {"type": "string", "enum": ["horizontal", "point"]}}, "required": ["mode"]})", [this](const Json& p) {
            if (ps(p, "mode") == "point") map.mirrorPoint(); else map.mirrorHorizontal();
            note({Change::Map, 0, 0, map.data.w - 1, map.data.h - 1, -1, "Симметрия"});
            Json r = ok();
            r.set("summary", str("Карта отражена (" + ps(p, "mode") + ")"));
            return r;
        });
    add("terrain_paint", "Paint terrain kind on a rect or circle: ground, water, blocked, nobuild, shallow, ramp, rock.",
        (std::string(R"({"properties": {"kind": {"type": "string"}, )") + kShapeSchema + R"(}, "required": ["kind"]})").c_str(), [this](const Json& p) {
            char c = cellChar(ps(p, "kind"));
            Shape s = readShape(p);
            map.beginEdit();
            if (s.rect) map.paintRect(s.x0, s.y0, s.x1, s.y1, c); else map.paint(s.cx, s.cy, s.r, c);
            note({Change::Cells, s.x0, s.y0, s.x1, s.y1, -1, "Рельеф: " + ps(p, "kind")});
            Json r = ok();
            r.set("summary", str("Рельеф «" + ps(p, "kind") + "» в (" + std::to_string(s.x0) + "," + std::to_string(s.y0) + ")–(" + std::to_string(s.x1) + "," + std::to_string(s.y1) + ")"));
            return r;
        });
    add("terrain_level", "Set cliff level (0..9) on a rect or circle. Units change level only over 'ramp' cells; higher ground gives the bonuses defined in rules.heights.",
        (std::string(R"({"properties": {"level": {"type": "integer"}, )") + kShapeSchema + R"(}, "required": ["level"]})").c_str(), [this](const Json& p) {
            Shape s = readShape(p);
            int lv = pi(p, "level");
            map.beginEdit();
            if (s.rect) map.setLevelRect(s.x0, s.y0, s.x1, s.y1, lv); else map.setLevel(s.cx, s.cy, s.r, lv);
            note({Change::Cells, s.x0, s.y0, s.x1, s.y1, -1, "Уровень " + std::to_string(lv)});
            Json r = ok();
            r.set("summary", str("Плато уровня " + std::to_string(lv) + " в (" + std::to_string(s.x0) + "," + std::to_string(s.y0) + ")–(" + std::to_string(s.x1) + "," + std::to_string(s.y1) + ")"));
            return r;
        });
    add("terrain_height", "Raise (delta > 0) or lower the visual height smoothly in a circle (cosmetic; gameplay uses levels).",
        R"({"properties": {"circle": {"type": "array", "items": {"type": "number"}}, "delta": {"type": "integer"}}, "required": ["circle", "delta"]})", [this](const Json& p) {
            Shape s = readShape(p);
            map.beginEdit();
            map.raise(s.cx, s.cy, s.r, pi(p, "delta", 4));
            note({Change::Cells, s.x0, s.y0, s.x1, s.y1, -1, "Высота"});
            Json r = ok();
            r.set("summary", str("Высота изменена"));
            return r;
        });
    add("field_paint", "Paint a resource field (Red Alert ore, Dune spice) of 'resource' with density 1..9 (0 erases) on a rect or circle.",
        (std::string(R"({"properties": {"resource": {"type": "string"}, "amount": {"type": "integer"}, )") + kShapeSchema + R"(}, "required": ["resource"]})").c_str(),
        [this](const Json& p) {
            Shape s = readShape(p);
            std::string res = ps(p, "resource");
            if (project.rules.findResource(res) < 0) throw std::runtime_error("unknown resource '" + res + "'");
            int amt = pi(p, "amount", 9);
            map.beginEdit();
            if (s.rect) { for (int y = s.y0; y <= s.y1; ++y) for (int x = s.x0; x <= s.x1; ++x) map.paintField(x, y, 0, res, amt); }
            else map.paintField(s.cx, s.cy, s.r, res, amt);
            note({Change::Cells, s.x0, s.y0, s.x1, s.y1, -1, "Поле " + res});
            Json r = ok();
            r.set("summary", str("Поле ресурса " + res));
            return r;
        });
    add("region_paint", "Paint a region (Northgard-style territory) with a letter (A-Z, '.' erases); optional display name.",
        (std::string(R"({"properties": {"letter": {"type": "string"}, "name": {"type": "string"}, )") + kShapeSchema + R"(}, "required": ["letter"]})").c_str(),
        [this](const Json& p) {
            Shape s = readShape(p);
            std::string l = ps(p, "letter", ".");
            char c = l.empty() ? '.' : l[0];
            map.beginEdit();
            if (s.rect) map.paintRegionRect(s.x0, s.y0, s.x1, s.y1, c); else map.paintRegion(s.cx, s.cy, s.r, c);
            if (p.has("name") && c != '.') map.data.regionNames[c] = ps(p, "name");
            note({Change::Cells, s.x0, s.y0, s.x1, s.y1, -1, std::string("Регион ") + c});
            Json r = ok();
            r.set("summary", str(std::string("Регион ") + c + (p.has("name") ? " «" + ps(p, "name") + "»" : "")));
            return r;
        });
    add("entity_place", "Place objects of a type on the map. owner: 0 neutral, 1..N player, -1 hostile creeps. "
        "count > 1 places a cluster around x,z ('spread' radius).",
        R"({"properties": {"type": {"type": "string"}, "owner": {"type": "integer"}, "x": {"type": "number"}, "z": {"type": "number"},
            "id": {"type": "string"}, "garrison": {"type": "integer"}, "amount": {"type": "number"}, "count": {"type": "integer"}, "spread": {"type": "number"}},
            "required": ["type", "x", "z"]})",
        [this, entityJson](const Json& p) {
            std::string type = ps(p, "type");
            int ti = project.rules.findType(type);
            if (ti < 0) throw std::runtime_error("unknown type '" + type + "' (see rules_list)");
            const auto& t = project.rules.types[size_t(ti)];
            int count = std::clamp(pi(p, "count", 1), 1, 200);
            float spread = pf(p, "spread", 1.2f);
            map.beginEdit();
            Json placed = Json::array();
            for (int k = 0; k < count; ++k) {
                DocEntity e;
                e.type = type;
                e.owner = pi(p, "owner", 0);
                float a = k * 2.39996f, rr = count > 1 ? spread * std::sqrt(float(k) / float(count)) * 1.6f : 0.0f;
                e.x = pf(p, "x") + rr * std::cos(a);
                e.z = pf(p, "z") + rr * std::sin(a);
                if (t.structure) { // snap to the footprint grid like the game does
                    e.x = (t.footprintW % 2) ? std::floor(e.x) + 0.5f : std::round(e.x);
                    e.z = (t.footprintH % 2) ? std::floor(e.z) + 0.5f : std::round(e.z);
                }
                if (p.has("id") && count == 1) e.id = ps(p, "id");
                if (p.has("garrison")) e.garrison = pi(p, "garrison");
                if (p.has("amount")) e.amount = pf(p, "amount");
                map.data.entities.push_back(e);
                placed.push(entityJson(int(map.data.entities.size() - 1)));
                note({Change::Entity, int(e.x), int(e.z), int(e.x), int(e.z), int(map.data.entities.size() - 1), "Объект " + t.name});
            }
            map.touched();
            Json r = ok();
            r.set("placed", placed);
            r.set("summary", str("Поставлен" + std::string(count > 1 ? "о " + std::to_string(count) + " ×" : "") + " " + t.name + " в (" +
                                 std::to_string(int(pf(p, "x"))) + ", " + std::to_string(int(pf(p, "z"))) + ")"));
            return r;
        });
    add("entity_list", "List objects on the open map (optionally filtered by type / owner / rect).",
        R"({"properties": {"type": {"type": "string"}, "owner": {"type": "integer"}, "rect": {"type": "array", "items": {"type": "number"}}}})",
        [this, entityJson](const Json& p) {
            Json arr = Json::array();
            for (size_t i = 0; i < map.data.entities.size(); ++i) {
                const auto& e = map.data.entities[i];
                if (p.has("type") && e.type != ps(p, "type")) continue;
                if (p.has("owner") && e.owner != pi(p, "owner")) continue;
                if (p.has("rect")) {
                    Shape s = readShape(p);
                    if (e.x < s.x0 || e.x > s.x1 + 1 || e.z < s.y0 || e.z > s.y1 + 1) continue;
                }
                arr.push(entityJson(int(i)));
            }
            Json r = Json::object();
            r.set("entities", arr);
            return r;
        });
    add("entity_update", "Change an object (by id or index): type, owner, x, z, id, garrison, amount.",
        R"({"properties": {"id": {"type": "string"}, "index": {"type": "integer"}, "set": {"type": "object"}}, "required": ["set"]})",
        [this, entityRef, entityJson](const Json& p) {
            int i = entityRef(p);
            map.beginEdit();
            auto& e = map.data.entities[size_t(i)];
            const Json& s = p["set"];
            if (s.has("type")) { if (project.rules.findType(s.getStr("type")) < 0) throw std::runtime_error("unknown type"); e.type = s.getStr("type"); }
            if (s.has("owner")) e.owner = pi(s, "owner");
            if (s.has("x")) e.x = pf(s, "x");
            if (s.has("z")) e.z = pf(s, "z");
            if (s.has("id")) e.id = s.getStr("id");
            if (s.has("garrison")) e.garrison = pi(s, "garrison");
            if (s.has("amount")) e.amount = pf(s, "amount");
            map.touched();
            note({Change::Entity, int(e.x), int(e.z), int(e.x), int(e.z), i, "Изменён " + e.type});
            Json r = ok();
            r.set("entity", entityJson(i));
            r.set("summary", str("Изменён объект " + e.type));
            return r;
        });
    add("entity_delete", "Delete an object by id/index, or all objects matching type/owner/rect when 'all' is true.",
        R"({"properties": {"id": {"type": "string"}, "index": {"type": "integer"}, "all": {"type": "boolean"}, "type": {"type": "string"}, "owner": {"type": "integer"},
            "rect": {"type": "array", "items": {"type": "number"}}}})",
        [this, entityRef](const Json& p) {
            int removed = 0;
            if (p.getBool("all", false)) {
                map.beginEdit();
                for (int i = int(map.data.entities.size()) - 1; i >= 0; --i) {
                    const auto& e = map.data.entities[size_t(i)];
                    if (p.has("type") && e.type != ps(p, "type")) continue;
                    if (p.has("owner") && e.owner != pi(p, "owner")) continue;
                    if (p.has("rect")) { Shape s = readShape(p); if (e.x < s.x0 || e.x > s.x1 + 1 || e.z < s.y0 || e.z > s.y1 + 1) continue; }
                    std::vector<std::pair<int, int>> roads;
                    for (auto [a, b] : map.data.roads) if (a != i && b != i) roads.emplace_back(a > i ? a - 1 : a, b > i ? b - 1 : b);
                    map.data.roads = roads;
                    map.data.entities.erase(map.data.entities.begin() + i);
                    ++removed;
                }
                map.touched();
            } else { map.removeEntity(entityRef(p)); removed = 1; }
            note({Change::Map, 0, 0, -1, -1, -1, "Удалено объектов: " + std::to_string(removed)});
            Json r = ok();
            r.set("removed", Json::number(int64_t(removed)));
            r.set("summary", str("Удалено объектов: " + std::to_string(removed)));
            return r;
        });
    add("road_toggle", "Add or remove a road between two objects (ids or indices) — Mushroom Wars style movement graph.",
        R"({"properties": {"a": {}, "b": {}}, "required": ["a", "b"]})", [this](const Json& p) {
            auto ref = [&](const Json& v) { return v.isString() ? map.findEntity(v.str()) : int(v.toDouble(-1)); };
            int a = ref(p["a"]), b = ref(p["b"]);
            bool added = map.toggleRoad(a, b);
            note({Change::Map, 0, 0, -1, -1, a, added ? "Дорога добавлена" : "Дорога удалена"});
            Json r = ok();
            r.set("added", Json::fromBool(added));
            r.set("summary", str(added ? "Дорога добавлена" : "Дорога удалена"));
            return r;
        });
    add("player_set", "Change player N (1-based): name, team, faction, start [x,z] (faction start units spawn there), waves (scripted side), resources {id: amount}.",
        R"({"properties": {"player": {"type": "integer"}, "name": {"type": "string"}, "team": {"type": "integer"}, "faction": {"type": "string"},
            "start": {"type": "array", "items": {"type": "number"}}, "waves": {"type": "boolean"}, "resources": {"type": "object"}}, "required": ["player"]})",
        [this](const Json& p) {
            int n = pi(p, "player");
            map.beginEdit();
            while (int(map.data.players.size()) < n && map.data.players.size() < 16)
                map.data.players.push_back({"Player " + std::to_string(map.data.players.size() + 1), int(map.data.players.size() + 1)});
            if (n < 1 || n > int(map.data.players.size())) throw std::runtime_error("player out of range");
            auto& pl = map.data.players[size_t(n - 1)];
            if (p.has("name")) pl.name = ps(p, "name");
            if (p.has("team")) pl.team = pi(p, "team");
            if (p.has("faction")) pl.faction = ps(p, "faction");
            if (p.has("start")) { pl.hasStart = true; pl.startX = float(p["start"][0].toDouble()); pl.startZ = float(p["start"][1].toDouble()); }
            if (p.has("waves")) pl.waves = p.getBool("waves");
            for (auto& [k, v] : p["resources"].members()) {
                bool found = false;
                for (auto& [rid, val] : pl.resources) if (rid == k) { val = float(v.toDouble()); found = true; }
                if (!found) pl.resources.push_back({k, float(v.toDouble())});
            }
            map.touched();
            note({Change::Players, int(pl.startX), int(pl.startZ), int(pl.startX), int(pl.startZ), -1, "Игрок " + pl.name});
            Json r = ok();
            r.set("summary", str("Игрок " + std::to_string(n) + ": " + pl.name));
            return r;
        });
    add("player_remove", "Remove player N (their objects become neutral).", R"({"properties": {"player": {"type": "integer"}}, "required": ["player"]})",
        [this](const Json& p) {
            int n = pi(p, "player");
            if (n < 1 || n > int(map.data.players.size())) throw std::runtime_error("player out of range");
            map.beginEdit();
            for (auto& e : map.data.entities) { if (e.owner == n) e.owner = 0; else if (e.owner > n) --e.owner; }
            map.data.players.erase(map.data.players.begin() + (n - 1));
            map.touched();
            note({Change::Players, 0, 0, -1, -1, -1, "Игрок удалён"});
            Json r = ok();
            r.set("summary", str("Игрок " + std::to_string(n) + " удалён"));
            return r;
        });
    add("map_validate", "Check the open map against the rules and the real game loader. Returns errors and warnings.", "{}", [this](const Json&) {
        auto issues = map.validate(project.rules, project.rulesJson);
        Json arr = Json::array();
        for (auto& i : issues) {
            Json j = Json::object();
            j.set("error", Json::fromBool(i.error)); j.set("text", str(i.text));
            if (i.entity >= 0) j.set("entity", Json::number(int64_t(i.entity)));
            arr.push(j);
        }
        Json r = Json::object();
        r.set("issues", arr);
        r.set("ok", Json::fromBool(std::none_of(issues.begin(), issues.end(), [](const Issue& i) { return i.error; })));
        return r;
    });
    add("undo", "Undo the last map edit.", "{}", [this](const Json&) {
        map.undo();
        note({Change::Map, 0, 0, -1, -1, -1, "Отмена"});
        Json r = ok(); r.set("summary", str("Отменено")); return r;
    });
    add("redo", "Redo the last undone map edit.", "{}", [this](const Json&) {
        map.redo();
        note({Change::Map, 0, 0, -1, -1, -1, "Повтор"});
        Json r = ok(); r.set("summary", str("Повторено")); return r;
    });

    // ---------------------------------------------------------------- testing
    add("match_run", "Run a fast bot-vs-bot match on the open map (headless, deterministic) and report the outcome: winner, duration, "
        "per-player economy/army timeline and event counts. Use it to balance and to verify a map is playable.",
        R"({"properties": {"seconds": {"type": "number", "description": "time limit, default 900"}, "seed": {"type": "integer"},
            "difficulty": {"type": "integer", "description": "0 easy, 1 normal, 2 hard"}}})",
        [this, needProject](const Json& p) {
            needProject();
            SimConfig cfg;
            cfg.seed = uint64_t(std::max(0, pi(p, "seed", 1)));
            Simulation sim(cfg, std::make_unique<game::RtsModule>());
            sim.setup(project.rulesJson, map.toJson());
            auto& mod = *dynamic_cast<game::RtsModule*>(&sim.module());
            game::AIConfig ai;
            int diff = pi(p, "difficulty", 1);
            ai.thinkTicks = diff == 0 ? 20 : diff == 2 ? 6 : 10;
            std::vector<game::RtsAI> bots;
            for (auto& pl : sim.players()) bots.emplace_back(pl.slot, cfg.seed, ai);
            const uint32_t limit = uint32_t(pf(p, "seconds", 900) * float(sim.tickRate()));
            Json timeline = Json::array();
            std::map<std::string, int64_t> events;
            static const char* evNames[] = {"", "spawned", "died", "attack", "hit", "trainStarted", "trained", "buildPlaced", "built", "gathered", "captured",
                                            "defeated", "notEnough", "researched", "levelUp", "ability", "miss", "readyToPlace", "sold", "regionClaimed", "wave", "lowPower", "upgraded", "message"};
            while (sim.tick() < limit && sim.result() == 0) {
                for (auto& b : bots) b.update(sim);
                sim.step();
                for (auto& ev : sim.events()) if (ev.type < sizeof(evNames) / sizeof(evNames[0])) events[evNames[ev.type]]++;
                if (sim.tick() % (sim.tickRate() * 60) == 0) {
                    Json snap = Json::object();
                    snap.set("t", Json::number(int64_t(sim.tick() / sim.tickRate())));
                    Json ps_ = Json::array();
                    auto& gs = mod.state(sim);
                    for (auto& pl : sim.players()) {
                        Json pj = Json::object();
                        int units = 0, structs = 0;
                        sim.world().each<game::Unit, Owner>([&](Entity, game::Unit& u, Owner& o) { if (o.player == pl.slot) (mod.rules().types[u.type].structure ? structs : units)++; });
                        pj.set("player", Json::number(int64_t(pl.slot)));
                        pj.set("units", Json::number(int64_t(units))); pj.set("structures", Json::number(int64_t(structs)));
                        Json res = Json::object();
                        for (size_t r = 0; r < mod.rules().resources.size(); ++r) res.set(mod.rules().resources[r].id, Json::number(gs.players[pl.slot].stock[r].toInt()));
                        pj.set("resources", res);
                        pj.set("alive", Json::fromBool(pl.alive));
                        ps_.push(pj);
                    }
                    snap.set("players", ps_);
                    timeline.push(snap);
                }
            }
            Json r = Json::object();
            int res = sim.result();
            std::string winner = res == 0 ? "none (time limit)" : res < 0 ? "draw" : "team " + std::to_string(res);
            for (auto& pl : sim.players()) if (res > 0 && pl.team == res) { winner += " (" + pl.name + ")"; break; }
            r.set("winner", str(winner));
            r.set("seconds", Json::number(int64_t(sim.tick() / sim.tickRate())));
            r.set("timeline", timeline);
            Json ev = Json::object();
            for (auto& [k, v] : events) ev.set(k, Json::number(v));
            r.set("events", ev);
            char cs[32]; std::snprintf(cs, sizeof cs, "%016llx", (unsigned long long)sim.checksum());
            r.set("checksum", str(cs));
            r.set("summary", str("Тестовый матч: победа " + winner + " за " + std::to_string(sim.tick() / sim.tickRate()) + " с"));
            return r;
        });

    // ---------------------------------------------------------------- assets
    add("asset_import", "Copy a file (model .glb/.gltf, texture .png/.jpg, sound .wav/.ogg, font, UI .rml/.rcss) into the game folder.",
        R"({"properties": {"source": {"type": "string", "description": "absolute path of the file to import"}, "dest": {"type": "string", "description": "path inside the game folder, e.g. models/tank.glb"}}, "required": ["source", "dest"]})",
        [this, needProject](const Json& p) {
            needProject();
            fs::path dst = fs::path(project.dir) / ps(p, "dest");
            fs::path canon = fs::weakly_canonical(dst);
            if (canon.string().rfind(fs::weakly_canonical(project.dir).string(), 0) != 0) throw std::runtime_error("dest must stay inside the game folder");
            fs::create_directories(dst.parent_path());
            fs::copy_file(ps(p, "source"), dst, fs::copy_options::overwrite_existing);
            Json r = ok();
            r.set("summary", str("Импортирован " + ps(p, "dest")));
            return r;
        });
    add("asset_list", "List files of the game folder (optionally a subfolder), with sizes.",
        R"({"properties": {"dir": {"type": "string"}}})", [this, needProject](const Json& p) {
            needProject();
            Json arr = Json::array();
            std::error_code ec;
            fs::path root = fs::path(project.dir) / ps(p, "dir", "");
            for (auto it = fs::recursive_directory_iterator(root, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
                if (!it->is_regular_file()) continue;
                Json j = Json::object();
                j.set("path", str(relative(it->path().string())));
                j.set("bytes", Json::number(int64_t(it->file_size())));
                arr.push(j);
                if (arr.size() > 2000) break;
            }
            Json r = Json::object();
            r.set("files", arr);
            return r;
        });
}

} // namespace rts::editor
