// Map generator entry points: requests, presets, the pipeline and the match-time hook.
#include "GenInternal.h"
#include "engine/core/Hash.h"

namespace rts::gen {

namespace {

// Built-in presets (data). "shape" picks the terrain pipeline; every other key is a param documented in
// docs/game-format.md "Map generation". Games add or override presets with rules "mapgen": {"presets": {...}}.
const char* kPresets = R"JSON({
  "continents": {"title": "Континенты", "genre": "any", "params": {"shape": "continents", "size": [96, 96], "water": 0.4, "rivers": 0.5, "levels": 1, "connect": "shallows", "shallows": 0.6}},
  "islands":    {"title": "Архипелаг", "genre": "naval / any", "params": {"shape": "islands", "size": [96, 96], "water": 0.62, "islands": 4, "levels": 1, "connect": "shallows", "shallows": 0.8}},
  "highlands":  {"title": "Нагорье", "genre": "any", "params": {"shape": "highlands", "size": [80, 80], "water": 0.08, "ridged": 0.7, "levels": 2, "peaks": 0.9, "relief": 10}},
  "canyons":    {"title": "Каньоны", "genre": "any", "params": {"shape": "canyons", "size": [80, 80], "water": 0.0, "ridged": 1.0, "levels": 2, "plateau": 0.6, "peaks": 0.97, "scale": 30}},
  "classic":    {"title": "Классика (Warcraft / Red Alert)", "genre": "classic land RTS", "params": {"shape": "classic", "size": [64, 64], "water": 0.1, "rivers": 0.4, "levels": 1, "plateau": 0.3, "trees": 1.0}},
  "metal":      {"title": "Металлическая карта (TA)", "genre": "Total Annihilation, heightfield", "params": {"shape": "highlands", "size": [72, 72], "water": 0.0, "ridged": 0.5, "relief": 6, "levels": 0, "peaks": 1.0, "rivers": 0, "erosion": 4, "thermal": 10, "resources": 1.0, "baseRadius": 12, "nearRadius": 18}},
  "caves":      {"title": "Пещеры (Dungeon Keeper)", "genre": "dungeon (diggable)", "params": {"shape": "caves", "size": [48, 32], "gold": 0.03, "gems": 0.004, "bedrock": 0.06, "caverns": 0.06, "baseRadius": 4}},
  "lanes":      {"title": "Линии (lane battler)", "genre": "lanes / MOBA", "params": {"shape": "lanes", "size": [32, 64], "lanes": 3, "laneWidth": 4, "bend": 0.35, "baseRadius": 5}},
  "field":      {"title": "Поле (Mushroom Wars 2)", "genre": "garrison / lanes", "params": {"shape": "lanes", "open": true, "size": [44, 28], "obstacles": 0.12, "lanes": 1, "laneWidth": 3, "baseRadius": 3}},
  "rocky":      {"title": "Каменная планета", "genre": "planet", "params": {"shape": "planet", "faceSize": 32, "water": 0.25, "relief": 8, "ice": 0.12}},
  "ocean":      {"title": "Океанический мир", "genre": "planet", "params": {"shape": "planet", "faceSize": 32, "water": 0.7, "relief": 6, "ice": 0.1}},
  "moon":       {"title": "Луна", "genre": "planet", "params": {"shape": "planet", "faceSize": 16, "water": 0.0, "relief": 3, "craters": 10, "ice": 0.0, "baseRadius": 11}}
})JSON";

const Json& builtin() { static const Json j = Json::parse(kPresets); return j; }

void merge(Json& dst, const Json& src) {
    for (auto& [k, v] : src.members()) dst.set(k, v);
}

std::string lower(std::string s) { for (char& ch : s) if (ch >= 'A' && ch <= 'Z') ch = char(ch - 'A' + 'a'); return s; }

} // namespace

Json presetList(const Json& rulesJson) {
    Json out = Json::array();
    auto add = [&](const std::string& id, const Json& p, bool game) {
        for (size_t i = 0; i < out.size(); ++i)
            if (out[i].getStr("id") == id) { out.itemsMut().erase(out.itemsMut().begin() + std::ptrdiff_t(i)); break; }
        Json e = Json::object();
        e.set("id", Json::string(id));
        e.set("title", Json::string(p.getStr("title", id)));
        e.set("genre", Json::string(p.getStr("genre", "")));
        e.set("source", Json::string(game ? "game" : "engine"));
        e.set("params", p["params"]);
        out.push(e);
    };
    for (auto& [id, p] : builtin().members()) add(id, p, false);
    for (auto& [id, p] : rulesJson["mapgen"]["presets"].members()) add(id, p, true);
    return out;
}

