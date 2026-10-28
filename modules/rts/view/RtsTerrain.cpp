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
    if (planets_.active()) { // planet maps: the flat sim grid lies on face 2 of the start body (RtsPlanetView.cpp)
        demoW_ = g.w; demoH_ = g.h;
        if (const PlanetSurface* s = planets_.surface(demoBody_)) {
            const float N = float(s->faceSize());
            demoScale_ = std::min(1.0f, N * 0.9f / float(std::max(std::max(g.w, g.h), 1)));
            demoOff_[0] = (N - float(g.w) * demoScale_) * 0.5f; demoOff_[1] = (N - float(g.h) * demoScale_) * 0.5f;
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
