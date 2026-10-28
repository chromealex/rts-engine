// Terrain grid (cell types, cliff levels, ramps, creep, resource fields, regions), pathfinding
// (A* on 8-connected cells with level/ramp rules), road graph, spatial hash. Integer/fixed only.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "../../engine/data/Json.h"
#include "../../engine/core/ECS.h"
#include "../../engine/core/Fixed.h"
#include "RtsSurface.h"
#include <memory>

namespace rts::game {

enum CellFlags : uint8_t {
    kCellGround = 1,   // walkable for ground units
    kCellWater = 2,    // navigable for naval units
    kCellNoBuild = 4,  // no construction
    kCellRamp = 8,     // connects two cliff levels
    kCellRock = 16,    // rock (Dune: buildable; sand-only units can't enter)
    kCellRoad = 32,    // player road network (settlers economy): faster movement, connects buildings
};

// Terrain characters in maps:
//   '.' ground   '~' water   '#' blocked   ',' ground, no build   '=' shallow (ground+water)
//   '/' ramp     'R' rock (ground, buildable when rules require rock)   '+' road (ground)
uint8_t cellFlagsFromChar(char c);

constexpr int kMaxMapSize = 2048;   // cells per side

// Heightfield corners of big maps live in a raw file: terrain.cornersFile = "maps/<name>.r16", (w+1)*(h+1)
// int16 little-endian, row-major. Loaders read it with applyCornersFile (applyTilesToMap does it too), which
// attaches the bytes to the map JSON as terrain.cornersBlob (in memory only; never written to disk).
using ContentReader = std::function<bool(const std::string&, std::string&)>;
bool applyCornersFile(Json& map, const ContentReader& read, std::string* err);
std::string encodeCorners(const std::vector<int16_t>& corners);  // raw int16 LE bytes
bool decodeCorners(const Json& terrain, int w, int h, std::vector<int16_t>& out, std::string* err);

// Hierarchical pathfinding (HPA*) for big maps: the map is cut into kC x kC clusters; entrances on cluster
// borders are abstract nodes, linked by intra-cluster path costs and by the one-step border crossings.
// Derived from the grid (never hashed); clusters touched by structures / digging are rebuilt lazily.
struct HpaGraph {
    static constexpr int kC = 16;
    uint8_t layer = 0;
    int cw = 0, ch = 0;
    bool anyDirty = true;
    std::vector<uint8_t> dirty;                                   // per cluster
    std::vector<std::vector<std::pair<int32_t, int32_t>>> eastT;  // per cluster: (own cell, east neighbour cell)
    std::vector<std::vector<std::pair<int32_t, int32_t>>> southT; // per cluster: (own cell, south neighbour cell)
    std::vector<std::vector<int32_t>> nodes;                      // per cluster: sorted entrance cells
    std::vector<std::vector<std::vector<int32_t>>> partners;      // per cluster, per node: cells across the border
    std::vector<std::vector<uint32_t>> dist;                      // per cluster: k*k intra costs (UINT32_MAX: none)
};

struct MapGrid {
    int w = 0, h = 0;
    // Planet maps (world block): the cube-sphere faces of every body stacked as rows (RtsSurface.h). Null on flat
    // maps, where every function below keeps its classic behaviour bit for bit.
    std::shared_ptr<const Surface> surf;
    bool sphere() const { return surf != nullptr; }
    std::vector<uint8_t> flags;
    std::vector<int8_t> level;      // cliff level 0..9
    std::vector<int16_t> height;    // fine height in 1/16 units (visual + editor)
    // Heightfield terrain: heights at cell corners, (w+1)*(h+1), 1/16 units, row-major (corner (x,y) =
    // top-left corner of cell (x,y)). Empty = no heightfield (flat / cliff-level maps). Map JSON:
    // terrain.corners. Simulation use is governed by rules terrain.model (see docs/terrain.md).
    std::vector<int16_t> corner;
    bool hasCorners() const { return !corner.empty(); }
    int cornerIndex(int x, int y) const { return y * (w + 1) + x; }
    // Heightfield simulation (rules terrain.model "heightfield" + corners): set by enableHeightfield().
    // Heights are corner / 16 world units. slope: per cell steepness (rise per cell, 1/256 units, derived).
    bool hf = false;
    std::vector<uint16_t> slope;
    uint16_t slopeLimit[16] = {};   // [0]: ground default, [k]: slope class k (layer bits 4..7)
    uint16_t sandLimit = 0, buildLimit = 0;
    static uint16_t slopeUnits(Fixed risePerCell);  // Fixed -> 1/256 units, saturating
    void enableHeightfield(Fixed levelHeight);      // derives slope + cliff levels (floor(h / levelHeight))
    Fixed surfaceHeight(Fixed x, Fixed z) const;    // bilinear on corners (0 without corners)
    Fixed cellHeight(int x, int y) const;           // centre of the cell: mean of its 4 corners
    Vec3 gradient(Fixed x, Fixed z) const;          // dh/dx, 0, dh/dz of the cell under (x, z)
    uint16_t slopeLimitFor(uint8_t layer) const {
        const int cls = layer >> 4;
        return cls ? slopeLimit[cls] : (layer & 8) ? sandLimit : slopeLimit[0];
    }
    std::vector<uint16_t> blocked;  // structures covering the cell
    std::vector<uint8_t> creep;     // >0: creep present
    std::vector<int16_t> fieldRes;  // resource id of the field in this cell, -1 none
    std::vector<Fixed> fieldAmt;
    std::vector<int16_t> region;    // region index, -1 none
    // dungeon / territory layers
    std::vector<uint8_t> dig;       // diggable kind + 1 (0: not diggable); such cells are solid until dug
    std::vector<Fixed> digHp;       // remaining hardness
    std::vector<uint8_t> digMark;   // bit (slot-1): marked for digging by that player
    std::vector<uint8_t> territory; // owner slot of the cell (0: nobody) - DK floor, territory-war front
    std::vector<int16_t> room;      // painted room type on the cell, -1 none (owner = territory)
    // pathing helpers (not part of the hashed state: derived from it)
    std::vector<uint8_t> clearance; // ground cells: Chebyshev distance to the nearest non-walkable cell (capped)
    bool clearanceDirty = true;
    std::vector<uint8_t> soft;      // per cell: stationary friendly units (soft obstacles for surround paths)
    std::vector<int32_t> softCells; // cells set in `soft` (cleared each tick without a whole-map pass)
    void clearSoft() { for (int32_t i : softCells) soft[size_t(i)] = 0; softCells.clear(); }
    void markSoft(int x, int y) { uint8_t& s = soft[size_t(index(x, y))]; if (!s) { s = 1; softCells.push_back(index(x, y)); } }

