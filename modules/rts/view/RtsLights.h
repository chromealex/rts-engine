// Dynamic lights of the RTS presentation: glowing types, projectiles and combat flashes.
//
// Data (all optional, in a type's "view" block of rules.json):
//   "light":      { "color": [1, 0.6, 0.3], "radius": 4, "intensity": 1.5, "height": 1.2,
//                   "flicker": 0.2, "whileBuilding": false }        constant light of the object
//   "attackLight":     { "color", "radius", "intensity", "time" }   muzzle flash when it fires
//   "projectileLight": { "color", "radius", "intensity" }           its projectiles in flight
//   "hitLight":        { ... }                                       impact of its attacks
//   "deathLight":      { ... }                                       explosion when it dies
// Each flash block may be `false` to switch the default off. Defaults: ranged weapons flash and
// their projectiles glow, splash weapons light the impact, every death flashes (structures big).
// Night for testing: RTS_NIGHT=0..1, or setNight() from the client.
#pragma once
#include <cstdint>
#include <vector>
#include "../../../engine/render/Renderer3D.h"
#include "../RtsData.h"
#include "../RtsMap.h"

namespace rts::game {

struct RtsScene;
class RtsView;

class RtsLights {
public:
    void init(const Rules& rules);
    void setNight(float n) { night_ = n; }
    float night() const { return night_; }
    // Adds this frame's lights to r.lights (call between beginScene and render).
    void draw(render::Renderer3D& r, const RtsScene& s, double time, const std::vector<uint8_t>* vision, const MapGrid& grid, const RtsView& view);

    struct Flash { bool on = false; render::float3 color; float radius = 0, intensity = 0, time = 0; };
    struct TypeLights {
        bool glow = false, whileBuilding = false;
        render::float3 color;
        float radius = 0, intensity = 0, height = 1, flicker = 0;
        Flash attack, projectile, hit, death;
    };
    const TypeLights& typeLights(uint16_t type) const { return types_[type]; }

private:
    struct Active { render::float3 pos, color; float radius, intensity, born, life; };
    void applyNight(render::Lighting& l);
    void consumeEvents(const RtsScene& s, double time, const std::vector<uint8_t>* vision, const MapGrid& grid, const RtsView& view);
    void emitActive(render::LightSystem& ls, double time);

    const Rules* rules_ = nullptr;
    std::vector<TypeLights> types_;
    std::vector<Active> active_;
    uint32_t lastTick_ = UINT32_MAX;
    float night_ = 0, appliedNight_ = 0;
    bool haveBase_ = false;
    render::Lighting base_;
};

} // namespace rts::game
