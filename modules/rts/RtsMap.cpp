#include "RtsMap.h"
#include "../../engine/sim/CommonComponents.h"
#include <algorithm>
#include <climits>
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

bool applyCornersFile(Json& map, const ContentReader& read, std::string* err) {
    // planet maps: every body's terrain.cornersFile (6 faces x (N+1)^2 int16 LE)
    if (Json* world = map.find("world"); world && world->isObject())
        if (Json* bodies = world->find("bodies"); bodies && bodies->isArray())
            for (size_t i = 0; i < bodies->size(); ++i) {
                Json* t = bodies->at(i).find("terrain");
                if (!t || !t->isObject() || !t->has("cornersFile") || t->has("cornersBlob")) continue;
                const std::string file = t->getStr("cornersFile");
                std::string bytes;
                if (file.empty() || !read(file, bytes)) { if (err) *err = "map world body '" + bodies->at(i).getStr("id") + "' terrain.cornersFile: cannot read '" + file + "'"; return false; }
                t->set("cornersBlob", Json::string(std::move(bytes)));
            }
    Json* terrain = map.find("terrain");
    if (!terrain || !terrain->isObject() || !terrain->has("cornersFile") || terrain->has("cornersBlob")) return true;
    const std::string file = terrain->getStr("cornersFile");
    std::string bytes;
    if (file.empty() || !read(file, bytes)) { if (err) *err = "map terrain.cornersFile: cannot read '" + file + "'"; return false; }
    terrain->set("cornersBlob", Json::string(std::move(bytes)));
    return true;
}

std::string encodeCorners(const std::vector<int16_t>& c) {
    std::string out(c.size() * 2, '\0');
    for (size_t i = 0; i < c.size(); ++i) { out[2 * i] = char(uint16_t(c[i]) & 0xff); out[2 * i + 1] = char(uint16_t(c[i]) >> 8); }
    return out;
}

bool decodeCorners(const Json& terrain, int w, int h, std::vector<int16_t>& out, std::string* err) {
    const std::string file = terrain.getStr("cornersFile");
    if (!terrain["cornersBlob"].isString()) { if (err) *err = "map terrain.cornersFile '" + file + "' was not loaded (applyCornersFile)"; return false; }
    const std::string& b = terrain["cornersBlob"].str();
    const size_t n = size_t(w + 1) * size_t(h + 1);
    if (b.size() != n * 2) {
        if (err) *err = "map terrain.cornersFile '" + file + "': expected " + std::to_string(n * 2) + " bytes ((w+1)*(h+1) int16), got " + std::to_string(b.size());
        return false;
    }
    out.resize(n);
    for (size_t i = 0; i < n; ++i) out[i] = int16_t(uint16_t(uint8_t(b[2 * i])) | uint16_t(uint8_t(b[2 * i + 1])) << 8);
    return true;
}

void MapGrid::init(int width, int h_) {
    w = width; h = h_;
    const size_t n = size_t(w * h);
    flags.assign(n, kCellGround);
    level.assign(n, 0);
    height.assign(n, 0);
    corner.clear();
    hf = false;
    slope.clear();
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
    clearance.assign(n, 0);
    clearanceDirty = true;
    ++pathVersion_;
    soft.assign(n, 0);
    softCells.clear();
}

namespace {
int64_t isqrt64(int64_t v) {  // floor(sqrt(v)), integer only
    if (v <= 0) return 0;
    int64_t r = 0, bit = int64_t(1) << 62;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; }
        else r >>= 1;
        bit >>= 2;
    }
    return r;
}
} // namespace

uint16_t MapGrid::slopeUnits(Fixed rise) {
    int64_t v = rise.raw >> (Fixed::kFracBits - 8);
    return uint16_t(std::clamp<int64_t>(v, 0, 65535));
}

