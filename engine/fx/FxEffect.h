// Particle effects as data: one effect = one file `templates/<id>/fx/<name>.json` with 1..16 emitters.
// Presentation only (float math); nothing here is read by the simulation.
//
//   {
//     "emitters": [
//       { "texture": "builtin:smoke",          // builtin:soft|spark|ring|smoke, or a sprite path (texture loader)
//         "frames": [4, 4], "fps": 0,           // flipbook inside the sprite; fps 0 = whole flipbook over the life
//         "blend": "alpha",                     // alpha | additive (additive needs no sorting: prefer it)
//         "align": "camera",                    // camera | velocity (sparks, streaks) | ground (rings, scorch)
//         "burst": 12, "rate": 0, "perMeter": 0, // particles at start / per second / per metre moved (trails)
//         "delay": 0, "duration": 0.5, "loop": false,
//         "life": [0.6, 1.2], "speed": [1, 3], "size": [0.4, 0.8],
//         "rotation": [0, 360], "spin": [-60, 60],  // degrees, degrees per second
//         "direction": [0, 1, 0], "spread": 35,     // emission cone around direction (half angle, degrees)
//         "shape": "sphere", "radius": 0.3,          // point | sphere | disc (XZ) | ring (XZ circle)
//         "gravity": [0, -4, 0], "drag": 1.5, "stretch": 0.08,
//         "sizeOverLife": [[0, 0.6], [1, 1.8]],      // multiplier keys [t, k]
//         "color": [[0, "#ffd080ff"], [0.3, "#ff6020c0"], [1, "#30303000"]],  // keys [t, colour]
//         "minQuality": "medium"                     // skipped on lower tiers (low | medium | high)
//       }
//     ]
//   }
// Missing fields take the defaults below; unknown fields are errors (typos must not pass silently).
//
// Optional keys added for richer effects (Unity imports, docs/particles.md). All are off by default,
// so effects that don't use them behave exactly as before:
//   "blend": "premultiplied"                    // texture with premultiplied alpha (ONE, 1-srcAlpha)
//   "align": "upright"                          // billboard turning around Y only (Unity "vertical")
//   "aspect": [1, 1]                            // quad length / width (stretched: base length)
//   "offset": [0, 0, 0]                         // emitter position in effect space (like "direction")
//   "bursts": [[0.2, 10], { "time": 0.5, "count": [4, 8], "cycles": 3, "interval": 0.1 }]
//   "startColor": "#ff8040" | ["#ff8040", "#ffd080"] | [[0, "#.."], [1, "#.."]]   // random per particle
//   "space": "local"                            // live particles move with the playing effect
//   "maxParticles": 50                          // alive per played effect and emitter
//   "velocity": [0, 1, 0], "speedLimit": 5      // extra velocity (effect space); speed cap
//   "colorBySpeed": { "speed": [0, 5], "color": [[0, "#.."], [1, "#.."]] }
//   "sizeBySpeed":  { "speed": [0, 5], "size": [[0, 1], [1, 2]] }
//   "noise": { "strength": 1, "frequency": 0.5, "scroll": 0.2 }
//   "cycles": 2, "startFrame": [0, 3]           // flipbook repeats over the life; random first frame
//   "shape": "hemisphere" | "cone" | "box" | "line", "arc": 360, "thickness": 1, "box": [1, 1, 1]
//   "radial": 1                                 // -1..1: directions blend from the cone towards outward (1) / inward (-1)
//   "hidden": true                              // particles not drawn (they still carry trails, lights, sub-emitters)
//            (cone: disc of "radius", directions open to "spread" at the rim like Unity's cone)
//   "collision": { "height": 0, "bounce": 0.5, "dampen": 0.1, "lifeLoss": 0, "minKillSpeed": 0 }
//   "subEmitters": [{ "on": "birth" | "death" | "collision", "effect": "sparks", "chance": 1, "inheritColor": false }]
//   "trail": { "time": 0.3, "width": [[0, 1], [1, 0]], "color": [[0, "#.."], [1, "#..00"]], "texture": "builtin:soft",
//              "blend": "additive", "inheritColor": true, "sizeAffectsWidth": true, "minQuality": "medium" }
//   "light": { "color": "#ffc080", "radius": 3, "intensity": 1, "ratio": 1, "max": 8,
//              "useParticleColor": true, "sizeAffectsRadius": false, "alphaAffectsIntensity": true }
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "../data/Json.h"
#include "../render/Math3D.h"

