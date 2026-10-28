// CPU particle system rendered through the RHI. Presentation only: own float math and RNG,
// never read by the simulation, safe to skip, slow down or drop.
//
// Frame:
//   fx.setView(camera, viewProj, fbW, fbH);   // after Renderer3D::beginScene (culling, billboards)
//   fx.update(dt);
//   ... Renderer3D::render(pass) ...          // main pass is left open
//   fx.render(fog);                           // draws into the open pass (depth test, no depth write)
//
// Weak-GPU design (OpenGL ES 3.0 is the floor):
//   * one dynamic vertex buffer uploaded once per frame (GL: orphaned; Metal: per-frame ring),
//     one static 16-bit index buffer, one draw per (texture, blend) batch;
//   * hard particle budget, spawn-rate scale, on-screen size cap and alpha sorting per tier
//     (FxQuality) — fill rate is what kills old Mali/Adreno, so the size cap matters most;
//   * one-shot effects that start off-screen or far away are not spawned at all.
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "../gfx/Gfx.h"
#include "FxEffect.h"

namespace rts::fx {

struct QualityLimits {
    int maxParticles;       // pool size: spawns beyond it are dropped
    float spawnScale;       // multiplies burst / rate / perMeter
    float maxScreenSize;    // particle size cap as a fraction of the screen height
    float cullDistance;     // one-shots farther than this from the camera are not spawned
    bool sortAlpha;         // back-to-front sort of alpha-blended particles
};
QualityLimits limitsFor(FxQuality q);

// Default tier: env RTS_PARTICLES=off|low|medium|high, else by GPU name (same weak-GPU list as shadows).
FxQuality defaultQualityFor(const std::string& gpuName, bool mobile);
FxQuality defaultQualityFor(const gfx::Device& dev, bool mobile);

// A texture region for particles. Sprite atlases give a sub-rectangle of a shared texture; particles
// from the same texture share a draw call whatever their region.
struct FxTexture {
    gfx::TextureHandle tex;
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
};
// Loads a sprite for a particle texture name (not "builtin:*"). The apps connect
// render::TextureCache::resolve here — the same loader (and future atlas packer) as every sprite.
using TextureLoader = std::function<bool(const std::string& name, FxTexture& out)>;
// A mesh for mesh particles and the "mesh" shape: engine model space (Y up, right-handed, metres).
struct FxMesh {
    std::vector<float3> pos, nrm;     // nrm may be empty (computed)
    std::vector<float2> uv;           // may be empty
    std::vector<uint32_t> idx;        // triangles
};
// Loads a mesh by name (not "builtin:*"): the apps connect render::loadModel here (FBX / OBJ / glTF).
using MeshLoader = std::function<bool(const std::string& name, FxMesh& out)>;
// Reads a content file (`fx/<name>.json`): platform::readContentFile in the apps.
using FileReader = std::function<bool(const std::string& path, std::string& out)>;

struct FxHandle {
    uint32_t id = 0;
    bool valid() const { return id != 0; }
    bool operator==(const FxHandle&) const = default;
};

struct FxFog { float3 color{0.62f, 0.70f, 0.80f}; float density = 0; };  // Renderer3D::lighting fog (linear colour)

// A light carried by particles ("light" module): the apps add these to Renderer3D::lights.
struct FxPointLight { float3 pos; float radius = 1; float3 color{1, 1, 1}; float intensity = 1; };

// A live particle as seen from outside (tests, tools).
struct FxParticleInfo { float3 pos, vel; float size = 0, age = 0, life = 0, rotation = 0; float4 color; int effect = -1, emitter = 0; };

struct FxStats {
    int particles = 0, instances = 0, drawn = 0, batches = 0, drawCalls = 0;
    int trailQuads = 0;     // ribbon segments drawn (included in drawn)
    int meshVerts = 0;      // vertices of mesh particles drawn
    int dropped = 0;        // spawns refused by the budget (since the last resetStats)
    int culled = 0;         // one-shots not started: off-screen or too far
    float fill = 0;         // drawn particle area / screen area: the overdraw particles add
};

class ParticleSystem {
public:
    ParticleSystem();
    ~ParticleSystem();
    ParticleSystem(const ParticleSystem&) = delete;
    ParticleSystem& operator=(const ParticleSystem&) = delete;

