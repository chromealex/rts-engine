// Dynamic point lights that stay cheap on old mobile GPUs.
//
// Every tier uses a world-space light map: each frame the lights are splatted as additive radial
// quads into a small RGBA8 texture laid over the visible ground (XZ plane). The lit and model
// shaders add that light with ONE texture fetch per pixel, however many lights there are. Values
// are stored as light / kMapRange so an 8-bit target still holds overbright light. Low refreshes
// the map every other frame (the map is world-aligned, so a one-frame-old map stays in place).
// High additionally adds per-pixel detail for the kMaxPixelLights most important lights (N.L
// shaping around the map's light, specular). Every light always stays in the map, so a light moving
// in or out of the per-pixel set only changes fine detail, never the brightness (no flicker); the
// set is picked by stable importance (no fire flicker) with hysteresis on PointLight::key.
// Halos: optional camera-facing additive glows around lights (a cheap stand-in for bloom).
// Night glow: emissive materials are boosted by (1 + nightGlow) — windows light up at night.
// Reflections: glossy / metallic materials get highlights from the lights and mirror the lit ground
// along the reflected ray (Medium+, one fetch). High: water gets a real planar reflection of the
// units, buildings and light glows (mirrored pass at half resolution, terrain and effects skipped).
//
// Owned by Renderer3D (`Renderer3D::lights`): add lights between beginScene() and render().
#pragma once
#include <algorithm>
#include <cmath>
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
    // Spot light (cosOuter > -1): a cone along `dir`; full light inside cosInner, none outside cosOuter.
    float3 dir{0, -1, 0};
    float cosOuter = -2, cosInner = -2;
    float groundY = -1e9f;       // ground under the light (light map projection of spots); default: pos.y - 1
    float beam = 0;              // spot: strength of the visible light shaft (display space, additive)
    uint32_t key = 0;            // stable id across frames (0 = none): keeps a per-pixel slot (hysteresis)
    float rank = -1;             // importance for the per-pixel slots (>= 0: instead of intensity, e.g. without flicker)

    void makeSpot(float3 direction, float coneAngleDeg, float softness = 0.6f) {
        dir = normalize(direction);
        const float half = coneAngleDeg * 0.5f * 0.0174532925f;
        cosOuter = std::cos(half);
        cosInner = std::cos(half * (1.0f - std::clamp(softness, 0.02f, 1.0f)));
    }
    bool spot() const { return cosOuter > -1.5f; }
};

// Light uniform block (U2 of the lit shader, U4 of the model shader; std140 and MSL match).
struct LightUniforms {
    float map[4];                // origin.x, origin.z, 1 / extent, enabled
    float info[4];               // x per-pixel light count, y map range, z uv flipY, w night glow
    float cloud[4];              // noise offset.xz, noise scale, coverage 0..1
    float sky[4];                // x cloud shadow strength 0..1, y water reflection strength, zw 1 / view size
    float env[4];                // x environment reflections (glossy materials sample the light map)
    float pos[8][4];             // xyz, radius
    float col[8][4];             // rgb * intensity, spot cosInner
    float spot[8][4];            // spot direction xyz, cosOuter (< -1.5: point light)
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
    // After setPipeline() of a lit pipeline: light uniforms + light map texture (+ the water
    // reflection texture for the lit shader).
    void bind(int uniformSlot = 2, int textureSlot = 1, int reflectionSlot = -1);

    // Planar water reflection (Renderer3D::renderReflection): High tier only.
    void setWaterReflections(bool on) { reflectOn_ = on; }
    bool waterReflections() const { return reflectOn_; }
    bool reflectionsWanted() const { return reflectOn_ && quality_ == LightQuality::High && dev_; }
    bool beginReflection(int viewW, int viewH, float3 clearColor);   // opens the pass (half res)
    void endReflection();
    void drawHalos(const mat4& viewProj, bool decals = true);  // inside the main pass, after the opaque scene

    // Contact shadows / AO: soft dark decals on the ground under objects (all tiers, very cheap).
    // center on the ground; half extents along world X / Z; box = rounded rectangle (buildings).
    void addContactShadow(float3 center, float halfX, float halfZ, float strength, bool box = false, float softness = 0.5f);
    void setContactShadows(bool on) { contact_ = on; }
    bool contactShadows() const { return contact_; }

private:
    // spot: direction + height above ground, cone (cosOuter, cosInner, radius) — cosOuter < -1.5 = point
    struct MapVertex { float x, y, u, v, r, g, b, a, dx, dy, dz, h, co, ci, rad, pad; };
    struct HaloVertex { float x, y, z, u, v, r, g, b, a; };
    void createMap(int size);
    // Dynamic vertex data is appended (ring) instead of rewritten: Metal keeps one copy of a dynamic
    // buffer per frame in flight, so two uploads in one frame (water reflection + main pass, or the
    // editor's two views) would overwrite each other. Returns the byte offset to draw from.
    size_t stream(gfx::BufferHandle buf, size_t& cursor, size_t capacity, const void* data, size_t bytes);
    void pickPixelLights();

    gfx::Device* dev_ = nullptr;
    gfx::ShaderHandle shader_, haloShader_;
    gfx::PipelineHandle pipe_, haloPipe_;
    gfx::BufferHandle vb_, haloVB_;
    size_t vbCursor_ = 0, haloCursor_ = 0, beamCursor_ = 0, decalCursor_ = 0;
    size_t vbCap_ = 0, smallCap_ = 0;
    gfx::TextureHandle map_, mapDepth_, black_;
    int mapSize_ = 0;
    LightQuality quality_ = LightQuality::Medium;
    float originX_ = 0, originZ_ = 0, extent_ = 64;   // wanted this frame
    float mapX_ = 0, mapZ_ = 0, mapExtent_ = 64;      // what the map texture holds
    float3 focus_, camRight_{1, 0, 0}, camUp_{0, 1, 0};
    std::vector<PointLight> lights_;
    std::vector<uint8_t> perPixel_;
    std::vector<uint32_t> prevKeys_;   // per-pixel set of the last frame (sorted)
    std::vector<MapVertex> verts_;
    std::vector<HaloVertex> halos_, decals_;
    gfx::ShaderHandle decalShader_, beamShader_;
    gfx::PipelineHandle decalPipe_, beamPipe_;
    std::vector<HaloVertex> beams_;
    gfx::BufferHandle beamVB_;
    gfx::BufferHandle decalVB_;
    bool contact_ = true;
    gfx::TextureHandle reflColor_, reflDepth_;
    int reflW_ = 0, reflH_ = 0;
    float viewW_ = 1, viewH_ = 1;
    bool reflectOn_ = true, reflReady_ = false;
    LightUniforms u_{};
    float nightGlow_ = 0;
    uint32_t mapFrame_ = 0;
    bool mapReady_ = false, mapHasLights_ = false;
};

} // namespace rts::render
