// MapGrid on cube-sphere surfaces (planet maps): pathing, clearance, slopes and the spatial hash over the
// stacked face rows (RtsSurface.h). Flat maps never reach this file.
#include "RtsMap.h"
#include "engine/core/FixedMath.h"
#include <algorithm>
#include <cstdlib>
#include <queue>

namespace rts::game {

namespace {
int nbIndex(const Surface& s, int c0, int c1) {
    for (int k = 0; k < 8; ++k) if (s.neighbor(c0, k) == c1) return k;
    return -1;
}
} // namespace

bool MapGrid::canMoveCells(int c0, int c1, uint8_t layer) const {
    if (c0 == c1) return true;
    if (c0 < 0 || c1 < 0) return false;
    if (!surf) return stepOk(c0 % w, c0 / w, c1 % w, c1 / w, layer);
    const int k = nbIndex(*surf, c0, c1);
    return k >= 0 && canMoveDir(c0, k, layer);
}

bool MapGrid::canMoveDir(int c0, int k, uint8_t layer) const {
    const int c1 = surf->neighbor(c0, k);
    if (c1 < 0 || !canStep(c0 % w, c0 / w, c1 % w, c1 / w, layer)) return false;
    if (k < 4) return true;
    // diagonal: both orthogonal routes through the 2x2 block must be legal (no corner cutting)
    const int o1 = surf->neighbor(c0, Surface::kDx[k] > 0 ? 0 : 1), o2 = surf->neighbor(c0, Surface::kDy[k] > 0 ? 2 : 3);
    if (o1 < 0 || o2 < 0) return false;
    auto st = [&](int a, int b) { return canStep(a % w, a / w, b % w, b / w, layer); };
    return st(c0, o1) && st(c0, o2) && st(o1, c1) && st(o2, c1);
}

void MapGrid::enableSphereHeights(Fixed levelHeight) {
    hf = true;
    slope.assign(size_t(w * h), 0);
    for (int c = 0; c < w * h; ++c) {
        int b, f, x, y;
        if (!surf->cellFace(c, b, f, x, y)) continue;
        const int64_t a = surf->corner(b, f, x, y), bb = surf->corner(b, f, x + 1, y);
        const int64_t cc = surf->corner(b, f, x, y + 1), d = surf->corner(b, f, x + 1, y + 1);
        const int64_t gx = (bb - a) + (d - cc), gz = (cc - a) + (d - bb);
        int64_t s = fxSqrt(Fixed::fromInt((gx * gx + gz * gz) * 256 * 256 / 4)).floorInt() / 16;
        for (int64_t e : {bb - a, d - cc, cc - a, d - bb}) s = std::max(s, (e < 0 ? -e : e) * 16);
        // rise per cell -> rise per world unit (planet cells are cellSize units wide)
        const Fixed cs = surf->cellSize(b);
        if (cs.raw > 0) s = (s * Fixed::kOne) / cs.raw;
        slope[size_t(c)] = uint16_t(std::min<int64_t>(s, 65535));
        const Fixed hc = surf->cellHeight(c);
        int lv = levelHeight.raw > 0 ? int((hc / levelHeight).floorInt()) : 0;
        level[size_t(c)] = int8_t(std::clamp(lv, 0, 9));
    }
    clearanceDirty = true;
}

void MapGrid::updateClearanceSphere() {
    clearanceDirty = false;
    const size_t n = size_t(w * h);
    clearance.assign(n, 3);
    std::vector<int> q;
    for (size_t i = 0; i < n; ++i)
        if (!(flags[i] & kCellGround) || blocked[i] || (hf && slope[i] > slopeLimit[0])) { clearance[i] = 0; q.push_back(int(i)); }
    for (size_t qi = 0; qi < q.size(); ++qi) {
        const int c = q[qi];
        for (int k = 0; k < 8; ++k) {
            const int ni = surf->neighbor(c, k);
            if (ni < 0) continue;
            if (clearance[size_t(ni)] > clearance[size_t(c)] + 1) { clearance[size_t(ni)] = uint8_t(clearance[size_t(c)] + 1); q.push_back(ni); }
        }
    }
}

bool MapGrid::lineWalkableSphere(int a, int b, uint8_t layer) const {
    if (!passable(a % w, a / w, layer)) return false;
    const int body = surf->bodyOfCell(a);
    if (surf->bodyOfCell(b) != body) return false;
    const CubeSphere& cs = surf->cubeOf(body);
    const Vec3 da = surf->cellDir(a), db = surf->cellDir(b);
    // samples every third of a cell along the chord (cellFromDir takes any non-zero vector)
    const Fixed cellTurn = Fixed::fromRatio(1, 4 * cs.faceSize());
    const Fixed arc = sphereArcTurns(da, db);
    const int n = int((arc * 3 / cellTurn).floorInt()) + 2;
    int cur = a;
    for (int i = 1; i <= n; ++i) {
        const Vec3 p = da + (db - da) * Fixed::fromRatio(i, n);
        const SphereCell sc = cs.cell(cs.cellFromDir(p));
        const int c = surf->gridCell(body, sc.face, sc.x, sc.y);
        if (c == cur) continue;
        if (!lineOk(c % w, c / w) || !canMoveCells(cur, c, layer)) return false;
        cur = c;
    }
    return cur == b;
}

bool MapGrid::astarSphere(int start, int goal, uint8_t layer, int maxNodes, std::vector<int>& cells) {
    bool found = false;
    const size_t n = size_t(w * h);
    if (gCost_.size() != n) { gCost_.assign(n, 0); stamp_.assign(n, 0); parent_.assign(n, -1); }
    ++search_;
    struct Node { uint32_t f, hh; int32_t idx; };
    auto cmp = [](const Node& a, const Node& b) {
        if (a.f != b.f) return a.f > b.f;
        if (a.hh != b.hh) return a.hh > b.hh;
        return a.idx > b.idx;
    };
    std::priority_queue<Node, std::vector<Node>, decltype(cmp)> open(cmp);
    // heuristic: chord to the goal in cells, x9 (cells near face corners are ~0.8 of the mean: keeps it
    // close to admissible), integer
    const int body = surf->bodyOfCell(start);
    const int N = surf->cubeOf(body).faceSize();
    const Vec3 gd = surf->cellDir(goal);
    const Fixed hk = Fixed::fromInt(9 * 4 * N) / kFxTwoPi;
    auto heur = [&](int c) { return uint32_t((((surf->cellDir(c) - gd).length()) * hk).floorInt()); };
    stamp_[size_t(start)] = search_; gCost_[size_t(start)] = 0; parent_[size_t(start)] = -1;
    open.push({heur(start), heur(start), start});
    int best = start; uint32_t bestH = heur(start);
    int expanded = 0;
    while (!open.empty() && expanded < maxNodes) {
        Node cur = open.top(); open.pop();
        const uint32_t g = gCost_[size_t(cur.idx)];
        if (cur.f != g + cur.hh) continue;
        ++expanded;
        if (cur.idx == goal) { best = cur.idx; found = true; break; }
        if (cur.hh < bestH) { bestH = cur.hh; best = cur.idx; }
        for (int k = 0; k < 8; ++k) {
            const int ni = surf->neighbor(cur.idx, k);
            if (ni < 0 || !canMoveDir(cur.idx, k, layer)) continue;
            uint32_t ng = g + (k >= 4 ? 14u : 10u);
            if (pathClear_ > 0 && clearance[size_t(ni)] < pathClear_) ng += 30u * uint32_t(pathClear_ - clearance[size_t(ni)]);
            if (pathSoft_ && soft[size_t(ni)] && ni != pathEndB_) ng += 25u;
            if (stamp_[size_t(ni)] == search_ && gCost_[size_t(ni)] <= ng) continue;
            stamp_[size_t(ni)] = search_; gCost_[size_t(ni)] = ng; parent_[size_t(ni)] = cur.idx;
            const uint32_t hh = heur(ni);
            open.push({ng + hh, hh, ni});
        }
    }
    for (int c = best; c != -1; c = parent_[size_t(c)]) cells.push_back(c);
    std::reverse(cells.begin(), cells.end());
    return found;
}

bool MapGrid::findPathSphere(Vec3 from, Vec3 to, uint8_t layer, std::vector<Vec3>& out, int maxNodes, int clear, bool useSoft) {
    if (layer & 2) { out.push_back(to); return true; } // air / orbit: free movement (great circle)
    int s = surf->cellAt(from), gl = surf->cellAt(to);
    if (s < 0 || gl < 0 || surf->bodyOfCell(s) != surf->bodyOfCell(gl)) return false; // other bodies: transit only
    if (!passable(s % w, s / w, layer)) {   // stuck in a blocked cell: nearest passable within 3 rings
        for (int r = 1, done = 0; r <= 3 && !done; ++r)
            for (int dy = -r; dy <= r && !done; ++dy)
                for (int dx = -r; dx <= r && !done; ++dx) {
                    if (std::abs(dx) != r && std::abs(dy) != r) continue;
                    const int c = surf->offset(s, dx, dy);
                    if (c >= 0 && passable(c % w, c / w, layer)) { s = c; done = 1; }
                }
    }
    if (!(layer & 1)) clear = 0;
    if (clear > 0 && clearanceDirty) updateClearanceSphere();
    pathClear_ = clear;
    pathSoft_ = useSoft && soft.size() == flags.size();
    pathEndA_ = s;
    pathEndB_ = gl;
    struct Reset { MapGrid* g; ~Reset() { g->pathClear_ = 0; g->pathSoft_ = false; g->pathEndA_ = g->pathEndB_ = -1; } } reset{this};
    const bool goalOk = passable(gl % w, gl / w, layer);
    if (s == gl) { out.push_back(to); return true; }
    if (goalOk && lineWalkableSphere(s, gl, layer)) { out.push_back(to); return true; }
    std::vector<int> cells;
    bool found = false, routed = false;
    // long trips on big bodies: HPA* route refined with A* (RtsHpaSphere.cpp)
    const int body = surf->bodyOfCell(s);
    if (goalOk && sphereHpaBody(body)) {
        const int N = surf->cubeOf(body).faceSize();
        const Fixed cellsApart = (surf->cellDir(s) - surf->cellDir(gl)).length() * Fixed::fromInt(4 * N) / kFxTwoPi;
        if (cellsApart > Fixed::fromInt(2 * SphereHpaTopo::kC)) {
            routed = found = hpaPathSphere(s, gl, layer, cells);
            ++hpaStats[routed ? 0 : 1];
        }
    }
    if (!routed) { cells.clear(); found = astarSphere(s, gl, layer, maxNodes, cells); }
    std::vector<int> pts;
    size_t i = 0;
    const size_t window = 48;
    while (i + 1 < cells.size()) {
        size_t j = std::min(cells.size() - 1, i + window);
        while (j > i + 1 && !lineWalkableSphere(cells[i], cells[j], layer)) --j;
        pts.push_back(cells[j]);
        i = j;
    }
    for (size_t k = 0; k < pts.size(); ++k) {
        const bool last = k + 1 == pts.size();
        if (last && found && goalOk) out.push_back(to);
        else out.push_back(surf->cellCenter(pts[k]));
    }
    if (out.empty()) out.push_back(found && goalOk ? to : surf->cellCenter(s));
    return found;
}

} // namespace rts::game
