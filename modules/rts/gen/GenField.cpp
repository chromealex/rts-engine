#include "GenField.h"
#include "GenNoise.h"
#include "engine/core/FixedMath.h"
#include <algorithm>
#include <queue>

namespace rts::gen {

// ---------------------------------------------------------------- symmetry

bool Sym::make(const std::string& name, int players, int w, int h, Sym& s, std::string* err) {
    s = Sym{}; s.w = w; s.h = h; s.n = std::max(1, players);
    std::string k = name.empty() ? "auto" : name;
    if (k == "auto") k = s.n == 1 ? "none" : s.n == 2 ? "point" : s.n == 4 ? (w == h ? "rot4" : "mirror4") : "rotational";
    if (k == "none") { s.kind = None; return true; }
    auto need = [&](int n) {
        if (s.n == n) return true;
        if (err) *err = "generate: symmetry '" + k + "' needs " + std::to_string(n) + " players (got " + std::to_string(s.n) + ")";
        return false;
    };
    if (k == "point") { s.kind = Point; return need(2); }
    if (k == "mirror" || k == "mirror-x") { s.kind = MirrorX; return need(2); }
    if (k == "mirror-y") { s.kind = MirrorY; return need(2); }
    if (k == "mirror4") { s.kind = Mirror4; return need(4); }
    if (k == "rot4") {
        s.kind = Rot4;
        if (w != h) { if (err) *err = "generate: symmetry 'rot4' needs a square map"; return false; }
        return need(4);
    }
    if (k == "rotational") {
        if (s.n < 2 || s.n > 8) { if (err) *err = "generate: rotational symmetry needs 2..8 players"; return false; }
        s.kind = RotN; return true;
    }
    if (err) *err = "generate: unknown symmetry '" + name + "' (auto, none, point, mirror, mirror-y, mirror4, rot4, rotational)";
    return false;
}

const char* Sym::name() const {
    switch (kind) {
        case Point: return "point"; case MirrorX: return "mirror"; case MirrorY: return "mirror-y";
        case Mirror4: return "mirror4"; case Rot4: return "rot4"; case RotN: return "rotational"; default: return "none";
    }
}

namespace {
inline void rotTurns(int k, int n, int64_t dx, int64_t dy, int64_t& ox, int64_t& oy) {
    const Fixed a = Fixed::fromRatio(k, n);
    const int64_t c = fxCosTurns(a).raw, s = fxSinTurns(a).raw;
    ox = (dx * c - dy * s) >> 16;
    oy = (dx * s + dy * c) >> 16;
}
} // namespace

void Sym::image(int k, int64_t x, int64_t y, int64_t& ox, int64_t& oy) const {
    // centre doubled to stay exact for odd sizes: dx2 = 2x - w
    const int64_t cx = int64_t(w) << 15, cy = int64_t(h) << 15;
    int64_t dx = x - cx, dy = y - cy, rx = dx, ry = dy;
    switch (kind) {
        case Point: if (k == 1) { rx = -dx; ry = -dy; } break;
        case MirrorX: if (k == 1) rx = -dx; break;
        case MirrorY: if (k == 1) ry = -dy; break;
        case Mirror4: if (k == 1 || k == 2) rx = -dx; if (k == 2 || k == 3) ry = -dy; break;
        case Rot4: for (int i = 0; i < (k & 3); ++i) { int64_t t = rx; rx = -ry; ry = t; } break;
        case RotN: if (k) rotTurns(k, n, dx, dy, rx, ry); break;
        default: break;
    }
    ox = cx + rx; oy = cy + ry;
}

int Sym::fold(int64_t x, int64_t y, int64_t& ox, int64_t& oy) const {
    const int64_t cx = int64_t(w) << 15, cy = int64_t(h) << 15;
    const int64_t dx = x - cx, dy = y - cy;
    int k = 0;
    switch (kind) {
        case Point: k = (dy > 0 || (dy == 0 && dx > 0)) ? 1 : 0; break;
        case MirrorX: k = dx > 0 ? 1 : 0; break;
        case MirrorY: k = dy > 0 ? 1 : 0; break;
        case Mirror4: k = dx <= 0 ? (dy <= 0 ? 0 : 3) : (dy <= 0 ? 1 : 2); break;
        case Rot4:     // domain: dx < 0, dy <= 0 (top-left quadrant incl. its upper edge); k-th image = rotated k times
            if (dx == 0 && dy == 0) k = 0;
            else if (dx < 0 && dy <= 0) k = 0;
            else if (dx >= 0 && dy < 0) k = 1;
            else if (dx > 0 && dy >= 0) k = 2;
            else k = 3;
            break;
        case RotN: {
            if (dx == 0 && dy == 0) break;
            int64_t a = fxAtan2Turns(Fixed::fromRaw(dy), Fixed::fromRaw(dx)).raw;   // (-0.5, 0.5]
            a += kOne / 2;                                                         // domain starts at the -x axis
            if (a >= kOne) a -= kOne;
            if (a < 0) a += kOne;
            k = int(a * n / kOne);
            if (k >= n) k = n - 1;
            break;
        }
        default: break;
    }
    if (k == 0) { ox = x; oy = y; return 0; }
    // inverse image: apply the inverse transform
    int64_t rx = dx, ry = dy;
    switch (kind) {
        case Point: rx = -dx; ry = -dy; break;
        case MirrorX: rx = -dx; break;
        case MirrorY: ry = -dy; break;
        case Mirror4: if (k == 1 || k == 2) rx = -dx; if (k == 2 || k == 3) ry = -dy; break;
        case Rot4: for (int i = 0; i < 4 - k; ++i) { int64_t t = rx; rx = -ry; ry = t; } break;
        case RotN: rotTurns(n - k, n, dx, dy, rx, ry); break;
        default: break;
    }
    ox = cx + rx; oy = cy + ry;
    return k;
}

void Sym::imageCell(int k, int x, int y, int& ox, int& oy) const {
    int64_t px, py;
    image(k, (int64_t(x) << 16) + 32768, (int64_t(y) << 16) + 32768, px, py);
    ox = std::clamp(int(px >> 16), 0, w - 1);
    oy = std::clamp(int(py >> 16), 0, h - 1);
}

int Sym::foldCell(int x, int y, int& ox, int& oy) const {
    int64_t px, py;
    int k = fold((int64_t(x) << 16) + 32768, (int64_t(y) << 16) + 32768, px, py);
    ox = std::clamp(int(px >> 16), 0, w - 1);
    oy = std::clamp(int(py >> 16), 0, h - 1);
    return k;
}

void Sym::foldCorner(int x, int y, int& ox, int& oy) const {
    int64_t px, py;
    fold(int64_t(x) << 16, int64_t(y) << 16, px, py);
    ox = std::clamp(int((px + 32768) >> 16), 0, w);
    oy = std::clamp(int((py + 32768) >> 16), 0, h);
}

// ---------------------------------------------------------------- grids

int32_t Grid::cellMax(int x, int y) const { return std::max(std::max(at(x, y), at(x + 1, y)), std::max(at(x, y + 1), at(x + 1, y + 1))); }
int32_t Grid::cellMin(int x, int y) const { return std::min(std::min(at(x, y), at(x + 1, y)), std::min(at(x, y + 1), at(x + 1, y + 1))); }

void thermalErosion(Grid& g, int iterations, int32_t talus, int32_t rate) {
    const int W = g.w + 1, H = g.h + 1;
    std::vector<int32_t> d(g.v.size());
    for (int it = 0; it < iterations; ++it) {
        std::fill(d.begin(), d.end(), 0);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const int i = y * W + x;
                for (auto [dx, dy] : {std::pair{1, 0}, std::pair{0, 1}}) {
                    const int nx = x + dx, ny = y + dy;
                    if (nx >= W || ny >= H) continue;
                    const int j = ny * W + nx;
                    const int32_t diff = g.v[size_t(i)] - g.v[size_t(j)];
                    const int32_t ad = diff < 0 ? -diff : diff;
                    if (ad <= talus) continue;
                    const int32_t m = int32_t((int64_t(ad - talus) * rate) >> 18);   // /4 neighbours
                    if (diff > 0) { d[size_t(i)] -= m; d[size_t(j)] += m; } else { d[size_t(i)] += m; d[size_t(j)] -= m; }
                }
            }
        for (size_t i = 0; i < d.size(); ++i) g.v[i] += d[i];
    }
}

