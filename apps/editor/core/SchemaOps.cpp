// The loader is the specification: instead of keeping a second list of keys next to the parser,
// these tools run the real game loader (Simulation::setup) with a JsonAccessLog installed and look
// at which keys it asked for.
// - rules_lint: keys in rules.json / the map that nothing reads (typos, misplaced blocks), with the
//   closest key the loader asked for at that place.
// - rules_schema: the doc text plus, per block path ("types.*.weapons[]"), every key the loader
//   asks for there, collected over all games in games/; keys missing from docs/game-format.md are
//   listed as undocumented.
// Presentation-only blocks (view, sprite, visual terrain/fog keys, lights, ...) are read by the
// view, not the simulation loader, so they are skipped.
#include "SchemaOps.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include "engine/sim/Simulation.h"
#include "modules/rts/RtsModule.h"

namespace rts::editor {

namespace fs = std::filesystem;
using namespace rts::game;

namespace {
Json str(const std::string& s) { return Json::string(s); }

// Keys read by the view / UI / tools, never by the simulation loader (anywhere in the tree).
const std::set<std::string> kPresentationKeys = {
    "view", "sprite", "name", "description", "comment", "_comment", "//", "notes", "note", "icon", "portrait", "hotkey",
    "tooltip", "lore", "label", "color", "colour", "sound", "sounds", "voice", "music", "ui", "hud", "card", "help", "title",
};
// Top-level blocks whose other keys are visual (the sim reads only some of them).
const std::set<std::string> kMixedBlocks = {"terrain", "fogOfWar"};
// Map keys owned by the editor / view.
const std::set<std::string> kMapPresentation = {"lights", "lighting", "tiles", "camera", "world", "author", "preview", "editor", "version", "tileset", "decals", "props"};

struct Access {
    std::set<std::pair<const Json*, std::string>> keys;
    std::map<const Json*, std::vector<std::string>> askedAt;   // keys looked up per object (present or not)
    std::set<const Json*> iterated;
    std::set<std::string> anyName;                              // every key name asked for anywhere
};

Access record(const Json& rules, const Json& map, std::string* err) {
    JsonAccessLog log;
    Json::setAccessLog(&log);
    try {
        SimConfig cfg;
        Simulation sim(cfg, std::make_unique<RtsModule>());
        sim.setup(rules, map);
    } catch (const std::exception& e) {
        if (err) *err = e.what();
    }
    Json::setAccessLog(nullptr);
    Access a;
    for (auto& [obj, key] : log.keys) {
        if (a.keys.insert({obj, key}).second) a.askedAt[obj].push_back(key);
        a.anyName.insert(key);
    }
    a.iterated.insert(log.iterated.begin(), log.iterated.end());
    return a;
}

int editDistance(const std::string& a, const std::string& b) {
    std::vector<int> d(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) d[j] = int(j);
    for (size_t i = 1; i <= a.size(); ++i) {
        int prev = d[0];
        d[0] = int(i);
        for (size_t j = 1; j <= b.size(); ++j) {
            int cur = d[j];
            int sub = prev + (std::tolower((unsigned char)a[i - 1]) == std::tolower((unsigned char)b[j - 1]) ? 0 : 1);
            d[j] = std::min({d[j] + 1, d[j - 1] + 1, sub});
            prev = cur;
        }
    }
    return d[b.size()];
}

std::string closest(const std::vector<std::string>& names, const std::string& key) {
    std::string best;
    int bd = 1 << 30;
    for (auto& n : names) {
        if (n == key) continue;
        int d = editDistance(n, key);
        if (d < bd) { bd = d; best = n; }
    }
    return bd <= (key.size() < 8 ? 1 : 2) ? best : "";
}

void lintWalk(const Access& a, const Json& j, const std::string& path, bool isMap, int depth, Json& out) {
    if (j.isArray()) {
        for (size_t i = 0; i < j.items().size(); ++i) lintWalk(a, j.items()[i], path + "[" + std::to_string(i) + "]", isMap, depth + 1, out);
        return;
    }
    if (!j.isObject()) return;
    const bool iterated = a.iterated.count(&j) > 0;
    auto asked = a.askedAt.find(&j);
    std::set<std::string> present;
    for (auto& [k, v] : j.members()) present.insert(k);
    for (auto& [k, v] : j.members()) {
        const std::string sub = path.empty() ? k : path + "." + k;
        if (kPresentationKeys.count(k)) continue;   // the view reads these blocks, read or not by the loader
        const bool read = iterated || a.keys.count({&j, k});
        if (read) { lintWalk(a, v, sub, isMap, depth + 1, out); continue; }
        if (depth == 0 && (isMap ? kMapPresentation.count(k) > 0 : kMixedBlocks.count(k) > 0)) continue;
        if (depth == 1 && kMixedBlocks.count(path)) continue;
        if (sub.find(".levels[") != std::string::npos) continue;   // level overrides are merged into copies before parsing
        if (out.size() >= 200) return;
        Json issue = Json::object();
        issue.set("path", str((isMap ? "map." : "") + sub));
        std::string hint = asked != a.askedAt.end() ? closest(asked->second, k) : "";
        if (!hint.empty() && present.count(hint)) issue.set("problem", str("unknown key, a misspelt duplicate of '" + hint + "' (which is set here too)?"));
        else if (!hint.empty()) issue.set("problem", str("unknown key, did you mean '" + hint + "'?"));
        else if (a.anyName.count(k)) issue.set("problem", str("not read here (the loader reads '" + k + "' in other blocks)"));
        else issue.set("problem", str("unknown key: the loader never reads it"));
        if (!hint.empty()) issue.set("suggest", str(hint));
        out.push(issue);
    }
}

// ---------------------------------------------------------------- schema
struct Schema {
    std::map<std::string, std::set<std::string>> keys;   // normalized path -> keys asked there
    std::set<std::string> games;
    std::set<std::string> ids;   // data ids (resources, types, techs, rooms, map chars...): keys that are values, not fields
};

void addIds(Schema& s, const Json& rules) {
    for (const char* block : {"resources", "types", "techs", "rooms", "diggable", "statuses", "factions", "armorTypes", "damageTable"})
        for (auto& [k, v] : rules[block].members()) {
            s.ids.insert(k);
            if (std::string(block) == "damageTable") for (auto& [k2, v2] : v.members()) s.ids.insert(k2);
        }
    for (auto& a : rules["armorTypes"].items()) s.ids.insert(a.str());
}

bool wordIn(const std::string& doc, const std::string& w) {
    for (size_t at = doc.find(w); at != std::string::npos; at = doc.find(w, at + 1)) {
        auto isId = [](char c) { return std::isalnum((unsigned char)c) || c == '_'; };
        if ((at == 0 || !isId(doc[at - 1])) && (at + w.size() >= doc.size() || !isId(doc[at + w.size()]))) return true;
    }
    return false;
}

void schemaWalk(const Access& a, const Json& j, const std::string& path, std::map<const Json*, std::string>& paths) {
    if (j.isArray()) { for (auto& v : j.items()) schemaWalk(a, v, path + "[]", paths); return; }
    if (!j.isObject()) return;
    paths[&j] = path;
    auto asked = a.askedAt.find(&j);
    const bool idMap = a.iterated.count(&j) && asked == a.askedAt.end();
    for (auto& [k, v] : j.members())
        schemaWalk(a, v, idMap ? (path.empty() ? "*" : path + ".*") : (path.empty() ? k : path + "." + k), paths);
}

void addSchema(Schema& s, const Access& a, const Json& rules, const Json& map) {
    std::map<const Json*, std::string> paths;
    schemaWalk(a, rules, "", paths);
    schemaWalk(a, map, "map", paths);
    for (auto& [obj, ks] : a.askedAt) {
        auto it = paths.find(obj);
        if (it == paths.end()) continue;
        for (auto& k : ks) s.keys[it->second.empty() ? "(top)" : it->second].insert(k);
    }
}

std::string readText(const std::string& f) {
    std::ifstream in(f, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// First map of a game folder: game.json "maps" entries, else maps/*.json (sorted).
std::vector<std::string> gameMaps(const fs::path& dir, const Json& manifest) {
    std::vector<std::string> out;
    for (auto& m : manifest["maps"].items()) {
        std::string f = m.isString() ? m.str() : m.getStr("file");
        if (!f.empty()) out.push_back((dir / f).string());
    }
    if (out.empty() && fs::exists(dir / "maps")) {
        for (auto& e : fs::directory_iterator(dir / "maps")) if (e.path().extension() == ".json") out.push_back(e.path().string());
        std::sort(out.begin(), out.end());
    }
    return out;
}

Json schemaJson(Workspace& ws, const std::string& prefix) {
    Schema s;
    auto add = [&](const std::string& id, const Json& rules, const Json& map) {
        Access a = record(rules, map, nullptr);
        addSchema(s, a, rules, map);
        addIds(s, rules);
        s.games.insert(id);
    };
    for (auto& gi : ws.listGames()) {
        try {
            Project pr;
            std::string err;
            if (!pr.load(gi.dir, &err)) continue;
            auto maps = gameMaps(gi.dir, pr.manifest);
            for (size_t i = 0; i < maps.size() && i < 3; ++i) {
                MapDoc md;
                if (!md.load(maps[i], &err)) continue;
                add(gi.id, pr.rulesJson, md.toJson());
            }
        } catch (...) {}
    }
    if (!ws.project.dir.empty()) add("(open game)", ws.project.rulesJson, ws.map.toJson());

    const std::string doc = readText(ws.engineDir + "/docs/game-format.md");
    Json blocks = Json::object();
    std::set<std::string> undocumented;
    for (auto& [path, ks] : s.keys) {
        if (!prefix.empty() && path.rfind(prefix, 0) != 0) continue;
        Json arr = Json::array();
        for (auto& k : ks) {
            if (s.ids.count(k) || (k.size() == 1 && std::isupper((unsigned char)k[0]))) continue;   // ids and map characters
            arr.push(str(k));
            if (!doc.empty() && !wordIn(doc, k)) undocumented.insert(k);
        }
        if (arr.size()) blocks.set(path, arr);
    }
    Json r = Json::object();
    r.set("blocks", blocks);
    Json u = Json::array();
    for (auto& k : undocumented) u.push(str(k));
    r.set("undocumented", u);
    Json g = Json::array();
    for (auto& id : s.games) g.push(str(id));
    r.set("fromGames", g);
    r.set("note", str("Keys the real loader asks for at each block path ('*' = any id, '[]' = array items), collected by loading every game "
                      "in games/. Blocks no game uses yet show only their top key; their fields are in the doc text."));
    return r;
}

Json lintJson(const Json& issues, const std::string& loadError) {
    Json r = Json::object();
    r.set("ok", Json::fromBool(issues.size() == 0 && loadError.empty()));
    if (!loadError.empty()) r.set("loadError", str(loadError));
    r.set("issues", issues);
    r.set("summary", str(issues.size() == 0 ? "Неизвестных ключей нет" : "Ключей, которые загрузчик не читает: " + std::to_string(issues.size())));
    return r;
}
} // namespace

Json lintGameData(const Json& rules0, const Json& map, std::string* loadError) {
    // unit designs expand into plain type blocks before loading: lint the expanded copy
    Json expanded;
    if (rules0.has("designs")) {
        try { expanded = game::expandDesigns(rules0); } catch (const std::exception& e) { if (loadError) *loadError = e.what(); return Json::array(); }
    }
    const Json& rules = rules0.has("designs") ? expanded : rules0;
    Access a = record(rules, map, loadError);
    Json issues = Json::array();
    lintWalk(a, rules, "", false, 0, issues);
    lintWalk(a, map, "", true, 0, issues);
    return issues;
}

void registerSchemaOps(Workspace& ws) {
    ws.registerOp({"rules_lint",
        "Find keys in rules.json and the open map that the game loader never reads (typos like 'helth', blocks in the wrong place), "
        "with the closest key the loader expects there. Runs the real loader; presentation blocks (view, sprite, lights) are skipped.",
        Json::parse("{}"), [&ws](const Json&) {
            if (ws.project.dir.empty()) throw std::runtime_error("no game is open (open_game)");
            std::string err;
            Json issues = lintGameData(ws.project.rulesJson, ws.map.toJson(), &err);
            return lintJson(issues, err);
        }});
    ws.registerOp({"rules_schema",
        "Reference of the game data format. 'part': 'doc' (docs/game-format.md: meaning and examples), 'keys' (every key the real loader "
        "reads, per block path, from all games, plus keys missing from the doc), or 'all' (default). 'path' narrows keys to a prefix, "
        "e.g. 'types.*.weapons'. Read this before editing rules.",
        Json::parse(R"({"properties": {"part": {"type": "string", "enum": ["all", "doc", "keys"]}, "path": {"type": "string"}}})"),
        [&ws](const Json& p) {
            const std::string part = p.getStr("part", "all");
            Json r = Json::object();
            if (part != "keys") {
                std::string text = readText(ws.engineDir + "/docs/game-format.md");
                r.set("text", str(text.empty() ? "docs/game-format.md not found in " + ws.engineDir : text));
            }
            if (part != "doc") r.set("keys", schemaJson(ws, p.getStr("path")));
            return r;
        }});

    // edits report unknown keys right away
    for (const char* name : {"rules_set", "rules_merge", "map_validate"}) {
        auto it = std::find_if(ws.ops().begin(), ws.ops().end(), [&](const Workspace::Op& o) { return o.name == name; });
        if (it == ws.ops().end()) continue;
        Workspace::Op op = *it;
        auto inner = op.fn;
        const bool isValidate = std::string(name) == "map_validate";
        op.fn = [&ws, inner, isValidate](const Json& p) {
            Json r = inner(p);
            if (ws.project.dir.empty() || r.has("error")) return r;
            std::string err;
            Json issues = lintGameData(ws.project.rulesJson, ws.map.toJson(), &err);
            if (issues.size() == 0) return r;
            if (isValidate) {
                for (auto& i : issues.items()) {
                    Json w = Json::object();
                    w.set("error", Json::fromBool(false));
                    w.set("text", str(i.getStr("path") + ": " + i.getStr("problem")));
                    r.member("issues").push(w);
                }
            } else {
                r.set("unknownKeys", issues);
            }
            return r;
        };
        ws.registerOp(std::move(op));
    }
}

} // namespace rts::editor