void MapGrid::enableHeightfield(Fixed levelHeight) {
    hf = true;
    slope.assign(size_t(w * h), 0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const int64_t a = corner[size_t(cornerIndex(x, y))], b = corner[size_t(cornerIndex(x + 1, y))];
            const int64_t c = corner[size_t(cornerIndex(x, y + 1))], d = corner[size_t(cornerIndex(x + 1, y + 1))];
            // steepness = max(gradient length, steepest edge): a twisted cell has no gradient but steep edges
            const int64_t gx = (b - a) + (d - c), gz = (c - a) + (d - b);       // 2x gradient, 1/16 units
            int64_t s = isqrt64((gx * gx + gz * gz) * 256 * 256 / 4) / 16;      // 1/256 units per cell
            for (int64_t e : {b - a, d - c, c - a, d - b}) s = std::max(s, (e < 0 ? -e : e) * 16);
            slope[size_t(index(x, y))] = uint16_t(std::min<int64_t>(s, 65535));
            Fixed hc = cellHeight(x, y);
            int lv = levelHeight.raw > 0 ? int((hc / levelHeight).floorInt()) : 0;
            level[size_t(index(x, y))] = int8_t(std::clamp(lv, 0, 9));
        }
    clearanceDirty = true;
    ++pathVersion_;
}

Fixed MapGrid::cellHeight(int x, int y) const {
    if (corner.empty() || !inside(x, y)) return Fixed{};
    int64_t s = int64_t(corner[size_t(cornerIndex(x, y))]) + corner[size_t(cornerIndex(x + 1, y))] +
                corner[size_t(cornerIndex(x, y + 1))] + corner[size_t(cornerIndex(x + 1, y + 1))];
    return Fixed::fromRaw(s << (Fixed::kFracBits - 6)); // sum / 4 / 16
}

Fixed MapGrid::surfaceHeight(Fixed x, Fixed z) const {
    if (corner.empty() || w == 0) return Fixed{};
    const int64_t one = int64_t(1) << Fixed::kFracBits;
    int64_t rx = std::clamp<int64_t>(x.raw, 0, int64_t(w) * one), rz = std::clamp<int64_t>(z.raw, 0, int64_t(h) * one);
    int cx = int(rx >> Fixed::kFracBits), cz = int(rz >> Fixed::kFracBits);
    int64_t fx = rx & (one - 1), fz = rz & (one - 1);
    if (cx >= w) { cx = w - 1; fx = one; }
    if (cz >= h) { cz = h - 1; fz = one; }
    const int64_t a = corner[size_t(cornerIndex(cx, cz))], b = corner[size_t(cornerIndex(cx + 1, cz))];
    const int64_t c = corner[size_t(cornerIndex(cx, cz + 1))], d = corner[size_t(cornerIndex(cx + 1, cz + 1))];
    const int64_t top = a * (one - fx) + b * fx, bot = c * (one - fx) + d * fx;   // corner * 2^16
    const int64_t v = (top >> 4) * (one - fz) + (bot >> 4) * fz;                  // corner * 2^28
    return Fixed::fromRaw(v >> (28 - Fixed::kFracBits + 4));                     // / 16 units
}

Vec3 MapGrid::gradient(Fixed x, Fixed z) const {
    if (corner.empty() || w == 0) return {};
    int cx = std::clamp(int(x.floorInt()), 0, w - 1), cz = std::clamp(int(z.floorInt()), 0, h - 1);
    const int64_t a = corner[size_t(cornerIndex(cx, cz))], b = corner[size_t(cornerIndex(cx + 1, cz))];
    const int64_t c = corner[size_t(cornerIndex(cx, cz + 1))], d = corner[size_t(cornerIndex(cx + 1, cz + 1))];
    // (diff sum / 2) / 16 units per cell
    return {Fixed::fromRaw(((b - a) + (d - c)) << (Fixed::kFracBits - 5)), Fixed{}, Fixed::fromRaw(((c - a) + (d - b)) << (Fixed::kFracBits - 5))};
}

int MapGrid::clearanceFor(Fixed radius) {
    // a cell is 1 wide: bodies up to 0.5 fit anywhere, every further half cell needs one more free ring
    if (radius <= Fixed::fromRatio(1, 2)) return 0;
    return std::min(3, int(((radius - Fixed::fromRatio(1, 2)) * 2 + Fixed::fromRaw(65535)).floorInt()));
}

void MapGrid::updateClearance() {
    if (surf) { updateClearanceSphere(); return; }
    clearanceDirty = false;
    const size_t n = size_t(w * h);
    clearance.assign(n, 3);
    std::vector<int> q;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            size_t i = size_t(index(x, y));
            if (!(flags[i] & kCellGround) || blocked[i] || (hf && slope[i] > slopeLimit[0])) { clearance[i] = 0; q.push_back(int(i)); }
            else if (x == 0 || y == 0 || x == w - 1 || y == h - 1) { clearance[i] = 1; q.push_back(int(i)); }
        }
    for (size_t qi = 0; qi < q.size(); ++qi) {
        int c = q[qi], x = c % w, y = c / w;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                if (!inside(x + dx, y + dy)) continue;
                size_t ni = size_t(index(x + dx, y + dy));
                if (clearance[ni] > clearance[size_t(c)] + 1) { clearance[ni] = uint8_t(clearance[size_t(c)] + 1); q.push_back(int(ni)); }
            }
    }
}

