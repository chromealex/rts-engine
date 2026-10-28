// agent-mech stage 4, BA: flow fields. One reverse Dijkstra from the goal cell gives every cell its cost to the
// goal (same step costs as A*: 10 / 14, the clearance penalty); each unit's path is traced down the field and then
// string-pulled like an A* path. Fields are cached per (goal, layer, clearance) and dropped when the grid changes.
#include "RtsMap.h"
#include <algorithm>
#include <queue>

namespace rts::game {

namespace {
const int kDx[8] = {1, -1, 0, 0, 1, 1, -1, -1};
const int kDy[8] = {0, 0, 1, -1, 1, -1, 1, -1};
constexpr uint32_t kInf = 0xFFFFFFFFu;
} // namespace

const MapGrid::FlowField& MapGrid::flowFor(int goal, uint8_t layer) {
    ++flowUse_;
    for (auto& f : flow_)
        if (f.goal == goal && f.layer == layer && f.clear == pathClear_ && f.version == pathVersion_) { f.used = flowUse_; return f; }
    // reuse the least recently used slot
    FlowField* slot = nullptr;
    if (int(flow_.size()) < std::max(1, flowCacheSize)) { flow_.emplace_back(); slot = &flow_.back(); }
    else { slot = &flow_[0]; for (auto& f : flow_) if (f.used < slot->used) slot = &f; }
    slot->goal = goal; slot->layer = layer; slot->clear = pathClear_; slot->version = pathVersion_; slot->used = flowUse_;
    const size_t n = size_t(w) * size_t(h);
    std::vector<uint32_t>& d = slot->dist;
    d.assign(n, kInf);
    struct Node { uint32_t g; int32_t idx; };
    auto cmp = [](const Node& a, const Node& b) { return a.g != b.g ? a.g > b.g : a.idx > b.idx; };
    std::priority_queue<Node, std::vector<Node>, decltype(cmp)> open(cmp);
    d[size_t(goal)] = 0;
    open.push({0, goal});
    while (!open.empty()) {
        Node cur = open.top(); open.pop();
        if (cur.g != d[size_t(cur.idx)]) continue;
        const int cx = cur.idx % w, cy = cur.idx / w;
        // entering `cur` costs its clearance penalty (as in astarCells, where the penalty is on the entered cell)
        uint32_t enter = 0;
        if (pathClear_ > 0 && clearance[size_t(cur.idx)] < pathClear_) enter = 30u * uint32_t(pathClear_ - clearance[size_t(cur.idx)]);
        for (int k = 0; k < 8; ++k) {
            const int nx = cx + kDx[k], ny = cy + kDy[k];
            if (!inside(nx, ny) || !stepOk(nx, ny, cx, cy, layer)) continue;   // the unit steps from n into cur
            const int ni = index(nx, ny);
            const uint32_t ng = cur.g + (k >= 4 ? 14u : 10u) + enter;
            if (ng >= d[size_t(ni)]) continue;
            d[size_t(ni)] = ng;
            open.push({ng, ni});
        }
    }
    ++flowStats[0];
    return *slot;
}

bool MapGrid::flowCells(int sx, int sy, int gx, int gy, uint8_t layer, std::vector<int>& cells) {
    const int goal = index(gx, gy);
    const FlowField& f = flowFor(goal, layer);
    int cur = index(sx, sy);
    if (f.dist[size_t(cur)] == kInf) return false;   // unreachable from here: the caller falls back to A*
    cells.clear();
    cells.push_back(cur);
    const size_t limit = size_t(w) * size_t(h);
    while (cur != goal && cells.size() < limit) {
        const int cx = cur % w, cy = cur / w;
        int best = -1;
        uint32_t bestCost = kInf;
        for (int k = 0; k < 8; ++k) {
            const int nx = cx + kDx[k], ny = cy + kDy[k];
            if (!inside(nx, ny) || !stepOk(cx, cy, nx, ny, layer)) continue;
            const int ni = index(nx, ny);
            const uint32_t dn = f.dist[size_t(ni)];
            if (dn == kInf) continue;
            uint32_t c = dn + (k >= 4 ? 14u : 10u);
            if (pathClear_ > 0 && clearance[size_t(ni)] < pathClear_) c += 30u * uint32_t(pathClear_ - clearance[size_t(ni)]);
            if (c < bestCost) { bestCost = c; best = ni; }
        }
        if (best < 0 || f.dist[size_t(best)] >= f.dist[size_t(cur)]) return false;
        cur = best;
        cells.push_back(cur);
    }
    ++flowStats[1];
    return cur == goal;
}

} // namespace rts::game
