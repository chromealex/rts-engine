// Visual effects for the generic RTS module: turns simulation events into particle effects from
// the game's `fx/fx.json`. Presentation only — reads the sim and never writes into it (events are
// copied out by a read-only system at the end of each tick, exactly like RtsAudio).
//
//   "events":    { "died": "explosion", "died.structure": "explosion_big", "died.air": "air_burst",
//                  "hit": "spark", "trail": "smoke_trail", "damaged": { "effect": "fire", "below": 0.4 } },
//   "types":     { "tank": { "died": { "effect": "explosion_big", "scale": 1.3 },
//                            "weapons": { "cannon": { "attack": "muzzle_big", "hit": "boom", "trail": "shell" } } } },
//   "weapons":   { "bow": { "hit": "arrow_hit", "trail": "arrow_trail" } },   // by weapon id, any type
//   "abilities": { "heal": "heal_glow" }                                      // by ability id
//
// A binding is an effect name (fx/<name>.json), null (none), or
//   { "effect": "name", "height": 0.8, "scale": 1.2, "teamColor": true, "below": 0.5 }
// height: metres above the ground (air units also get the flying height); teamColor: tint with the
// owner's colour; below: "damaged" plays while hp is under this fraction (default 0.5).
// Lookup: type entry -> weapon/ability by id -> "events.<cue>.<structure|air|unit>" -> "events.<cue>".
// Cues: attack, hit, miss, died, spawned, ready, buildPlaced, buildDone, gathered, captured, levelUp,
// ability, upgraded, sold, regionClaimed, wave, trail (projectiles), damaged (looping on hurt objects).
#pragma once
#include <array>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "../../../engine/fx/ParticleSystem.h"
#include "../../../engine/sim/Simulation.h"
#include "../RtsData.h"

namespace rts::game {

struct SceneObject;

enum class FxCue : uint8_t {
    Attack, Hit, Miss, Died, Spawned, Ready, BuildPlaced, BuildDone, Gathered, Captured, LevelUp,
    Ability, Upgraded, Sold, RegionClaimed, Wave, Trail, Damaged, Count
};
const char* fxCueName(FxCue c);

class RtsFx {
public:
    struct Binding {
        int effect = -1;
        float height = 0, scale = 1, below = 0.5f;
        bool teamColor = false;
        bool set = false;   // given in fx.json (an explicit null switches a fallback off)
        bool valid() const { return effect >= 0; }
    };

    // `bank` = parsed fx/fx.json; effects are loaded into `ps` through its file reader. A missing or
    // broken effect is a warning (that cue stays without effect); a malformed bank returns false.
    bool init(fx::ParticleSystem& ps, const Rules& rules, const Json& bank, std::string* error = nullptr);
    const std::vector<std::string>& warnings() const { return warnings_; }

    // Ground height for effect positions (RtsView::heightAt) and team colours (RtsView::playerColor).
    void setHeightFn(std::function<float(float x, float z)> fn) { height_ = std::move(fn); }
    void setPlayerColorFn(std::function<fx::float4(int player)> fn) { playerColor_ = std::move(fn); }

    // Start/stop following a match. `viewer` = local player (0 = spectator: sees everything).
    void attach(Simulation& sim, uint8_t viewer);
    void detach();   // also stops attached effects

    // Once per frame after the simulation advanced and the scene was built: plays effects for the
    // events since the last call, keeps projectile trails and "damaged" loops on their objects.
    void update(Simulation& sim, const std::vector<SceneObject>* scene);

    int played() const { return played_; }   // effects started by events since attach (tests, overlay)

    static constexpr float kAirHeight = 2.4f;   // RtsView draws flyers this high

private:
    struct TypeFx {
        std::array<Binding, size_t(FxCue::Count)> cue;
        struct W { Binding attack, hit, trail; };
        std::vector<W> weapons;
        std::vector<Binding> abilities;
    };
    struct Collector;
    struct Attached { fx::FxHandle h; uint32_t frame; };

    Binding resolve(const Json& v, const std::string& where);
    const Binding& lookup(int type, FxCue c) const;
    const Binding& trailFor(int type) const;
    bool playAt(const Binding& b, const Vec3& pos, int type, fx::float3 dir, int owner);
    void handle(Simulation& sim, const SimEvent& ev);

    fx::ParticleSystem* ps_ = nullptr;
    const Rules* rules_ = nullptr;
    std::array<Binding, size_t(FxCue::Count)> global_{};
    std::array<std::array<Binding, 3>, size_t(FxCue::Count)> category_{};   // structure / air / unit
    std::vector<TypeFx> types_;
    std::vector<std::string> warnings_;
    std::shared_ptr<Collector> collector_;
    std::function<float(float, float)> height_;
    std::function<fx::float4(int)> playerColor_;
    std::unordered_map<uint64_t, Attached> trails_, damaged_;
    uint32_t frame_ = 0;
    uint8_t viewer_ = 0;
    int played_ = 0;
};

} // namespace rts::game
