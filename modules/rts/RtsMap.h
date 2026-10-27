// Terrain grid, pathfinding (A* on 8-connected cells), line-of-sight smoothing, optional road
// graph (Mushroom-Wars-style maps) and a spatial hash for neighbour queries. All integer/fixed.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "../../engine/core/ECS.h"
#include "../../engine/core/Fixed.h"

namespace rts::game {

enum CellFlags : uint8_t {
    kCellGround = 1,   // walkable for ground units / buildable
    kCellWater = 2,    // navigable for naval units
    kCellNoBuild = 4,  // walkable but not buildable
};

struct MapGrid {
    int w = 0, h = 0;
    std::vector<uint8_t> flags;     // CellFlags
    std::vector<int16_t> height;    // terrain height in 1/16 world units (rendering, future LOS)
    std::vector<uint16_t> blocked;  // number of structures covering the cell

    void init(int width, int height_);
    bool inside(int x, int y) const { return x >= 0 && y >= 0 && x < w && y < h; }
    int index(int x, int y) const { return y * w + x; }
    bool passable(int x, int y, uint8_t layer) const;
    int cellX(Fixed x) const { return int(x.floorInt()); }
    int cellY(Fixed z) const { return int(z.floorInt()); }
    Vec3 cellCenter(int x, int y) const { return {Fixed::fromInt(x) + Fixed::fromRatio(1, 2), Fixed{}, Fixed::fromInt(y) + Fixed::fromRatio(1, 2)}; }
    void block(int x0, int y0, int fw, int fh, int delta);
    bool areaFree(int x0, int y0, int fw, int fh) const; // buildable & unblocked

    // A* from world position to world position for `layer`. Returns world waypoints (excluding
    // the start). If the goal is unreachable, the path ends at the closest reachable cell.
    bool findPath(Vec3 from, Vec3 to, uint8_t layer, std::vector<Vec3>& out, int maxNodes = 40000);
    bool lineWalkable(int x0, int y0, int x1, int y1, uint8_t layer) const;
    // Nearest passable cell to (x,y) within radius (spiral search).
    bool nearestPassable(int& x, int& y, uint8_t layer, int radius = 12) const;

private:
    std::vector<uint32_t> gCost_, stamp_;
    std::vector<int32_t> parent_;
    uint32_t search_ = 0;
};

// Road graph between structures (optional). Units that follow roads only travel along edges.
struct RoadGraph {
    std::vector<Entity> nodes;               // structure entities
    std::vector<std::vector<uint16_t>> adj;
    bool empty() const { return nodes.empty(); }
    int nodeOf(Entity e) const;
    // Node-to-node shortest path by Euclidean edge length (Dijkstra, deterministic ties).
    std::vector<uint16_t> path(World& w, int from, int to) const;
};

// Uniform-grid buckets rebuilt every tick; query order is deterministic (insertion order).
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
