// Hierarchical pathfinding (HPA*) on cube-sphere bodies; the planet counterpart of RtsHpa.cpp (same costs,
// entrance rule and refinement), on the stacked face rows of the planet MapGrid (RtsSurface.h).
//
// Topology (static per map, shared): every face of a body is cut into kC x kC clusters, the last row / column
// partial when faceSize is not a multiple of kC. A cluster is a plain rect of grid cells (a face's rows are
// contiguous in the grid). Two clusters share a border when an orthogonal step joins them; across a face seam
// that step comes from the surface's neighbour table, so seams are handled in both directions and wherever the
// adjacent face's coordinates run reversed. At a cube corner only orthogonal steps exist between the three
// faces (the diagonal through the vertex does not), so a corner cell simply has two seam pairs, one per face.
// Bodies never share a border (no edges between bodies; transit is separate).
// Graphs (one per path layer, incl. slope classes) are pure functions of the grid: rebuilt per dirty cluster,
// with the borders of a dirty cluster and the clusters on their other side, so lockstep peers always agree.
#include "RtsMap.h"
#include "engine/core/FixedMath.h"
#include <algorithm>
#include <climits>
#include <queue>

namespace rts::game {

namespace {
constexpr uint32_t kInf = UINT32_MAX;

std::shared_ptr<const SphereHpaTopo> buildTopo(const Surface& S) {
    auto T = std::make_shared<SphereHpaTopo>();
    const int k = SphereHpaTopo::kC, W = S.W;
    T->clusterOfCell.assign(size_t(S.W) * size_t(S.H), -1);
    for (size_t bi = 0; bi < S.bodies.size(); ++bi) {
        T->bodyFirst.push_back(int32_t(T->clusters.size()));
        const BodyDef& b = S.bodies[bi];
        if (!b.hasSurface()) continue;
        const int N = b.faceSize, tiles = (N + k - 1) / k;
        for (int f = 0; f < 6; ++f)
            for (int ty = 0; ty < tiles; ++ty)
                for (int tx = 0; tx < tiles; ++tx) {
                    SphereHpaTopo::Cluster c;
                    c.body = int(bi);
                    c.gx0 = tx * k; c.gy0 = b.rowBase + f * N + ty * k;
                    c.cw = std::min(k, N - tx * k); c.ch = std::min(k, N - ty * k);
                    const int id = int(T->clusters.size());
                    for (int y = 0; y < c.ch; ++y)
                        for (int x = 0; x < c.cw; ++x) T->clusterOfCell[size_t((c.gy0 + y) * W + c.gx0 + x)] = id;
                    T->clusters.push_back(c);
                }
    }
    T->bodyFirst.push_back(int32_t(T->clusters.size()));
    // borders: every orthogonal step between two clusters, recorded once from the lower cluster id
    std::vector<std::pair<std::pair<int32_t, int32_t>, std::pair<int32_t, std::pair<int32_t, int32_t>>>> raw;  // ((a,b),(key,(ca,cb)))
    for (int32_t a = 0; a < int32_t(T->clusters.size()); ++a) {
        const auto& c = T->clusters[size_t(a)];
        for (int y = 0; y < c.ch; ++y)
            for (int x = 0; x < c.cw; ++x) {
                const bool edgeX0 = x == 0, edgeX1 = x == c.cw - 1, edgeY0 = y == 0, edgeY1 = y == c.ch - 1;
                if (!edgeX0 && !edgeX1 && !edgeY0 && !edgeY1) continue;
                const int cell = (c.gy0 + y) * W + c.gx0 + x;
                for (int dir = 0; dir < 4; ++dir) {   // kDx/kDy order: +x, -x, +y, -y
                    const int n = S.neighbor(cell, dir);
                    if (n < 0) continue;
                    const int32_t b = T->clusterOfCell[size_t(n)];
                    if (b <= a) continue;   // same cluster, or recorded from b
                    const int32_t key = dir * k + (dir < 2 ? y : x);   // side of a, position along it
                    raw.push_back({{a, b}, {key, {cell, n}}});
                }
            }
    }
    std::sort(raw.begin(), raw.end());
    for (auto& r : raw) {
        if (T->borders.empty() || T->borders.back().a != r.first.first || T->borders.back().b != r.first.second) {
            SphereHpaTopo::Border bd; bd.a = r.first.first; bd.b = r.first.second;
            T->borders.push_back(bd);
            T->clusters[size_t(bd.a)].borders.push_back(int32_t(T->borders.size() - 1));
            T->clusters[size_t(bd.b)].borders.push_back(int32_t(T->borders.size() - 1));
        }
        T->borders.back().keys.push_back(r.second.first);
        T->borders.back().pairs.push_back(r.second.second);
    }
    return T;
}

uint32_t chordHeur(const Surface& S, int c, Vec3 goalDir, Fixed hk) {
    return uint32_t(((S.cellDir(c) - goalDir).length() * hk).floorInt());
}
} // namespace

bool MapGrid::sphereHpaBody(int body) const {
    if (!surf || body < 0 || size_t(body) >= surf->bodies.size()) return false;
    const BodyDef& b = surf->bodies[size_t(body)];
    return b.hasSurface() && int64_t(6) * b.faceSize * b.faceSize >= sphereHpaMinCells;
}

SphereHpaGraph& MapGrid::hpaSphereFor(uint8_t layer) {
    if (!sphereTopo_) sphereTopo_ = buildTopo(*surf);
    for (auto& G : sphereHpa_) if (G.layer == layer) return G;
    SphereHpaGraph G;
    G.layer = layer;
    const size_t n = sphereTopo_->clusters.size();
    G.dirty.assign(n, 1);   // built lazily, per active body (refresh skips bodies below the threshold)
    G.trans.assign(sphereTopo_->borders.size(), {});
    G.nodes.assign(n, {}); G.partners.assign(n, {}); G.dist.assign(n, {});
    sphereHpa_.push_back(std::move(G));
    return sphereHpa_.back();
}

void MapGrid::prepareHpaSphere(uint8_t layer) {
    if (!surf) return;
    bool any = false;
    for (size_t b = 0; b < surf->bodies.size(); ++b) any |= sphereHpaBody(int(b));
    if (any) hpaSphereRefresh(hpaSphereFor(layer));
}

void MapGrid::markSphereDirty(int cell) {
    if (!sphereTopo_ || cell < 0 || size_t(cell) >= sphereTopo_->clusterOfCell.size()) return;
    const int32_t c = sphereTopo_->clusterOfCell[size_t(cell)];
    if (c < 0) return;
    for (auto& G : sphereHpa_) { G.dirty[size_t(c)] = 1; G.anyDirty = true; }
}

// Entrances on one border: runs of consecutive pairs (same side, adjacent positions) that can step across both
// ways; a short run keeps its middle, a long one both ends (as RtsHpa.cpp).
void MapGrid::hpaSphereBorder(SphereHpaGraph& G, int bi) {
    const auto& B = sphereTopo_->borders[size_t(bi)];
    auto& out = G.trans[size_t(bi)];
    out.clear();
    auto ok = [&](size_t i) {
        const int a = B.pairs[i].first, b = B.pairs[i].second;
        return passable(a % w, a / w, G.layer) && canStep(a % w, a / w, b % w, b / w, G.layer) && canStep(b % w, b / w, a % w, a / w, G.layer);
    };
    size_t runStart = SIZE_MAX;
    for (size_t i = 0; i <= B.pairs.size(); ++i) {
        const bool good = i < B.pairs.size() && ok(i);
        const bool contiguous = runStart != SIZE_MAX && i < B.pairs.size() && B.keys[i] == B.keys[i - 1] + 1 && B.keys[i] / SphereHpaTopo::kC == B.keys[i - 1] / SphereHpaTopo::kC;
        if (runStart != SIZE_MAX && (!good || !contiguous)) {
            const size_t r0 = runStart, r1 = i - 1;
            if (r1 - r0 + 1 >= 6) { out.push_back(B.pairs[r0]); out.push_back(B.pairs[r1]); }
            else out.push_back(B.pairs[(r0 + r1) / 2]);
            runStart = SIZE_MAX;
        }
        if (good && runStart == SIZE_MAX) runStart = i;
    }
}

// Dijkstra from src inside cluster c (8-connected, no corner cutting, the same steps as A*).
void MapGrid::sphereClusterCosts(int c, int src, uint8_t layer, std::vector<uint32_t>& d) const {
    const auto& C = sphereTopo_->clusters[size_t(c)];
    d.assign(size_t(C.cw * C.ch), kInf);
    auto local = [&](int cell) { return (cell / w - C.gy0) * C.cw + (cell % w - C.gx0); };
    using QE = std::pair<uint32_t, int>;
    std::priority_queue<QE, std::vector<QE>, std::greater<QE>> q;
    d[size_t(local(src))] = 0;
    q.push({0, src});
    while (!q.empty()) {
        auto [dc, cell] = q.top(); q.pop();
        if (dc != d[size_t(local(cell))]) continue;
        for (int k = 0; k < 8; ++k) {
            const int n = surf->neighbor(cell, k);
            if (n < 0 || sphereTopo_->clusterOfCell[size_t(n)] != c || !canMoveDir(cell, k, layer)) continue;
            const uint32_t nd = dc + (k >= 4 ? 14u : 10u);
            uint32_t& dn = d[size_t(local(n))];
            if (nd < dn) { dn = nd; q.push({nd, n}); }
        }
    }
}

void MapGrid::hpaSphereCluster(SphereHpaGraph& G, int c) {
    const auto& C = sphereTopo_->clusters[size_t(c)];
    std::vector<std::pair<int32_t, int32_t>> links;
    for (int32_t bi : C.borders) {
        const auto& B = sphereTopo_->borders[size_t(bi)];
        for (auto& t : G.trans[size_t(bi)]) links.push_back(B.a == c ? t : std::pair<int32_t, int32_t>{t.second, t.first});
    }
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
    std::vector<uint32_t> d;
    for (size_t i = 0; i < k; ++i) {
        sphereClusterCosts(c, nodes[i], G.layer, d);
        for (size_t j = 0; j < k; ++j) dist[i * k + j] = d[size_t((nodes[j] / w - C.gy0) * C.cw + (nodes[j] % w - C.gx0))];
    }
}

void MapGrid::hpaSphereRefresh(SphereHpaGraph& G) {
    if (!G.anyDirty) return;
    G.anyDirty = false;
    const size_t n = sphereTopo_->clusters.size();
    std::vector<uint8_t> active(surf->bodies.size());
    for (size_t b = 0; b < active.size(); ++b) active[b] = sphereHpaBody(int(b));
    std::vector<uint8_t> rebuild(n, 0), borderDone(sphereTopo_->borders.size(), 0);
    for (size_t c = 0; c < n; ++c) {
        if (!G.dirty[c]) continue;
        if (!active[size_t(sphereTopo_->clusters[c].body)]) { G.anyDirty = true; continue; }   // stays dirty
        G.dirty[c] = 0;
        rebuild[c] = 1;
        for (int32_t bi : sphereTopo_->clusters[c].borders) {
            if (!borderDone[size_t(bi)]) { hpaSphereBorder(G, bi); borderDone[size_t(bi)] = 1; }
            const auto& B = sphereTopo_->borders[size_t(bi)];
            rebuild[size_t(B.a)] = rebuild[size_t(B.b)] = 1;
        }
    }
    for (size_t c = 0; c < n; ++c) if (rebuild[c]) hpaSphereCluster(G, int(c));
}

bool MapGrid::hpaPathSphere(int sc, int gc, uint8_t layer, std::vector<int>& cells) {
    SphereHpaGraph& G = hpaSphereFor(layer);
    hpaSphereRefresh(G);
    const SphereHpaTopo& T = *sphereTopo_;
    const int cs = T.clusterOfCell[size_t(sc)], cg = T.clusterOfCell[size_t(gc)];
    if (cs < 0 || cg < 0 || cs == cg || T.clusters[size_t(cs)].body != T.clusters[size_t(cg)].body) return false;
    const int body = T.clusters[size_t(cs)].body;
    const int c0 = T.bodyFirst[size_t(body)], c1 = T.bodyFirst[size_t(body) + 1];   // clusters of this body
    // abstract ids: per-cluster bases (this body only) + entrance index; start / goal last
    std::vector<int32_t> base(size_t(c1 - c0) + 1, 0);
    for (int c = c0; c < c1; ++c) base[size_t(c - c0) + 1] = base[size_t(c - c0)] + int32_t(G.nodes[size_t(c)].size());
    const int32_t NN = base.back(), S = NN, Tg = NN + 1;
    auto idOf = [&](int cell) {
        const int c = T.clusterOfCell[size_t(cell)];
        if (c < c0 || c >= c1) return -1;
        auto& nd = G.nodes[size_t(c)];
        auto it = std::lower_bound(nd.begin(), nd.end(), cell);
        return it != nd.end() && *it == cell ? base[size_t(c - c0)] + int32_t(it - nd.begin()) : -1;
    };
    auto clusterOfId = [&](int32_t id) { return c0 + int(std::upper_bound(base.begin(), base.end(), id) - base.begin()) - 1; };
    auto cellOf = [&](int32_t id) {
        if (id == S) return sc;
        if (id == Tg) return gc;
        const int c = clusterOfId(id);
        return int(G.nodes[size_t(c)][size_t(id - base[size_t(c - c0)])]);
    };
    auto localCost = [&](int c, const std::vector<uint32_t>& dd, int cell) {
        const auto& C = T.clusters[size_t(c)];
        return dd[size_t((cell / w - C.gy0) * C.cw + (cell % w - C.gx0))];
    };
    std::vector<uint32_t> d, fromS(G.nodes[size_t(cs)].size()), toT(G.nodes[size_t(cg)].size());
    sphereClusterCosts(cs, sc, layer, d);
    for (size_t i = 0; i < fromS.size(); ++i) fromS[i] = localCost(cs, d, G.nodes[size_t(cs)][i]);
    sphereClusterCosts(cg, gc, layer, d);
    for (size_t i = 0; i < toT.size(); ++i) toT[i] = localCost(cg, d, G.nodes[size_t(cg)][i]);

    const int N = surf->cubeOf(body).faceSize();
    const Vec3 gd = surf->cellDir(gc);
    const Fixed hk = Fixed::fromInt(9 * 4 * N) / kFxTwoPi;   // same heuristic as astarSphere
    std::vector<uint32_t> gcost(size_t(NN) + 2, kInf);
    std::vector<int32_t> par(size_t(NN) + 2, -1);
    struct QN { uint32_t f, hh; int32_t id; };
    auto cmp = [](const QN& a, const QN& b) {
        if (a.f != b.f) return a.f > b.f;
        if (a.hh != b.hh) return a.hh > b.hh;
        return a.id > b.id;
    };
    std::priority_queue<QN, std::vector<QN>, decltype(cmp)> open(cmp);
    auto heur = [&](int32_t id) { return chordHeur(*surf, cellOf(id), gd, hk); };
    auto relax = [&](int32_t from, int32_t to, uint32_t cost) {
        if (cost == kInf) return;
        const uint32_t ng = gcost[size_t(from)] + cost;
        if (ng >= gcost[size_t(to)]) return;
        gcost[size_t(to)] = ng; par[size_t(to)] = from;
        const uint32_t hh = heur(to);
        open.push({ng + hh, hh, to});
    };
    gcost[size_t(S)] = 0;
    open.push({heur(S), heur(S), S});
    bool found = false;
    while (!open.empty()) {
        const QN cur = open.top(); open.pop();
        if (cur.f != gcost[size_t(cur.id)] + cur.hh) continue;
        if (cur.id == Tg) { found = true; break; }
        if (cur.id == S) {
            for (size_t i = 0; i < fromS.size(); ++i) relax(S, base[size_t(cs - c0)] + int32_t(i), fromS[i]);
            continue;
        }
        const int c = clusterOfId(cur.id);
        const size_t li = size_t(cur.id - base[size_t(c - c0)]), kk = G.nodes[size_t(c)].size();
        for (size_t j = 0; j < kk; ++j) if (j != li) relax(cur.id, base[size_t(c - c0)] + int32_t(j), G.dist[size_t(c)][li * kk + j]);
        for (int32_t across : G.partners[size_t(c)][li]) {
            const int32_t o = idOf(across);
            if (o >= 0) relax(cur.id, o, 10u);   // entrance pairs are orthogonal steps
        }
        if (c == cg) relax(cur.id, Tg, toT[li]);
    }
    if (!found) return false;
    std::vector<int> way;
    for (int32_t id = Tg; id != -1; id = par[size_t(id)]) way.push_back(cellOf(id));
    std::reverse(way.begin(), way.end());
    // refine: A* between consecutive waypoints (inside or next to one cluster, so each search stays small)
    cells.clear();
    cells.push_back(way[0]);
    std::vector<int> seg;
    const int k = SphereHpaTopo::kC;
    for (size_t i = 0; i + 1 < way.size(); ++i) {
        const int a = way[i], b = way[i + 1];
        bool adjacent = a == b;
        for (int n = 0; n < 8 && !adjacent; ++n) adjacent = surf->neighbor(a, n) == b && canMoveDir(a, n, layer);
        if (adjacent) { if (a != b) cells.push_back(b); continue; }
        seg.clear();
        if (!astarSphere(a, b, layer, 4 * k * k, seg) || seg.empty() || seg.back() != b) return false;
        cells.insert(cells.end(), seg.begin() + 1, seg.end());
    }
    return true;
}

uint64_t MapGrid::hpaSphereDigest(uint8_t layer) {
    SphereHpaGraph& G = hpaSphereFor(layer);
    hpaSphereRefresh(G);
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
    for (size_t c = 0; c < G.nodes.size(); ++c) {
        mix(G.nodes[c].size());
        for (size_t i = 0; i < G.nodes[c].size(); ++i) {
            mix(uint64_t(G.nodes[c][i]));
            for (int32_t p : G.partners[c][i]) mix(uint64_t(p) + 7);
        }
        for (uint32_t d : G.dist[c]) mix(d);
    }
    return h;
}

bool MapGrid::sphereCellPath(int s, int g, uint8_t layer, bool hpa, std::vector<int>& cells, int maxNodes) {
    cells.clear();
    if (!surf || s < 0 || g < 0) return false;
    if (hpa && hpaPathSphere(s, g, layer, cells)) return true;
    if (hpa && sphereTopo_ && sphereTopo_->clusterOfCell[size_t(s)] != sphereTopo_->clusterOfCell[size_t(g)]) return false;
    cells.clear();
    return astarSphere(s, g, layer, maxNodes, cells);
}

} // namespace rts::game
