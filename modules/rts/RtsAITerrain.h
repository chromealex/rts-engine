// Terrain analysis for the computer player (agent-ai-terrain): built once per match from the map grid, cheap
// (a few linear passes), integer only. Works on flat maps (cliff levels or heightfield) and on planets (the
// stacked cube-sphere grid, neighbours across face seams). The AI uses it to pick rally points on high ground,
// attack routes that avoid climbing into the enemy and cells overlooked by higher ground, siege spots, watch
// posts for scouts and chokepoints for towers.
#pragma once
#include <cstdint>
#include <vector>
#include "RtsModule.h"

namespace rts::game {

struct AITerrain {
    static constexpr int kUnit = 256;     // elevation of one cliff level (height advantage 1)
    int cells = 0, w = 0;
    bool sphere = false;
    bool relief = false;                  // some cells are higher than others
    std::vector<int32_t> elev;            // height advantage units * kUnit: cliff level, or height / levelHeight
    std::vector<uint8_t> pass;            // walkable for the default ground layer (terrain + static neutral blockers)
    std::vector<uint8_t> clear;           // steps to the nearest non-walkable cell (capped at 15): corridor half-width
    std::vector<int32_t> over;            // how much the highest ground within kOverR cells rises above this cell
    std::vector<int32_t> crest;           // elev minus the lowest ground within kOverR cells (prominence)
    static constexpr int kOverR = 4;

    void build(Simulation& sim, RtsModule& m);
    bool valid() const { return cells > 0; }
    // 8 neighbours of a walkable cell, f(n, cost 10 | 14); no corner cutting; never across bodies
    template <class F> void neighbors(const MapGrid& g, int c, F&& f) const;
    int nearestWalkable(const MapGrid& g, int c, int radius = 8) const;
    // Weighted shortest route between two cells (Dijkstra, ties by cell index). Extra cost per step:
    // climb (elevation gained) * climbW / kUnit, over[n] * exposureW / kUnit, avoid[n] when given.
    bool route(const MapGrid& g, int from, int to, int climbW, int exposureW, const std::vector<uint16_t>* avoid,
               std::vector<int>& out, int maxVisit = 1 << 22) const;
    // Plain step distances (10 per orthogonal step) from a set of cells, capped at `limit` (kFar beyond).
    static constexpr int32_t kFar = 1 << 30;
    void distances(const MapGrid& g, const std::vector<int>& from, int32_t limit, std::vector<int32_t>& out) const;
};

template <class F> void AITerrain::neighbors(const MapGrid& g, int c, F&& f) const {
    static constexpr int dx[8] = {1, -1, 0, 0, 1, 1, -1, -1}, dy[8] = {0, 0, 1, -1, 1, -1, 1, -1};
    auto stepOk = [&](int a, int b) {
        if (b < 0 || !pass[size_t(b)]) return false;
        if (g.hf || sphere) return true;
        const int la = g.level[size_t(a)], lb = g.level[size_t(b)];
        if (la == lb) return true;
        return (la - lb == 1 || lb - la == 1) && ((g.flags[size_t(a)] | g.flags[size_t(b)]) & kCellRamp);
    };
    int orth[4];
    for (int k = 0; k < 4; ++k) {
        int n;
        if (sphere) n = g.surf->neighbor(c, k);
        else { const int x = c % w + dx[k], y = c / w + dy[k]; n = x >= 0 && y >= 0 && x < w && y < cells / w ? y * w + x : -1; }
        orth[k] = stepOk(c, n) ? n : -1;
        if (orth[k] >= 0) f(orth[k], 10);
    }
    for (int k = 4; k < 8; ++k) {
        const int o1 = orth[dx[k] > 0 ? 0 : 1], o2 = orth[dy[k] > 0 ? 2 : 3];
        if (o1 < 0 || o2 < 0) continue;
        int n;
        if (sphere) n = g.surf->neighbor(c, k);
        else n = (c / w + dy[k]) * w + c % w + dx[k];
        if (stepOk(c, n) && stepOk(o1, n) && stepOk(o2, n)) f(n, 14);
    }
}

} // namespace rts::game
