// Dynamic lights of the RTS presentation: glowing types, projectiles, combat flashes, day/night.
//
// Per type (all optional, in the type's "view" block of rules.json):
//   "light":      { "color": [1, 0.6, 0.3], "radius": 4, "intensity": 1.5, "height": 1.2,
//                   "flicker": 0.2, "halo": 0.8, "whileBuilding": false }   constant light
//                   ("halo": world size of the glow sprite, false = none; default radius * 0.2)
//   "attackLight":     { "color", "radius", "intensity", "time", "halo" }   muzzle flash
//   "projectileLight": { "color", "radius", "intensity", "halo" }           its projectiles
//   "hitLight":        { ... }                                              impact of its attacks
//   "deathLight":      { ... }                                              explosion when it dies
// A flash block may be `false` to switch the default off. Defaults: ranged weapons flash and
// their projectiles glow, splash weapons light the impact, every death flashes (structures big).
//
// Day/night: "lighting" block in game.json, overridden per map by the map's "lighting" block:
//   { "time": 0.5,        time of day at the start, 0..1 (0 midnight, 0.25 dawn, 0.5 noon, 0.75 dusk)
//     "dayLength": 600,   game seconds per full day (0 = the time of day stays)
//     "nightLevel": 1,    how dark the night gets, 0..1
//     "clouds": 0.5,      cloud shadow coverage 0..1 (0 = clear sky)
//     "cloudStrength": 0.45, "cloudScale": 1, "wind": [1.2, 0.5]   (world units / s)
//     "storm": 0 }        0..1: overcast + lightning flashes (deterministic in game time)
// Decorative lights (lanterns, lava, campfires) in the map's "lights" array:
//   [{ "x": 10, "z": 12, "height": 1, "color": [1, 0.6, 0.3], "radius": 4, "intensity": 1.5,
//      "flicker": 0.2, "halo": 0.8 }]   ("pos": [x, z] works too); shown once the cell is explored.
// Without the block the scene keeps the renderer's default (noon) lighting.
// Testing: RTS_TIME_OF_DAY=0..1 sets the start time, RTS_NIGHT=0..1 forces a fixed night amount.
// At night emissive materials glow brighter (Renderer3D::lights.setNightGlow).
#pragma once
#include <cstdint>
#include <vector>
#include "../../../engine/data/Json.h"
#include "../../../engine/render/Renderer3D.h"
#include "../RtsData.h"
#include "../RtsMap.h"

namespace rts::game {

struct RtsScene;
class RtsView;

class RtsLights {
public:
    void init(const Rules& rules);
    // Day/night settings: game.json "lighting", then the map's "lighting" (either may be null).
    void configure(const Json& gameLighting, const Json& mapLighting);
    // ... plus the map's decorative "lights" array.
    void configure(const Json& gameLighting, const Json& mapLighting, const Json& mapLights);
    void setNight(float n) { forcedNight_ = n; }          // < 0: follow the cycle
    // Editor preview: overrides time of day / clouds / storm until switched off.
    void setPreview(bool on, float timeOfDay, float clouds, float storm) { preview_ = on; previewTime_ = timeOfDay; previewClouds_ = clouds; previewStorm_ = storm; }
    struct MapLight { render::float3 color{1, 0.6f, 0.3f}; float x = 0, z = 0, height = 1, radius = 4, intensity = 1.5f, flicker = 0, halo = -1; };
    static std::vector<MapLight> parseMapLights(const Json& lights);
    const std::vector<MapLight>& mapLights() const { return mapLights_; }
    float night() const { return night_; }
    float lightning() const { return lightning_; }   // current lightning flash 0..1
    float timeOfDay() const { return timeOfDay_; }
    // Adds this frame's lights to r.lights (call between beginScene and render).
    void draw(render::Renderer3D& r, const RtsScene& s, double time, const std::vector<uint8_t>* vision, const MapGrid& grid, const RtsView& view);

    struct Flash { bool on = false; render::float3 color; float radius = 0, intensity = 0, time = 0, halo = -1; };
    struct TypeLights {
        bool glow = false, whileBuilding = false;
        render::float3 color;
        float radius = 0, intensity = 0, height = 1, flicker = 0, halo = 0;
        Flash attack, projectile, hit, death;
    };
    const TypeLights& typeLights(uint16_t type) const { return types_[type]; }

private:
    struct Active { render::float3 pos, color; float radius, intensity, born, life, halo; };
    void updateSky(render::Lighting& l, double seconds);
    void consumeEvents(const RtsScene& s, double time, const std::vector<uint8_t>* vision, const MapGrid& grid, const RtsView& view);
    void emitActive(render::LightSystem& ls, double time);

    const Rules* rules_ = nullptr;
    std::vector<TypeLights> types_;
    std::vector<Active> active_;
    uint32_t lastTick_ = UINT32_MAX;
    // day/night
    bool cycle_ = false;                 // a lighting block or a test override is present
    float startTime_ = 0.5f, dayLength_ = 0, nightLevel_ = 1, forcedNight_ = -1;
    float clouds_ = 0, cloudStrength_ = 0.45f, cloudScale_ = 1, windX_ = 1.2f, windZ_ = 0.5f, storm_ = 0;
    float lightning_ = 0;
    bool preview_ = false;
    float previewTime_ = 0.5f, previewClouds_ = 0, previewStorm_ = 0;
    std::vector<MapLight> mapLights_;
    float night_ = 0, dusk_ = 0, timeOfDay_ = 0.5f;
    bool haveBase_ = false, touched_ = false;
    render::Lighting base_;
};

} // namespace rts::game