bool presetParams(const std::string& id, const Json& rulesJson, Json& out, std::string* err) {
    const Json& game = rulesJson["mapgen"]["presets"][id];
    const Json& eng = builtin()[id];
    if (!game.isObject() && !eng.isObject()) {
        std::string names;
        const Json list = presetList(rulesJson);
        for (auto& p : list.items()) names += (names.empty() ? "" : ", ") + p.getStr("id");
        if (err) *err = "generate: unknown preset '" + id + "' (" + names + ")";
        return false;
    }
    out = Json::object();
    if (game.isObject() && game.has("base")) {   // a game preset can extend a built-in one
        if (!presetParams(game.getStr("base"), Json::object(), out, err)) return false;
    } else if (eng.isObject()) merge(out, eng["params"]);
    if (game.isObject()) merge(out, game["params"]);
    return true;
}

bool parseRequest(const Json& b, uint64_t matchSeed, int defaultPlayers, GenRequest& r, std::string* err) {
    if (!b.isObject()) { if (err) *err = "generate: expected an object {\"preset\", \"seed\", \"players\", \"symmetry\", \"params\"}"; return false; }
    r.preset = b.getStr("preset", "continents");
    const Json& s = b["seed"];
    if (s.isString()) {
        if (s.str() != "match") { if (err) *err = "generate.seed: a number or \"match\""; return false; }
        r.seed = matchSeed;
    } else if (s.isNumber()) r.seed = uint64_t(s.integer(1));
    else r.seed = matchSeed;
    r.players = int(b.getInt("players", defaultPlayers));
    if (r.players < 1 || r.players > 8) { if (err) *err = "generate.players: 1..8"; return false; }
    r.symmetry = b.getStr("symmetry", "auto");
    r.params = b["params"].isObject() ? b["params"] : Json::object();
    return true;
}

uint64_t mapHash(const Json& map) {
    Hasher h;
    const std::string s = map.dump(-1);
    h.bytes(s.data(), s.size());
    return h.value();
}

// ------------------------------------------------------------------ the pipeline


