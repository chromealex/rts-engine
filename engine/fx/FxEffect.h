// Particle effects as data: one effect = one file `games/<id>/fx/<name>.json` with 1..8 emitters.
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
enum class FxBlend : uint8_t { Alpha, Additive };
enum class FxAlign : uint8_t { Camera, Velocity, Ground };
enum class FxShape : uint8_t { Point, Sphere, Disc, Ring };

const char* qualityName(FxQuality q);            // "off" | "low" | "medium" | "high"
bool qualityFromName(const std::string& s, FxQuality& out);

struct FxRange {
    float min = 0, max = 0;
    bool operator==(const FxRange&) const = default;
};
struct FxColorKey { float t = 0; float4 rgba{1, 1, 1, 1}; };
struct FxSizeKey { float t = 0; float k = 1; };

constexpr int kMaxEmitters = 8;

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
