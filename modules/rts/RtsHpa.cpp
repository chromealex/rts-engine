// Hierarchical pathfinding (HPA*) for big maps. The graph is a pure function of the grid (clusters are
// rebuilt from the current cells whenever something marked them dirty), so lockstep peers agree on every
// route no matter which queries each of them ran before. Integer costs (10 straight / 14 diagonal).
#include "RtsMap.h"
#include <algorithm>
#include <climits>
#include <cstdlib>
#include <queue>

namespace rts::game {

namespace {
constexpr uint32_t kInf = UINT32_MAX;
uint32_t octile(int ax, int ay, int bx, int by) {
    uint32_t dx = uint32_t(std::abs(ax - bx)), dy = uint32_t(std::abs(ay - by));
    return 10 * (dx + dy) - 6 * std::min(dx, dy);
}
} // namespace

HpaGraph& MapGrid::hpaFor(uint8_t layer) {
    for (auto& G : hpa_) if (G.layer == layer) return G;
    HpaGraph G;
    G.layer = layer;
    G.cw = (w + HpaGraph::kC - 1) / HpaGraph::kC;
    G.ch = (h + HpaGraph::kC - 1) / HpaGraph::kC;
    const size_t n = size_t(G.cw * G.ch);
    G.dirty.assign(n, 1);
    G.eastT.assign(n, {}); G.southT.assign(n, {});
    G.nodes.assign(n, {}); G.partners.assign(n, {}); G.dist.assign(n, {});
    hpa_.push_back(std::move(G));
    return hpa_.back();
}

// Transitions across the east (or south) border of cluster c: runs of cell pairs that can step across;
// a short run keeps its middle, a long one both ends (classic HPA* entrances).
void MapGrid::hpaBorder(HpaGraph& G, int c, bool east) {
    auto& out = east ? G.eastT[size_t(c)] : G.southT[size_t(c)];
    out.clear();
    const int k = HpaGraph::kC, cx = c % G.cw, cy = c / G.cw;
    if (east ? cx + 1 >= G.cw : cy + 1 >= G.ch) return;
    const int len = east ? std::min(k, h - cy * k) : std::min(k, w - cx * k);
    auto pairAt = [&](int i, int& a, int& b) {
        int x = east ? cx * k + k - 1 : cx * k + i, y = east ? cy * k + i : cy * k + k - 1;
        int nx = east ? x + 1 : x, ny = east ? y : y + 1;
        a = index(x, y); b = index(nx, ny);
        return passable(x, y, G.layer) && stepOk(x, y, nx, ny, G.layer) && stepOk(nx, ny, x, y, G.layer);
    };
    int runStart = -1;
    for (int i = 0; i <= len; ++i) {
        int a = 0, b = 0;
        bool ok = i < len && pairAt(i, a, b);
        if (ok && runStart < 0) runStart = i;
        if (!ok && runStart >= 0) {
            int r0 = runStart, r1 = i - 1;
            std::vector<int> picks = r1 - r0 + 1 >= 6 ? std::vector<int>{r0, r1} : std::vector<int>{(r0 + r1) / 2};
            for (int p : picks) { int pa, pb; pairAt(p, pa, pb); out.push_back({pa, pb}); }
            runStart = -1;
        }
    }
}

// Move mask of cluster c: bit n of a cell = the 8-neighbour step n stays inside the cluster and is legal.
static void clusterMask(const MapGrid& g, int c, int cw, uint8_t layer, std::vector<uint8_t>& mask,
                        bool (*step)(const MapGrid&, int, int, int, int, uint8_t)) {
    const int k = HpaGraph::kC, x0 = (c % cw) * k, y0 = (c / cw) * k;
    const int x1 = std::min(g.w, x0 + k), y1 = std::min(g.h, y0 + k), cwid = x1 - x0;
    static const int dxs[8] = {1, -1, 0, 0, 1, 1, -1, -1};
    static const int dys[8] = {0, 0, 1, -1, 1, -1, 1, -1};
    mask.assign(size_t(cwid * (y1 - y0)), 0);
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) {
            if (!g.passable(x, y, layer)) continue;
            uint8_t m = 0;
            for (int n = 0; n < 8; ++n) {
                int nx = x + dxs[n], ny = y + dys[n];
                if (nx >= x0 && ny >= y0 && nx < x1 && ny < y1 && step(g, x, y, nx, ny, layer)) m = uint8_t(m | (1 << n));
            }
            mask[size_t((y - y0) * cwid + (x - x0))] = m;
        }
}

void MapGrid::clusterCosts(int c, int src, uint8_t layer, std::vector<uint32_t>& d) const {
    std::vector<uint8_t> mask;
    clusterMask(*this, c, (w + HpaGraph::kC - 1) / HpaGraph::kC, layer, mask,
                [](const MapGrid& g, int x, int y, int nx, int ny, uint8_t l) { return g.stepAllowed(x, y, nx, ny, l); });
    clusterCostsMasked(c, src, mask, d);
}

