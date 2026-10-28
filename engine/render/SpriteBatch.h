// Batched textured quads for 2D games: sprites (units, buildings, effects, shadow blobs) and tile
// maps, drawn through the RHI with one pipeline: alpha test + alpha blend, depth tested against the
// 3D scene (terrain hides sprites behind hills) but not depth written, painter-sorted per frame.
// Consecutive quads with the same texture are one draw call; tile maps are static meshes.
//
// Frame:  begin() -> quad()... -> flush(dev, viewProj)   (inside an open pass, e.g. from
//         Renderer3D::drawCustom, so sprites sit above opaque meshes and below overlays)
//
// Sun shadows (optional): caster() queues alpha-tested quads for the shadow map, drawn by
// flushCasters() inside Renderer3D's shadow pass. Quads flagged `lit` use the lit sprite shader
// (sun, sun shadows, point lights; the caller binds the lit frame through `LitBinder`), quads
// flagged `decal` lie on a surface (projected ground decals, blob shadows) and are drawn with a
// depth bias towards the camera so they never z-fight with the ground they were cut from.
#pragma once
#include <cstdint>
#include <functional>
#include <vector>
#include "../gfx/Gfx.h"
#include "Math3D.h"
#include "TextureCache.h"

namespace rts::render {

// rpos: receive point for lighting (lit shader): xyz, w > 0 = upright sprite (see SpriteShaders.h)
struct SpriteVertex { float3 pos; float2 uv; uint32_t color; uint32_t team; float4 rpos; };

enum SpriteQuadFlags : uint32_t { kSpriteLit = 1, kSpriteDecal = 2 };

// Lit draws: binds U1 / U2 / tex0 / tex1 (Renderer3D::bindLitFrame); `ref` = reference light
// (linear rgb) at which a lit sprite shows its painted colours; receiveShadows = sample the sun map.
struct SpriteLighting {
    std::function<void()> bind;
    float3 ref{1, 1, 1};
    bool receiveShadows = true;
}; 

struct SpriteMesh {          // static quads (4 vertices each), e.g. a tile map
    gfx::BufferHandle vb;
    uint32_t quads = 0;
};

class SpriteBatch {
public:
    bool init(gfx::Device& dev);
    void shutdown();

    void begin() { quads_.clear(); verts_.clear(); casters_.clear(); }
    // Corners in order top-left, top-right, bottom-right, bottom-left (as seen on screen);
    // uv = logical rect (u0, v0, u1, v1) inside `tex` (mapped through TextureRef, so atlases work).
    // Sorted by layer, then sortKey (smaller first = further back), then submission order.
    void quad(const TextureRef& tex, const float3 (&corners)[4], float4 uv, uint32_t color, uint32_t team, int layer, float sortKey);
    // Full control: per-corner texture-space uv and receive points (rpos), SpriteQuadFlags.
    // A triangle is a quad whose last two corners are equal.
    void quadEx(gfx::TextureHandle tex, const SpriteVertex (&v)[4], int layer, float sortKey, uint32_t flags);
    // `lighting` is required when any quad is flagged kSpriteLit (otherwise those draw unlit).
    void flush(gfx::Device& dev, const mat4& viewProj, const SpriteLighting* lighting = nullptr);
    // shadow casters (texture-space uv, alpha-tested at 0.5)
    void caster(gfx::TextureHandle tex, const float3 (&corners)[4], const float2 (&uv)[4]);
    size_t casterCount() const { return casters_.size(); }
    void flushCasters(gfx::Device& dev, const mat4& lightViewProj);   // inside the shadow pass
    size_t size() const { return quads_.size(); }
    uint32_t drawCalls() const { return drawCalls_; }

    // Static meshes: build once, draw every frame before the dynamic sprites.
    static void appendQuad(std::vector<SpriteVertex>& out, const TextureRef& tex, const float3 (&corners)[4], float4 uv, uint32_t color);
    SpriteMesh buildMesh(gfx::Device& dev, const std::vector<SpriteVertex>& verts);
    void destroyMesh(gfx::Device& dev, SpriteMesh& m);
    void drawMesh(gfx::Device& dev, const SpriteMesh& m, gfx::TextureHandle tex, const mat4& viewProj, const SpriteLighting* lighting = nullptr);

private:
    struct Quad { gfx::TextureHandle tex; int layer; float key; uint32_t seq; uint32_t flags; SpriteVertex v[4]; };
    struct Caster { gfx::TextureHandle tex; SpriteVertex v[4]; };
    void drawRange(gfx::Device& dev, gfx::BufferHandle vb, uint32_t firstQuad, uint32_t count);
    void setUniforms(gfx::Device& dev, const mat4& viewProj, bool lit, const SpriteLighting* lighting);

    gfx::Device* dev_ = nullptr;
    gfx::ShaderHandle shader_, litShader_, casterShader_;
    gfx::PipelineHandle blendPipe_, groundPipe_, decalPipe_, litPipe_, litGroundPipe_, litDecalPipe_, casterPipe_;
    gfx::BufferHandle vb_, ib_, casterVb_;
    std::vector<Caster> casters_;
    std::vector<SpriteVertex> casterVerts_;
    std::vector<Quad> quads_;
    std::vector<uint32_t> order_;
    std::vector<SpriteVertex> verts_;
    uint32_t drawCalls_ = 0;
};

} // namespace rts::render
