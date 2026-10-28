// Tile maps for 2D games (Red Alert / Dune II / Warcraft II): tilesets, the map's "tiles" layer and
// autotiling. Integer-only and deterministic: the same code decides gameplay terrain (cell kinds,
// cliff levels, resource fields written into the map's existing layers before the simulation
// loads it) and, in the view/editor, which atlas tile each cell shows.
//
// Tileset (games/<id>/tilesets/<name>.json):
//   {"name": "Desert", "atlas": "tilesets/desert.png", "tileSize": [32, 32], "columns": 8, "filter": "nearest",
//    "terrains": [                                   // array order = draw priority (later draws over earlier)
//      {"id": "sand", "char": "s", "cell": ".", "tiles": [0, 1, {"tile": 2, "weight": 0.2}]},
//      {"id": "water", "char": "w", "cell": "~", "under": "sand",
//       "autotile": {"type": "blob47", "first": 16}},  // or "tiles": [47 indices]; "with": ["deep"]
//      {"id": "spice", "char": "o", "cell": ".", "field": {"resource": "spice", "density": 7}, ...}]}
//
// Map: "tiles": {"tileset": "tilesets/desert.json", "rows": ["sssw", ...], "fixed": [[x, y, tile], ...], "seed": 0}
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "../../engine/data/Json.h"

namespace rts::game {

enum class AutoTile : uint8_t { None, Mask16, Corner16, Blob47 };

struct TileVariant { int tile = 0; int weight = 1; };   // weight in 1/1000 (integer, deterministic)

struct TileTerrain {
    std::string id, name;
    char ch = '?';                 // character in the map's tiles.rows
    char cell = '.';               // gameplay cell kind written into terrain.rows (see RtsMap.h)
    int level = -1;                // cliff level written into terrain.levels (-1 keeps the map's)
    std::string fieldResource;     // non-empty: a resource field of `fieldDensity` (1..9) on these cells
    int fieldDensity = 0;
    uint8_t color[3] = {128, 128, 128};  // palette / minimap colour
    std::vector<TileVariant> tiles;      // base (interior) variants
    AutoTile autoKind = AutoTile::None;
    std::vector<int> autoTiles;          // 16 or 47 atlas indices, by canonical mask order
    std::vector<int> same;               // terrain indices that count as "same" for the mask (incl. self)
    int under = -1;                      // terrain whose base is drawn below the autotile (transparent edges)
    int depleted = -1;                   // view only: terrain shown once the cell's field is used up
};

struct Tileset {
    std::string name, atlas;
    int tileW = 32, tileH = 32, columns = 8;
    bool nearest = true;
    std::vector<TileTerrain> terrains;
    int find(const std::string& id) const;
    int findChar(char c) const;
};

Tileset loadTileset(const Json& j);   // throws std::runtime_error with a readable message

struct TileLayer {
    std::string tileset;
    int w = 0, h = 0;
    std::vector<uint8_t> terrain;        // terrain index per cell
    std::vector<int> fixed;              // per cell: explicit atlas tile (autotile off), -1 = automatic
    uint32_t seed = 0;
    bool inside(int x, int y) const { return x >= 0 && y >= 0 && x < w && y < h; }
    int at(int x, int y) const { return terrain[size_t(y * w + x)]; }
};

// Unknown characters use terrain 0. Missing rows: terrain 0.
TileLayer loadTileLayer(const Json& tiles, const Tileset& ts, int w, int h);
Json tileLayerToJson(const TileLayer& layer, const Tileset& ts);

// The tiles one cell shows: `under` (may be -1) then `top`.
struct CellTiles { int under = -1, top = -1; };
CellTiles resolveCell(const Tileset& ts, const TileLayer& layer, int x, int y);

// Canonical 47-tile blob order: index of an 8-neighbour mask (N=1 NE=2 E=4 SE=8 S=16 SW=32 W=64 NW=128).
int blob47Index(int mask8);
uint32_t tileHash(uint32_t seed, int x, int y);

// Gameplay: overwrite terrain.rows / terrain.levels and the tiles-made resource field of `map`
// from its tiles layer. Idempotent. Old maps (no "tiles") are untouched.
void applyTilesToMap(Json& map, const Tileset& ts);
// Same, reading the tileset with `read(relativePath, out)`. Returns false with `err` on failure;
// returns true when the map has no tiles layer.
bool applyTilesToMap(Json& map, const std::function<bool(const std::string&, std::string&)>& read, std::string* err);

} // namespace rts::game