    // `dev` may be null: simulation without drawing (tests, headless tools).
    bool init(gfx::Device* dev);
    void shutdown();
    const std::string& lastError() const { return error_; }

    void setQuality(FxQuality q);
    FxQuality quality() const { return quality_; }
    void setFileReader(FileReader r) { reader_ = std::move(r); }
    void setTextureLoader(TextureLoader l);
    void setMeshLoader(MeshLoader l);
    // Ground height for "collision" (world y = 0 without it): RtsView::heightAt in the apps.
    void setGroundHeight(std::function<float(float x, float z)> fn);
    // Wind (world units / s^2) acting on emitters with "externalForces" (Unity wind zones).
    void setWind(float3 wind);
    // Towards the sun: shading of mesh particles (Renderer3D::lighting.sunDir).
    void setLightDir(float3 dir);
    // Curved worlds (planets), all optional; unset = the flat behaviour, unchanged:
    //  - up(pos): local "up" at a particle position. Gravity / wind (given with +Y up) turn onto it;
    //    ground-aligned / upright particles follow it.
    //  - altitude(pos): height above the ground along up ("collision" ground instead of setGroundHeight).
    //  - view space: particles live in their own stable space (planets: body-local of the camera's body) and
    //    are drawn through `toView` (that space -> the renderer's world); setView() takes the renderer's camera.
    void setUpFunction(std::function<float3(float3 pos)> fn);
    void setAltitudeFunction(std::function<float(float3 pos)> fn);
    void setViewSpace(const render::mat4* toView);

    // Effects by name: "explosion" loads "fx/explosion.json" through the file reader (once).
    // Returns -1 when missing or invalid; the reason is in errors().
    int effect(const std::string& name);
    // Adds or replaces an effect (editor live edit). Particles of a replaced effect are removed.
    int define(const std::string& name, const EffectDef& def);
    // Drop cached effects so the next effect() reloads them from files (editor: files changed).
    void reloadAll();
    const EffectDef* def(int id) const;
    const std::string& effectName(int id) const;
    const std::vector<std::string>& errors() const { return errors_; }
    void clearErrors() { errors_.clear(); }

    // `dir`: emission axis (the effect's "direction" is relative to it, +Y = dir). `scale` sizes
    // the whole effect. Returns an invalid handle when nothing was started (off, culled, budget).
    FxHandle play(int effect, float3 pos, float3 dir = {0, 1, 0}, float scale = 1, float4 tint = {1, 1, 1, 1});
    FxHandle play(const std::string& effectName, float3 pos, float3 dir = {0, 1, 0}, float scale = 1, float4 tint = {1, 1, 1, 1});
    void move(FxHandle h, float3 pos);                 // attached effects (trails, burning buildings)
    void move(FxHandle h, float3 pos, float3 dir);
    void stop(FxHandle h);                             // stops emitting; live particles fade out
    void kill(FxHandle h);                             // stops and removes its particles at once
    bool playing(FxHandle h) const;                    // still emitting (its particles may outlive it)
    void emitManual(FxHandle h);                       // fires the "manual" sub-emitters of its live particles
    void clear();                                      // everything (new match)

    void setView(const render::RTSCamera& cam, const render::mat4& viewProj, int fbW, int fbH);
    void update(float dt);
    void render(const FxFog& fog);
    // Lights of emitters with a "light" module (after update): appended to `out`, at most `maxLights`
    // in total (fewer on low tiers). Call between Renderer3D::beginScene and render.
    void collectLights(std::vector<FxPointLight>& out, int maxLights = 32) const;

    void snapshot(std::vector<FxParticleInfo>& out) const;   // every live particle (current size and colour)
    const FxStats& stats() const { return stats_; }
    void resetStats() { stats_.dropped = stats_.culled = 0; }

    // Built-in texture names (always available, no files): soft, spark, ring, smoke.
    static const std::vector<std::string>& builtinTextures();
    // Built-in meshes ("builtin:<name>"): cube, sphere, capsule, cylinder, plane, quad (Unity's primitives).
    static const std::vector<std::string>& builtinMeshes();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    FxQuality quality_ = FxQuality::Medium;
    FileReader reader_;
    std::vector<std::string> errors_;
    FxStats stats_;
    std::string error_;
};

} // namespace rts::fx