bool generateMap(const GenRequest& req, const game::Rules& rules, const Json& rulesJson, Json& map, std::string* err, const Json* visual) {
    Ctx c(rules, rulesJson);
    c.preset = req.preset;
    if (!presetParams(req.preset, rulesJson, c.P, err)) return false;
    merge(c.P, req.params);
    c.seed64 = mix64(req.seed ^ 0x6d6170676e6e6e31ull);
    c.seed = uint32_t(c.seed64 ^ (c.seed64 >> 32));
    c.np = req.players;
    c.shape = lower(c.pstr("shape", "classic"));
    c.model = rules.terrain.model;
    // template map: player 1's kit + neutral object counts
    if (map["generate"]["templateData"].isObject()) c.tmpl = map["generate"]["templateData"];
    else { c.tmpl = map; c.tmpl.erase("generate"); }
    if (c.shape == "planet") {   // whole planet map (GenWorld.cpp)
        for (auto& l : map["generate"]["layerIds"].items()) c.layerIds.push_back(l.str());
        if (visual) for (auto& l : (*visual)["layers"].items()) c.layerIds.push_back(l.getStr("id"));
        if (c.layerIds.empty()) for (auto& l : rulesJson["terrain"]["layers"].items()) c.layerIds.push_back(l.getStr("id"));
        return generateWorld(c, req, map, err);
    }
    const Json& sz = c.P["size"];
    c.w = int(sz.isArray() ? sz[size_t(0)].integer(64) : map["size"][size_t(0)].integer(64));
    c.h = int(sz.isArray() ? sz[size_t(1)].integer(64) : map["size"][size_t(1)].integer(64));
    if (req.params["size"].isNull() && map["size"].isArray() && !c.P["keepPresetSize"].boolean(false) && !map["generate"]["templateData"].isObject()) {
        c.w = int(map["size"][size_t(0)].integer(c.w)); c.h = int(map["size"][size_t(1)].integer(c.h));
    }
    if (c.w < 16 || c.h < 16 || c.w > 1024 || c.h > 1024) { if (err) *err = "generate: size must be 16..1024 cells per side"; return false; }
    std::string sym = req.symmetry;
    if (sym == "auto" && c.np == 4 && c.w != c.h) sym = "mirror4";
    if (!Sym::make(sym, c.np, c.w, c.h, c.sym, err)) return false;
    c.baseRadius = std::clamp(c.pint("baseRadius", std::max(4, std::min(c.w, c.h) / 10)), 2, 16);
    c.levels = std::clamp(c.pint("levels", 1), 0, 9);
    c.connect = c.pstr("connect", "ground");
    if (c.connect != "ground" && c.connect != "shallows" && c.connect != "none") { if (err) *err = "generate.params.connect: ground, shallows or none"; return false; }
    for (auto& t : rules.types) if (t.structure && t.terrainReq == game::TerrainReq::Rock) c.rockBases = true;
    for (auto& d : rules.diggables) {
        if (d.yields.empty() && !c.digPlain) c.digPlain = d.ch;
        else if (!d.yields.empty() && d.infinite && !c.digGem) c.digGem = d.ch;
        else if (!d.yields.empty() && !c.digGold) c.digGold = d.ch;
    }
    if (c.shape == "caves" && !c.digPlain) c.digPlain = c.digGold;   // no plain rock: veins only
    if (c.shape != "caves") c.digPlain = c.digGold = c.digGem = 0;
    for (auto& l : map["generate"]["layerIds"].items()) c.layerIds.push_back(l.str());
    if (visual) for (auto& l : (*visual)["layers"].items()) c.layerIds.push_back(l.getStr("id"));
    if (c.layerIds.empty()) for (auto& l : rulesJson["terrain"]["layers"].items()) c.layerIds.push_back(l.getStr("id"));

    // pipeline
    if (c.shape == "caves") { chooseStarts(c); genCavesTerrain(c); }
    else if (c.shape == "lanes") { chooseStarts(c); genLanesTerrain(c); }
    else {
        genHeights(c);
        chooseStarts(c);
        flattenBases(c);
        classifyCells(c);
    }
    ensureConnectivity(c);
    if (c.model == game::TerrainModel::Heightfield && c.shape != "caves" && c.shape != "lanes") {
        // the corners moved (roads): water cells follow the final heights
        for (int y = 0; y < c.h; ++y)
            for (int x = 0; x < c.w; ++x) {
                char& ch = c.cell[size_t(c.ci(x, y))];
                if (ch == '~' && c.hg.cell(x, y) >= c.sea) ch = '.';
            }
    }
    placeContent(c);
    unblockStarts(c);
    genRegions(c);
    validateFairness(c);

    // ---- write the map
    Json out = Json::object();
    out.set("name", Json::string(map.getStr("name", "Generated")));
    Json size = Json::array(); size.push(Json::number(int64_t(c.w))); size.push(Json::number(int64_t(c.h)));
    Json players = Json::array();
    for (int p = 0; p < c.np; ++p) {
        Json pj = p < int(map["players"].size()) ? map["players"][size_t(p)] : Json::object();
        if (!pj.has("name")) pj.set("name", Json::string("Player " + std::to_string(p + 1)));
        if (!pj.has("team")) pj.set("team", Json::number(int64_t(p + 1)));
        Json st = Json::array();
        st.push(Json::numberText(fmtQ16(c.starts[size_t(p)].x))); st.push(Json::numberText(fmtQ16(c.starts[size_t(p)].y)));
        pj.set("start", st);
        players.push(pj);
    }
    for (auto& [k, v] : map.members()) {      // keep every block the generator does not own
        if (k == "generate" || k == "terrain" || k == "fields" || k == "entities" || k == "regions" || k == "roads" || k == "terrainMaterial" ||
            k == "size" || k == "players" || k == "name" || k == "tiles" || k == "territory" || k == "generated") continue;
        out.set(k, v);
    }
    out.set("size", size);
    out.set("players", players);
    Json terrain = Json::object();
    Json rows = Json::array();
    for (int y = 0; y < c.h; ++y) rows.push(Json::string(std::string(c.cell.begin() + y * c.w, c.cell.begin() + (y + 1) * c.w)));
    terrain.set("rows", rows);
    if (c.model == game::TerrainModel::Levels) {
        bool any = false;
        Json lv = Json::array();
        for (int y = 0; y < c.h; ++y) {
            std::string r(size_t(c.w), '0');
            for (int x = 0; x < c.w; ++x) { r[size_t(x)] = char('0' + c.lvl[size_t(c.ci(x, y))]); any = any || r[size_t(x)] != '0'; }
            lv.push(Json::string(r));
        }
        if (any) terrain.set("levels", lv);
        if (Json hs = heightsOut(c); hs.isArray()) terrain.set("heights", hs);
    }
    if (c.model == game::TerrainModel::Heightfield || c.pbool("visualCorners", false)) {
        Json cs = Json::array();
        for (int16_t v : cornersOut(c)) cs.push(Json::number(int64_t(v)));
        terrain.set("corners", cs);
    }
    out.set("terrain", terrain);
    if (c.fields.size()) out.set("fields", c.fields);
    if (c.regions.isObject()) out.set("regions", c.regions);
    out.set("entities", c.entities);
    if (c.roads.size()) { out.set("roads", c.roads); if (map.has("roadsOnly")) out.set("roadsOnly", map["roadsOnly"]); }
    if (Json mr = materialRows(c); mr.isArray()) out.set("terrainMaterial", mr);
    Json g = Json::object();
    g.set("preset", Json::string(req.preset));
    g.set("seed", Json::number(int64_t(req.seed & 0x7fffffffffffffffull)));
    g.set("players", Json::number(int64_t(c.np)));
    g.set("symmetry", Json::string(c.sym.name()));
    for (auto& [k, v] : c.info.members()) g.set(k, v);
    if (!c.warnings.empty()) { Json w = Json::array(); for (auto& s : c.warnings) w.push(Json::string(s)); g.set("warnings", w); }
    out.set("generated", g);
    map = std::move(out);
    return true;
}