namespace rts::fx {

using render::float2;
using render::float3;
using render::float4;

enum class FxQuality : uint8_t { Off, Low, Medium, High };
enum class FxBlend : uint8_t { Alpha, Additive, Premultiplied };
enum class FxAlign : uint8_t { Camera, Velocity, Ground, Upright };
enum class FxShape : uint8_t { Point, Sphere, Disc, Ring, Hemisphere, Cone, Box, Line };
enum class FxSubEvent : uint8_t { Birth, Death, Collision };

const char* qualityName(FxQuality q);            // "off" | "low" | "medium" | "high"
bool qualityFromName(const std::string& s, FxQuality& out);

struct FxRange {
    float min = 0, max = 0;
    bool operator==(const FxRange&) const = default;
};
struct FxColorKey { float t = 0; float4 rgba{1, 1, 1, 1}; };
struct FxSizeKey { float t = 0; float k = 1; };

constexpr int kMaxEmitters = 16;

struct FxBurst { float time = 0; FxRange count{1, 1}; int cycles = 1; float interval = 0; };
struct FxSubEmitter { FxSubEvent on = FxSubEvent::Death; std::string effect; float chance = 1; bool inheritColor = false; };
struct FxTrail {
    bool enabled = false;
    float time = 0.3f;                          // seconds a trail point lives
    std::vector<FxSizeKey> width{{0, 1}, {1, 0}};  // along the trail: 0 = head, 1 = tail (x particle size)
    std::vector<FxColorKey> color{{0, {1, 1, 1, 1}}, {1, {1, 1, 1, 0}}};
    std::string texture = "builtin:soft";
    FxBlend blend = FxBlend::Additive;
    bool inheritColor = true, sizeAffectsWidth = true;
    FxQuality minQuality = FxQuality::Medium;   // ribbons are the first thing to go on weak GPUs
    bool operator==(const FxTrail&) const;
};
struct FxLight {
    bool enabled = false;
    float4 color{1, 0.8f, 0.6f, 1};
    float radius = 3, intensity = 1, ratio = 1;
    int max = 8;
    bool useParticleColor = true, sizeAffectsRadius = false, alphaAffectsIntensity = true;
};
struct FxCollision { bool enabled = false; float height = 0, bounce = 0.5f, dampen = 0, lifeLoss = 0, minKillSpeed = 0; };
struct FxNoise { float strength = 0, frequency = 0.5f, scroll = 0; };

struct EmitterDef {
    std::string texture = "builtin:soft";
    int frameCols = 1, frameRows = 1;
    float fps = 0;
    FxBlend blend = FxBlend::Additive;
    FxAlign align = FxAlign::Camera;
    int burst = 0;
    float rate = 0, perMeter = 0;
    float delay = 0, duration = 0;
    bool loop = false;
    FxRange life{0.5f, 1.0f}, speed{1, 2}, size{0.3f, 0.5f}, rotation{0, 0}, spin{0, 0};
    float3 direction{0, 1, 0};
    float spread = 180;
    FxShape shape = FxShape::Point;
    float radius = 0;
    float3 gravity{0, 0, 0};
    float drag = 0, stretch = 0;
    std::vector<FxSizeKey> sizeOverLife{{0, 1}, {1, 1}};
    std::vector<FxColorKey> color{{0, {1, 1, 1, 1}}, {1, {1, 1, 1, 0}}};
    FxQuality minQuality = FxQuality::Low;

    // optional modules (see the top of this file)
    FxRange aspect{1, 1};
    float3 offset{0, 0, 0};
    std::vector<FxBurst> bursts;
    std::vector<FxColorKey> startColor;       // empty: white; 1 key: constant; 2 keys at t 0/1 from a pair: lerp
    bool startColorPair = false;               // ["#a", "#b"]: random between two colours (else: random point of the gradient)
    bool localSpace = false;
    int maxParticles = 0;                      // 0 = unlimited (only the global budget)
    float3 velocity{0, 0, 0};
    float speedLimit = 0;
    FxRange colorSpeed{0, 1}, sizeSpeed{0, 1};
    std::vector<FxColorKey> colorBySpeed;
    std::vector<FxSizeKey> sizeBySpeed;
    FxNoise noise;
    float cycles = 1;
    FxRange startFrame{0, 0};
    float arc = 360, thickness = 1, radial = 0;
    bool hidden = false;
    float3 box{1, 1, 1};
    FxCollision collision;
    std::vector<FxSubEmitter> subEmitters;
    FxTrail trail;
    FxLight light;

    bool continuous() const { return rate > 0 || perMeter > 0; }
    float endTime() const;  // when this emitter stops creating particles (loop: infinity)
};

struct EffectDef {
    std::vector<EmitterDef> emitters;
    bool looping() const;   // any looping emitter: lives until stop()
    float lifetime() const; // longest time until the last particle dies (looping: infinity)
};

// Parses an effect. `where` prefixes error messages ("fx/boom.json"). Returns false on errors;
// `out` is still filled with everything that parsed.
bool parseEffect(const Json& j, const std::string& where, EffectDef& out, std::vector<std::string>& errors);
// Writes only the fields that differ from the defaults, in a stable order (editor save, diffs).
Json effectToJson(const EffectDef& def);

bool parseColor(const Json& j, float4& out);  // "#rrggbb", "#rrggbbaa" or [r, g, b(, a)] in 0..1
std::string colorToHex(float4 c);

} // namespace rts::fx
