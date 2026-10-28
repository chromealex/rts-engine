// RtsView terrain entry points. The surface math lives in RtsTerrainSurface.h (shared with the
// editor core), the chunked mesh / materials / water in RtsTerrainRender.cpp.
#include "../RtsModule.h"
#include "RtsTerrainSurface.h"
#include "RtsView.h"
#include <algorithm>

namespace rts::game {

using namespace render;

// The single height query of the view: models, sprites, overlays, lights, editor gizmos. Matches the
// mesh: heightfield corners (two triangles per cell) when the map has them, otherwise levels + fine
// heights + ramps.
float RtsView::heightAt(const MapGrid& g, float x, float z) const {
    if (terrain_.detailActive()) return terrain_.surfaceAt(g, x, z);   // the sub-cell detail surface that is drawn
    return terrain::heightAt(g, rules_ ? float(rules_->levelHeight.toDouble()) : 1.5f, x, z);
}

void RtsView::buildTerrain(gfx::Device& dev, const MapGrid& g) {
    if (planets_.active()) { // planet maps: the grid holds the stacked faces of every body (RtsSurface.h)
        const PlanetWorld& W = planets_.world();
        if (W.sim && g.surf == W.sim) // cell flags changed (editor play / sim): recolour those bodies
            for (size_t i = 0; i < W.bodies.size(); ++i) {
                if (!W.bodies[i].surface()) continue;
                const int N = W.bodies[i].faceSize;
                std::vector<uint8_t> f(size_t(6 * N * N));
                for (int fc = 0; fc < 6; ++fc)
                    for (int y = 0; y < N; ++y)
                        for (int x = 0; x < N; ++x) f[(size_t(fc) * size_t(N) + size_t(y)) * size_t(N) + size_t(x)] = g.flags[size_t(W.sim->gridCell(int(i), fc, x, y))];
                planets_.updateFlags(int(i), f);
            }
        fog_.build(dev, g.w, g.h, [](float, float) { return 0.0f; });   // field only (the sheet is not drawn)
        hasTerrain_ = true;
        return;
    }
    const bool flatDig = groundFx_.drawsDig() && g.dig.size() == g.flags.size(); // RtsGroundFx draws the rock blocks
    // ground overlays follow the surface: rebuild them whenever terrain geometry changed
    if (terrain_.build(dev, g, flatDig) || !hasTerrain_) groundFx_.invalidate();
    hasTerrain_ = true;
    fogMeshDirty_ = true;   // the fog sheet follows the ground
    if (twoD_) twoD_->buildTiles(dev, g, rules_ ? float(rules_->levelHeight.toDouble()) : 1.5f);
}

} // namespace rts::game
