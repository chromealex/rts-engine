// Visual detail surface of the terrain: a heightmap with `k` samples per cell ((w*k+1) x (h*k+1)),
// decoupled from the simulation grid. Presentation only (float): the simulation never reads it.
//
// Sources, in order:
//   1. map terrain.detail = {"scale": k, "file": "maps/<name>.detail.r16"}: int16 LE, absolute height in
//      1/16 world units, row-major (sample (i, j) at world (i / k, j / k));
//   2. synthesised at load when the visual block says terrain.style = "smooth" (deterministic, cached):
//      levels maps get bevelled cliffs with organic (warped) outlines, rounded plateau corners and smooth
//      ramps; heightfield maps get bicubic (Catmull-Rom) interpolation of the corners; both get fine noise
//      per cell kind.
// The renderer draws it (RtsTerrainRender: height texture + CDLOD patches) and RtsView::heightAt samples
// it, so units and overlays stand on what is drawn. The editor core uses the same synthesis to create a
// paintable detail layer and to compare it with the simulation surface (map_validate).
// Library rts_terrain_detail (linked by the view and the editor core).
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "../../../engine/data/Json.h"
#include "../RtsMap.h"

namespace rts::game::terrain {

struct DetailParams {
    int scale = 4;            // samples per cell wanted (1, 2, 4, 8); capped by maxSamples
    float bevel = 0.35f;      // levels: half width of the cliff slope (cells)
    float wobble = 0.3f;      // levels: outline warp amplitude (cells)
    float noise = 0.04f;      // fine noise amplitude (world units), scaled per cell kind (rock more, sand less)
    uint32_t seed = 1;
};

struct DetailSurface {
    int w = 0, h = 0, k = 0;  // cells, samples per cell
    std::vector<float> hgt;   // sw() * sh(), world units
    bool valid() const { return k > 0 && !hgt.empty(); }
    int sw() const { return w * k + 1; }
    int sh() const { return h * k + 1; }
    float at(int i, int j) const;                 // clamped sample
    float sample(float x, float z) const;         // bilinear, world (cell) coordinates, clamped to the map
    void init(int width, int height, int scale);  // flat 0
};

// Largest power-of-two scale <= want whose sample grid fits maxSamples (>= 1).
int detailScaleFor(int w, int h, int want, size_t maxSamples);
// Synthesis of the samples [i0, i1) x [j0, j1) (all when i1 <= i0). `out` must be init()ed.
// flatDig: diggable cells stay flat (the ground overlays draw the rock blocks).
void synthLevels(const MapGrid& g, float levelHeight, bool flatDig, const DetailParams& p, DetailSurface& out,
                 int i0 = 0, int j0 = 0, int i1 = 0, int j1 = 0);
void synthCorners(const MapGrid& g, const DetailParams& p, DetailSurface& out, int i0 = 0, int j0 = 0, int i1 = 0, int j1 = 0);
// Margin (samples) a cell change spreads over in synthLevels (warp + bevel).
int synthMargin(const DetailParams& p, int k);
// File codec: int16 LE, 1/16 units.
bool decodeDetail(const std::string& bytes, int w, int h, int k, DetailSurface& out, std::string* err);
std::string encodeDetail(const DetailSurface& d);
// Resample to another scale (bilinear); used when a quality tier draws fewer samples than the file has.
DetailSurface resampleDetail(const DetailSurface& d, int k);

// Map key terrain.detail = {"scale": k, "file": "maps/x.detail.r16", "splat": "maps/x.splat.raw"}: reads the
// files with `read` (paths relative to the game folder). splat (optional): RGBA8 weights of layers 0..3
// per detail sample. Returns false without a detail block or on a broken file (err set then).
bool loadMapDetail(const Json& map, const ContentReader& read, DetailSurface& out, std::vector<uint8_t>& splat, std::string* err);
// int16 (1/16 units) <-> surface
void detailFromInt16(int w, int h, int k, const std::vector<int16_t>& v, DetailSurface& out);
std::vector<int16_t> detailToInt16(const DetailSurface& d);

} // namespace rts::game::terrain
