// Shared state of one generator run (MapGen.cpp, GenTerrain.cpp, GenContent.cpp). Internal.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "GenField.h"
#include "GenNoise.h"
#include "MapGen.h"
#include <algorithm>
#include <array>
#include "modules/rts/RtsMap.h"

namespace rts::gen {

enum Biome : uint8_t { BOcean, BCoast, BGrass, BForest, BDesert, BSteppe, BRock, BSnow, BSwamp, BCount };
const char* biomeName(int b);

struct P2 { int64_t x = 0, y = 0; };     // Q16 cell units

struct Ctx {
    const game::Rules& R;
    const Json& rulesJson;
    Json P;                               // merged params
    std::string preset;
    uint32_t seed = 1;
    uint64_t seed64 = 1;
    int w = 64, h = 64, np = 2;
    Sym sym;
    game::TerrainModel model = game::TerrainModel::Levels;
    std::string shape;                    // terrain shape of the preset
    Grid hg;                              // corner heights, kHU per world unit
    int32_t sea = 0;
    int32_t relief = 6 * kHU;
    std::vector<char> cell;               // map rows characters
    std::vector<int8_t> lvl;
    std::vector<uint8_t> biome, occ;      // occ: 1 object, 2 corridor, 3 base core
    std::vector<int32_t> moist;
    std::vector<P2> starts;               // per player (Q16 cell units, base centre)
    Json entities = Json::array(), fields = Json::array(), roads = Json::array();
    std::vector<std::string> warnings;
    Json info = Json::object();
    Json tmpl = Json::object();                       // template map: player 1's kit, neutral object counts
    Json regions;                                     // generated regions block (Null: none)
    std::vector<std::pair<std::string, int>> ids;   // entity ids (roads)
    char digPlain = 0, digGold = 0, digGem = 0;      // diggable characters from the rules (caves)
    std::vector<std::vector<int>> corridors;          // cell paths kept free of objects (connectivity)
    std::string connect = "ground";                   // ground | shallows | none (naval / air only)
    std::vector<std::array<int, 4>> blockRects;       // placed structure footprints (x0, y0, w, h)
    std::vector<std::string> layerIds;                // visual terrain layer ids (material mapping)
    int levels = 1;                                   // cliff levels above the lowest (levels model)
    int32_t levelStep = kHU;
    int baseRadius = 7;
    bool rockBases = false;                           // rules have structures that need rock (Dune)

    Ctx(const game::Rules& r, const Json& rj) : R(r), rulesJson(rj) {}
    int ci(int x, int y) const { return y * w + x; }
    bool inside(int x, int y) const { return x >= 0 && y >= 0 && x < w && y < h; }
    int32_t pq(const char* k, int32_t def) const { const Json& v = P[k]; return v.isNumber() ? int32_t(v.fx().raw) : def; }
    int pint(const char* k, int def) const { const Json& v = P[k]; return v.isNumber() ? int(v.integer(def)) : def; }
    bool pbool(const char* k, bool def) const { return P[k].type() == Json::Type::Bool ? P[k].boolean(def) : def; }
    std::string pstr(const char* k, const std::string& def) const { return P[k].isString() ? P[k].str() : def; }
    uint32_t sub(uint32_t salt) const { return hash32(seed ^ hash32(salt)); }
    // symmetric edits: apply fn to every image of the cell (x, y) (after folding it into the domain)
    template <class Fn> void forImages(int x, int y, Fn&& fn) const {
        if (sym.kind == Sym::None) { fn(x, y, 0); return; }
        int fx, fy;
        sym.foldCell(x, y, fx, fy);
        for (int k = 0; k < sym.n; ++k) { int ix, iy; sym.imageCell(k, fx, fy, ix, iy); fn(ix, iy, k); }
    }
    template <class Fn> void forCornerImages(int x, int y, Fn&& fn) const {
        if (sym.kind == Sym::None) { fn(x, y); return; }
        int fx, fy;
        sym.foldCorner(x, y, fx, fy);
        for (int k = 0; k < sym.n; ++k) {
            int64_t px, py;
            sym.image(k, int64_t(fx) << 16, int64_t(fy) << 16, px, py);
            fn(std::clamp(int((px + 32768) >> 16), 0, w), std::clamp(int((py + 32768) >> 16), 0, h));
        }
    }
    void warn(const std::string& s) { warnings.push_back(s); }
};

// GenTerrain.cpp
void genHeights(Ctx& c);
void flattenBases(Ctx& c);
void classifyCells(Ctx& c);
void genLanesTerrain(Ctx& c);
void genCavesTerrain(Ctx& c);
void ensureConnectivity(Ctx& c);
void buildGrid(const Ctx& c, game::MapGrid& g, bool withObjects);
// reachable cells from (sx, sy) on the ground layer (diggable cells count as passable when `dig`)
std::vector<uint8_t> reachable(const Ctx& c, const game::MapGrid& g, int sx, int sy, bool dig = false);
std::vector<int16_t> cornersOut(const Ctx& c);
// cheapest 4-connected cell path a -> b; cells the grid does not let a ground unit step into cost extra (water less)
// objects: per-cell object mask; then `g` is the terrain-only grid and object cells cost extra (unblocking)
std::vector<int> repairPath(const Ctx& c, const game::MapGrid& g, int ax, int ay, int bx, int by, bool dig,
                            const std::vector<uint8_t>* objects = nullptr);
Json materialRows(const Ctx& c);          // terrainMaterial rows (Null when no layer matches)
Json heightsOut(const Ctx& c);            // levels model: fine visual heights per cell (Null when flat)

// GenContent.cpp
void chooseStarts(Ctx& c);
void placeContent(Ctx& c);
void validateFairness(Ctx& c);
void unblockStarts(Ctx& c);               // removes objects that cut starts off from each other
void genRegions(Ctx& c);                  // Voronoi regions (when the template map has regions)
std::string fmtQ16(int64_t raw);          // exact decimal text of a Q16 value (multiples of 1/16 stay short)

} // namespace rts::gen
