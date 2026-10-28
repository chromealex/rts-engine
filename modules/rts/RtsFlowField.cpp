// Flow fields (see FlowField in RtsMap.h and docs/pathfinding.md): one integration field per goal area shared by
// every unit heading there. Flat maps and planets alike: neighbours, step legality and clusters come from the same
// sources as A* / HPA* (stepOk / canMoveDir, HpaGraph / SphereHpaGraph), so a field descends exactly along legal
// A* moves, across face seams and cube corners included.
#include "RtsMap.h"
#include <algorithm>
#include <queue>

namespace rts::game {

namespace {
constexpr uint32_t kInf = FlowField::kInf;
const int kDx[8] = {1, -1, 0, 0, 1, 1, -1, -1}, kDy[8] = {0, 0, 1, -1, 1, -1, 1, -1};
}

size_t FlowField::bytes() const {
    size_t b = sizeof(FlowField) + value.capacity() * 4 + dir.capacity() + base.capacity() * 4 + D.capacity() * 4 + settled.capacity() +
               heap.capacity() * 8 + blockOf.capacity() * 4 + blockCluster.capacity() * 4 + seeds.capacity() * 4 + blockSeedHash.capacity() * 8 +
               (blockStamp.capacity() + blockBuilt.capacity()) * 4;
    for (auto& v : blockVal) b += v.capacity() * 4 + sizeof(v);
    for (auto& d : blockDir) b += d.capacity() + sizeof(d);
    return b;
}

size_t MapGrid::flowBytes() const {
    size_t b = 0;
    for (auto& f : flows_) b += f.bytes();
    return b;
}

// ---------------------------------------------------------------- topology helpers

bool MapGrid::flowHierFor(int cell) const { return surf ? sphereHpaBody(surf->bodyOfCell(cell)) : bigMap(); }

int MapGrid::flowClusterCount() const {
    if (surf) return sphereTopo_ ? int(sphereTopo_->clusters.size()) : 0;
    const int k = HpaGraph::kC;
    return ((w + k - 1) / k) * ((h + k - 1) / k);
}

int MapGrid::flowClusterOf(int cell) const {
    if (cell < 0) return -1;
    if (surf) return sphereTopo_ ? sphereTopo_->clusterOfCell[size_t(cell)] : -1;
    const int k = HpaGraph::kC;
    return ((cell / w) / k) * ((w + k - 1) / k) + (cell % w) / k;
}

void MapGrid::flowClusterRect(int c, int& x0, int& y0, int& cw, int& ch) const {
    if (c < 0) { x0 = 0; y0 = 0; cw = w; ch = h; return; }
    if (surf) { const auto& C = sphereTopo_->clusters[size_t(c)]; x0 = C.gx0; y0 = C.gy0; cw = C.cw; ch = C.ch; return; }
    const int k = HpaGraph::kC, ncx = (w + k - 1) / k;
    x0 = (c % ncx) * k; y0 = (c / ncx) * k; cw = std::min(k, w - x0); ch = std::min(k, h - y0);
}

int MapGrid::flowLocalIndex(int c, int cell) const {
    int x0, y0, cw, ch;
    flowClusterRect(c, x0, y0, cw, ch);
    return (cell / w - y0) * cw + (cell % w - x0);
}

int MapGrid::flowNeighbor(int x, int k) const {
    if (surf) return surf->neighbor(x, k);
    const int nx = x % w + kDx[k], ny = x / w + kDy[k];
    return inside(nx, ny) ? index(nx, ny) : -1;
}

bool MapGrid::flowStepTo(int x, int k, uint8_t layer) const {
    if (surf) return canMoveDir(x, k, layer);
    return stepOk(x % w, x / w, x % w + kDx[k], x / w + kDy[k], layer);
}

bool MapGrid::flowStepInto(int v, int u, uint8_t layer, bool& diag) const {
    if (!surf) {
        diag = v % w != u % w && v / w != u / w;
        return stepOk(v % w, v / w, u % w, u / w, layer);
    }
    for (int k = 0; k < 8; ++k)
        if (surf->neighbor(v, k) == u) { diag = k >= 4; return canMoveDir(v, k, layer); }
    return false;
}

bool MapGrid::flowLine(int a, int b, uint8_t layer) const {
    if (surf) return lineWalkableSphere(a, b, layer);
    return lineWalkable(a % w, a / w, b % w, b / w, layer);
}

MapGrid::FlowGraphView MapGrid::flowGraph(uint8_t layer) {
    if (surf) { SphereHpaGraph& G = hpaSphereFor(layer); hpaSphereRefresh(G); return {&G.nodes, &G.partners, &G.dist}; }
    HpaGraph& G = hpaFor(layer);
    hpaRefresh(G);
    return {&G.nodes, &G.partners, &G.dist};
}

void MapGrid::flowTouch(int cell) {
    const int n = flowClusterCount();
    if (n <= 0) return;
    if (int(flowClusterStamp_.size()) != n) flowClusterStamp_.assign(size_t(n), 0);
    const int c = flowClusterOf(cell);
    if (c >= 0) flowClusterStamp_[size_t(c)] = flowStamp_;
}

// ---------------------------------------------------------------- integration

// Backward Dijkstra: val[cell] = cheapest cost of walking from cell to a seed (seed value + 10/14 per step, + the A*
// clearance penalty of each entered cell when clear > 0), only through cells of cluster c (c < 0: the whole grid).
void MapGrid::flowLocal(int c, const std::vector<std::pair<int32_t, uint32_t>>& seeds, uint8_t layer, int clear, std::vector<uint32_t>& val) const {
    int x0, y0, cw, ch;
    flowClusterRect(c, x0, y0, cw, ch);
    val.assign(size_t(cw) * size_t(ch), kInf);
    auto inR = [&](int cell) { const int x = cell % w - x0, y = cell / w - y0; return cell >= 0 && x >= 0 && y >= 0 && x < cw && y < ch; };
    auto li = [&](int cell) { return size_t((cell / w - y0) * cw + (cell % w - x0)); };
    using QE = std::pair<uint32_t, int32_t>;
    std::priority_queue<QE, std::vector<QE>, std::greater<QE>> q;
    for (auto& [cell, d] : seeds) {
        if (!inR(cell)) continue;
        uint32_t& v = val[li(cell)];
        if (d < v) { v = d; q.push({d, cell}); }
    }
    while (!q.empty()) {
        const auto [d, u] = q.top();
        q.pop();
        if (d != val[li(u)]) continue;
        const uint32_t pen = clear > 0 && clearance[size_t(u)] < clear ? 30u * uint32_t(clear - clearance[size_t(u)]) : 0u;
        for (int k = 0; k < 8; ++k) {
            const int v = flowNeighbor(u, k);
            if (v < 0 || !inR(v)) continue;
            bool diag = false;
            if (!flowStepInto(v, u, layer, diag)) continue;
            const uint32_t nd = d + (diag ? 14u : 10u) + pen;
            uint32_t& dv = val[li(v)];
            if (nd < dv) { dv = nd; q.push({nd, v}); }
        }
    }
}

void MapGrid::flowSeeds(FlowField& f) const {
    f.seeds.clear();
    for (int r = f.radius; r <= f.radius + 6 && f.seeds.empty(); ++r)
        for (int dy = -r; dy <= r; ++dy)
            for (int dx = -r; dx <= r; ++dx) {
                if (dx * dx + dy * dy > r * r) continue;
                int c = -1;
                if (surf) c = surf->offset(f.goal, dx, dy);
                else if (inside(f.goal % w + dx, f.goal / w + dy)) c = f.goal + dy * w + dx;
                if (c >= 0 && passable(c % w, c / w, f.layer)) f.seeds.push_back(c);
            }
    std::sort(f.seeds.begin(), f.seeds.end());
    f.seeds.erase(std::unique(f.seeds.begin(), f.seeds.end()), f.seeds.end());
}

void MapGrid::flowValidate(FlowField& f) {
    if (f.stamp == flowStamp_) return;
    f.stamp = flowStamp_;
    flowSeeds(f);
    if (!f.hier) { f.built = false; return; }
    flowGraph(f.layer);   // HPA* graph (and the sphere topology) up to date with the grid
    f.dInit = false;
    for (auto& d : f.blockDir) std::fill(d.begin(), d.end(), int8_t(-2));   // neighbours' values may have changed
}

void MapGrid::flowBuildFull(FlowField& f) {
    if (f.clear > 0 && clearanceDirty) updateClearance();
    std::vector<std::pair<int32_t, uint32_t>> sd;
    for (int32_t s : f.seeds) sd.push_back({s, 0u});
    flowLocal(-1, sd, f.layer, f.clear, f.value);
    f.dir.assign(f.value.size(), int8_t(-2));
    f.built = true;
}

// Abstract level: D[node] = cost from an HPA* entrance to the goal area. Seeded from the goal clusters, then a
// Dijkstra over the entrance graph that runs only as far as the clusters asked for need (resumable).
void MapGrid::flowComputeD(FlowField& f) {
    const FlowGraphView gv = flowGraph(f.layer);
    const int nc = flowClusterCount();
    f.base.assign(size_t(nc) + 1, 0);
    for (int c = 0; c < nc; ++c) f.base[size_t(c) + 1] = f.base[size_t(c)] + int32_t((*gv.nodes)[size_t(c)].size());
    const size_t N = size_t(f.base.back());
    f.D.assign(N, kInf);
    f.settled.assign(N, 0);
    f.heap.clear();
    if (int(f.blockOf.size()) != nc) {
        f.blockOf.assign(size_t(nc), -1);
        f.blockCluster.clear(); f.blockVal.clear(); f.blockDir.clear(); f.blockSeedHash.clear(); f.blockStamp.clear(); f.blockBuilt.clear();
    }
    std::vector<int> goalClusters;
    for (int32_t s : f.seeds) goalClusters.push_back(flowClusterOf(s));
    std::sort(goalClusters.begin(), goalClusters.end());
    goalClusters.erase(std::unique(goalClusters.begin(), goalClusters.end()), goalClusters.end());
    std::vector<uint32_t> val;
    for (int c : goalClusters) {
        if (c < 0) continue;
        std::vector<std::pair<int32_t, uint32_t>> sd;
        for (int32_t s : f.seeds) if (flowClusterOf(s) == c) sd.push_back({s, 0u});
        flowLocal(c, sd, f.layer, 0, val);
        const auto& nodes = (*gv.nodes)[size_t(c)];
        for (size_t i = 0; i < nodes.size(); ++i) {
            const uint32_t d = val[size_t(flowLocalIndex(c, nodes[i]))];
            const int32_t id = f.base[size_t(c)] + int32_t(i);
            if (d < f.D[size_t(id)]) { f.D[size_t(id)] = d; f.heap.push_back({d, id}); std::push_heap(f.heap.begin(), f.heap.end(), std::greater<>()); }
        }
    }
    f.dInit = true;
}

void MapGrid::flowEnsureCluster(FlowField& f, int c) {
    if (!f.dInit) flowComputeD(f);
    const FlowGraphView gv = flowGraph(f.layer);
    const auto& nodes = *gv.nodes;
    int pending = 0;
    for (size_t i = 0; i < nodes[size_t(c)].size(); ++i) pending += !f.settled[size_t(f.base[size_t(c)]) + i];
    auto relax = [&](int32_t id, uint32_t d) {
        if (d >= f.D[size_t(id)]) return;
        f.D[size_t(id)] = d;
        f.heap.push_back({d, id});
        std::push_heap(f.heap.begin(), f.heap.end(), std::greater<>());
    };
    while (pending > 0 && !f.heap.empty()) {
        std::pop_heap(f.heap.begin(), f.heap.end(), std::greater<>());
        const auto [d, id] = f.heap.back();
        f.heap.pop_back();
        if (f.settled[size_t(id)] || d != f.D[size_t(id)]) continue;
        f.settled[size_t(id)] = 1;
        const int cc = int(std::upper_bound(f.base.begin(), f.base.end(), id) - f.base.begin()) - 1;
        const size_t li = size_t(id - f.base[size_t(cc)]), kk = nodes[size_t(cc)].size();
        if (cc == c) --pending;
        const auto& dist = (*gv.dist)[size_t(cc)];
        for (size_t j = 0; j < kk; ++j) {
            if (j == li) continue;
            const uint32_t cost = dist[j * kk + li];   // from entrance j to this one
            if (cost != kInf) relax(f.base[size_t(cc)] + int32_t(j), d + cost);
        }
        const int cell = nodes[size_t(cc)][li];
        for (int32_t across : (*gv.partners)[size_t(cc)][li]) {
            const int oc = flowClusterOf(across);
            if (oc < 0) continue;
            const auto& on = nodes[size_t(oc)];
            auto it = std::lower_bound(on.begin(), on.end(), across);
            if (it == on.end() || *it != across) continue;
            const uint32_t step = (!surf && across % w != cell % w && across / w != cell / w) ? 14u : 10u;
            relax(f.base[size_t(oc)] + int32_t(it - on.begin()), d + step);
        }
    }
}

// Values of cluster c: Dijkstra inside the cluster seeded with its entrances' D and the goal cells it holds.
// Recomputed only when its seeds or its cells changed.
int MapGrid::flowBlock(FlowField& f, int c) {
    int b = f.blockOf.empty() ? -1 : f.blockOf[size_t(c)];
    if (b >= 0 && f.blockStamp[size_t(b)] == f.stamp && f.dInit) return b;
    flowEnsureCluster(f, c);
    const FlowGraphView gv = flowGraph(f.layer);
    const auto& nodes = (*gv.nodes)[size_t(c)];
    std::vector<std::pair<int32_t, uint32_t>> sd;
    for (size_t i = 0; i < nodes.size(); ++i) {
        const uint32_t d = f.D[size_t(f.base[size_t(c)]) + i];
        if (d != kInf) sd.push_back({nodes[i], d});
    }
    for (int32_t s : f.seeds) if (flowClusterOf(s) == c) sd.push_back({s, 0u});
    std::sort(sd.begin(), sd.end());
    uint64_t hash = 1469598103934665603ull;
    for (auto& [cell, d] : sd) { hash = (hash ^ uint64_t(uint32_t(cell))) * 1099511628211ull; hash = (hash ^ d) * 1099511628211ull; }
    if (b >= 0) {
        const uint32_t changed = size_t(c) < flowClusterStamp_.size() ? flowClusterStamp_[size_t(c)] : 0u;
        if (f.blockSeedHash[size_t(b)] == hash && changed <= f.blockBuilt[size_t(b)]) { f.blockStamp[size_t(b)] = f.stamp; return b; }
    } else {
        b = int(f.blockCluster.size());
        f.blockOf[size_t(c)] = b;
        f.blockCluster.push_back(c);
        f.blockVal.emplace_back(); f.blockDir.emplace_back(); f.blockSeedHash.push_back(0); f.blockStamp.push_back(0); f.blockBuilt.push_back(0);
    }
    flowLocal(c, sd, f.layer, 0, f.blockVal[size_t(b)]);
    f.blockDir[size_t(b)].assign(f.blockVal[size_t(b)].size(), int8_t(-2));
    f.blockSeedHash[size_t(b)] = hash;
    f.blockBuilt[size_t(b)] = flowStamp_;
    f.blockStamp[size_t(b)] = f.stamp;
    return b;
}

// ---------------------------------------------------------------- public API

int MapGrid::flowFind(int goalCell, int radius, uint8_t layer, int clear) const {
    for (size_t i = 0; i < flows_.size(); ++i) {
        const FlowField& f = flows_[i];
        if (f.goal == goalCell && f.radius == radius && f.layer == layer && f.clear == (f.hier ? 0 : clear)) return int(i);
    }
    return -1;
}

int MapGrid::flowAcquire(Vec3 goalPos, int radius, uint8_t layer, int clear, uint32_t tick) {
    const int g = cellOf(goalPos);
    if (g < 0) return -1;
    radius = std::clamp(radius, 0, 32);
    const bool hier = flowHierFor(g);
    if (hier) clear = 0;   // HPA* routing has no clearance classes; refinement keeps bodies out of walls
    int fi = flowFind(g, radius, layer, clear);
    if (fi < 0) {
        FlowField f;
        f.goal = g; f.radius = uint8_t(radius); f.layer = layer; f.clear = uint8_t(clear); f.hier = hier; f.stamp = flowStamp_ - 1;
        flows_.push_back(std::move(f));
        fi = int(flows_.size()) - 1;
    }
    FlowField& f = flows_[size_t(fi)];
    f.lastUse = tick;
    flowValidate(f);
    return f.seeds.empty() ? -1 : fi;
}

uint32_t MapGrid::flowValue(int fi, int cell) {
    FlowField& f = flows_[size_t(fi)];
    flowValidate(f);
    if (cell < 0) return kInf;
    if (!f.hier) {
        if (!f.built) flowBuildFull(f);
        return f.value[size_t(cell)];
    }
    if (surf && surf->bodyOfCell(cell) != surf->bodyOfCell(f.goal)) return kInf;
    const int c = flowClusterOf(cell);
    if (c < 0) return kInf;
    const int b = flowBlock(f, c);
    return f.blockVal[size_t(b)][size_t(flowLocalIndex(c, cell))];
}

int MapGrid::flowDir(int fi, int cell) {
    const uint32_t v = flowValue(fi, cell);
    if (v == kInf || v == 0) return -1;
    auto cache = [&]() -> int8_t& {
        FlowField& f = flows_[size_t(fi)];
        if (!f.hier) return f.dir[size_t(cell)];
        const int c = flowClusterOf(cell);
        return f.blockDir[size_t(f.blockOf[size_t(c)])][size_t(flowLocalIndex(c, cell))];
    };
    if (cache() != -2) return cache();
    const uint8_t layer = flows_[size_t(fi)].layer;
    const int clear = flows_[size_t(fi)].clear;
    int best = -1;
    uint64_t bestCost = UINT64_MAX;
    for (int k = 0; k < 8; ++k) {
        const int n = flowNeighbor(cell, k);
        if (n < 0 || !flowStepTo(cell, k, layer)) continue;
        const uint32_t vn = flowValue(fi, n);
        if (vn >= v) continue;   // strictly downhill (values are integers: the descent always ends in the goal area)
        const uint32_t pen = clear > 0 && clearance[size_t(n)] < clear ? 30u * uint32_t(clear - clearance[size_t(n)]) : 0u;
        const uint64_t cost = uint64_t(vn) + (k >= 4 ? 14u : 10u) + pen;
        if (cost < bestCost) { bestCost = cost; best = k; }
    }
    flowValue(fi, cell);   // make sure this cell's block exists again (neighbour lookups may have added blocks)
    cache() = int8_t(best);
    return best;
}

bool MapGrid::flowSteer(int fi, Vec3 from, Vec3 goalPos, std::vector<Vec3>& out, bool& arrived) {
    out.clear();
    arrived = false;
    const int s = cellOf(from);
    if (s < 0) return false;
    const uint32_t v = flowValue(fi, s);
    if (v == kInf) return false;
    if (v == 0) { arrived = true; return true; }
    const uint8_t layer = flows_[size_t(fi)].layer;
    const int g = flows_[size_t(fi)].goal;
    // straight line to the goal when it is close and clear (the field's own descent bends at cell centres)
    const Fixed cs = surf ? surf->cellSize(surf->bodyOfCell(s)) : Fixed::fromInt(1);
    if (passable(g % w, g / w, layer) && (centerOf(s) - centerOf(g)).length() <= cs * 48 && flowLine(s, g, layer)) { out.push_back(goalPos); return true; }
    std::vector<int> cells{s};
    int cur = s;
    for (int i = 0; i < 40; ++i) {
        const int k = flowDir(fi, cur);
        if (k < 0) break;
        cur = flowNeighbor(cur, k);
        cells.push_back(cur);
        if (flowValue(fi, cur) == 0) break;
    }
    if (cells.size() < 2) return false;
    // farthest visible descent cell: doubling then bisection (a few line tests instead of one per cell)
    size_t ok = 1, lo = 1, hi = cells.size() - 1;
    for (size_t step = 4; ; step *= 2) {
        const size_t j = std::min(hi, step);
        if (!flowLine(s, cells[j], layer)) { hi = j; break; }
        ok = lo = j;
        if (j == hi) { hi = j + 1; break; }
    }
    while (hi - lo > 1) { const size_t mid = (lo + hi) / 2; if (flowLine(s, cells[mid], layer)) ok = lo = mid; else hi = mid; }
    out.push_back(centerOf(cells[ok]));
    return true;
}

void MapGrid::flowHousekeeping() {
    while (int(flows_.size()) > flowMaxFields || flowBytes() > flowBudgetBytes) {
        int victim = -1;
        for (size_t i = 0; i < flows_.size(); ++i)
            if (flows_[i].refs == 0 && (victim < 0 || flows_[i].lastUse < flows_[size_t(victim)].lastUse)) victim = int(i);
        if (victim < 0) break;
        flows_.erase(flows_.begin() + victim);
    }
}

int64_t MapGrid::astarCellCost(int s, int g, uint8_t layer, std::vector<int>* cellsOut) {
    std::vector<int> cells;
    bool found;
    if (surf) found = astarSphere(s, g, layer, 1 << 30, cells);
    else found = astarCells(s % w, s / w, g % w, g / w, layer, 1 << 30, cells);
    if (!found || cells.empty() || cells.back() != g) return -1;
    int64_t cost = 0;
    for (size_t i = 1; i < cells.size(); ++i) {
        const int a = cells[i - 1], b = cells[i];
        bool diag = false;
        if (!flowStepInto(a, b, layer, diag)) return -1;
        cost += diag ? 14 : 10;
    }
    if (cellsOut) *cellsOut = cells;
    return cost;
}

} // namespace rts::game
