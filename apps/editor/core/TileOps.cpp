#include "TileOps.h"
#include <algorithm>
#include <filesystem>
#include <map>
#include <memory>
#include <stdexcept>

namespace fs = std::filesystem;

namespace rts::editor {

namespace {
struct Cached { fs::file_time_type mtime; std::unique_ptr<game::Tileset> ts; std::string error; };
std::map<std::string, Cached>& cache() { static std::map<std::string, Cached> c; return c; }

Json num(int64_t v) { return Json::number(v); }
Json str(const std::string& s) { return Json::string(s); }

template <class Fn> void forCells(const MapData& d, bool rect, int a, int b, int c, int e, Fn&& fn) {
    if (rect) {
        for (int y = std::max(0, std::min(b, e)); y <= std::min(d.h - 1, std::max(b, e)); ++y)
            for (int x = std::max(0, std::min(a, c)); x <= std::min(d.w - 1, std::max(a, c)); ++x) fn(x, y);
        return;
    }
    for (int y = b - c; y <= b + c; ++y)
        for (int x = a - c; x <= a + c; ++x)
            if (d.inside(x, y) && (x - a) * (x - a) + (y - b) * (y - b) <= c * c + c) fn(x, y);
}

void paintImpl(Workspace& ws, bool rect, int a, int b, int c, int e, int terrain, int fixedTile) {
    const game::Tileset* ts = mapTileset(ws);
    if (!ts) throw std::runtime_error("the map has no tiles layer: call tiles_init first");
    if (terrain < 0 || terrain >= int(ts->terrains.size())) throw std::runtime_error("unknown terrain");
    MapData& d = ws.map.data;
    game::TileLayer L = game::loadTileLayer(d.extra["tiles"], *ts, d.w, d.h);
    const game::TileTerrain& t = ts->terrains[size_t(terrain)];
    int fieldIdx = -1;
    if (!t.fieldResource.empty()) {
        auto it = std::find(d.fieldResources.begin(), d.fieldResources.end(), t.fieldResource);
        fieldIdx = int(it - d.fieldResources.begin());
        if (it == d.fieldResources.end()) d.fieldResources.push_back(t.fieldResource);
    }
    forCells(d, rect, a, b, c, e, [&](int x, int y) {
        size_t i = d.idx(x, y);
        const game::TileTerrain& old = ts->terrains[std::min<size_t>(L.terrain[i], ts->terrains.size() - 1)];
        L.terrain[i] = uint8_t(terrain);
        L.fixed[i] = fixedTile;
        // gameplay bake: what the simulation will load
        d.cells[i] = t.cell;
        if (t.level >= 0) d.levels[i] = int8_t(t.level);
        if (fieldIdx >= 0) { d.fieldRes[i] = int8_t(fieldIdx); d.fieldAmt[i] = uint8_t(t.fieldDensity); }
        else if (!old.fieldResource.empty()) { d.fieldRes[i] = -1; d.fieldAmt[i] = 0; }
    });
    d.extra.set("tiles", game::tileLayerToJson(L, *ts));
    ws.map.touched();
}

} // namespace

std::vector<std::string> listTilesets(const Workspace& ws) {
    std::vector<std::string> out;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(fs::path(ws.project.dir) / "tilesets", ec))
        if (e.path().extension() == ".json") out.push_back("tilesets/" + e.path().filename().string());
    std::sort(out.begin(), out.end());
    return out;
}

const game::Tileset* tileset(const Workspace& ws, const std::string& path, std::string* err) {
    std::string full = ws.project.dir + "/" + path;
    std::error_code ec;
    auto mt = fs::last_write_time(full, ec);
    if (ec) { if (err) *err = "tileset not found: " + path; return nullptr; }
    Cached& c = cache()[full];
    if ((!c.ts && c.error.empty()) || c.mtime != mt) {
        c.mtime = mt; c.ts.reset(); c.error.clear();
        try { c.ts = std::make_unique<game::Tileset>(game::loadTileset(Json::parseFile(full))); }
        catch (const std::exception& e) { c.error = e.what(); }
    }
    if (!c.ts && err) *err = c.error;
    return c.ts.get();
}

const game::Tileset* mapTileset(const Workspace& ws, std::string* err) {
    const Json& t = ws.map.data.extra["tiles"];
    if (!t.isObject()) { if (err) *err = "the map has no tiles layer"; return nullptr; }
    return tileset(ws, t.getStr("tileset"), err);
}

