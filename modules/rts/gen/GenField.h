// Terrain generator building blocks: symmetry of fair layouts, corner height grids and integer erosion /
// drainage (priority flood, flow accumulation, rivers, lakes). Integer only (see GenNoise.h).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace rts::gen {

// Symmetry of a fair flat map: n images of the fundamental domain (player k+1 lives in image k).
// Coordinates are Q16 cell units (corner (x, y) = (x << 16, y << 16)); the centre is (w/2, h/2).
struct Sym {
    enum Kind : uint8_t { None, Point, MirrorX, MirrorY, Mirror4, Rot4, RotN };
    Kind kind = None;
    int n = 1, w = 0, h = 0;
    // "auto" | "none" | "point" | "mirror" (left/right) | "mirror-x" | "mirror-y" | "mirror4" | "rot4" | "rotational"
    static bool make(const std::string& name, int players, int w, int h, Sym& out, std::string* err);
    const char* name() const;
    bool exact() const { return kind != RotN; }   // images of cells are cells (rotations by 1/n turn are not)
    void image(int k, int64_t x, int64_t y, int64_t& ox, int64_t& oy) const;    // domain point -> k-th image
    int fold(int64_t x, int64_t y, int64_t& ox, int64_t& oy) const;            // any point -> domain, returns k
    bool inDomain(int64_t x, int64_t y) const { int64_t a, b; return fold(x, y, a, b) == 0; }
    // cell helpers (cell centre), clamped to the map
    void imageCell(int k, int x, int y, int& ox, int& oy) const;
    int foldCell(int x, int y, int& ox, int& oy) const;
    void foldCorner(int x, int y, int& ox, int& oy) const;
};

struct Grid {          // corner heights (w+1) x (h+1), Q12 world units (4096 = 1 world unit)
    int w = 0, h = 0;
    std::vector<int32_t> v;
    void init(int w_, int h_) { w = w_; h = h_; v.assign(size_t((w + 1) * (h + 1)), 0); }
    int idx(int x, int y) const { return y * (w + 1) + x; }
    int32_t& at(int x, int y) { return v[size_t(idx(x, y))]; }
    int32_t at(int x, int y) const { return v[size_t(idx(x, y))]; }
    int32_t cell(int x, int y) const { return int32_t((int64_t(at(x, y)) + at(x + 1, y) + at(x, y + 1) + at(x + 1, y + 1)) / 4); }
    int32_t cellMax(int x, int y) const;
    int32_t cellMin(int x, int y) const;
};
constexpr int kHU = 4096;       // height unit: 1 world unit in Grid values

void thermalErosion(Grid& g, int iterations, int32_t talus, int32_t rateQ16);
void smoothGrid(Grid& g, int passes, int32_t amountQ16);
// Priority flood from the map edges and every corner at or below `sea`: filled surface without pits
// (+1 per step, so every land corner drains), the flow receiver of every corner (-1: outlet), the flood
// order (ascending), and flow accumulation (corners drained through each corner, incl. itself).
struct Drainage {
    std::vector<int32_t> filled, receiver, order, flux;
};
void drainage(const Grid& g, int32_t sea, Drainage& d);
// Stream-power erosion: h -= k * sqrt(flux) * slope, never below the receiver; `iterations` passes.
void hydraulicErosion(Grid& g, int32_t sea, int iterations, int32_t kQ16);
// Copies the fundamental domain onto every image (corners).
void symmetrizeCorners(Grid& g, const Sym& s);
template <class T> void symmetrizeCells(std::vector<T>& cells, int w, int h, const Sym& s) {
    if (s.kind == Sym::None) return;
    std::vector<T> src = cells;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            int fx, fy;
            s.foldCell(x, y, fx, fy);
            cells[size_t(y * w + x)] = src[size_t(fy * w + fx)];
        }
}
// Height at quantile q (Q16, 0..65536) of the values (copy + nth_element; deterministic).
int32_t quantile(std::vector<int32_t> vals, int32_t qQ16);

} // namespace rts::gen