bool prepareGenerate(Json& map, const std::function<bool(const std::string&, std::string&)>& read, std::string* err) {
    Json* gen = map.find("generate");
    bool any = false;
    if (gen && gen->isObject()) {
        any = true;
        const int np = int(gen->getInt("players", int64_t(std::max<size_t>(map["players"].size(), 2))));
        if (np < 1 || np > 8) { if (err) *err = "generate.players: 1..8"; return false; }
        Json players = map["players"].isArray() ? map["players"] : Json::array();
        static const char* kNames[] = {"Синие", "Красные", "Зелёные", "Жёлтые", "Фиолетовые", "Оранжевые", "Бирюзовые", "Серые"};
        while (int(players.size()) < np) {
            Json p = Json::object();
            p.set("name", Json::string(kNames[players.size() % 8]));
            p.set("team", Json::number(int64_t(players.size() + 1)));
            if (players.size() > 0 && players[size_t(0)].has("faction")) p.set("faction", players[size_t(players.size() % 2)]["faction"]);
            players.push(p);
        }
        while (int(players.size()) > np) players.itemsMut().pop_back();
        map.set("players", players);
        gen = map.find("generate");
        if (gen->has("template") && !gen->has("templateData")) {
            std::string text, file = gen->getStr("template");
            if (!read(file, text)) { if (err) *err = "generate.template: cannot read '" + file + "'"; return false; }
            try { gen->set("templateData", Json::parse(text)); }
            catch (const std::exception& e) { if (err) *err = "generate.template '" + file + "': " + e.what(); return false; }
        }
    }
    for (auto& b : map["world"]["bodies"].items()) if (b["generate"].isObject()) any = true;
    if (any && !(gen && gen->has("layerIds"))) {   // visual material layers (game.json terrain.layers)
        std::string text;
        Json ids = Json::array();
        if (read("game.json", text)) {
            try { const Json man = Json::parse(text); for (auto& l : man["terrain"]["layers"].items()) ids.push(Json::string(l.getStr("id"))); }
            catch (const std::exception&) {}
        }
        if (gen && gen->isObject()) gen->set("layerIds", ids);
    }
    return true;
}

bool applyGenerate(Json& map, const game::Rules& rules, const Json& rulesJson, uint64_t matchSeed, int players, std::string* err) {
    if (map["generate"].isObject()) {
        GenRequest req;
        if (!parseRequest(map["generate"], matchSeed, players, req, err)) return false;
        req.players = players;   // the simulation's player list is authoritative (prepareGenerate sized it)
        if (!generateMap(req, rules, rulesJson, map, err)) return false;
    }
    if (map["world"]["bodies"].isArray()) {
        Json* world = map.find("world");
        Json* bodies = world->find("bodies");
        for (size_t i = 0; i < bodies->size(); ++i) {
            Json& b = bodies->at(i);
            if (!b["generate"].isObject()) continue;
            GenRequest req;
            if (!parseRequest(b["generate"], matchSeed ^ (uint64_t(i + 1) * 0x9e3779b97f4a7c15ull), players, req, err)) return false;
            Ctx c(rules, rulesJson);
            c.preset = req.preset;
            if (!presetParams(req.preset, rulesJson, c.P, err)) return false;
            merge(c.P, req.params);
            c.seed64 = mix64(req.seed ^ 0x706c616e6574ull);
            c.seed = uint32_t(c.seed64 ^ (c.seed64 >> 32));
            c.np = req.players;
            c.model = rules.terrain.model;
            for (auto& l : map["generate"]["layerIds"].items()) c.layerIds.push_back(l.str());
            if (!generatePlanet(c, b, err)) return false;
        }
    }
    return true;
}

} // namespace rts::gen
