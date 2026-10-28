#include "AtlasPacker.h"

#include <algorithm>
#include <map>
#include <tuple>

namespace rts::atlas {

// ------------------------------------------------------------------ MaxRects (BSSF)
MaxRects::MaxRects(int width, int height) : w_(width), h_(height) { free_.push_back({0, 0, width, height}); }

bool MaxRects::insert(int w, int h, bool allowRotate, PackRect& out, bool& rotated) {
    // Score: (short side leftover, long side leftover, y, x) — lexicographic, fully deterministic.
    std::tuple<int, int, int, int> best{1 << 30, 1 << 30, 1 << 30, 1 << 30};
    bool found = false;
    for (const PackRect& f : free_) {
        for (int rot = 0; rot < (allowRotate && w != h ? 2 : 1); ++rot) {
            int rw = rot ? h : w, rh = rot ? w : h;
            if (rw > f.w || rh > f.h) continue;
            int dw = f.w - rw, dh = f.h - rh;
            std::tuple<int, int, int, int> score{std::min(dw, dh), std::max(dw, dh), f.y, f.x};
            if (score < best) { best = score; out = {f.x, f.y, rw, rh}; rotated = rot != 0; found = true; }
        }
    }
    if (found) place(out);
    return found;
}

void MaxRects::place(const PackRect& r) {
    // Free rects are kept maximal (none contains another). Split pieces are subsets of the rect
    // they came from, so only new pieces can be redundant: prune them against everything else.
    std::vector<PackRect> keep, fresh;
    keep.reserve(free_.size() + 4);
    for (const PackRect& f : free_) {
        if (r.x >= f.x + f.w || r.x + r.w <= f.x || r.y >= f.y + f.h || r.y + r.h <= f.y) { keep.push_back(f); continue; }
        if (r.x > f.x) fresh.push_back({f.x, f.y, r.x - f.x, f.h});
        if (r.x + r.w < f.x + f.w) fresh.push_back({r.x + r.w, f.y, f.x + f.w - (r.x + r.w), f.h});
        if (r.y > f.y) fresh.push_back({f.x, f.y, f.w, r.y - f.y});
        if (r.y + r.h < f.y + f.h) fresh.push_back({f.x, r.y + r.h, f.w, f.y + f.h - (r.y + r.h)});
    }
    auto inside = [](const PackRect& a, const PackRect& b) { return a.x >= b.x && a.y >= b.y && a.x + a.w <= b.x + b.w && a.y + a.h <= b.y + b.h; };
    size_t untouched = keep.size();
    for (size_t i = 0; i < fresh.size(); ++i) {
        bool redundant = false;
        for (size_t k = 0; k < untouched && !redundant; ++k) redundant = inside(fresh[i], keep[k]);
        // among new pieces, drop i if contained in j (equal pieces: keep the first one)
        for (size_t j = 0; j < fresh.size() && !redundant; ++j)
            if (j != i && inside(fresh[i], fresh[j]) && (!inside(fresh[j], fresh[i]) || j < i)) redundant = true;
        if (!redundant) keep.push_back(fresh[i]);
    }
    free_ = std::move(keep);
}

// ------------------------------------------------------------------ pages
int footprint(int size, const PackSettings& s) {
    int f = size + 2 * s.extrude + s.padding;
    int a = std::max(1, s.align);
    return (f + a - 1) / a * a;
}

namespace {

struct Ctx {
    const std::vector<PackItem>& items;
    const PackSettings& s;
};

bool fits(const Ctx& c, size_t i, int pageW, int pageH) {
    int fw = footprint(c.items[i].w, c.s), fh = footprint(c.items[i].h, c.s);
    int bw = pageW + c.s.padding, bh = pageH + c.s.padding;
    return (fw <= bw && fh <= bh) || (c.items[i].allowRotate && fh <= bw && fw <= bh);
}

// Offline order: big first, then by area, then by path.
void sortForPacking(const Ctx& c, std::vector<size_t>& idx) {
    std::stable_sort(idx.begin(), idx.end(), [&](size_t a, size_t b) {
        const PackItem &A = c.items[a], &B = c.items[b];
        int ma = std::max(A.w, A.h), mb = std::max(B.w, B.h);
        if (ma != mb) return ma > mb;
        long long aa = 1LL * A.w * A.h, ab = 1LL * B.w * B.h;
        if (aa != ab) return aa > ab;
        return A.key < B.key;
    });
}

bool insertInto(const Ctx& c, MaxRects& mr, size_t i, Placement& p) {
    PackRect r;
    bool rot = false;
    if (!mr.insert(footprint(c.items[i].w, c.s), footprint(c.items[i].h, c.s), c.items[i].allowRotate, r, rot)) return false;
    p.x = r.x + c.s.extrude;
    p.y = r.y + c.s.extrude;
    p.rotated = rot;
    return true;
}

bool tryPack(const Ctx& c, std::vector<size_t> idx, int w, int h, std::vector<Placement>& out) {
    sortForPacking(c, idx);
    MaxRects mr(w + c.s.padding, h + c.s.padding);
    out.assign(c.items.size(), Placement{});
    for (size_t i : idx) if (!insertInto(c, mr, i, out[i])) return false;
    return true;
}

std::vector<int> pageSizes(const PackSettings& s) {
    std::vector<int> v;
    for (int x = std::max(4, s.minPage); x < s.maxPage; x *= 2) v.push_back(x);
    v.push_back(s.maxPage);
    return v;
}

// Smallest page that holds `idx`; returns false if even maxPage x maxPage fails.
bool shrink(const Ctx& c, const std::vector<size_t>& idx, int& outW, int& outH, std::vector<Placement>& out) {
    long long area = 0;
    int minW = 1, minH = 1;
    for (size_t i : idx) {
        area += 1LL * footprint(c.items[i].w, c.s) * footprint(c.items[i].h, c.s);
        int a = footprint(c.items[i].w, c.s) - c.s.padding, b = footprint(c.items[i].h, c.s) - c.s.padding;
        if (!c.items[i].allowRotate) { minW = std::max(minW, a); minH = std::max(minH, b); }
        else { minW = std::max(minW, std::min(a, b)); minH = std::max(minH, std::min(a, b)); }
    }
    std::vector<std::pair<int, int>> cand;
    auto sizes = pageSizes(c.s);
    for (int w : sizes)
        for (int h : sizes)
            if (w >= minW && h >= minH && 1LL * (w + c.s.padding) * (h + c.s.padding) >= area) cand.push_back({w, h});
    std::stable_sort(cand.begin(), cand.end(), [](auto a, auto b) {
        long long A = 1LL * a.first * a.second, B = 1LL * b.first * b.second;
        if (A != B) return A < B;
        int ma = std::max(a.first, a.second), mb = std::max(b.first, b.second);
        if (ma != mb) return ma < mb;
        return a.first > b.first; // wide before tall
    });
    bool found = false;
    for (auto [w, h] : cand)
        if (tryPack(c, idx, w, h, out)) { outW = w; outH = h; found = true; break; }
    if (!found) return false;
    if (!c.s.pow2) {
        // Multiples of 4: trim height, then width, below the power of two that fits.
        std::vector<Placement> tmp;
        auto reduce = [&](int& dim, int fixedOther, bool isH, int minDim) {
            int lo = std::max(4, (minDim + 3) / 4 * 4), hi = dim;
            while (lo < hi) {
                int mid = (lo + hi) / 2 / 4 * 4;
                if (mid < lo) mid = lo;
                bool ok = isH ? tryPack(c, idx, fixedOther, mid, tmp) : tryPack(c, idx, mid, fixedOther, tmp);
                if (ok) { hi = mid; } else { lo = mid + 4; }
            }
            dim = hi;
        };
        reduce(outH, outW, true, minH);
        reduce(outW, outH, false, minW);
        if (!tryPack(c, idx, outW, outH, out)) return false; // cannot happen: last size was verified
    }
    return true;
}

} // namespace

PackedPages packRects(const std::vector<PackItem>& items, const PackSettings& s) {
    PackedPages res;
    Ctx c{items, s};
    res.placements.assign(items.size(), Placement{});
    for (size_t i = 0; i < items.size(); ++i)
        if (items[i].w <= 0 || items[i].h <= 0 || !fits(c, i, s.maxPage, s.maxPage)) {
            res.error = "'" + items[i].key + "' does not fit into a " + std::to_string(s.maxPage) + " page";
            return res;
        }
    if (items.empty()) { res.ok = true; return res; }

    std::vector<size_t> all(items.size());
    for (size_t i = 0; i < all.size(); ++i) all[i] = i;

    std::vector<std::vector<size_t>> groups;
    std::vector<Placement> tmp;
    if (tryPack(c, all, s.maxPage, s.maxPage, tmp)) {
        groups.push_back(all);
    } else {
        // Several pages: whole clusters first-fit into pages (online), so things drawn together
        // share a page; a cluster larger than a page is spread item by item.
        std::map<std::string, std::vector<size_t>> clusters;
        for (size_t i = 0; i < items.size(); ++i) clusters[items[i].cluster].push_back(i);
        std::vector<MaxRects> bins;
        auto fresh = [&]() { bins.emplace_back(s.maxPage + s.padding, s.maxPage + s.padding); groups.emplace_back(); };
        for (auto& [name, members] : clusters) {
            std::vector<size_t> m = members;
            sortForPacking(c, m);
            bool placed = false;
            for (size_t b = 0; b <= bins.size() && !placed; ++b) {
                if (b == bins.size()) fresh();
                MaxRects trial = bins[b];
                Placement p;
                bool ok = true;
                for (size_t i : m) if (!insertInto(c, trial, i, p)) { ok = false; break; }
                if (ok) { bins[b] = trial; groups[b].insert(groups[b].end(), m.begin(), m.end()); placed = true; }
                else if (b + 1 == bins.size() && groups[b].empty()) break; // cluster bigger than a page
            }
            if (placed) continue;
            if (!groups.empty() && groups.back().empty()) { bins.pop_back(); groups.pop_back(); }
            for (size_t i : m) {
                Placement p;
                bool ok = false;
                for (size_t b = 0; b < bins.size() && !ok; ++b)
                    if (insertInto(c, bins[b], i, p)) { groups[b].push_back(i); ok = true; }
                if (!ok) { fresh(); insertInto(c, bins.back(), i, p); groups.back().push_back(i); }
            }
        }
    }

    for (auto& g : groups) {
        int w = s.maxPage, h = s.maxPage;
        std::vector<Placement> pl;
        if (!shrink(c, g, w, h, pl)) {
            // Offline re-pack lost to the online layout: redo the online order at full size.
            MaxRects mr(s.maxPage + s.padding, s.maxPage + s.padding);
            pl.assign(items.size(), Placement{});
            for (size_t i : g)
                if (!insertInto(c, mr, i, pl[i])) { res.error = "internal: page re-pack failed"; return res; }
            w = h = s.maxPage;
        }
        int page = int(res.pages.size());
        res.pages.push_back({0, 0, w, h});
        for (size_t i : g) { res.placements[i] = pl[i]; res.placements[i].page = page; }
    }
    res.ok = true;
    return res;
}

} // namespace rts::atlas
