// Dynamic point lights that stay cheap on old mobile GPUs.
//
// Every tier uses a world-space light map: each frame the lights are splatted as additive radial
// quads into a small RGBA8 texture laid over the visible ground (XZ plane). The lit and model
// shaders add that light with ONE texture fetch per pixel, however many lights there are. Values
// are stored as light / kMapRange so an 8-bit target still holds overbright light. Low refreshes
// the map every other frame (the map is world-aligned, so a one-frame-old map stays in place).
// High additionally shades the kMaxPixelLights most important lights per pixel (N.L, specular,
// 3D distance); those are left out of the map so nothing is counted twice.
// Halos: optional camera-facing additive glows around lights (a cheap stand-in for bloom).
// Night glow: emissive materials are boosted by (1 + nightGlow) — windows light up at night.
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
    float halo = 0;              // > 0: camera-facing glow of this world size around pos
    float haloAlpha = 1;         // halo brightness (display space, additive)
};

// Light uniform block (U2 of the lit shader, U4 of the model shader; std140 and MSL match).
struct LightUniforms {
    float map[4];                // origin.x, origin.z, 1 / extent, enabled
    float info[4];               // x per-pixel light count, y map range, z uv flipY, w night glow
    float cloud[4];              // noise offset.xz, noise scale, coverage 0..1
    float sky[4];                // x cloud shadow strength 0..1
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
    void setNightGlow(float g) { nightGlow_ = g; }   // extra emissive multiplier (0 by day)
    float nightGlow() const { return nightGlow_; }
    // Cloud shadows on the sun: coverage 0..1, strength 0..1 (0 = off), drift offset in world units.
    void setClouds(float coverage, float strength, float scale, float offsetX, float offsetZ);

    void begin(const RTSCamera& cam, float aspect);  // Renderer3D::beginScene
    void add(const PointLight& l);
    void add(float3 pos, float radius, float3 color, float intensity = 1) { add(PointLight{pos, radius, color, intensity}); }
    size_t count() const { return lights_.size(); }

    void renderMap();            // Renderer3D::render, before the main pass (own render pass)
    // After setPipeline() of a lit pipeline: light uniforms + light map texture.
    void bind(int uniformSlot = 2, int textureSlot = 1);
    void drawHalos(const mat4& viewProj);  // inside the main pass, after the opaque scene (also draws decals)

    // Contact shadows / AO: soft dark decals on the ground under objects (all tiers, very cheap).
    // center on the ground; half extents along world X / Z; box = rounded rectangle (buildings).
    void addContactShadow(float3 center, float halfX, float halfZ, float strength, bool box = false, float softness = 0.5f);
    void setContactShadows(bool on) { contact_ = on; }
    bool contactShadows() const { return contact_; }

private:
    struct MapVertex { float x, y, u, v, r, g, b, a; };
    struct HaloVertex { float x, y, z, u, v, r, g, b, a; };
    void createMap(int size);
    void pickPixelLights();

    gfx::Device* dev_ = nullptr;
    gfx::ShaderHandle shader_, haloShader_;
    gfx::PipelineHandle pipe_, haloPipe_;
    gfx::BufferHandle vb_, haloVB_;
    gfx::TextureHandle map_, mapDepth_, black_;
    int mapSize_ = 0;
    LightQuality quality_ = LightQuality::Medium;
    float originX_ = 0, originZ_ = 0, extent_ = 64;   // wanted this frame
    float mapX_ = 0, mapZ_ = 0, mapExtent_ = 64;      // what the map texture holds
    float3 focus_, camRight_{1, 0, 0}, camUp_{0, 1, 0};
    std::vector<PointLight> lights_;
    std::vector<uint8_t> perPixel_;
    std::vector<MapVertex> verts_;
    std::vector<HaloVertex> halos_, decals_;
    gfx::ShaderHandle decalShader_;
    gfx::PipelineHandle decalPipe_;
    gfx::BufferHandle decalVB_;
    bool contact_ = true;
    LightUniforms u_{};
    float nightGlow_ = 0;
    uint32_t mapFrame_ = 0;
    bool mapReady_ = false, mapHasLights_ = false;
};

} // namespace rts::render