void smoothGrid(Grid& g, int passes, int32_t amount) {
    const int W = g.w + 1, H = g.h + 1;
    std::vector<int32_t> src;
    for (int p = 0; p < passes; ++p) {
        src = g.v;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                int64_t s = 0; int c = 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = x + dx, ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= W || ny >= H) continue;
                        s += src[size_t(ny * W + nx)]; ++c;
                    }
                const int64_t avg = s / c, cur = src[size_t(y * W + x)];
                g.v[size_t(y * W + x)] = int32_t(cur + (((avg - cur) * amount) >> 16));
            }
    }
}

void drainage(const Grid& g, int32_t sea, Drainage& d) {
    const int W = g.w + 1, H = g.h + 1, N = W * H;
    d.filled.assign(size_t(N), 0); d.receiver.assign(size_t(N), -1); d.order.clear(); d.order.reserve(size_t(N));
    d.flux.assign(size_t(N), 1);
    std::vector<uint8_t> done(static_cast<size_t>(N), 0);
    using Item = std::pair<int64_t, int32_t>;          // (height, index): total order -> deterministic
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> pq;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const int i = y * W + x;
            if (x == 0 || y == 0 || x == W - 1 || y == H - 1 || g.v[size_t(i)] <= sea) {
                done[size_t(i)] = 1; d.filled[size_t(i)] = g.v[size_t(i)];
                pq.push({g.v[size_t(i)], i});
            }
        }
    while (!pq.empty()) {
        auto [hh, i] = pq.top(); pq.pop();
        d.order.push_back(i);
        const int x = i % W, y = i / W;
        for (auto [dx, dy] : {std::pair{1, 0}, std::pair{-1, 0}, std::pair{0, 1}, std::pair{0, -1}}) {
            const int nx = x + dx, ny = y + dy;
            if (nx < 0 || ny < 0 || nx >= W || ny >= H) continue;
            const int j = ny * W + nx;
            if (done[size_t(j)]) continue;
            done[size_t(j)] = 1;
            d.filled[size_t(j)] = std::max<int32_t>(g.v[size_t(j)], int32_t(hh) + 1);
            d.receiver[size_t(j)] = i;
            pq.push({d.filled[size_t(j)], j});
        }
    }
    for (size_t k = d.order.size(); k-- > 0;) {
        const int i = d.order[k];
        if (d.receiver[size_t(i)] >= 0) d.flux[size_t(d.receiver[size_t(i)])] += d.flux[size_t(i)];
    }
}