bool MapGrid::lineOk(int x, int y) const {
    int i = index(x, y);
    if (i == pathEndA_ || i == pathEndB_) return true;
    if (pathSoft_ && soft[size_t(i)]) return false;
    if (pathClear_ > 0 && clearance[size_t(i)] < pathClear_) return false;
    return true;
}

bool MapGrid::passable(int x, int y, uint8_t layer) const {
    if (!inside(x, y)) return false;
    if (layer & 2 /*air*/) return true;
    const size_t i = size_t(index(x, y));
    if (blocked[i]) return false;
    if (layer & 4 /*naval*/) { if (flags[i] & kCellWater) return true; if (!(layer & 1)) return false; }   // ground | naval: amphibious
    const bool ok = (layer & 8 /*sand only*/) ? (flags[i] & kCellGround) && !(flags[i] & (kCellRock | kCellRamp)) : (flags[i] & kCellGround) != 0;
    return ok && (!hf || slope[i] <= slopeLimitFor(layer)); // heightfield: too steep for this body
}

bool MapGrid::canStep(int x0, int y0, int x1, int y1, uint8_t layer) const {
    if (!passable(x1, y1, layer)) return false;
    if ((layer & 2) || hf) return true;  // heightfield: slopes (in passable) replace cliffs and ramps
    const size_t a = size_t(index(x0, y0)), b = size_t(index(x1, y1));
    int la = level[a], lb = level[b];
    if (la == lb) return true;
    // different levels: only through a ramp, one level at a time
    if (std::abs(la - lb) > 1) return false;
    return (flags[a] & kCellRamp) || (flags[b] & kCellRamp);
}

void MapGrid::block(int x0, int y0, int fw, int fh, int delta) {
    ++flowStamp_;   // flow fields revalidate (RtsFlowField.cpp)
    for (int y = y0; y < y0 + fh; ++y) for (int x = x0; x < x0 + fw; ++x) if (inside(x, y)) flowTouch(index(x, y));
    for (int y = y0; y < y0 + fh; ++y)
        for (int x = x0; x < x0 + fw; ++x)
            if (inside(x, y)) { auto& b = blocked[size_t(index(x, y))]; b = uint16_t(int(b) + delta); }
    ++pathVersion_;
    // clearance is capped at 3, so only cells within 3 of the footprint change: patch them (same values as a
    // full recompute) instead of a whole-map pass on the next path query
    if (surf) clearanceDirty = true; // planet cells: rows are not geometric neighbours across seams
    else if (!clearanceDirty && clearance.size() == flags.size()) updateClearanceRect(x0 - 3, y0 - 3, x0 + fw + 2, y0 + fh + 2);
    else clearanceDirty = true;
    if (surf)
        for (int y = y0; y < y0 + fh; ++y)
            for (int x = x0; x < x0 + fw; ++x) if (inside(x, y)) markSphereDirty(index(x, y));
    for (auto& G : hpa_)
        for (int cy = std::max(0, y0) / HpaGraph::kC; cy <= std::min(h - 1, y0 + fh - 1) / HpaGraph::kC && cy < G.ch; ++cy)
            for (int cx = std::max(0, x0) / HpaGraph::kC; cx <= std::min(w - 1, x0 + fw - 1) / HpaGraph::kC && cx < G.cw; ++cx) {
                G.dirty[size_t(cy * G.cw + cx)] = 1; G.anyDirty = true;
            }
}

void MapGrid::cellChanged(int x, int y) {
    clearanceDirty = true;
    ++flowStamp_;
    if (inside(x, y)) flowTouch(index(x, y));
    ++pathVersion_;
    if (surf && inside(x, y)) markSphereDirty(index(x, y));
    for (auto& G : hpa_)
        if (inside(x, y)) { G.dirty[size_t((y / HpaGraph::kC) * G.cw + x / HpaGraph::kC)] = 1; G.anyDirty = true; }
}

