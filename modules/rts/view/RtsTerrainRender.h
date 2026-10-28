// Terrain renderer of the RTS view (RtsView owns one). Static geometry in 32x32-cell chunks,
// frustum-culled, rebuilt only when the cells a chunk depends on change (editor painting: a per-chunk
// hash of the visual inputs). What changes during a match is read by the shader from small per-cell
// textures instead of rebuilding the mesh: resource field amounts and creep (tint), fog of war on
// heightfield maps.
//
// Surfaces (RtsTerrainSurface.h, the same math as RtsView::heightAt):
//   levels       plateaus per cliff level with sharp walls, ramps, soft colour transitions and the
//                apron around the map - the classic look, unchanged
//   heightfield  map terrain.corners: a smooth grid; big maps get CDLOD levels (2^k cell steps per
//                chunk, chosen by camera distance) with geomorphing in the vertex shader
//
// Materials: optional visual `terrain` block (rules.json, game.json wins per key) with up to four
// tiling splat layers per cell kind, a cliff texture and the water look; see docs/game-format.md
// ("Terrain materials"). Quality: Renderer3D::materialQuality() - Low = vertex colours, Medium =
// splat, High = splat + normal maps + triplanar cliffs (RTS_TERRAIN=low|medium|high overrides).
// Without the block every tier draws the classic vertex colours.
#pragma once
#include "RtsFog.h"
#include <cstdint>
#include <string>
#include <vector>
#include "../../../engine/render/Renderer3D.h"
#include "../../../engine/render/TextureCache.h"
#include "../RtsData.h"
#include "../RtsMap.h"
#include "RtsTerrainDetail.h"

namespace rts::game {

struct TerrainLook {
    struct Layer { std::string id, texture, normal; float scale = 4; render::float3 tint{1, 1, 1}; };
    std::vector<Layer> layers;                 // <= 4 splat layers
    int cellLayer[8] = {0, 0, 0, 0, 0, 0, 0, 0}; // per cell kind (CellKind) -> layer index
    bool hasColor[8] = {};
    render::float3 color[8];                    // per cell kind: vertex colour override (all tiers)
    std::string cliffTexture, cliffNormal;
    float cliffScale = 3, slopeStart = 0.35f, slopeEnd = 0.6f, splatStrength = 1;
    render::float3 cliffTint{1, 1, 1};
    enum class Water { Auto, Sheet, Bodies } water = Water::Auto;
    render::float4 waterColor{0.22f, 0.45f, 0.62f, 0.78f};
    float waterOffset = -0.12f;                 // below the surrounding ground (bodies) / ground level (sheet)
    int lod = -1;                               // -1 auto (heightfield maps above 256 cells per side), 0 off, 1 on
    float lodRange = 128;                       // world distance of the finest LOD
    // detail surface (RtsTerrainDetail.h): "classic" = the cell-aligned look (default), "smooth" =
    // synthesised sub-cell detail (bevelled organic cliffs, bicubic relief, fine noise)
    bool smooth = false;
    int detailScale = 4;                        // samples per cell wanted (capped by the quality tier)
    float bevel = 0.35f, wobble = 0.3f, roughness = 0.04f;
    float blendSharpness = 0.5f;                // height-based splat blending (0 = linear)
    float macro = 0.25f;                        // macro variation strength (hides tiling)
    float detailRange = 24;                     // world distance of the finest detail LOD (at 4 samples per cell)
    double detailBudgetMB = 96;                 // detail LOD meshes kept on the GPU (LRU beyond)
    render::float3 rockColor{0.45f, 0.41f, 0.36f}; // steep slopes without a cliff texture
    bool hasRockColor = false;
    bool materials() const { return !layers.empty(); }
};

// Cell kinds, in the classic colour order.
enum CellKind : uint8_t { kKindGrass, kKindSand, kKindRamp, kKindRock, kKindBlocked, kKindLakebed, kKindShallows, kKindDug };
const char* cellKindName(int k);
// Map key "terrainMaterial" (rows of '0'..'3', other chars = by cell kind) -> per cell layer (255 = by kind).
std::vector<uint8_t> parseMaterialRows(const Json& rows, int w, int h);

class TerrainRenderer {
public:
    void init(gfx::Device& dev, const Rules& rules, const render::FileReader& read, const std::string& contentBase);
    void shutdown(gfx::Device& dev);
    // Optional per-cell material layer painted on the map (0..3, 255 = by cell kind). Next build().
    void setMaterialLayer(std::vector<uint8_t> layer) { materialLayer_ = std::move(layer); }
    // (Re)builds the chunks whose inputs changed. Returns true when any geometry changed.
    bool build(gfx::Device& dev, const MapGrid& g, bool flatDig);
    // Queues the terrain (opaque, main pass) and water. vision: fog of war per cell (heightfield maps
    // darken in the shader; levels maps keep the classic fog sheet drawn by RtsView).
    void draw(render::Renderer3D& r, const MapGrid& g, const std::vector<uint8_t>* vision, bool flatDig);
    bool built() const { return built_; }
    bool shaderFog() const { return heightfield_; }
    // Explicit detail heightmap of the map (terrain.detail file, editor detail layer); null = none
    // (synthesised when the look says "smooth"). splat: optional RGBA8 weights per detail sample.
    void setDetail(const terrain::DetailSurface* d, const std::vector<uint8_t>* splat = nullptr);
    bool detailActive() const { return detailOn_; }
    // The drawn surface (detail when active, otherwise RtsTerrainSurface.h) - RtsView::heightAt.
    float surfaceAt(const MapGrid& g, float x, float z) const;
    const terrain::DetailSurface* detail() const { return detailOn_ ? &detail_ : nullptr; }
    // Fog of war look + processed field from RtsFog (fade, blur); null = raw vision, default look.
    void setFog(const std::vector<uint8_t>* field, const FogLook& look, bool changed) { fogField_ = field; fogLook_ = look; fogFieldChanged_ = fogFieldChanged_ || changed; }
    const TerrainLook& look() const { return look_; }