void initTiles(Workspace& ws, const std::string& path, const std::string& fill, bool fromTerrain) {
    std::string err;
    const game::Tileset* ts = tileset(ws, path, &err);
    if (!ts) throw std::runtime_error(err);
    MapData& d = ws.map.data;
    int fi = fill.empty() ? 0 : ts->find(fill);
    if (fi < 0) throw std::runtime_error("tileset '" + ts->name + "': unknown terrain '" + fill + "'");
    game::TileLayer L;
    L.tileset = path; L.w = d.w; L.h = d.h;
    L.terrain.assign(size_t(d.w * d.h), uint8_t(fi));
    L.fixed.assign(size_t(d.w * d.h), -1);
    for (int i = 0; i < d.w * d.h; ++i) {
        if (!fromTerrain) {
            const auto& t = ts->terrains[size_t(fi)];
            d.cells[size_t(i)] = t.cell;
            if (t.level >= 0) d.levels[size_t(i)] = int8_t(t.level);
            continue;
        }
        // keep the gameplay terrain: pick the first terrain with the same cell kind (and level)
        int best = -1;
        for (size_t k = 0; k < ts->terrains.size() && best < 0; ++k) {
            const auto& t = ts->terrains[k];
            if (t.cell == d.cells[size_t(i)] && (t.level < 0 || t.level == d.levels[size_t(i)]) && t.fieldResource.empty()) best = int(k);
        }
        if (best >= 0) L.terrain[size_t(i)] = uint8_t(best);
    }
    d.extra.set("tiles", game::tileLayerToJson(L, *ts));
    ws.map.touched();
}

void paintTiles(Workspace& ws, int cx, int cy, int r, int terrain, int fixedTile) { paintImpl(ws, false, cx, cy, r, 0, terrain, fixedTile); }
void paintTilesRect(Workspace& ws, int x0, int y0, int x1, int y1, int terrain, int fixedTile) { paintImpl(ws, true, x0, y0, x1, y1, terrain, fixedTile); }