    void init(int width, int height_);
    bool inside(int x, int y) const { return x >= 0 && y >= 0 && x < w && y < h; }
    int index(int x, int y) const { return y * w + x; }
    bool passable(int x, int y, uint8_t layer) const;
    // Can a unit step from (x0,y0) to the neighbour (x1,y1)? Checks cliffs/ramps.
    bool canStep(int x0, int y0, int x1, int y1, uint8_t layer) const;
    int levelAt(int x, int y) const { return inside(x, y) ? level[size_t(index(x, y))] : 0; }
    bool passableAt(Vec3 p, uint8_t layer) const { int x, y; cellXY(p, x, y); return passable(x, y, layer); }
    int cellX(Fixed x) const { return int(x.floorInt()); }
    int cellY(Fixed z) const { return int(z.floorInt()); }
    Vec3 cellCenter(int x, int y) const { if (surf) return surf->cellCenter(index(x, y)); return {Fixed::fromInt(x) + Fixed::fromRatio(1, 2), Fixed{}, Fixed::fromInt(y) + Fixed::fromRatio(1, 2)}; }
    void block(int x0, int y0, int fw, int fh, int delta);
    void cellChanged(int x, int y);  // terrain of a cell changed at runtime (digging): refresh derived pathing data
    void invalidatePathing() { clearanceDirty = true; for (auto& G : hpa_) { std::fill(G.dirty.begin(), G.dirty.end(), uint8_t(1)); G.anyDirty = true; } }   // after loading a save
    bool areaBuildable(int x0, int y0, int fw, int fh) const;  // ground, no-build-free, unblocked, flat
    // clearance: cells needed around the path for a body of that size (0 = any walkable cell); narrower
    // cells cost more instead of being forbidden. soft: also route around cells marked in `soft`.
    bool findPath(Vec3 from, Vec3 to, uint8_t layer, std::vector<Vec3>& out, int maxNodes = 40000, int clearance = 0, bool soft = false);
    void updateClearance();
    bool stepAllowed(int cx, int cy, int nx, int ny, uint8_t layer) const { return stepOk(cx, cy, nx, ny, layer); }
    void prepareHpa(uint8_t layer) { if (bigMap()) hpaRefresh(hpaFor(layer)); } // build at load, not mid-match
    // Big maps (w*h >= hpaMinCells): long paths go through the HPA* graph and are refined with A*.
    int hpaMinCells = 256 * 256;
    bool bigMap() const { return !surf && w * h >= hpaMinCells; }
    int hpaStats[2] = {};  // queries answered by HPA*, fallbacks to plain A* (diagnostics only)
    static int clearanceFor(Fixed radius);  // body radius -> cells of clearance
    bool lineWalkable(int x0, int y0, int x1, int y1, uint8_t layer) const;
    bool nearestPassable(int& x, int& y, uint8_t layer, int radius = 12) const;