void MapGrid::updateClearanceRect(int x0, int y0, int x1, int y1) {
    x0 = std::max(0, x0); y0 = std::max(0, y0); x1 = std::min(w - 1, x1); y1 = std::min(h - 1, y1);
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            int v = 3; // multi-source Chebyshev distance: solid cells count 0, map-edge cells 1 (as updateClearance)
            for (int dy = -3; dy <= 3; ++dy)
                for (int dx = -3; dx <= 3; ++dx) {
                    int nx = x + dx, ny = y + dy;
                    if (!inside(nx, ny)) continue;
                    size_t i = size_t(index(nx, ny));
                    int d = std::max(std::abs(dx), std::abs(dy));
                    if (!(flags[i] & kCellGround) || blocked[i] || (hf && slope[i] > slopeLimit[0])) v = std::min(v, d);
                    else if (nx == 0 || ny == 0 || nx == w - 1 || ny == h - 1) v = std::min(v, d + 1);
                }
            clearance[size_t(index(x, y))] = uint8_t(v);
        }
}

bool MapGrid::areaBuildable(int x0, int y0, int fw, int fh) const {
    if (!inside(x0, y0)) return false;
    const int lv = level[size_t(index(x0, y0))];
    for (int y = y0; y < y0 + fh; ++y)
        for (int x = x0; x < x0 + fw; ++x) {
            if (!inside(x, y)) return false;
            size_t i = size_t(index(x, y));
            if (blocked[i] || !(flags[i] & kCellGround) || (flags[i] & kCellNoBuild)) return false;
            if (hf ? slope[i] > buildLimit : level[i] != lv) return false;
            if (fieldRes[i] >= 0 && fieldAmt[i].raw > 0) return false;
        }
    return true;
}

