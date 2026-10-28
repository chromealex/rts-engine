// RtsView terrain entry points. The surface math lives in RtsTerrainSurface.h (shared with the
// editor core), the chunked mesh / materials / water in RtsTerrainRender.cpp.
#include "../RtsModule.h"
#include "RtsTerrainSurface.h"
#include "RtsView.h"

namespace rts::game {

using namespace render;

// The single height query of the view: models, sprites, overlays, lights, editor gizmos. Matches the
// mesh: heightfield corners (two triangles per cell) when the map has them, otherwise levels + fine
// heights + ramps.
float RtsView::heightAt(const MapGrid& g, float x, float z) const {
    return terrain::heightAt(g, rules_ ? float(rules_->levelHeight.toDouble()) : 1.5f, x, z);
}

void RtsView::buildTerrain(gfx::Device& dev, const MapGrid& g) {
    const bool flatDig = groundFx_.drawsDig() && g.dig.size() == g.flags.size(); // RtsGroundFx draws the rock blocks
    // ground overlays follow the surface: rebuild them whenever terrain geometry changed
    if (terrain_.build(dev, g, flatDig) || !hasTerrain_) groundFx_.invalidate();
    hasTerrain_ = true;
    fogMeshDirty_ = true;   // the fog sheet follows the ground
    if (twoD_) twoD_->buildTiles(dev, g, rules_ ? float(rules_->levelHeight.toDouble()) : 1.5f);
}

} // namespace rts::game
