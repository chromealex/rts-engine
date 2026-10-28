// Crowd helpers (flat maps): a per-tick sorted grid of agent positions for neighbour queries. Queries return a
// superset of the points inside the window, in a fixed order (cell key, then insertion index), so callers that
// filter by exact distance and either sort or sum exactly (Fixed addition is exact) get bit-identical results to
// the spatial hash, at a fraction of the cost in dense crowds (no per-candidate component lookups).
#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>
#include "../../engine/core/Fixed.h"

namespace rts::game {

struct CrowdGrid {
    int w = 1, h = 1;
    std::vector<std::pair<int64_t, int32_t>> items;   // (cell key, payload), sorted
    void build(int width, int height, size_t n, const auto& posOf) {   // posOf(i) -> Vec2 (x, z)
        w = std::max(1, width); h = std::max(1, height);
        items.clear();
        items.reserve(n);
        for (size_t i = 0; i < n; ++i) { const Vec2 p = posOf(i); items.push_back({key(cx(p.x), cy(p.y)), int32_t(i)}); }
        std::sort(items.begin(), items.end());
    }
    int cx(Fixed x) const { return std::clamp(int(x.floorInt()), 0, w - 1); }
    int cy(Fixed z) const { return std::clamp(int(z.floorInt()), 0, h - 1); }
    int64_t key(int x, int y) const { return int64_t(y) * w + x; }
    // every payload whose cell lies in the square [p - r, p + r] (clamped to the map)
    template <class Fn> void query(Vec2 p, Fixed r, Fn&& fn) const {
        const int x0 = cx(p.x - r), x1 = cx(p.x + r), y0 = cy(p.y - r), y1 = cy(p.y + r);
        for (int y = y0; y <= y1; ++y) {
            auto it = std::lower_bound(items.begin(), items.end(), std::pair<int64_t, int32_t>{key(x0, y), INT32_MIN});
            const int64_t end = key(x1, y);
            for (; it != items.end() && it->first <= end; ++it) fn(it->second);
        }
    }
};

} // namespace rts::game