bool MapGrid::nearestPassable(int& x, int& y, uint8_t layer, int radius) const {
    if (passable(x, y, layer)) return true;
    if (surf) { // rings across face seams
        if (!inside(x, y)) return false;
        const int c0 = index(x, y);
        for (int r = 1; r <= radius; ++r)
            for (int dy = -r; dy <= r; ++dy)
                for (int dx = -r; dx <= r; ++dx) {
                    if (std::abs(dx) != r && std::abs(dy) != r) continue;
                    const int c = surf->offset(c0, dx, dy);
                    if (c >= 0 && passable(c % w, c / w, layer)) { x = c % w; y = c / w; return true; }
                }
        return false;
    }
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
        if (!lineOk(x, y)) return false;
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

bool MapGrid::findPath(Vec3 from, Vec3 to, uint8_t layer, std::vector<Vec3>& out, int maxNodes, int clear, bool useSoft) {
    struct Timed { MapGrid& g; int64_t t0; ~Timed() { if (g.clockNs) g.pathNs += g.clockNs() - t0; } } timed{*this, clockNs ? clockNs() : 0};
    out.clear();
    if (surf) return findPathSphere(from, to, layer, out, maxNodes, clear, useSoft);
    if (layer & 2) { out.push_back(to); return true; } // air: straight line
    int sx = cellX(from.x), sy = cellY(from.z), gx = cellX(to.x), gy = cellY(to.z);
    if (!inside(sx, sy) || w == 0) return false;
    if (!passable(sx, sy, layer)) nearestPassable(sx, sy, layer, 3);
    if (!(layer & 1)) clear = 0;   // clearance is computed for ground cells
    if (clear > 0 && clearanceDirty) updateClearance();
    pathClear_ = clear;
    pathSoft_ = useSoft && soft.size() == flags.size();
    pathEndA_ = index(sx, sy);
    pathEndB_ = inside(gx, gy) ? index(gx, gy) : -1;
    struct Reset { MapGrid* g; ~Reset() { g->pathClear_ = 0; g->pathSoft_ = false; g->pathEndA_ = g->pathEndB_ = -1; } } reset{this};
    bool goalOk = passable(gx, gy, layer);
    if (!goalOk && blockedGoalNearest && inside(gx, gy)) {
        // goal inside a structure / obstacle (attacking a building, walking up to it): aim at the free cell around
        // it nearest to the start instead of letting A* search the whole reachable area for the closest cell.
        // Opt-in (rules / map movement.blockedGoals: "nearest"): the classic search picks other cells.
        int best = -1, bestD = INT32_MAX;
        for (int r = 1; r <= 6 && best < 0; ++r)
            for (int dy = -r; dy <= r; ++dy)
                for (int dx = -r; dx <= r; ++dx) {
                    if (std::abs(dx) != r && std::abs(dy) != r) continue;
                    const int x = gx + dx, y = gy + dy;
                    if (!passable(x, y, layer)) continue;
                    const int ddx = std::abs(x - sx), ddy = std::abs(y - sy);
                    const int d = 10 * (ddx + ddy) - 6 * std::min(ddx, ddy);
                    if (d < bestD) { bestD = d; best = index(x, y); }
                }
        if (best >= 0) {
            const bool ok = findPath(from, cellCenter(best % w, best / w), layer, out, maxNodes, clear, useSoft);
            (void)ok;
            return false;   // as before: the goal itself is not reachable
        }
    }
    if (sx == gx && sy == gy) { out.push_back(to); return true; }
    if (goalOk && lineWalkable(sx, sy, gx, gy, layer)) { out.push_back(to); return true; }

    std::vector<int> cells;
    bool found = false, routed = false;
    // agent-mech BA: long ground trips follow the goal's flow field (one Dijkstra per goal cell for a whole army)
    if (flowFields && goalOk && !pathSoft_ && (layer & 1) && std::max(std::abs(gx - sx), std::abs(gy - sy)) >= flowMinCells)
        routed = found = flowCells(sx, sy, gx, gy, layer, cells);
    // long trips on big maps: HPA* route refined with A* (plain A* would run out of nodes)
    if (!routed && bigMap() && goalOk && std::max(std::abs(gx - sx), std::abs(gy - sy)) > 2 * HpaGraph::kC) {
        routed = found = hpaPath(sx, sy, gx, gy, layer, cells);
        ++hpaStats[routed ? 0 : 1];
    }
    if (!routed) { cells.clear(); found = astarCells(sx, sy, gx, gy, layer, maxNodes, cells); }
    // string pulling: keep only cells needed for line of sight
    std::vector<int> pts;
    size_t i = 0;
    const size_t window = bigMap() ? 48 : cells.size(); // bounded look-ahead keeps long paths O(n)
    while (i + 1 < cells.size()) {
        size_t j = std::min(cells.size() - 1, i + window);
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

bool MapGrid::astarCells(int sx, int sy, int gx, int gy, uint8_t layer, int maxNodes, std::vector<int>& cells) {
    bool found = false;
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
            if (pathClear_ > 0 && clearance[size_t(ni)] < pathClear_) ng += 30u * uint32_t(pathClear_ - clearance[size_t(ni)]); // tight spot for a big body
            if (pathSoft_ && soft[size_t(ni)] && ni != pathEndB_) ng += 25u;                                                // walk around standing allies
            if (stamp_[size_t(ni)] == search_ && gCost_[size_t(ni)] <= ng) continue;
            stamp_[size_t(ni)] = search_; gCost_[size_t(ni)] = ng; parent_[size_t(ni)] = cur.idx;
            uint32_t hh = heur(nx, ny);
            open.push({ng + hh, hh, ni});
        }
    }
    // rebuild cells
    for (int c = best; c != -1; c = parent_[size_t(c)]) cells.push_back(c);
    std::reverse(cells.begin(), cells.end());
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
    if (surf) {
        const size_t n = size_t(surf->bucketCount());
        if (buckets.size() != n) { buckets.assign(n, {}); used.clear(); }
        for (int32_t i : used) buckets[size_t(i)].clear();
        used.clear();
        return;
    }
    cell = cellSize;
    w = (worldW + cell - 1) / cell + 1;
    h = (worldH + cell - 1) / cell + 1;
    if (buckets.size() != size_t(w * h)) { buckets.assign(size_t(w * h), {}); used.clear(); }
    for (int32_t i : used) buckets[size_t(i)].clear();
    used.clear();
}

void SpatialHash::insert(Entity e, Vec3 p) {
    if (surf) {
        const int i = surf->bucketOf(p);
        auto& b = buckets[size_t(i)];
        if (b.empty()) used.push_back(i);
        b.push_back(e);
        return;
    }
    int x = std::clamp(int(p.x.floorInt()) / cell, 0, w - 1), y = std::clamp(int(p.z.floorInt()) / cell, 0, h - 1);
    auto& b = buckets[size_t(y * w + x)];
    if (b.empty()) used.push_back(y * w + x);
    b.push_back(e);
}

} // namespace rts::game