void registerTileOps(Workspace& ws) {
    auto add = [&ws](const char* name, const char* desc, const char* sch, Workspace::OpFn fn) {
        ws.registerOp({name, desc, Json::parse(sch), std::move(fn)});
    };
    Workspace* w = &ws;
    auto terrainRef = [](const game::Tileset& ts, const Json& p) {
        std::string id = p.getStr("terrain");
        int t = ts.find(id);
        if (t < 0) throw std::runtime_error("tileset '" + ts.name + "': unknown terrain '" + id + "' (see tileset_list)");
        return t;
    };

    add("tileset_list", "Tilesets of the open game (games/<id>/tilesets/*.json) with their terrains: id, map char, gameplay cell kind, level, resource field, autotile type. The map's tile layer uses one of them.",
        "{}", [w](const Json&) {
            if (w->project.dir.empty()) throw std::runtime_error("no game is open: call open_game first");
            Json list = Json::array();
            for (auto& path : listTilesets(*w)) {
                Json e = Json::object();
                e.set("path", str(path));
                std::string err;
                const game::Tileset* ts = tileset(*w, path, &err);
                if (!ts) { e.set("error", str(err)); list.push(e); continue; }
                e.set("name", str(ts->name));
                e.set("atlas", str(ts->atlas));
                Json terr = Json::array();
                for (auto& t : ts->terrains) {
                    Json tj = Json::object();
                    tj.set("id", str(t.id)); tj.set("name", str(t.name));
                    tj.set("char", str(std::string(1, t.ch))); tj.set("cell", str(std::string(1, t.cell)));
                    if (t.level >= 0) tj.set("level", num(t.level));
                    if (!t.fieldResource.empty()) tj.set("field", str(t.fieldResource));
                    const char* kinds[] = {"none", "mask16", "corner16", "blob47"};
                    tj.set("autotile", str(kinds[int(t.autoKind)]));
                    terr.push(tj);
                }
                e.set("terrains", terr);
                list.push(e);
            }
            Json r = Json::object();
            r.set("tilesets", list);
            const Json& cur = w->map.data.extra["tiles"];
            if (cur.isObject()) r.set("mapTileset", str(cur.getStr("tileset")));
            return r;
        });
    add("tiles_init", "Give the open map a tile layer (2D tile games): every cell gets terrain 'fill' (default: the tileset's first), or with fromTerrain=true the first terrain matching each cell's current kind/level. Replaces an existing tile layer.",
        R"({"properties": {"tileset": {"type": "string", "description": "e.g. tilesets/desert.json"}, "fill": {"type": "string"}, "fromTerrain": {"type": "boolean"}}, "required": ["tileset"]})",
        [w](const Json& p) {
            if (w->project.dir.empty()) throw std::runtime_error("no game is open: call open_game first");
            w->map.beginEdit();
            initTiles(*w, p.getStr("tileset"), p.getStr("fill"), p.getBool("fromTerrain", false));
            w->note({Change::Cells, 0, 0, w->map.data.w - 1, w->map.data.h - 1, -1, "Тайлы: " + p.getStr("tileset")});
            Json r = Json::object();
            r.set("ok", Json::fromBool(true));
            r.set("summary", str("Слой тайлов " + p.getStr("tileset")));
            return r;
        });
    add("tile_paint", "Paint a tileset terrain on the map's tile layer at a circle (x, y, radius) or rect [x0,y0,x1,y1]. Edges/corners are chosen automatically (autotile) from neighbours; the terrain's gameplay (cell kind, cliff level, resource field) is written into the map. 'tile' places one exact atlas tile instead (autotile off). Undoable.",
        R"({"properties": {"terrain": {"type": "string"}, "x": {"type": "integer"}, "y": {"type": "integer"}, "radius": {"type": "integer"},
            "rect": {"type": "array", "items": {"type": "number"}}, "tile": {"type": "integer"}}, "required": ["terrain"]})",
        [w, terrainRef](const Json& p) {
            std::string err;
            const game::Tileset* ts = mapTileset(*w, &err);
            if (!ts) throw std::runtime_error(err + " (call tiles_init first)");
            int t = terrainRef(*ts, p);
            int tile = int(p.getInt("tile", -1));
            w->map.beginEdit();
            int x0, y0, x1, y1;
            if (p.has("rect")) {
                x0 = int(p["rect"][0].toDouble()); y0 = int(p["rect"][1].toDouble()); x1 = int(p["rect"][2].toDouble()); y1 = int(p["rect"][3].toDouble());
                paintTilesRect(*w, x0, y0, x1, y1, t, tile);
            } else {
                if (!p.has("x") || !p.has("y")) throw std::runtime_error("expected x, y (and radius) or rect");
                int x = int(p.getInt("x")), y = int(p.getInt("y")), r = int(p.getInt("radius", 0));
                paintTiles(*w, x, y, r, t, tile);
                x0 = x - r; y0 = y - r; x1 = x + r; y1 = y + r;
            }
            w->note({Change::Cells, std::min(x0, x1), std::min(y0, y1), std::max(x0, x1), std::max(y0, y1), -1, "Тайлы: " + p.getStr("terrain")});
            Json r = Json::object();
            r.set("ok", Json::fromBool(true));
            r.set("summary", str("Тайлы «" + p.getStr("terrain") + "»"));
            return r;
        });
    add("tile_info", "What the tile layer has at cell (x, y): terrain and the atlas tiles autotiling picked (under/top).",
        R"({"properties": {"x": {"type": "integer"}, "y": {"type": "integer"}}, "required": ["x", "y"]})", [w](const Json& p) {
            std::string err;
            const game::Tileset* ts = mapTileset(*w, &err);
            if (!ts) throw std::runtime_error(err);
            const MapData& d = w->map.data;
            int x = int(p.getInt("x")), y = int(p.getInt("y"));
            if (!d.inside(x, y)) throw std::runtime_error("cell outside the map");
            game::TileLayer L = game::loadTileLayer(d.extra["tiles"], *ts, d.w, d.h);
            game::CellTiles ct = game::resolveCell(*ts, L, x, y);
            Json r = Json::object();
            r.set("terrain", str(ts->terrains[L.at(x, y)].id));
            r.set("under", num(ct.under)); r.set("top", num(ct.top));
            r.set("cell", str(std::string(1, d.cells[d.idx(x, y)])));
            r.set("level", num(d.levels[d.idx(x, y)]));
            return r;
        });
    add("tiles_remove", "Remove the tile layer of the open map (gameplay terrain stays as baked).", "{}", [w](const Json&) {
        w->map.beginEdit();
        w->map.data.extra.erase("tiles");
        w->map.touched();
        w->note({Change::Map, 0, 0, -1, -1, -1, "Слой тайлов удалён"});
        Json r = Json::object();
        r.set("ok", Json::fromBool(true));
        r.set("summary", str("Слой тайлов удалён"));
        return r;
    });
}

} // namespace rts::editor