void MapGrid::clusterCostsMasked(int c, int src, const std::vector<uint8_t>& mask, std::vector<uint32_t>& d) const {
    const int k = HpaGraph::kC, cw = (w + k - 1) / k, x0 = (c % cw) * k, y0 = (c / cw) * k;
    const int x1 = std::min(w, x0 + k), y1 = std::min(h, y0 + k), cwid = x1 - x0;
    d.assign(size_t(cwid * (y1 - y0)), kInf);
    using QE = std::pair<uint32_t, int>;
    std::priority_queue<QE, std::vector<QE>, std::greater<QE>> q;
    const int s = ((src / w) - y0) * cwid + (src % w - x0);
    d[size_t(s)] = 0;
    q.push({0, s});
    static const int dxs[8] = {1, -1, 0, 0, 1, 1, -1, -1};
    static const int dys[8] = {0, 0, 1, -1, 1, -1, 1, -1};
    while (!q.empty()) {
        auto [dc, li] = q.top(); q.pop();
        if (dc != d[size_t(li)]) continue;
        const uint8_t m = mask[size_t(li)];
        for (int n = 0; n < 8; ++n) {
            if (!(m & (1 << n))) continue;
            uint32_t nd = dc + (n >= 4 ? 14u : 10u);
            int ni = li + dys[n] * cwid + dxs[n];
            if (nd < d[size_t(ni)]) { d[size_t(ni)] = nd; q.push({nd, ni}); }
        }
    }
}

void MapGrid::hpaCluster(HpaGraph& G, int c) {
    const int cx = c % G.cw, cy = c / G.cw;
    std::vector<std::pair<int32_t, int32_t>> links; // (own entrance cell, cell across the border)
    for (auto& t : G.eastT[size_t(c)]) links.push_back({t.first, t.second});
    for (auto& t : G.southT[size_t(c)]) links.push_back({t.first, t.second});
    if (cx > 0) for (auto& t : G.eastT[size_t(c - 1)]) links.push_back({t.second, t.first});
    if (cy > 0) for (auto& t : G.southT[size_t(c - G.cw)]) links.push_back({t.second, t.first});
    std::sort(links.begin(), links.end());
    auto& nodes = G.nodes[size_t(c)];
    auto& part = G.partners[size_t(c)];
    nodes.clear(); part.clear();
    for (auto& [a, b] : links) {
        if (nodes.empty() || nodes.back() != a) { nodes.push_back(a); part.push_back({}); }
        part.back().push_back(b);
    }
    const size_t k = nodes.size();
    auto& dist = G.dist[size_t(c)];
    dist.assign(k * k, kInf);
    const int x0 = cx * HpaGraph::kC, y0 = cy * HpaGraph::kC, cwid = std::min(w, x0 + HpaGraph::kC) - x0;
    std::vector<uint32_t> d;
    std::vector<uint8_t> mask;
    if (k) clusterMask(*this, c, G.cw, G.layer, mask, [](const MapGrid& g, int x, int y, int nx, int ny, uint8_t l) { return g.stepAllowed(x, y, nx, ny, l); });
    for (size_t i = 0; i < k; ++i) {
        clusterCostsMasked(c, nodes[i], mask, d);
        for (size_t j = 0; j < k; ++j) {
            int x = nodes[j] % w, y = nodes[j] / w;
            dist[i * k + j] = d[size_t((y - y0) * cwid + (x - x0))];
        }
    }
}

void MapGrid::hpaRefresh(HpaGraph& G) {
    if (!G.anyDirty) return;
    G.anyDirty = false;
    const int n = G.cw * G.ch;
    std::vector<uint8_t> rebuild(size_t(n), 0);
    for (int c = 0; c < n; ++c) {
        if (!G.dirty[size_t(c)]) continue;
        const int cx = c % G.cw, cy = c / G.cw;
        hpaBorder(G, c, true); hpaBorder(G, c, false);
        if (cx > 0) hpaBorder(G, c - 1, true);
        if (cy > 0) hpaBorder(G, c - G.cw, false);
        rebuild[size_t(c)] = 1;
        if (cx > 0) rebuild[size_t(c - 1)] = 1;
        if (cx + 1 < G.cw) rebuild[size_t(c + 1)] = 1;
        if (cy > 0) rebuild[size_t(c - G.cw)] = 1;
        if (cy + 1 < G.ch) rebuild[size_t(c + G.cw)] = 1;
    }
    for (int c = 0; c < n; ++c) if (rebuild[size_t(c)]) hpaCluster(G, c);
    std::fill(G.dirty.begin(), G.dirty.end(), 0);
}

