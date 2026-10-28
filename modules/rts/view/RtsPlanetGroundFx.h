// Ground overlays on planets (the sphere counterpart of RtsGroundFx): territory fill + owner borders and road
// cells on the cube-sphere faces of every body, built body-local from the simulation's Surface (cell corners as
// the planet mesh draws them, neighbours across face seams via Surface::offset) and drawn with Renderer3D's lit
// mesh path (translucent, border glow = emissive). Fog-of-war memory like RtsGroundFx: territory is learned by
// seeing it, roads are known. Looks come from RtsGroundFx (visual `terrain.territory` / `terrain.roads`).
// Not on planets (yet): diggable rock blocks, MW2 lanes / map roads between buildings.
#pragma once
#include <cstdint>
#include <vector>
#include "../../../engine/render/Renderer3D.h"
#include "../RtsMap.h"
#include "RtsGroundFx.h"
#include "RtsPlanetRender.h"

namespace rts::game {

class RtsPlanetGroundFx {
public:
    void shutdown(gfx::Device& dev);
    // Per frame (planet maps): rescans the layers a few times per second, rebuilds changed bodies, queues meshes.
    // demo: per grid cell owner slots to show instead of MapGrid::territory (testing the overlay while the
    // simulation does not claim territory on spheres); empty = the simulation's layer
    void draw(gfx::Device& dev, render::Renderer3D& r, const PlanetRenderer& planets, const PlanetViewFrame& frame, double seconds,
              double time, const std::vector<uint8_t>* vision, const MapGrid& grid, const RtsGroundFx& looks, const Rules& rules,
              const std::vector<uint8_t>* demo = nullptr);
    int quads() const { return quads_; }

private:
    struct BodyMesh { render::Mesh fill, border, road; bool dirty = true; };
    void build(gfx::Device& dev, int body, const PlanetRenderer& planets, const MapGrid& grid, const RtsGroundFx& looks);
    std::vector<BodyMesh> bodies_;
    std::vector<uint8_t> memT_, memR_;
    double nextScan_ = -1;
    int quads_ = 0;
    bool init_ = false;
};

} // namespace rts::game
