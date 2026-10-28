// Terrain analysis for the computer player (see RtsAITerrain.h). Integer maths only, deterministic.
#include "RtsAITerrain.h"
#include <algorithm>
#include <queue>

namespace rts::game {

void AITerrain::build(Simulation& sim, RtsModule& m) {
    World& wd = sim.world();
    const MapGrid& g = m.state(sim).grid;
    const Rules& R = m.rules();
    cells = int(g.flags.size());
    w = g.w;
    sphere = g.surf != nullptr;
    elev.assign(size_t(cells), 0);
    pass.assign(size_t(cells), 0);
    const Fixed lh = R.levelHeight.raw > 0 ? R.levelHeight : Fixed::fromInt(1);
    const uint16_t lim = g.slopeLimitFor(kLayerGround);
    // structures of players don't count as terrain (they die); neutral blockers (trees, mines, rocks) do
    std::vector<uint8_t> playerBlock(size_t(cells), 0);
    wd.each<Footprint, Owner>([&](Entity, Footprint& f, Owner& o) {
        if (o.player == 0 || o.player == kHostileNeutral) return;
        for (int y = f.y; y < f.y + f.h; ++y)
            for (int x = f.x; x < f.x + f.w; ++x)
                if (x >= 0 && y >= 0 && x < g.w && y < g.h) playerBlock[size_t(y * g.w + x)] = 1;
    });
    int32_t lo = 1 << 30, hi = -(1 << 30);
    for (int c = 0; c < cells; ++c) {
        const size_t i = size_t(c);
        const bool ground = (g.flags[i] & kCellGround) && (!g.hf || g.slope.empty() || g.slope[i] <= lim);
        pass[i] = ground && (!g.blocked[i] || playerBlock[i]) ? 1 : 0;
        Fixed h{};
        if (sphere) h = g.surf->cellHeight(c) / lh;
        else if (g.hf) h = g.cellHeight(c % g.w, c / g.w) / lh;
        else h = Fixed::fromInt(g.level[i]);
        elev[i] = int32_t((h * kUnit).floorInt());
        if (g.flags[i] & kCellGround) { lo = std::min(lo, elev[i]); hi = std::max(hi, elev[i]); }
    }
    relief = hi - lo >= kUnit / 4;
    // corridor width: BFS from every non-walkable cell (4 neighbours, map edge counts as a wall)
    clear.assign(size_t(cells), 15);
    std::vector<int> q;
    q.reserve(size_t(cells));
    auto nb4 = [&](int c, int k) {
        if (sphere) return g.surf->neighbor(c, k);
        static constexpr int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
        const int x = c % w + dx[k], y = c / w + dy[k];
        return x >= 0 && y >= 0 && x < w && y < g.h ? y * w + x : -1;
    };
    for (int c = 0; c < cells; ++c) {
        bool edge = !pass[size_t(c)];
        if (!edge) for (int k = 0; k < 4; ++k) if (nb4(c, k) < 0) edge = true;
        if (edge) { clear[size_t(c)] = pass[size_t(c)] ? 1 : 0; q.push_back(c); }
    }
    for (size_t qi = 0; qi < q.size(); ++qi) {
        const int c = q[qi];
        const uint8_t d = clear[size_t(c)];
        if (d >= 15) continue;
        for (int k = 0; k < 4; ++k) {
            const int n = nb4(c, k);
            if (n < 0 || clear[size_t(n)] <= d + 1) continue;
            clear[size_t(n)] = uint8_t(d + 1);
            q.push_back(n);
        }
    }
    // highest / lowest ground within kOverR cells: kOverR passes of a 3x3 max / min filter
    std::vector<int32_t> mx = elev, mn = elev, tmx = elev, tmn = elev;
    if (relief) {
        for (int pass_ = 0; pass_ < kOverR; ++pass_) {
            for (int c = 0; c < cells; ++c) {
                int32_t a = mx[size_t(c)], b = mn[size_t(c)];
                for (int k = 0; k < 8; ++k) {
                    int n;
                    if (sphere) n = g.surf->neighbor(c, k);
                    else {
                        static constexpr int dx[8] = {1, -1, 0, 0, 1, 1, -1, -1}, dy[8] = {0, 0, 1, -1, 1, -1, 1, -1};
                        const int x = c % w + dx[k], y = c / w + dy[k];
                        n = x >= 0 && y >= 0 && x < w && y < g.h ? y * w + x : -1;
                    }
                    if (n < 0) continue;
                    a = std::max(a, mx[size_t(n)]);
                    b = std::min(b, mn[size_t(n)]);
                }
                tmx[size_t(c)] = a; tmn[size_t(c)] = b;
            }
            mx.swap(tmx); mn.swap(tmn);
        }
    }
    over.assign(size_t(cells), 0);
    crest.assign(size_t(cells), 0);
    for (int c = 0; c < cells; ++c) {
        over[size_t(c)] = std::max(0, mx[size_t(c)] - elev[size_t(c)]);
        crest[size_t(c)] = elev[size_t(c)] - mn[size_t(c)];
    }
}

int AITerrain::nearestWalkable(const MapGrid& g, int c, int radius) const {
    if (c < 0 || c >= cells) return -1;
    if (pass[size_t(c)]) return c;
    for (int r = 1; r <= radius; ++r)
        for (int dy = -r; dy <= r; ++dy)
            for (int dx = -r; dx <= r; ++dx) {
                if (std::max(std::abs(dx), std::abs(dy)) != r) continue;
                const int n = g.offsetCell(c, dx, dy);
                if (n >= 0 && pass[size_t(n)]) return n;
            }
    return -1;
}

bool AITerrain::route(const MapGrid& g, int from, int to, int climbW, int exposureW, const std::vector<uint16_t>* avoid,
                      std::vector<int>& out, int maxVisit) const {
    out.clear();
    from = nearestWalkable(g, from);
    to = nearestWalkable(g, to);
    if (from < 0 || to < 0) return false;
    std::vector<int32_t> dist(size_t(cells), kFar);
    std::vector<int32_t> parent(size_t(cells), -1);
    using Node = std::pair<int64_t, int>;   // (cost, cell): ties by cell index
    std::priority_queue<Node, std::vector<Node>, std::greater<Node>> open;
    dist[size_t(from)] = 0;
    open.push({0, from});
    int visited = 0;
    while (!open.empty()) {
        auto [d, c] = open.top();
        open.pop();
        if (d != dist[size_t(c)]) continue;
        if (c == to || ++visited > maxVisit) break;
        neighbors(g, c, [&](int n, int step) {
            int64_t cost = step;
            const int32_t climb = elev[size_t(n)] - elev[size_t(c)];
            if (climb > 0) cost += int64_t(climb) * climbW / kUnit;
            cost += int64_t(over[size_t(n)]) * exposureW / kUnit;
            if (avoid) cost += (*avoid)[size_t(n)];
            const int64_t nd = d + cost;
            if (nd >= kFar || nd >= dist[size_t(n)]) return;
            dist[size_t(n)] = int32_t(nd);
            parent[size_t(n)] = c;
            open.push({nd, n});
        });
    }
    if (dist[size_t(to)] >= kFar) return false;
    for (int c = to; c != -1; c = parent[size_t(c)]) out.push_back(c);
    std::reverse(out.begin(), out.end());
    return true;
}

void AITerrain::distances(const MapGrid& g, const std::vector<int>& from, int32_t limit, std::vector<int32_t>& out) const {
    out.assign(size_t(cells), kFar);
    std::vector<int> q;
    for (int c : from) {
        const int s = nearestWalkable(g, c);
        if (s >= 0 && out[size_t(s)] != 0) { out[size_t(s)] = 0; q.push_back(s); }
    }
    for (size_t qi = 0; qi < q.size(); ++qi) {
        const int c = q[qi];
        const int32_t d = out[size_t(c)];
        if (d >= limit) continue;
        neighbors(g, c, [&](int n, int step) {
            if (step != 10 || out[size_t(n)] <= d + 10) return;   // BFS over orthogonal steps (exact for 4-paths)
            out[size_t(n)] = d + 10;
            q.push_back(n);
        });
    }
}

} // namespace rts::game
