// Dynamic point lights that stay cheap on old mobile GPUs.
//
// Every tier uses a world-space light map: each frame the lights are splatted as additive radial
// quads into a small RGBA8 texture laid over the visible ground (XZ plane). The lit shader adds that
// light with ONE texture fetch per pixel, however many lights there are. Values are stored as
// light / kMapRange so an 8-bit target still holds overbright light.
// High additionally shades the kMaxPixelLights most important lights per pixel (N.L, specular,
// 3D distance); those are left out of the map so nothing is counted twice.
//
// Owned by Renderer3D (`Renderer3D::lights`): add lights between beginScene() and render().
#pragma once
#include <cstdint>
#include <vector>
#include "../gfx/Gfx.h"
#include "Math3D.h"

namespace rts::render {

enum class LightQuality { Off, Low, Medium, High };

struct PointLight {
    float3 pos;
    float radius = 3;            // world units; light reaches zero here
    float3 color{1, 0.75f, 0.5f};
    float intensity = 1;         // overbright allowed (clamped to kMapRange in the map)
    float priority = 0;          // added to the score when picking per-pixel lights (High)
};

// Uniform block U2 of the lit shader (std140 and MSL layouts match).
struct LightUniforms {
    float map[4];                // origin.x, origin.z, 1 / extent, enabled
    float info[4];               // x per-pixel light count, y map range, z uv flipY, w unused
    float pos[8][4];             // xyz, radius
    float col[8][4];             // rgb * intensity, unused
};

class LightSystem {
public:
    static constexpr int kMaxPixelLights = 8;
    static constexpr float kMapRange = 4.0f;
    static constexpr size_t kMaxLights = 1024;

    bool init(gfx::Device& dev);
    void shutdown();
    void setQuality(LightQuality q);
    LightQuality quality() const { return quality_; }
    static LightQuality defaultQualityFor(const gfx::Device& dev, bool mobile);

    void begin(const RTSCamera& cam, float aspect);  // Renderer3D::beginScene
    void add(const PointLight& l);
    void add(float3 pos, float radius, float3 color, float intensity = 1) { add(PointLight{pos, radius, color, intensity, 0}); }
    size_t count() const { return lights_.size(); }

    void renderMap();            // Renderer3D::render, before the main pass (own render pass)
    void bind();                 // after setPipeline() of a lit pipeline: uniforms U2 + texture slot 1

private:
    struct MapVertex { float x, y, u, v, r, g, b, a; };
    void createMap(int size);
    void pickPixelLights();

    gfx::Device* dev_ = nullptr;
    gfx::ShaderHandle shader_;
    gfx::PipelineHandle pipe_;
    gfx::BufferHandle vb_;
    gfx::TextureHandle map_, mapDepth_, black_;
    int mapSize_ = 0;
    LightQuality quality_ = LightQuality::Medium;
    float originX_ = 0, originZ_ = 0, extent_ = 64;
    float3 focus_;
    std::vector<PointLight> lights_;
    std::vector<uint8_t> perPixel_;
    std::vector<MapVertex> verts_;
    LightUniforms u_{};
    bool mapReady_ = false;
};

} // namespace rts::render
