// Ground overlays of the RTS view, drawn as separate passes on top of the terrain (they never touch
// the terrain mesh and only read the simulation):
//   territory  owner colour on the ground + glowing border along territory edges, room floor tint
//   dig        diggable rock as raised blocks with walls, gold / gem veins per diggable kind, the local
//              player's dig marks and digging progress; dug cells disappear
//   lanes      Mushroom Wars 2 lanes (GameState::lanes) and map roads between buildings as soft ribbons
//   roads      road cells (map '+', player road networks) as road surfaces with joins and curved corners
// Everything is placed with RtsView::heightAt and drawn through Renderer3D's lit pipeline (sun
// shadows + point lights). Geometry is rebuilt only when the layers it shows change: the territory /
// dig / room / road layers are compared against a fog-of-war memory (what the viewer last saw) a few
// times per second while GameState::territoryDirty is set, lanes when the lane signature changes.
//
// Look: optional visual `terrain` block in rules.json and/or game.json (game.json wins per key):
//   "terrain": { "territory": {...}, "dig": {...}, "lanes": {...}, "roads": {...} }  (any block may be false)
// Keys are documented in docs/game-format.md ("Terrain overlays"). Without them: built-in defaults.
// Quality: MaterialQuality::Low (weak Android GPUs) drops the glow pulse, vein crystals, side lanes
// and road edge/cobble geometry; RTS_GROUNDFX=low|high overrides for testing.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include "../../../engine/render/Renderer3D.h"
#include "../../../engine/render/TextureCache.h"
#include "../RtsData.h"
#include "../RtsMap.h"

namespace rts::game {

struct RtsScene;
class RtsView;

class RtsGroundFx {
public:
    struct Tex {                       // optional world-space texture of a layer (TextureCache::resolve)
        std::string path;
        float scale = 2, strength = 1; // world units per repeat, mix with the vertex colour
        render::TextureRef ref;
    };
    struct TerritoryLook {
        bool on = true;
        float fill = 0.28f;            // alpha of the owner colour on owned cells
        float border = 0.26f;          // glow width (cells)
        float glow = 0.85f;            // emissive strength of the border
        bool pulse = true;             // animated border (high quality only)
        std::vector<render::float3> colors;            // per player slot (index 0 = slot 1); default playerColor
        std::map<std::string, render::float3> rooms;   // room id -> floor tint
        float roomFill = 0.5f;
        Tex tex;
    };
    struct DigKind { render::float3 rock, vein; int style = 0; /*0 plain, 1 vein, 2 gem*/ float glow = 0.5f; };
    struct DigLook {
        bool on = true;
        float height = 1.15f;          // keep below the fog-of-war sheet (1.3)
        render::float3 mark{1.0f, 0.80f, 0.22f};
        bool progress = true;
        std::vector<DigKind> kinds;    // per Rules::diggables index
        Tex tex;                       // rock texture (top and walls, triplanar)
    };
    struct LaneLook {
        bool on = true;
        render::float3 color{0.74f, 0.63f, 0.44f}, edge{0.50f, 0.40f, 0.27f};
        float alpha = 0.75f, width = 0; // width 0: covers every lane of a pair
        bool sideLanes = false;         // thin tracks for the offset lanes (high quality only)
        bool all = false;               // false: skip a lane that runs through a third building
        Tex tex;
    };
    struct RoadLook {
        bool on = true;
        render::float3 color{0.72f, 0.63f, 0.48f}, edge{0.40f, 0.33f, 0.24f};
        float width = 0.7f, alpha = 0.96f;
        bool cobbles = true;
        Tex tex;
    };
    struct ConcreteLook {              // foundation cells (rules "foundation": Dune II concrete slabs)
        bool on = true;
        render::float3 color{0.56f, 0.55f, 0.52f}, seam{0.30f, 0.29f, 0.27f};
        float alpha = 1.0f;
        Tex tex;
    };