bool MapGrid::hpaPath(int sx, int sy, int gx, int gy, uint8_t layer, std::vector<int>& cells) {
    HpaGraph& G = hpaFor(layer);
    hpaRefresh(G);
    const int k = HpaGraph::kC;
    const int cs = (sy / k) * G.cw + sx / k, cg = (gy / k) * G.cw + gx / k;
    if (cs == cg) return false;
    // abstract node ids: cluster bases + entrance index; start / goal come last
    const int nc = G.cw * G.ch;
    std::vector<int32_t> base(size_t(nc) + 1, 0);
    for (int c = 0; c < nc; ++c) base[size_t(c) + 1] = base[size_t(c)] + int32_t(G.nodes[size_t(c)].size());
    const int32_t N = base[size_t(nc)], S = N, T = N + 1;
    auto clusterOf = [&](int cell) { return ((cell / w) / k) * G.cw + (cell % w) / k; };
    auto idOf = [&](int cell) {  // entrance cell -> abstract id (-1 if not an entrance)
        int c = clusterOf(cell);
        auto& nd = G.nodes[size_t(c)];
        auto it = std::lower_bound(nd.begin(), nd.end(), cell);
        return it != nd.end() && *it == cell ? base[size_t(c)] + int32_t(it - nd.begin()) : -1;
    };
    auto cellOf = [&](int32_t id) {
        if (id == S) return index(sx, sy);
        if (id == T) return index(gx, gy);
        int c = int(std::upper_bound(base.begin(), base.end(), id) - base.begin()) - 1;
        return int(G.nodes[size_t(c)][size_t(id - base[size_t(c)])]);
    };
    // start / goal costs to the entrances of their clusters
    std::vector<uint32_t> d;
    auto localCost = [&](int c, const std::vector<uint32_t>& dd, int cell) {
        const int x0 = (c % G.cw) * k, y0 = (c / G.cw) * k, cwid = std::min(w, x0 + k) - x0;
        return dd[size_t((cell / w - y0) * cwid + (cell % w - x0))];
    };
    std::vector<uint32_t> fromS(G.nodes[size_t(cs)].size()), toT(G.nodes[size_t(cg)].size());
    clusterCosts(cs, index(sx, sy), layer, d);
    for (size_t i = 0; i < fromS.size(); ++i) fromS[i] = localCost(cs, d, G.nodes[size_t(cs)][i]);
    clusterCosts(cg, index(gx, gy), layer, d);
    for (size_t i = 0; i < toT.size(); ++i) toT[i] = localCost(cg, d, G.nodes[size_t(cg)][i]);

    std::vector<uint32_t> gc(size_t(N) + 2, kInf);
    std::vector<int32_t> par(size_t(N) + 2, -1);
    struct QN { uint32_t f, hh; int32_t id; };
    auto cmp = [](const QN& a, const QN& b) {
        if (a.f != b.f) return a.f > b.f;
        if (a.hh != b.hh) return a.hh > b.hh;
        return a.id > b.id;
    };
    std::priority_queue<QN, std::vector<QN>, decltype(cmp)> open(cmp);
    auto heur = [&](int32_t id) { int c = cellOf(id); return octile(c % w, c / w, gx, gy); };
    auto relax = [&](int32_t from, int32_t to, uint32_t cost) {
        if (cost == kInf) return;
        uint32_t ng = gc[size_t(from)] + cost;
        if (ng >= gc[size_t(to)]) return;
        gc[size_t(to)] = ng; par[size_t(to)] = from;
        uint32_t hh = heur(to);
        open.push({ng + hh, hh, to});
    };
    gc[size_t(S)] = 0;
    open.push({heur(S), heur(S), S});
    bool found = false;
    while (!open.empty()) {
        QN cur = open.top(); open.pop();
        if (cur.f != gc[size_t(cur.id)] + cur.hh) continue;
        if (cur.id == T) { found = true; break; }
        if (cur.id == S) {
            for (size_t i = 0; i < fromS.size(); ++i) relax(S, base[size_t(cs)] + int32_t(i), fromS[i]);
            continue;
        }
        const int cell = cellOf(cur.id), c = clusterOf(cell);
        const size_t li = size_t(cur.id - base[size_t(c)]), kk = G.nodes[size_t(c)].size();
        for (size_t j = 0; j < kk; ++j) if (j != li) relax(cur.id, base[size_t(c)] + int32_t(j), G.dist[size_t(c)][li * kk + j]);
        for (int32_t across : G.partners[size_t(c)][li]) {
            int32_t o = idOf(across);
            if (o >= 0) relax(cur.id, o, (across % w != cell % w && across / w != cell / w) ? 14u : 10u);
        }
        if (c == cg) relax(cur.id, T, toT[li]);
    }
    if (!found) return false;
    std::vector<int> way;
    for (int32_t id = T; id != -1; id = par[size_t(id)]) way.push_back(cellOf(id));
    std::reverse(way.begin(), way.end());
    // refine: A* between consecutive waypoints (in or next to one cluster, so each search stays small)
    cells.clear();
    cells.push_back(way[0]);
    std::vector<int> seg;
    for (size_t i = 0; i + 1 < way.size(); ++i) {
        int a = way[i], b = way[i + 1];
        if (std::max(std::abs(a % w - b % w), std::abs(a / w - b / w)) <= 1) { cells.push_back(b); continue; }
        seg.clear();
        if (!astarCells(a % w, a / w, b % w, b / w, layer, 4 * k * k, seg)) return false;
        cells.insert(cells.end(), seg.begin() + 1, seg.end());
    }
    return true;
}

} // namespace rts::game