void hydraulicErosion(Grid& g, int32_t sea, int iterations, int32_t kQ16) {
    Drainage d;
    for (int it = 0; it < iterations; ++it) {
        drainage(g, sea, d);
        for (int i : d.order) {                         // ascending: receivers are final before their donors
            const int r = d.receiver[size_t(i)];
            if (r < 0 || g.v[size_t(i)] <= sea) continue;
            const int64_t slope = int64_t(g.v[size_t(i)]) - g.v[size_t(r)];
            if (slope <= 0) continue;
            const int64_t e = ((isqrt(int64_t(d.flux[size_t(i)]) << 16) * slope >> 8) * kQ16) >> 24;
            g.v[size_t(i)] -= int32_t(std::min<int64_t>(e, slope * 3 / 4));
        }
    }
}

void symmetrizeCorners(Grid& g, const Sym& s) {
    if (s.kind == Sym::None) return;
    std::vector<int32_t> src = g.v;
    for (int y = 0; y <= g.h; ++y)
        for (int x = 0; x <= g.w; ++x) {
            int fx, fy;
            s.foldCorner(x, y, fx, fy);
            g.at(x, y) = src[size_t(g.idx(fx, fy))];
        }
}

int32_t quantile(std::vector<int32_t> vals, int32_t q) {
    if (vals.empty()) return 0;
    size_t k = size_t((int64_t(vals.size() - 1) * std::clamp(q, 0, kOne)) >> 16);
    std::nth_element(vals.begin(), vals.begin() + std::ptrdiff_t(k), vals.end());
    return vals[k];
}

} // namespace rts::gen
