// Forward renderer for RTS scenes: lit meshes with real-time sun shadows, plus immediate-mode
// colored lines/triangles for overlays (selection, roads, fog). Built only on the RHI, so it runs on
// every backend unchanged.
//
// Frame:  beginScene(camera) -> drawMesh()/line()/... -> render(pass)  [shadow pass + main pass;
//         the main pass is left open so the caller can draw UI, then calls device.endPass()]
#pragma once
#include <cstdint>
#include <vector>
#include "../gfx/Gfx.h"
#include "Math3D.h"

namespace rts::render {

struct Mesh {
    gfx::BufferHandle vb, ib;
    uint32_t indexCount = 0;
    float3 boundsMin, boundsMax;
};

struct MeshVertex { float3 pos; float3 nrm; uint32_t rgba; };

inline uint32_t rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) { return uint32_t(r) | (uint32_t(g) << 8) | (uint32_t(b) << 16) | (uint32_t(a) << 24); }
inline uint32_t rgbaf(float4 c) {
    auto q = [](float v) { return uint8_t(v <= 0 ? 0 : v >= 1 ? 255 : v * 255.0f + 0.5f); };
    return rgba(q(c.x), q(c.y), q(c.z), q(c.w));
}

// CPU-side mesh assembly: primitives with transforms and per-vertex colors, merged into one mesh.
class MeshBuilder {
public:
    MeshBuilder& color(uint32_t c) { color_ = c; return *this; }
    MeshBuilder& transform(const mat4& m) { xf_ = m; return *this; }
    MeshBuilder& box(float3 center, float3 size);
    MeshBuilder& cylinder(float3 base, float radius, float height, int segments = 16, bool caps = true, float topRadius = -1);
    MeshBuilder& cone(float3 base, float radius, float height, int segments = 16);
    MeshBuilder& sphere(float3 center, float radius, int rings = 8, int segments = 14);
    MeshBuilder& disc(float3 center, float radius, int segments = 24); // facing +Y
    MeshBuilder& quad(float3 a, float3 b, float3 c, float3 d);          // CCW seen from the front
    // Large meshes (terrain) use 32-bit indices automatically.
    Mesh upload(gfx::Device& dev) const;
    bool empty() const { return idx_.empty(); }
    size_t vertexCount() const { return verts_.size(); }
    void clear() { verts_.clear(); idx_.clear(); }

    // raw access for terrain-like meshes
    uint32_t addVertex(float3 p, float3 n, uint32_t c) { verts_.push_back({p, n, c}); return uint32_t(verts_.size() - 1); }
    void addTriangle(uint32_t a, uint32_t b, uint32_t c) { idx_.push_back(a); idx_.push_back(b); idx_.push_back(c); }

private:
    uint32_t add(float3 p, float3 n);
    void face(uint32_t a, uint32_t b, uint32_t c, float3 n);
    std::vector<MeshVertex> verts_;
    std::vector<uint32_t> idx_;
    uint32_t color_ = 0xffffffffu;
    mat4 xf_;
};

struct ColorVertex { float3 pos; uint32_t rgba; };

struct Lighting {
    float3 sunDir{0.55f, 1.0f, 0.35f};    // towards the sun
    float sunIntensity = 1.05f;
    float3 sunColor{1.0f, 0.93f, 0.80f};
    float3 skyColor{0.50f, 0.58f, 0.70f};
    float3 groundColor{0.33f, 0.29f, 0.23f};
    float3 fogColor{0.62f, 0.70f, 0.80f};
    float fogDensity = 0.0025f;
};

enum class ShadowQuality { Off, Low, Medium, High };

enum DrawFlags : uint32_t {
    kDrawUnlit = 1,
    kDrawNoCastShadow = 2,
    kDrawNoReceiveShadow = 4,
};

class Renderer3D {
public:
    bool init(gfx::Device& dev);
    void shutdown();
    void setShadowQuality(ShadowQuality q);
    ShadowQuality shadowQuality() const { return quality_; }
    static ShadowQuality defaultQualityFor(const gfx::Device& dev, bool mobile);

    void beginScene(const RTSCamera& cam, int fbW, int fbH);
    void drawMesh(const Mesh& m, const mat4& model, float4 color, uint32_t flags = 0, float specular = 0.25f, float emissive = 0.0f);
    void line(float3 a, float3 b, uint32_t color);
    void triangle(float3 a, float3 b, float3 c, uint32_t color);    // flat, alpha-blended
    void quadXZ(float3 a, float3 b, float width, uint32_t color, float y = 0.02f);
    void circleXZ(float3 c, float radius, uint32_t color, int segments = 40, float y = 0.03f);
    void render(const gfx::PassDesc& screenPass); // shadow pass + opens the main pass and draws everything

    const mat4& viewProj() const { return viewProj_; }
    Lighting lighting;
    uint32_t drawCalls() const { return drawCalls_; }

private:
    struct DrawItem { const Mesh* mesh; mat4 model; float4 color; uint32_t flags; float specular, emissive; };
    void computeShadowMatrix(const RTSCamera& cam, float aspect);
    void uploadFrameUniforms(bool forShadow);

    gfx::Device* dev_ = nullptr;
    gfx::ShaderHandle litShader_, shadowShader_, colorShader_;
    gfx::PipelineHandle litPipe_, litBlendPipe_, shadowPipe_, linePipe_, triPipe_;
    gfx::BufferHandle lineVB_, triVB_;
    gfx::TextureHandle shadowMap_;
    int shadowSize_ = 0;
    ShadowQuality quality_ = ShadowQuality::Medium;
    std::vector<DrawItem> items_;
    std::vector<ColorVertex> lines_, tris_;
    mat4 viewProj_, lightViewProj_;
    float3 camPos_;
    uint32_t drawCalls_ = 0;
};

} // namespace rts::render
