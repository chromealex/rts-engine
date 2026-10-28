#include "GenOps.h"
#include <fstream>
#include <iterator>
#include "modules/rts/gen/MapGen.h"
#include "modules/rts/RtsTiles.h"

namespace rts::editor {

namespace {
Json str(const std::string& s) { return Json::string(s); }

GenOptions optionsFrom(const Json& p) {
    GenOptions o;
    o.preset = p.getStr("preset", o.preset);
    o.seed = uint64_t(p.getInt("seed", 1));
    o.players = int(p.getInt("players", 2));
    o.symmetry = p.getStr("symmetry", "auto");
    if (p["params"].isObject()) o.params = p["params"];
    else if (p["params"].isString() && !p.getStr("params").empty()) o.params = Json::parse(p.getStr("params"));
    o.useTemplate = p.getBool("useTemplate", true);
    o.keepSize = p.getBool("keepSize", false);
    return o;
}
} // namespace

Json generateIntoMap(Workspace& ws, const GenOptions& o) {
    if (ws.project.dir.empty()) throw std::runtime_error("no game is open: call open_game first");
    Json cur = ws.map.data.w > 0 ? ws.map.toJson() : Json::object();
    Json in = Json::object();
    in.set("name", str(ws.map.data.name));
    Json size = Json::array(); size.push(Json::number(int64_t(ws.map.data.w))); size.push(Json::number(int64_t(ws.map.data.h)));
    in.set("size", size);
    for (auto& [k, v] : cur.members())   // keep victory, triggers, lighting, ... (players' names / factions too)
        if (k != "terrain" && k != "entities" && k != "fields" && k != "regions" && k != "roads" && k != "terrainMaterial" && k != "size" && k != "name")
            in.set(k, v);
    Json gen = Json::object();
    gen.set("preset", str(o.preset));
    gen.set("players", Json::number(int64_t(o.players)));
    if (o.useTemplate) {
        Json t = cur;
        t.erase("generate"); t.erase("generated");
        // a map that was itself generated is a fine template too: its kit / object counts carry over
        gen.set("templateData", t);
    } else {
        Json t = Json::object(); t.set("entities", Json::array()); t.set("players", Json::array());
        gen.set("templateData", t);
    }
    in.set("generate", gen);
    const std::string dir = ws.project.dir;
    auto read = [&dir](const std::string& p, std::string& out) {
        std::ifstream f(dir + "/" + p, std::ios::binary);
        if (!f) return false;
        out.assign(std::istreambuf_iterator<char>(f), {});
        return true;
    };
    std::string err;
    if (!gen::prepareGenerate(in, read, &err)) throw std::runtime_error(err);
    gen::GenRequest req;
    req.preset = o.preset; req.seed = o.seed; req.players = o.players; req.symmetry = o.symmetry; req.params = o.params;
    if (o.keepSize && !req.params.has("size")) req.params.set("size", size);
    Json visual = ws.project.manifest["terrain"];
    if (!gen::generateMap(req, ws.project.rules, ws.project.rulesJson, in, &err, &visual)) throw std::runtime_error(err);
    const Json summary = in["generated"];
    ws.map.beginEdit();
    std::string lerr;
    const std::string path = ws.map.path;
    if (!ws.map.loadJson(in, &lerr)) throw std::runtime_error("generated map does not load: " + lerr);
    ws.map.path = path;
    ws.map.touched();
    ws.note({Change::Map, 0, 0, ws.map.data.w - 1, ws.map.data.h - 1, -1, "Карта сгенерирована: " + o.preset});
    return summary;
}

void setMatchGenerate(Workspace& ws, const GenOptions& o, bool matchSeed) {
    if (ws.project.dir.empty()) throw std::runtime_error("no game is open: call open_game first");
    std::string err;
    Json probe;
    if (!gen::presetParams(o.preset, ws.project.rulesJson, probe, &err)) throw std::runtime_error(err);
    ws.map.beginEdit();
    Json g = Json::object();
    g.set("preset", str(o.preset));
    if (matchSeed) g.set("seed", str("match")); else g.set("seed", Json::number(int64_t(o.seed)));
    g.set("players", Json::number(int64_t(o.players)));
    g.set("symmetry", str(o.symmetry));
    if (o.params.size()) g.set("params", o.params);
    ws.map.data.extra.set("generate", g);
    ws.map.data.extra.erase("generated");
    ws.map.touched();
    ws.note({Change::Map, 0, 0, -1, -1, -1, "Генерация при старте матча: " + o.preset});
}

void registerGenOps(Workspace& ws) {
    Workspace* w = &ws;
    auto add = [&ws](const char* name, const char* desc, const char* sch, Workspace::OpFn fn) { ws.registerOp({name, desc, Json::parse(sch), std::move(fn)}); };

    add("gen_presets",
        "List the map generator presets (built into the engine + the game's rules 'mapgen.presets'): id, title, genre, default params. "
        "Planet presets (rocky, ocean, moon) are for world.bodies[].generate.",
        R"({"properties": {}})",
        [w](const Json&) {
            Json r = Json::object();
            r.set("presets", gen::presetList(w->project.rulesJson));
            r.set("params", str("shape, size [w,h], water 0..1, rivers 0..1, lakes, shallows, levels 0..9, plateau, peaks, relief (world units), "
                                "scale (feature size, cells), octaves, roughness, ridged, warp, thermal, erosion, erosionRate, talus, moisture, "
                                "temperature, latitude, baseRadius, startSpread, startJitter, starts [[x,y]...], kit [{type, offset, count}], "
                                "objects [{type, near, far, centre, owner, garrison, amount}], trees, resources (multipliers), fieldRadius, "
                                "fieldsFar, regions, regionsPerPlayer, roads, connect (ground|shallows|none), border, lanes, laneWidth, bend, "
                                "open, obstacles, gold, gems, bedrock, caverns, faceSize, ice, craters"));
            r.set("summary", str(std::to_string(r["presets"].size()) + " пресетов генерации"));
            return r;
        });

    add("map_generate",
        "Generate the open map with the deterministic map generator (same code as match-time generation): preset (gen_presets), "
        "seed, players (1..8), symmetry (auto | none | point | mirror | mirror-y | mirror4 | rot4 | rotational), params (overrides, "
        "see gen_presets). The open map's player 1 objects are the start kit and its neutral objects decide what is placed "
        "(useTemplate: false = rules heuristics). Result: starts, path lengths between starts (engine pathfinder), connectivity, "
        "warnings. mode: 'bake' (default) writes a static map (undoable); 'match' only stores a 'generate' block so every match "
        "generates a new map from the match seed ('seed': 'match'); 'fixed' stores the block with this seed.",
        R"({"properties": {"preset": {"type": "string"}, "seed": {"type": "integer"}, "players": {"type": "integer"},
            "symmetry": {"type": "string"}, "params": {"type": "object"}, "mode": {"type": "string", "enum": ["bake", "match", "fixed"]},
            "useTemplate": {"type": "boolean"}, "keepSize": {"type": "boolean"}}, "required": ["preset"]})",
        [w](const Json& p) {
            const GenOptions o = optionsFrom(p);
            const std::string mode = p.getStr("mode", "bake");
            Json r = Json::object();
            if (mode == "match" || mode == "fixed") {
                setMatchGenerate(*w, o, mode == "match");
                r.set("ok", Json::fromBool(true));
                r.set("summary", str("Карта будет генерироваться при старте матча (" + o.preset + ")"));
                return r;
            }
            if (mode != "bake") throw std::runtime_error("mode must be bake, match or fixed");
            Json g = generateIntoMap(*w, o);
            w->map.data.extra.erase("generate");
            r.set("ok", Json::fromBool(true));
            r.set("generated", g);
            r.set("size", Json::parse("[" + std::to_string(w->map.data.w) + ", " + std::to_string(w->map.data.h) + "]"));
            r.set("entities", Json::number(int64_t(w->map.data.entities.size())));
            std::string warn;
            for (auto& x : g["warnings"].items()) warn += "; " + x.str();
            r.set("summary", str("Сгенерировано: " + o.preset + " seed " + std::to_string(o.seed) + ", " + std::to_string(w->map.data.w) + "x" +
                                 std::to_string(w->map.data.h) + (g.getBool("connected", true) ? "" : ", базы не связаны") + warn));
            return r;
        });
}

} // namespace rts::editor