    void init(gfx::Device& dev, const Rules& rules, const render::FileReader& read, const std::string& contentBase);
    void shutdown(gfx::Device& dev);
    void invalidate() { geomDirty_ = true; }   // terrain rebuilt (heights may have changed): rebuild geometry
    // Map roads / lanes between buildings are drawn here (RtsView skips its flat road quads).
    bool drawsLinks() const { return lane_.on && pipe_.valid(); }
    // The terrain leaves diggable cells flat when these blocks are drawn instead.
    bool drawsDig() const { return dig_.on && rules_ && !rules_->diggables.empty(); }
    void draw(render::Renderer3D& r, const RtsScene& s, double time, const std::vector<uint8_t>* vision,
              const MapGrid& grid, const RtsView& view);
    // Looks as configured (planet overlays reuse them: RtsPlanetGroundFx).
    const TerritoryLook& territoryLook() const { return terr_; }
    const RoadLook& roadLook() const { return road_; }
    const DigLook& digLook() const { return dig_; }
    const LaneLook& laneLook() const { return lane_; }
    bool showsTerritory() const { return useT_ && terr_.on; }
    bool territoryOn() const { return terr_.on; }

private:
    struct Chunk {
        render::Mesh fill, border, rock, veins, gems, marks, road, concrete;
        bool dT = true, dD = true, dM = true, dR = true, dC = true;
        float yMin = 0, yMax = 2;
    };
    void parseLook(const Json& terrain);
    void resetGrid(const MapGrid& g);
    void scan(const MapGrid& g, const std::vector<uint8_t>* vision, uint8_t viewer);
    void dirty(int x, int y, bool neighbours, int what);
    void buildTerritory(Chunk& c, int cx, int cy, const MapGrid& g, const RtsView& v);
    void buildDig(Chunk& c, int cx, int cy, const MapGrid& g, const RtsView& v);
    void buildMarks(Chunk& c, int cx, int cy, const MapGrid& g, const RtsView& v);
    void buildRoads(Chunk& c, int cx, int cy, const MapGrid& g, const RtsView& v);
    void buildConcrete(Chunk& c, int cx, int cy, const MapGrid& g, const RtsView& v);
    void buildLanes(const RtsScene& s, const MapGrid& g, const RtsView& v);
    void drawProgress(render::Renderer3D& r, const MapGrid& g, const RtsView& v, const std::vector<uint8_t>* vision);
    float digTop(const MapGrid& g, const RtsView& v, int cx, int cy) const;
    void setMesh(render::Mesh& m, const render::MeshBuilder& b);
    void freeMesh(render::Mesh& m);
    render::float3 ownerColor(int slot) const;
    void loadTex(Tex& t);

    gfx::Device* dev_ = nullptr;
    const Rules* rules_ = nullptr;
    std::string base_;
    render::TextureCache textures_;
    gfx::ShaderHandle shader_;
    gfx::PipelineHandle pipe_;
    gfx::TextureHandle white_;
    TerritoryLook terr_;
    DigLook dig_;
    LaneLook lane_;
    RoadLook road_;
    ConcreteLook conc_;
    bool useT_ = false, useD_ = false, useR_ = false, useC_ = false;   // layers present in this game
    bool hi_ = true, invalid_ = true, geomDirty_ = false;
    int gridW_ = 0, gridH_ = 0, chunksX_ = 0, chunksY_ = 0;
    double nextScan_ = -1;
    uint8_t viewer_ = 0;
    // what the viewer last saw, per cell (fog of war memory)
    std::vector<uint8_t> memT_, memD_, memR_, memM_, memC_;
    std::vector<int16_t> memRoom_;
    std::vector<uint32_t> marked_;             // cells with a mark shown (progress bars)
    std::vector<Chunk> chunks_;
    std::vector<int> visible_;
    uint32_t laneSig_ = ~0u;
    size_t laneCount_ = ~size_t(0);
    render::Mesh lanes_;
    float time_ = 0;
};

} // namespace rts::game
