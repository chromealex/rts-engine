// Batched textured quads for 2D games: sprites (units, buildings, effects, shadow blobs) and tile
// maps, drawn through the RHI with one pipeline: alpha test + alpha blend, depth tested against the
// 3D scene (terrain hides sprites behind hills) but not depth written, painter-sorted per frame.
// Consecutive quads with the same texture are one draw call; tile maps are static meshes.
//
// Frame:  begin() -> quad()... -> flush(dev, viewProj)   (inside an open pass, e.g. from
//         Renderer3D::drawCustom, so sprites sit above opaque meshes and below overlays)
#pragma once
#include <cstdint>
#include <vector>
#include "../gfx/Gfx.h"
#include "Math3D.h"
#include "TextureCache.h"

namespace rts::render {

struct SpriteVertex { float3 pos; float2 uv; uint32_t color; uint32_t team; };

struct SpriteMesh {          // static quads (4 vertices each), e.g. a tile map
    gfx::BufferHandle vb;
    uint32_t quads = 0;
};

class SpriteBatch {
public:
    bool init(gfx::Device& dev);
    void shutdown();

    void begin() { quads_.clear(); verts_.clear(); }
    // Corners in order top-left, top-right, bottom-right, bottom-left (as seen on screen);
    // uv = logical rect (u0, v0, u1, v1) inside `tex` (mapped through TextureRef, so atlases work).
    // Sorted by layer, then sortKey (smaller first = further back), then submission order.
    void quad(const TextureRef& tex, const float3 (&corners)[4], float4 uv, uint32_t color, uint32_t team, int layer, float sortKey);
    void flush(gfx::Device& dev, const mat4& viewProj);
    size_t size() const { return quads_.size(); }
    uint32_t drawCalls() const { return drawCalls_; }

    // Static meshes: build once, draw every frame before the dynamic sprites.
    static void appendQuad(std::vector<SpriteVertex>& out, const TextureRef& tex, const float3 (&corners)[4], float4 uv, uint32_t color);
    SpriteMesh buildMesh(gfx::Device& dev, const std::vector<SpriteVertex>& verts);
    void destroyMesh(gfx::Device& dev, SpriteMesh& m);
    void drawMesh(gfx::Device& dev, const SpriteMesh& m, gfx::TextureHandle tex, const mat4& viewProj);

private:
    struct Quad { gfx::TextureHandle tex; int layer; float key; uint32_t seq; SpriteVertex v[4]; };
    void drawRange(gfx::Device& dev, gfx::BufferHandle vb, uint32_t firstQuad, uint32_t count);

    gfx::Device* dev_ = nullptr;
    gfx::ShaderHandle shader_;
    gfx::PipelineHandle blendPipe_, groundPipe_;
    gfx::BufferHandle vb_, ib_;
    std::vector<Quad> quads_;
    std::vector<uint32_t> order_;
    std::vector<SpriteVertex> verts_;
    uint32_t drawCalls_ = 0;
};

} // namespace rts::render
