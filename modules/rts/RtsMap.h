// Terrain grid (cell types, cliff levels, ramps, creep, resource fields, regions), pathfinding
// (A* on 8-connected cells with level/ramp rules), road graph, spatial hash. Integer/fixed only.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "../../engine/core/ECS.h"
#include "../../engine/core/Fixed.h"

namespace rts::game {

enum CellFlags : uint8_t {
    kCellGround = 1,   // walkable for ground units
    kCellWater = 2,    // navigable for naval units
    kCellNoBuild = 4,  // no construction
    kCellRamp = 8,     // connects two cliff levels
    kCellRock = 16,    // rock (Dune: buildable; sand-only units can't enter)
};

// Terrain characters in maps:
//   '.' ground   '~' water   '#' blocked   ',' ground, no build   '=' shallow (ground+water)
//   '/' ramp     'R' rock (ground, buildable when rules require rock)
uint8_t cellFlagsFromChar(char c);

struct MapGrid {
    int w = 0, h = 0;
    std::vector<uint8_t> flags;
    std::vector<int8_t> level;      // cliff level 0..9
    std::vector<int16_t> height;    // fine height in 1/16 units (visual + editor)
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

    void init(int width, int height_);
    bool inside(int x, int y) const { return x >= 0 && y >= 0 && x < w && y < h; }
    int index(int x, int y) const { return y * w + x; }
    bool passable(int x, int y, uint8_t layer) const;
    // Can a unit step from (x0,y0) to the neighbour (x1,y1)? Checks cliffs/ramps.
    bool canStep(int x0, int y0, int x1, int y1, uint8_t layer) const;
    int levelAt(int x, int y) const { return inside(x, y) ? level[size_t(index(x, y))] : 0; }
    int cellX(Fixed x) const { return int(x.floorInt()); }
    int cellY(Fixed z) const { return int(z.floorInt()); }
    Vec3 cellCenter(int x, int y) const { return {Fixed::fromInt(x) + Fixed::fromRatio(1, 2), Fixed{}, Fixed::fromInt(y) + Fixed::fromRatio(1, 2)}; }
    void block(int x0, int y0, int fw, int fh, int delta);
    bool areaBuildable(int x0, int y0, int fw, int fh) const;  // ground, no-build-free, unblocked, flat
    bool findPath(Vec3 from, Vec3 to, uint8_t layer, std::vector<Vec3>& out, int maxNodes = 40000);
    bool lineWalkable(int x0, int y0, int x1, int y1, uint8_t layer) const;
    bool nearestPassable(int& x, int& y, uint8_t layer, int radius = 12) const;

private:
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
    void reset(int worldW, int worldH, int cellSize = 4);
    void insert(Entity e, Vec3 p);
    template <class Fn> void query(Vec3 p, Fixed radius, Fn&& fn) const {
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
