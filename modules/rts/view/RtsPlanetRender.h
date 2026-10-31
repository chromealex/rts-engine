// Planet renderer of the RTS view (RtsView owns one when the map has a `world` block).
//
// Terrain: per body six faces, each a quadtree of chunks (16 x 16 quads; level k = 2^k cells per quad),
// CDLOD selection by camera distance with geomorphing in the vertex shader (odd vertices slide onto the
// coarser grid, diagonal (0,0)-(1,1) like the flat terrain), skirts on every chunk edge hide the cracks
// between levels and across face seams (seam vertices are bit-identical: RtsSphere corner directions).
// Chunks are built lazily on first use and kept. Culling per chunk: frustum (bounding sphere) + horizon
// (the body occludes chunks behind its limb).
// Look: the flat terrain's materials and tiers (TerrainLook: Low = vertex colours by cell kind, Medium+ =
// triplanar splat by cell kind + slope cliff texture), ocean sphere at seaLevel (same chunks projected,
// alpha blended), fog of war per cell (vision atlas), aerial haze; star = emissive sphere, sun direction
// from the star; atmosphere shell (rim scattering, Medium+), starfield sky, orbit lines.
// Coordinates: everything is drawn in the view world of PlanetViewFrame (RtsPlanetCamera.h).
#pragma once
#include <memory>
#include <string>
#include <vector>
#include "../../../engine/render/Renderer3D.h"
#include "../../../engine/render/TextureCache.h"
#include "RtsFog.h"
#include "RtsPlanet.h"
#include "RtsPlanetCamera.h"
#include "RtsTerrainRender.h"

namespace rts::game {

class PlanetRenderer {
public:
    void init(gfx::Device& dev, const TerrainLook& look, const render::FileReader& read, const std::string& contentBase);
    void shutdown(gfx::Device& dev);
    void setWorld(const PlanetWorld& w);
    const PlanetWorld& world() const { return world_; }
    bool active() const { return !world_.empty(); }
    const PlanetSurface* surface(int body) const { return body >= 0 && size_t(body) < bodies_.size() && bodies_[size_t(body)].surf ? &*bodies_[size_t(body)].surf : nullptr; }
    // Fog of war of one body: per cell 0 unexplored, 128 explored, 255 visible (6 * N * N); null = off.
    void setVision(int body, const std::vector<uint8_t>* field, const FogLook& look);
    // Edits (editor sculpt): new corner heights for a body; rebuilds the affected chunks.
    void updateHeights(int body, const std::vector<int16_t>& corners);   // only chunks over changed corners rebuild
    // Cell flags of a body changed (painting): colours / splat of every chunk of that body rebuild.
    void updateFlags(int body, const std::vector<uint8_t>& flags);
    // Explicit detail heights of a body changed (editor detail sculpt; same scale as drawn): rebuilds the
    // affected chunks. Returns false when the body draws no detail at that scale (caller re-sets the world).
    bool updateDetail(int body, const std::vector<int16_t>& detail);
    int detailScale(int body) const { return surface(body) ? surface(body)->detailScale() : 0; }

    struct Options { bool orbits = true, atmosphere = true, sky = true; float lodRange = 48; };
    Options options;
    // Queues everything; the lighting of `r` gets the star direction of `lightBody`.
    void draw(render::Renderer3D& r, const PlanetViewFrame& frame, double seconds, int lightBody, float planetBlend);

    struct Stats { int chunks = 0, drawn = 0, built = 0, culledHorizon = 0, culledFrustum = 0; uint64_t triangles = 0; double buildMs = 0; };
    const Stats& stats() const { return stats_; }

private:
    struct PV { render::float3 pos, nrm; uint32_t rgba; render::float3 morph; float uv[2]; uint32_t splat; float duv[2]; };
    struct Chunk {
        int face = 0, x0 = 0, y0 = 0, size = 0, level = 0;
        int child = -1;                     // index of the first of 4 children, -1 = leaf
        render::float3 center, dir;         // body-local centre of the chunk, its unit direction
        float boundR = 0, cosAngle = 1, minR = 0, maxR = 0;   // bounding sphere radius, angular extent, radial range
        bool hasSea = false;
        gfx::BufferHandle vb, ib;
        uint32_t indexCount = 0;
        bool built = false;
    };
    struct Body {
        std::unique_ptr<PlanetSurface> surf;
        std::vector<Chunk> chunks;          // [0..5]: face roots
        int levels = 1, grid = 16;
        gfx::TextureHandle vision;
        std::vector<uint8_t> visionPx;
        bool fog = false;
        FogLook fogLook;
        // detail (k > 0): normal + cavity atlas, faces in 3 x 2 blocks of (G+1)^2 texels (G = N * k)
        gfx::TextureHandle detailTex;
        std::vector<uint8_t> detailPx;
        int atlasW = 0, atlasH = 0;
    };
    int detailScaleFor(const PlanetBody& pb) const;
    void setupDetail(const PlanetBody& pb, Body& b);
    void fillAtlas(Body& b, int face, int i0, int j0, int i1, int j1);
    void uploadAtlas(Body& b);
    void freeChunksOver(Body& b, const PlanetSurface::Rect (&gridRect)[6], int margin);
    render::float3 sampleColor(const PlanetBody& pb, const PlanetSurface& s, int face, int i, int j, uint32_t& splat) const;
    void buildTree(Body& b, int idx);
    void computeBounds(Body& b, Chunk& c);
    void buildChunk(gfx::Device& dev, const PlanetBody& pb, Body& b, Chunk& c);
    void freeChunk(gfx::Device& dev, Chunk& c);
    render::float3 cornerColor(const PlanetBody& pb, const PlanetSurface& s, int face, int i, int j, uint32_t& splat) const;
    gfx::PipelineHandle terrainPipe(gfx::Device& dev, int tier, bool ocean, bool detail = false);
    void initSpace(gfx::Device& dev);
    int tierFor(const render::Renderer3D& r) const;

    gfx::Device* dev_ = nullptr;
    render::FileReader read_;
    std::string base_;
    TerrainLook look_;
    render::TextureCache textures_;
    render::TextureRef layerTex_[4], cliffTex_;
    bool texturesLoaded_ = false;
    gfx::TextureHandle white_, gray_, black_;
    gfx::ShaderHandle shaders_[3][2];          // [tier][detail]
    gfx::PipelineHandle pipes_[3][2][2];       // [tier][detail][ocean]
    bool shaderFailed_[3][2] = {};
    gfx::ShaderHandle skyShader_, atmoShader_;
    gfx::PipelineHandle skyPipe_, atmoPipe_;
    render::Mesh sphere_, atmoSphere_;   // unit spheres (stars, distant bodies / atmosphere shell)
    PlanetWorld world_;
    std::vector<Body> bodies_;
    Stats stats_;
};

} // namespace rts::game
