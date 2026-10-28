#include "RtsTiles.h"
#include "gen/MapGen.h"
#include "RtsMap.h"
#include <algorithm>
#include <array>
#include <stdexcept>

namespace rts::game {

namespace {
std::runtime_error err(const std::string& ts, const std::string& what) { return std::runtime_error("tileset '" + ts + "': " + what); }

std::vector<TileVariant> readVariants(const Json& a) {
    std::vector<TileVariant> out;
    for (auto& v : a.items()) {
        TileVariant tv;
        if (v.isObject()) {
            tv.tile = int(v.getInt("tile"));
            tv.weight = int((v.getFx("weight", Fixed::fromInt(1)) * 1000).toInt());
        } else {
            tv.tile = int(v.integer());
            tv.weight = 1000;
        }
        if (tv.weight > 0) out.push_back(tv);
    }
    return out;
}

int pickVariant(const std::vector<TileVariant>& v, uint32_t h) {
    if (v.empty()) return -1;
    int64_t total = 0;
    for (auto& t : v) total += t.weight;
    int64_t r = int64_t(h % uint32_t(total));
    for (auto& t : v) { if (r < t.weight) return t.tile; r -= t.weight; }
    return v.back().tile;
}

// 8-neighbour masks reduced to the 47 distinct blob shapes (a corner only counts when both of
// its edges are set), sorted ascending: the canonical tile order.
const std::array<int16_t, 256>& blobTable() {
    static const std::array<int16_t, 256> table = [] {
        std::array<int16_t, 256> t{};
        auto reduce = [](int m) {
            const bool n = m & 1, e = m & 4, s = m & 16, w = m & 64;
            if (!(n && e)) m &= ~2;
            if (!(s && e)) m &= ~8;
            if (!(s && w)) m &= ~32;
            if (!(n && w)) m &= ~128;
            return m;
        };
        std::vector<int> uniq;
        for (int m = 0; m < 256; ++m) { int r = reduce(m); if (std::find(uniq.begin(), uniq.end(), r) == uniq.end()) uniq.push_back(r); }
        std::sort(uniq.begin(), uniq.end());
        for (int m = 0; m < 256; ++m) t[size_t(m)] = int16_t(std::find(uniq.begin(), uniq.end(), reduce(m)) - uniq.begin());
        return t;
    }();
    return table;
}
} // namespace

int blob47Index(int mask8) { return blobTable()[size_t(mask8 & 255)]; }

uint32_t tileHash(uint32_t seed, int x, int y) {
    uint32_t h = seed * 0x9e3779b9u ^ uint32_t(x) * 73856093u ^ uint32_t(y) * 19349663u;
    h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15; h *= 0x27d4eb2du; h ^= h >> 16;
    return h;
}

int Tileset::find(const std::string& id) const {
    for (size_t i = 0; i < terrains.size(); ++i) if (terrains[i].id == id) return int(i);
    return -1;
}
int Tileset::findChar(char c) const {
    for (size_t i = 0; i < terrains.size(); ++i) if (terrains[i].ch == c) return int(i);
    return -1;
}

Tileset loadTileset(const Json& j) {
    Tileset ts;
    ts.name = j.getStr("name", "tileset");
    ts.atlas = j.getStr("atlas");
    if (ts.atlas.empty()) throw err(ts.name, "missing \"atlas\" (image path)");
    ts.tileW = int(j["tileSize"][0].integer(32));
    ts.tileH = int(j["tileSize"][1].integer(ts.tileW));
    ts.columns = int(j.getInt("columns", 8));
    if (ts.tileW <= 0 || ts.tileH <= 0 || ts.columns <= 0) throw err(ts.name, "tileSize and columns must be positive");
    ts.nearest = j.getStr("filter", "nearest") != "linear";
    const Json& terr = j["terrains"];
    if (!terr.isArray() || terr.size() == 0) throw err(ts.name, "\"terrains\" must be a non-empty array");
    if (terr.size() > 250) throw err(ts.name, "at most 250 terrains");
    for (auto& tj : terr.items()) {
        TileTerrain t;
        t.id = tj.getStr("id");
        if (t.id.empty()) throw err(ts.name, "terrain without \"id\"");
        if (ts.find(t.id) >= 0) throw err(ts.name, "duplicate terrain '" + t.id + "'");
        t.name = tj.getStr("name", t.id);
        std::string ch = tj.getStr("char", t.id.substr(0, 1));
        if (ch.size() != 1) throw err(ts.name, "terrain '" + t.id + "': \"char\" must be one character");
        t.ch = ch[0];
        if (ts.findChar(t.ch) >= 0) throw err(ts.name, "terrain '" + t.id + "': char '" + ch + "' already used");
        std::string cell = tj.getStr("cell", ".");
        if (cell.size() != 1 || std::string(".~#,=/R").find(cell[0]) == std::string::npos)
            throw err(ts.name, "terrain '" + t.id + "': \"cell\" must be one of . ~ # , = / R");
        t.cell = cell[0];
        t.level = int(tj.getInt("level", -1));
        if (t.level > 9) throw err(ts.name, "terrain '" + t.id + "': level must be 0..9");
        if (const Json& f = tj["field"]; f.isObject()) {
            t.fieldResource = f.getStr("resource");
            t.fieldDensity = int(std::clamp<int64_t>(f.getInt("density", 9), 1, 9));
            if (t.fieldResource.empty()) throw err(ts.name, "terrain '" + t.id + "': field needs \"resource\"");
        }
        const Json& c = tj["color"];
        for (size_t k = 0; k < 3 && k < c.size(); ++k) t.color[k] = uint8_t(std::clamp<int64_t>((c[k].fx() * 255).toInt(), 0, 255));
        t.tiles = readVariants(tj["tiles"]);
        if (const Json& a = tj["autotile"]; a.isObject()) {
            std::string type = a.getStr("type", "blob47");
            t.autoKind = type == "mask16" ? AutoTile::Mask16 : type == "corner16" ? AutoTile::Corner16 : type == "blob47" ? AutoTile::Blob47 : AutoTile::None;
            if (t.autoKind == AutoTile::None) throw err(ts.name, "terrain '" + t.id + "': unknown autotile type '" + type + "' (mask16, corner16, blob47)");
            size_t n = t.autoKind == AutoTile::Blob47 ? 47 : 16;
            if (a.has("tiles")) for (auto& v : a["tiles"].items()) t.autoTiles.push_back(int(v.integer()));
            else { int first = int(a.getInt("first", 0)); for (size_t k = 0; k < n; ++k) t.autoTiles.push_back(first + int(k)); }
            if (t.autoTiles.size() != n) throw err(ts.name, "terrain '" + t.id + "': " + type + " needs " + std::to_string(n) + " tiles");
        }
        if (t.tiles.empty() && t.autoTiles.empty()) throw err(ts.name, "terrain '" + t.id + "' has no \"tiles\" and no \"autotile\"");
        ts.terrains.push_back(std::move(t));
    }
    // second pass: references between terrains
    for (size_t i = 0; i < terr.size(); ++i) {
        const Json& tj = terr[i];
        TileTerrain& t = ts.terrains[i];
        t.same.push_back(int(i));
        for (auto& w : tj["autotile"]["with"].items()) {
            int k = ts.find(w.str());
            if (k < 0) throw err(ts.name, "terrain '" + t.id + "': autotile.with: unknown terrain '" + w.str() + "'");
            t.same.push_back(k);
        }
        if (tj.has("depleted")) {
            t.depleted = ts.find(tj.getStr("depleted"));
            if (t.depleted < 0) throw err(ts.name, "terrain '" + t.id + "': \"depleted\": unknown terrain '" + tj.getStr("depleted") + "'");
        }
        if (tj.has("under")) {
            t.under = ts.find(tj.getStr("under"));
            if (t.under < 0 || ts.terrains[size_t(t.under)].tiles.empty())
                throw err(ts.name, "terrain '" + t.id + "': \"under\" must name a terrain with base tiles");
        }
    }
    return ts;
}

TileLayer loadTileLayer(const Json& tiles, const Tileset& ts, int w, int h) {
    TileLayer L;
    L.tileset = tiles.getStr("tileset");
    L.w = w; L.h = h;
    L.terrain.assign(size_t(w * h), 0);
    L.fixed.assign(size_t(w * h), -1);
    L.seed = uint32_t(tiles.getInt("seed", 0));
    const Json& rows = tiles["rows"];
    for (size_t y = 0; y < rows.size() && int(y) < h; ++y) {
        std::string r = rows[y].str();
        for (size_t x = 0; x < r.size() && int(x) < w; ++x) L.terrain[y * size_t(w) + x] = uint8_t(std::max(0, ts.findChar(r[x])));
    }
    for (auto& f : tiles["fixed"].items()) {
        int x = int(f[0].integer()), y = int(f[1].integer());
        if (L.inside(x, y)) L.fixed[size_t(y * w + x)] = int(f[2].integer());
    }
    return L;
}

Json tileLayerToJson(const TileLayer& L, const Tileset& ts) {
    Json j = Json::object();
    j.set("tileset", Json::string(L.tileset));
    if (L.seed) j.set("seed", Json::number(int64_t(L.seed)));
    Json rows = Json::array();
    for (int y = 0; y < L.h; ++y) {
        std::string r(size_t(L.w), '?');
        for (int x = 0; x < L.w; ++x) r[size_t(x)] = ts.terrains[std::min<size_t>(L.at(x, y), ts.terrains.size() - 1)].ch;
        rows.push(Json::string(r));
    }
    j.set("rows", rows);
    Json fixed = Json::array();
    for (int y = 0; y < L.h; ++y)
        for (int x = 0; x < L.w; ++x)
            if (int t = L.fixed[size_t(y * L.w + x)]; t >= 0) {
                Json e = Json::array();
                e.push(Json::number(int64_t(x))); e.push(Json::number(int64_t(y))); e.push(Json::number(int64_t(t)));
                fixed.push(e);
            }
    if (fixed.size()) j.set("fixed", fixed);
    return j;
}

CellTiles resolveCell(const Tileset& ts, const TileLayer& L, int x, int y) {
    CellTiles out;
    if (!L.inside(x, y) || ts.terrains.empty()) return out;
    const int ti = std::min<int>(L.at(x, y), int(ts.terrains.size()) - 1);
    const TileTerrain& t = ts.terrains[size_t(ti)];
    const uint32_t h = tileHash(L.seed, x, y);
    if (int f = L.fixed[size_t(y * L.w + x)]; f >= 0) {
        out.top = f;
        if (t.under >= 0) out.under = pickVariant(ts.terrains[size_t(t.under)].tiles, tileHash(L.seed + 1, x, y));
        return out;
    }
    if (t.autoKind == AutoTile::None) { out.top = pickVariant(t.tiles, h); return out; }
    // out-of-map neighbours count as the same terrain: no shore along the map border
    auto same = [&](int dx, int dy) {
        if (!L.inside(x + dx, y + dy)) return true;
        int n = L.at(x + dx, y + dy);
        return std::find(t.same.begin(), t.same.end(), n) != t.same.end();
    };
    const bool N = same(0, -1), E = same(1, 0), S = same(0, 1), W = same(-1, 0);
    int index = 0;
    bool full = false;
    switch (t.autoKind) {
    case AutoTile::Mask16:
        index = (N ? 1 : 0) | (E ? 2 : 0) | (S ? 4 : 0) | (W ? 8 : 0);
        full = index == 15 && same(1, -1) && same(1, 1) && same(-1, 1) && same(-1, -1);
        break;
    case AutoTile::Corner16:
        index = (N && E && same(1, -1) ? 1 : 0) | (S && E && same(1, 1) ? 2 : 0) | (S && W && same(-1, 1) ? 4 : 0) | (N && W && same(-1, -1) ? 8 : 0);
        full = index == 15;
        break;
    default: {
        int m = (N ? 1 : 0) | (same(1, -1) ? 2 : 0) | (E ? 4 : 0) | (same(1, 1) ? 8 : 0) | (S ? 16 : 0) | (same(-1, 1) ? 32 : 0) | (W ? 64 : 0) | (same(-1, -1) ? 128 : 0);
        index = blob47Index(m);
        full = m == 255;
    }
    }
    if (full && !t.tiles.empty()) { out.top = pickVariant(t.tiles, h); return out; }
    out.top = t.autoTiles[size_t(index)];
    if (!full && t.under >= 0) out.under = pickVariant(ts.terrains[size_t(t.under)].tiles, tileHash(L.seed + 1, x, y));
    return out;
}

void applyTilesToMap(Json& map, const Tileset& ts) {
    const Json& tiles = map["tiles"];
    if (!tiles.isObject()) return;
    int w = int(map["size"][0].integer(64)), h = int(map["size"][1].integer(64));
    if (w <= 0 || h <= 0 || w > 1024 || h > 1024) return;
    TileLayer L = loadTileLayer(tiles, ts, w, h);
    Json& terrain = map.member("terrain");
    auto readRows = [&](const char* key, char fill) {
        std::vector<std::string> rows(size_t(h), std::string(size_t(w), fill));
        const Json& src = terrain[key];
        for (size_t y = 0; y < src.size() && y < rows.size(); ++y) {
            std::string r = src[y].str();
            for (size_t x = 0; x < r.size() && x < size_t(w); ++x) rows[y][x] = r[x];
        }
        return rows;
    };
    auto writeRows = [](const std::vector<std::string>& rows) { Json a = Json::array(); for (auto& r : rows) a.push(Json::string(r)); return a; };
    std::vector<std::string> cells = readRows("rows", '.'), levels = readRows("levels", '0');
    bool anyLevel = false;
    std::vector<std::string> fieldRes;   // resources made by terrains, in tileset order
    for (auto& t : ts.terrains) {
        if (t.level >= 0) anyLevel = true;
        if (!t.fieldResource.empty() && std::find(fieldRes.begin(), fieldRes.end(), t.fieldResource) == fieldRes.end()) fieldRes.push_back(t.fieldResource);
    }
    std::vector<std::vector<std::string>> fieldRows(fieldRes.size(), std::vector<std::string>(size_t(h), std::string(size_t(w), '0')));
    std::vector<uint8_t> fieldCell(size_t(w * h), 0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const TileTerrain& t = ts.terrains[std::min<size_t>(L.at(x, y), ts.terrains.size() - 1)];
            cells[size_t(y)][size_t(x)] = t.cell;
            if (t.level >= 0) levels[size_t(y)][size_t(x)] = char('0' + t.level);
            if (!t.fieldResource.empty()) {
                size_t r = size_t(std::find(fieldRes.begin(), fieldRes.end(), t.fieldResource) - fieldRes.begin());
                fieldRows[r][size_t(y)][size_t(x)] = char('0' + t.fieldDensity);
                fieldCell[size_t(y * w + x)] = 1;
            }
        }
    terrain.set("rows", writeRows(cells));
    if (anyLevel) terrain.set("levels", writeRows(levels));
    if (fieldRes.empty()) return;
    // the tiles own the field on their cells: drop earlier tile-made entries and digits other
    // row fields have there, so loading the same map twice gives the same amounts
    Json fields = Json::array();
    for (auto& f : map["fields"].items()) {
        if (f.getBool("fromTiles", false)) continue;
        Json g = f;
        if (g.has("rows")) {
            Json rows = Json::array();
            for (size_t y = 0; y < g["rows"].size(); ++y) {
                std::string r = g["rows"][y].str();
                for (size_t x = 0; x < r.size() && x < size_t(w) && y < size_t(h); ++x) if (fieldCell[y * size_t(w) + x]) r[x] = '0';
                rows.push(Json::string(r));
            }
            g.set("rows", rows);
        }
        fields.push(g);
    }
    for (size_t r = 0; r < fieldRes.size(); ++r) {
        Json f = Json::object();
        f.set("resource", Json::string(fieldRes[r]));
        f.set("fromTiles", Json::fromBool(true));
        f.set("rows", writeRows(fieldRows[r]));
        fields.push(f);
    }
    map.set("fields", fields);
}

bool applyTilesToMap(Json& map, const std::function<bool(const std::string&, std::string&)>& read, std::string* errOut) {
    if (!applyCornersFile(map, read, errOut)) return false; // big heightfield maps: terrain.cornersFile (RtsMap.h)
    if (!gen::prepareGenerate(map, read, errOut)) return false; // generated maps: players array, template, layers
    if (!map["tiles"].isObject()) return true;
    std::string path = map["tiles"].getStr("tileset"), text;
    try {
        if (path.empty()) throw std::runtime_error("map tiles: missing \"tileset\"");
        if (!read(path, text)) throw std::runtime_error("map tiles: tileset not found: " + path);
        applyTilesToMap(map, loadTileset(Json::parse(text)));
    } catch (const std::exception& e) {
        if (errOut) *errOut = e.what();
        return false;
    }
    return true;
}

} // namespace rts::game
