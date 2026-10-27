#include "RtsMap.h"
#include "../../engine/sim/CommonComponents.h"
#include <algorithm>
#include <cstdlib>
#include <queue>

namespace rts::game {

uint8_t cellFlagsFromChar(char c) {
    switch (c) {
        case '~': return kCellWater;
        case '#': return 0;
        case ',': return uint8_t(kCellGround | kCellNoBuild);
        case '=': return uint8_t(kCellGround | kCellWater | kCellNoBuild);
        case '/': return uint8_t(kCellGround | kCellRamp | kCellNoBuild);
        case 'R': return uint8_t(kCellGround | kCellRock);
        case '+': return uint8_t(kCellGround | kCellRoad);
        default: return kCellGround;
    }
}

void MapGrid::init(int width, int h_) {
    w = width; h = h_;
    const size_t n = size_t(w * h);
    flags.assign(n, kCellGround);
    level.assign(n, 0);
    height.assign(n, 0);
    blocked.assign(n, 0);
    creep.assign(n, 0);
    fieldRes.assign(n, -1);
    fieldAmt.assign(n, Fixed{});
    region.assign(n, -1);
    dig.assign(n, 0);
    digHp.assign(n, Fixed{});
    digMark.assign(n, 0);
    territory.assign(n, 0);
    room.assign(n, -1);
}

bool MapGrid::passable(int x, int y, uint8_t layer) const {
    if (!inside(x, y)) return false;
    if (layer & 2 /*air*/) return true;
    const size_t i = size_t(index(x, y));
    if (blocked[i]) return false;
    if (layer & 4 /*naval*/) return flags[i] & kCellWater;
    if (layer & 8 /*sand only*/) return (flags[i] & kCellGround) && !(flags[i] & (kCellRock | kCellRamp));
    return flags[i] & kCellGround;
}

bool MapGrid::canStep(int x0, int y0, int x1, int y1, uint8_t layer) const {
    if (!passable(x1, y1, layer)) return false;
    if (layer & 2) return true;
    const size_t a = size_t(index(x0, y0)), b = size_t(index(x1, y1));
    int la = level[a], lb = level[b];
    if (la == lb) return true;
    // different levels: only through a ramp, one level at a time
    if (std::abs(la - lb) > 1) return false;
    return (flags[a] & kCellRamp) || (flags[b] & kCellRamp);
}

void MapGrid::block(int x0, int y0, int fw, int fh, int delta) {
    for (int y = y0; y < y0 + fh; ++y)
        for (int x = x0; x < x0 + fw; ++x)
            if (inside(x, y)) { auto& b = blocked[size_t(index(x, y))]; b = uint16_t(int(b) + delta); }
}

bool MapGrid::areaBuildable(int x0, int y0, int fw, int fh) const {
    if (!inside(x0, y0)) return false;
    const int lv = level[size_t(index(x0, y0))];
    for (int y = y0; y < y0 + fh; ++y)
        for (int x = x0; x < x0 + fw; ++x) {
            if (!inside(x, y)) return false;
            size_t i = size_t(index(x, y));
            if (blocked[i] || !(flags[i] & kCellGround) || (flags[i] & kCellNoBuild) || level[i] != lv) return false;
            if (fieldRes[i] >= 0 && fieldAmt[i].raw > 0) return false;
        }
    return true;
}

bool MapGrid::nearestPassable(int& x, int& y, uint8_t layer, int radius) const {
    if (passable(x, y, layer)) return true;
    for (int r = 1; r <= radius; ++r)
        for (int dy = -r; dy <= r; ++dy)
            for (int dx = -r; dx <= r; ++dx) {
                if (std::abs(dx) != r && std::abs(dy) != r) continue;
                if (passable(x + dx, y + dy, layer)) { x += dx; y += dy; return true; }
            }
    return false;
}

bool MapGrid::lineWalkable(int x0, int y0, int x1, int y1, uint8_t layer) const {
    // Supercover-ish Bresenham: also test the side cells on diagonal steps so paths do not clip corners.
    int dx = std::abs(x1 - x0), dy = std::abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx - dy, x = x0, y = y0;
    if (!passable(x, y, layer)) return false;
    for (;;) {
        if (x == x1 && y == y1) return true;
        int e2 = 2 * err;
        int nx = x, ny = y;
        if (e2 > -dy) { err -= dy; nx += sx; }
        if (e2 < dx) { err += dx; ny += sy; }
        // diagonal: every continuous route through the 2x2 block must be legal (cliff corners)
        if (nx != x && ny != y && (!canStep(x, y, nx, y, layer) || !canStep(x, y, x, ny, layer) ||
                                   !canStep(nx, y, nx, ny, layer) || !canStep(x, ny, nx, ny, layer))) return false;
        if (!canStep(x, y, nx, ny, layer)) return false;
        x = nx; y = ny;
    }
}

bool MapGrid::findPath(Vec3 from, Vec3 to, uint8_t layer, std::vector<Vec3>& out, int maxNodes) {
    out.clear();
    if (layer & 2) { out.push_back(to); return true; } // air: straight line
    int sx = cellX(from.x), sy = cellY(from.z), gx = cellX(to.x), gy = cellY(to.z);
    if (!inside(sx, sy) || w == 0) return false;
    if (!passable(sx, sy, layer)) nearestPassable(sx, sy, layer, 3);
    bool goalOk = passable(gx, gy, layer);
    if (sx == gx && sy == gy) { out.push_back(to); return true; }
    if (goalOk && lineWalkable(sx, sy, gx, gy, layer)) { out.push_back(to); return true; }

    const size_t n = size_t(w * h);
    if (gCost_.size() != n) { gCost_.assign(n, 0); stamp_.assign(n, 0); parent_.assign(n, -1); }
    ++search_;
    struct Node { uint32_t f, hh; int32_t idx; };
    auto cmp = [](const Node& a, const Node& b) {  // min-heap on (f, h, idx): fully deterministic
        if (a.f != b.f) return a.f > b.f;
        if (a.hh != b.hh) return a.hh > b.hh;
        return a.idx > b.idx;
    };
    std::priority_queue<Node, std::vector<Node>, decltype(cmp)> open(cmp);
    auto heur = [&](int x, int y) {
        uint32_t dx = uint32_t(std::abs(x - gx)), dy = uint32_t(std::abs(y - gy));
        return 10 * (dx + dy) - 6 * std::min(dx, dy); // octile, 10/14 costs
    };
    int start = index(sx, sy);
    stamp_[size_t(start)] = search_; gCost_[size_t(start)] = 0; parent_[size_t(start)] = -1;
    open.push({heur(sx, sy), heur(sx, sy), start});
    int best = start; uint32_t bestH = heur(sx, sy);
    int expanded = 0;
    static const int dxs[8] = {1, -1, 0, 0, 1, 1, -1, -1};
    static const int dys[8] = {0, 0, 1, -1, 1, -1, 1, -1};
    bool found = false;
    while (!open.empty() && expanded < maxNodes) {
        Node cur = open.top(); open.pop();
        int cx = cur.idx % w, cy = cur.idx / w;
        uint32_t g = gCost_[size_t(cur.idx)];
        if (cur.f != g + heur(cx, cy)) continue; // stale entry
        ++expanded;
        if (cx == gx && cy == gy) { best = cur.idx; found = true; break; }
        if (cur.hh < bestH) { bestH = cur.hh; best = cur.idx; }
        for (int k = 0; k < 8; ++k) {
            int nx = cx + dxs[k], ny = cy + dys[k];
            if (!canStep(cx, cy, nx, ny, layer)) continue;
            if (k >= 4 && (!canStep(cx, cy, nx, cy, layer) || !canStep(cx, cy, cx, ny, layer) ||
                           !canStep(nx, cy, nx, ny, layer) || !canStep(cx, ny, nx, ny, layer))) continue; // no corner cutting
            int ni = index(nx, ny);
            uint32_t ng = g + (k >= 4 ? 14u : 10u);
            if (stamp_[size_t(ni)] == search_ && gCost_[size_t(ni)] <= ng) continue;
            stamp_[size_t(ni)] = search_; gCost_[size_t(ni)] = ng; parent_[size_t(ni)] = cur.idx;
            uint32_t hh = heur(nx, ny);
            open.push({ng + hh, hh, ni});
        }
    }
    // rebuild cells
    std::vector<int> cells;
    for (int c = best; c != -1; c = parent_[size_t(c)]) cells.push_back(c);
    std::reverse(cells.begin(), cells.end());
    // string pulling: keep only cells needed for line of sight
    std::vector<int> pts;
    size_t i = 0;
    while (i + 1 < cells.size()) {
        size_t j = cells.size() - 1;
        int ax = cells[i] % w, ay = cells[i] / w;
        while (j > i + 1 && !lineWalkable(ax, ay, cells[j] % w, cells[j] / w, layer)) --j;
        pts.push_back(cells[j]);
        i = j;
    }
    for (size_t k = 0; k < pts.size(); ++k) {
        bool last = k + 1 == pts.size();
        if (last && found && goalOk) out.push_back(to);
        else out.push_back(cellCenter(pts[k] % w, pts[k] / w));
    }
    if (out.empty()) out.push_back(found && goalOk ? to : cellCenter(sx, sy));
    return found;
}

int RoadGraph::nodeOf(Entity e) const {
    for (size_t i = 0; i < nodes.size(); ++i) if (nodes[i] == e) return int(i);
    return -1;
}

std::vector<uint16_t> RoadGraph::path(World& w, int from, int to) const {
    std::vector<uint16_t> out;
    const size_t n = nodes.size();
    if (from < 0 || to < 0 || size_t(from) >= n || size_t(to) >= n) return out;
    std::vector<Fixed> dist(n, Fixed::fromInt(int64_t(1) << 30));
    std::vector<int> prev(n, -1);
    std::vector<char> done(n, 0);
    dist[size_t(from)] = Fixed{};
    for (size_t it = 0; it < n; ++it) {
        int u = -1;
        for (size_t k = 0; k < n; ++k) if (!done[k] && (u < 0 || dist[k] < dist[size_t(u)])) u = int(k);
        if (u < 0 || dist[size_t(u)].raw >= (int64_t(1) << 46)) break;
        done[size_t(u)] = 1;
        if (u == to) break;
        auto* tu = w.tryGet<Transform>(nodes[size_t(u)]);
        if (!tu) continue;
        for (uint16_t v : adj[size_t(u)]) {
            auto* tv = w.tryGet<Transform>(nodes[v]);
            if (!tv) continue;
            Fixed d = dist[size_t(u)] + (tv->pos - tu->pos).length();
            if (d < dist[v]) { dist[v] = d; prev[v] = u; }
        }
    }
    if (prev[size_t(to)] < 0 && from != to) return out;
    for (int c = to; c != from; c = prev[size_t(c)]) out.push_back(uint16_t(c));
    std::reverse(out.begin(), out.end());
    return out;
}

void SpatialHash::reset(int worldW, int worldH, int cellSize) {
    cell = cellSize;
    w = (worldW + cell - 1) / cell + 1;
    h = (worldH + cell - 1) / cell + 1;
    if (buckets.size() != size_t(w * h)) buckets.assign(size_t(w * h), {});
    for (auto& b : buckets) b.clear();
}

void SpatialHash::insert(Entity e, Vec3 p) {
    int x = std::clamp(int(p.x.floorInt()) / cell, 0, w - 1), y = std::clamp(int(p.z.floorInt()) / cell, 0, h - 1);
    buckets[size_t(y * w + x)].push_back(e);
}

} // namespace rts::game