    // ---- position <-> cell on any surface (flat: cellX/cellY of p.x/p.z). Grid cell index or -1.
    int cellOf(Vec3 p) const {
        if (surf) return surf->cellAt(p);
        const int x = cellX(p.x), y = cellY(p.z);
        return inside(x, y) ? index(x, y) : -1;
    }
    bool cellXY(Vec3 p, int& x, int& y) const {      // false: outside the map (x, y still set on flat maps)
        if (!surf) { x = cellX(p.x); y = cellY(p.z); return inside(x, y); }
        const int c = surf->cellAt(p);
        x = c < 0 ? -1 : c % w; y = c < 0 ? -1 : c / w;
        return c >= 0;
    }
    Vec3 centerOf(int c) const { return surf ? surf->cellCenter(c) : cellCenter(c % w, c / w); }
    int offsetCell(int c, int dx, int dy) const {    // cell at a grid offset (across seams on spheres), -1 none
        if (c < 0) return -1;
        if (surf) return surf->offset(c, dx, dy);
        const int x = c % w + dx, y = c / w + dy;
        return inside(x, y) ? index(x, y) : -1;
    }
    // One step between neighbouring cells (8-connected, no corner cutting); on spheres via the neighbour table.
    bool canMoveCells(int c0, int c1, uint8_t layer) const;
    // Sphere: derives slopes / cliff levels from the bodies' face corners (rules terrain.model heightfield).
    void enableSphereHeights(Fixed levelHeight);

private:
    bool findPathSphere(Vec3 from, Vec3 to, uint8_t layer, std::vector<Vec3>& out, int maxNodes, int clear, bool soft);
    bool astarSphere(int s, int g, uint8_t layer, int maxNodes, std::vector<int>& cells);
    bool lineWalkableSphere(int a, int b, uint8_t layer) const;
    void updateClearanceSphere();
    int pathClear_ = 0;
    bool pathSoft_ = false;
    int pathEndA_ = -1, pathEndB_ = -1;
    bool lineOk(int x, int y) const;
    bool stepOk(int cx, int cy, int nx, int ny, uint8_t layer) const { // one 8-neighbour move, no corner cutting
        if (!canStep(cx, cy, nx, ny, layer)) return false;
        if (nx == cx || ny == cy) return true;
        return canStep(cx, cy, nx, cy, layer) && canStep(cx, cy, cx, ny, layer) && canStep(nx, cy, nx, ny, layer) && canStep(cx, ny, nx, ny, layer);
    }
    void updateClearanceRect(int x0, int y0, int x1, int y1);
    // A* on cells from (sx,sy) towards (gx,gy); `cells` = start..goal (or the closest reached cell). True if found.
    bool astarCells(int sx, int sy, int gx, int gy, uint8_t layer, int maxNodes, std::vector<int>& cells);
    // HPA*: abstract route (start, entrances..., goal) refined into cells. False: no route (caller falls back).
    bool hpaPath(int sx, int sy, int gx, int gy, uint8_t layer, std::vector<int>& cells);
    HpaGraph& hpaFor(uint8_t layer);
    void hpaRefresh(HpaGraph& G);
    void hpaBorder(HpaGraph& G, int c, bool east);
    void hpaCluster(HpaGraph& G, int c);
    void clusterCosts(int c, int src, uint8_t layer, std::vector<uint32_t>& d) const; // Dijkstra inside cluster c
    void clusterCostsMasked(int c, int src, const std::vector<uint8_t>& mask, std::vector<uint32_t>& d) const;
    std::vector<HpaGraph> hpa_;
    std::vector<uint32_t> gCost_, stamp_;
    std::vector<int32_t> parent_;
    uint32_t search_ = 0;
};

struct RoadGraph {
    std::vector<Entity> nodes;
    std::vector<std::vector<uint16_t>> adj;
    bool empty() const { return nodes.empty(); }
    int nodeOf(Entity e) const;
    std::vector<uint16_t> path(World& w, int from, int to) const;
};

struct SpatialHash {
    int cell = 4, w = 0, h = 0;
    std::vector<std::vector<Entity>> buckets;
    std::vector<int32_t> used;      // non-empty buckets (reset clears only these: big maps have ~65k buckets)
    std::shared_ptr<const Surface> surf;   // planets: buckets are coarse cube-sphere cells per body
    void reset(int worldW, int worldH, int cellSize = 4);
    void insert(Entity e, Vec3 p);
    template <class Fn> void query(Vec3 p, Fixed radius, Fn&& fn) const {
        if (surf) {
            thread_local std::vector<int32_t> near;
            surf->bucketsAround(p, radius, near);
            for (int32_t b : near) for (Entity e : buckets[size_t(b)]) fn(e);
            return;
        }
        int r = int((radius.raw >> Fixed::kFracBits) / cell) + 1;
        int cx = int(p.x.floorInt()) / cell, cy = int(p.z.floorInt()) / cell;
        for (int y = cy - r; y <= cy + r; ++y)
            for (int x = cx - r; x <= cx + r; ++x) {
                if (x < 0 || y < 0 || x >= w || y >= h) continue;
                for (Entity e : buckets[size_t(y * w + x)]) fn(e);
            }
    }
};

} // namespace rts::game
