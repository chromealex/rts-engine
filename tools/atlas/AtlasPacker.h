// Rectangle packing for texture atlases (host tools only). Pure geometry: no images, no I/O.
//
// MaxRects with the best-short-side-fit rule. Every item reserves a footprint of
//   (w + 2*extrude + padding) x (h + 2*extrude + padding), rounded up to `align`,
// and its content starts at (x + extrude, y + extrude). Bins are (page + padding) wide so the
// trailing padding of items touching the right/bottom page edge costs nothing.
//
// packRects() splits items into pages so that items sharing a `cluster` (e.g. all frames of one
// unit, one faction's folder) stay on the same page whenever the cluster fits on one page, then
// shrinks every page to the smallest size (power of two or multiple of 4) that still holds it.
// Results depend only on the input (stable sorts, explicit tie-breaks): reproducible builds.
#pragma once
#include <string>
#include <vector>

namespace rts::atlas {

struct PackRect { int x = 0, y = 0, w = 0, h = 0; };

class MaxRects {
public:
    MaxRects(int width, int height);
    // Finds a place for a w x h rect (or h x w when rotation is allowed and fits better).
    bool insert(int w, int h, bool allowRotate, PackRect& out, bool& rotated);
    int width() const { return w_; }
    int height() const { return h_; }

private:
    void place(const PackRect& r);
    int w_, h_;
    std::vector<PackRect> free_;
};

struct PackSettings {
    int maxPage = 4096;     // page width/height limit
    int padding = 2;        // empty texels between neighbouring (extruded) items
    int extrude = 1;        // edge texels repeated around each item (filtering seams)
    int align = 1;          // item origins/footprints aligned to this (4 = block/mip friendly)
    bool pow2 = true;       // page sizes: powers of two, else multiples of 4
    int minPage = 16;
};

struct PackItem {
    int w = 0, h = 0;               // content size
    bool allowRotate = false;
    std::string cluster;            // items drawn together (same folder)
    std::string key;                // stable tie-break (path)
};

struct Placement {
    int page = -1;
    int x = 0, y = 0;               // content origin in the page (after extrusion)
    bool rotated = false;           // content stored turned 90 degrees clockwise
};

struct PackedPages {
    std::vector<PackRect> pages;    // only w/h used
    std::vector<Placement> placements; // parallel to the input items
    std::string error;
    bool ok = false;
};

int footprint(int size, const PackSettings& s);
PackedPages packRects(const std::vector<PackItem>& items, const PackSettings& s);

} // namespace rts::atlas