    struct Stats { int chunks = 0, drawn = 0, lodMax = 0, detailK = 0; uint64_t triangles = 0; double buildMs = 0; int rebuilt = 0; size_t geometryBytes = 0; };
    const Stats& stats() const { return stats_; }

private:
    struct TV { render::float3 pos, nrm; uint32_t rgba; float morph[2]; };
    struct Lod { gfx::BufferHandle vb, ib; uint32_t indexCount = 0, vertexCount = 0; };
    struct Chunk {
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;          // cell range [x0, x1)
        std::vector<Lod> lods;                       // levels mode: one
        render::float3 bmin, bmax;
        uint64_t hash = 0, geoHash = 0;
        bool valid = false;
        std::vector<uint32_t> lastUse;               // detail mode: frame each LOD was last drawn (LRU)
    };
    void parseLook(const Json& t);
    void loadTextures(gfx::Device& dev);
    uint64_t chunkHash(const MapGrid& g, const Chunk& c, bool flatDig) const;
    void buildLevelsChunk(gfx::Device& dev, const MapGrid& g, Chunk& c, bool flatDig);
    void buildHeightChunk(gfx::Device& dev, const MapGrid& g, Chunk& c);
    // detail mode (RtsTerrainDetailMesh.cpp)
    bool buildDetail(gfx::Device& dev, const MapGrid& g, bool flatDig, bool all);
    void buildDetailLod(gfx::Device& dev, const MapGrid& g, Chunk& c, int lod);
    void updateDetailTextures(gfx::Device& dev, int i0, int j0, int i1, int j1);
    uint64_t detailHash(const Chunk& c) const;
    size_t detailSampleCap() const;
    void buildApron(gfx::Device& dev, const MapGrid& g);
    void buildWater(gfx::Device& dev, const MapGrid& g, bool flatDig);
    void freeChunk(gfx::Device& dev, Chunk& c);
    void upload(gfx::Device& dev, const std::vector<TV>& v, const std::vector<uint32_t>& idx, Lod& out);
    int kindOf(const MapGrid& g, size_t i, bool flatDig) const;
    render::float3 cellColor(const MapGrid& g, int x, int y, bool flatDig) const;
    void updateCellTextures(gfx::Device& dev, const MapGrid& g, bool flatDig);
    void updateDynamic(gfx::Device& dev, const MapGrid& g, bool all);
    void updateVision(gfx::Device& dev, const std::vector<uint8_t>& vision);
    gfx::PipelineHandle pipeline(gfx::Device& dev, int variant);   // tier + 3 * detail
    int tierFor(const render::Renderer3D& r) const;
    int tierFor0() const;                       // device / env tier without a renderer (detail sample count)

    gfx::Device* dev_ = nullptr;
    const Rules* rules_ = nullptr;
    std::string base_;
    render::FileReader read_;
    TerrainLook look_;
    render::TextureCache textures_;
    gfx::ShaderHandle shaders_[6];              // [detail * 3 + tier]
    gfx::PipelineHandle pipes_[6];
    bool shaderFailed_[6] = {};
    // per-cell textures
    gfx::TextureHandle cellTex_, dynTex_, splatTex_, visionTex_, white_, flatN_, gray_;
    std::vector<uint8_t> cellPx_, dynPx_, splatPx_, visionPx_;
    const std::vector<uint8_t>* fogField_ = nullptr;
    FogLook fogLook_;
    bool fogFieldChanged_ = true;
    int texW_ = 0, texH_ = 0, dynRow_ = 0;
    bool visionInit_ = false;
    std::vector<uint8_t> materialLayer_;
    render::TextureRef layerTex_[4], layerNrm_[4], cliffTex_, cliffNrm_;
    bool texturesLoaded_ = false;

    std::vector<Chunk> chunks_;
    int cw_ = 0, ch_ = 0, mapW_ = 0, mapH_ = 0, lodLevels_ = 1;
    bool heightfield_ = false, built_ = false;
    float lh_ = 1.5f;
    uint64_t globalHash_ = 0;
    Lod apron_;
    render::Mesh water_;
    bool waterSheet_ = false, hasWater_ = false;
    uint64_t waterHash_ = 0;
    Stats stats_;
    // detail surface
    terrain::DetailSurface detail_, fileDetail_;
    std::vector<uint8_t> fileSplat_;
    bool hasFileDetail_ = false, fileDetailChanged_ = false, detailOn_ = false;
    int detailK_ = 0;
    int dirtyI0_ = 1 << 30, dirtyJ0_ = 1 << 30, dirtyI1_ = -1, dirtyJ1_ = -1;   // edited explicit detail samples
    gfx::TextureHandle detailNrmTex_, detailSplatTex_;
    int nrmW_ = 0, nrmH_ = 0, dsplatW_ = 0, dsplatH_ = 0;
    std::vector<uint8_t> nrmPx_, dsplatPx_;
    std::vector<render::float3> cellCol_;       // classic cell colours (detail vertex colours)
    std::vector<uint8_t> cellLayerOf_;          // splat layer by cell kind
    uint32_t frame_ = 0;
    size_t geoBytes_ = 0;
};

} // namespace rts::game
