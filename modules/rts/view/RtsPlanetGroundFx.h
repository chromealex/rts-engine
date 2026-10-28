// Ground overlays on planets (the sphere counterpart of RtsGroundFx), built body-local from the simulation's
// Surface (cell corners as the planet mesh draws them, neighbours across face seams via Surface::offset) and
// drawn with Renderer3D's lit mesh path:
//   territory  owner fill + glowing borders towards other owners (translucent, border glow = emissive)
//   dig        diggable rock as raised blocks with walls towards open cells, gold nuggets / gem crystals per
//              diggable kind, the local player's dig marks with a digging progress bar
//   roads      road cells (map '+', player road networks): centre square + arms to road neighbours
//   lanes      MW2 lanes (GameState::lanes, projected onto the surface by the sim) and map roads between
//              buildings (great-circle ribbons)
// Every face is cut into tiles of kTile x kTile cells; a tile rebuilds only when a layer changes in it (or next
// to it, across seams). Fog-of-war memory like RtsGroundFx: territory and rock are learned by seeing them,
// roads and dig marks are known. Looks come from RtsGroundFx (visual `terrain.territory/dig/roads/lanes`).
#pragma once
#include <cstdint>
#include <vector>
#include "../../../engine/render/Renderer3D.h"
#include "../RtsMap.h"
#include "RtsGroundFx.h"
#include "RtsPlanetRender.h"

namespace rts::game {

struct RtsScene;

class RtsPlanetGroundFx {
public:
    void shutdown(gfx::Device& dev);
    // Per frame (planet maps): rescans the layers a few times per second, rebuilds changed tiles, queues meshes.
    // demo: per grid cell owner slots to show instead of MapGrid::territory (RTS_PLANET_TERRITORY_DEMO, testing);
    // empty = the simulation's layer. Returns true when the rock layer seen by the viewer changed (the planet
    // terrain then needs the new cell flags: RtsView::drawPlanets).
    bool draw(gfx::Device& dev, render::Renderer3D& r, const PlanetRenderer& planets, const PlanetViewFrame& frame, const RtsScene& s,
              double time, const std::vector<uint8_t>* vision, const MapGrid& grid, const RtsGroundFx& looks, const Rules& rules,
              const std::vector<uint8_t>* demo = nullptr);
    int quads() const { return quads_; }
    static constexpr int kTile = 32;

private:
    struct Tile { render::Mesh fill, border, road, rock, veins, gems, marks; bool dT = true, dD = true, dM = true, dR = true; };
    struct Body { int n = 0, tn = 0; std::vector<Tile> tiles; render::Mesh lanes; };
    void dirtyCell(int c, int what, bool neighbours);
    void buildTerritory(gfx::Device& dev, Tile& t, int body, int face, int tx, int ty, const PlanetRenderer& planets, const RtsGroundFx& looks);
    void buildDig(gfx::Device& dev, Tile& t, int body, int face, int tx, int ty, const PlanetRenderer& planets, const RtsGroundFx& looks, bool hi);
    void buildMarks(gfx::Device& dev, Tile& t, int body, int face, int tx, int ty, const PlanetRenderer& planets, const MapGrid& g,
                    const RtsGroundFx& looks, const Rules& rules);
    void buildRoads(gfx::Device& dev, Tile& t, int body, int face, int tx, int ty, const PlanetRenderer& planets, const RtsGroundFx& looks);
    void buildLanes(gfx::Device& dev, const PlanetRenderer& planets, const RtsScene& s, const RtsGroundFx& looks, const Rules& rules, bool hi);
    const Surface* S_ = nullptr;
    std::vector<Body> bodies_;
    std::vector<uint8_t> memT_, memR_, memD_, memM_, memP_;   // territory, road, rock kind, own marks, dig progress (0..16)
    double nextScan_ = -1;
    uint32_t laneSig_ = ~0u;
    size_t laneCount_ = ~size_t(0);
    int quads_ = 0;
    bool init_ = false, hi_ = true;
};

} // namespace rts::game
