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
//
// The rest of Unity's ParticleSystem (docs/particles.md § Unity modules). Off by default as well.
// Any range ("life", "speed", "size", "rotation", "rate", "spin", "drag", "speedLimit", ...) may also be
// a curve: { "curve": [[t, v], ...] } or, random between two curves, { "curve": [...], "curveMin": [...] }.
// Start values and emission rates follow the emitter time (t = time / duration), "...OverLife" values
// and spin / drag / speedLimit / noise strength the particle's age (t = age / life).
//   "delay": [0, 0.3]                           // random start delay per play
//   "prewarm": true                             // looping: starts as if it had run one duration
//   "bursts": [{ "time": 0, "count": 5, "cycles": 0, "interval": 0.2, "chance": 0.5 }]   // cycles 0 = forever
//   "shape": "donut" | "mesh", "donutRadius": 0.2, "length": 2, "emitFrom": "volume" | "shell" | "edge" | "vertex" | "triangle",
//   "mesh": "builtin:cube" | "models/rock.fbx",  "shapeRotation": [0, 0, 0],   // mesh shape scale: "box"
//   "randomDirection": 0.3, "sphericalDirection": 0, "randomPosition": 0.1, "alignToDirection": true,
//   "arcMode": "random" | "loop" | "pingpong" | "spread", "arcSpeed": 1, "arcSpread": 0.1
//   "sizeY": [0.5, 1], "sizeZ": 1, "rotation3D": { "x": [0, 360], "y": 0 }, "gravityScale": [0.5, 1]
//   "flipRotation": 0.5                         // chance to mirror a particle's rotation and spin
//   "startColor": { "gradient": [[t, "#.."], ...], "gradientMin": [...], "overTime": true }
//   "colorMin": [[t, "#.."], ...], "sizeOverLifeMin": [[t, k], ...], "sizeOverLifeY": [...], "sizeOverLifeZ": [...]
//   "spin3D": { "x": 90, "y": [-30, 30] }, "spinBySpeed": { "speed": [0, 5], "spin": [[0, 0], [1, 360]] }
//   "velocityOverLife": { "x": 0, "y": { "curve": [[0, 2], [1, 0]] }, "z": 0, "space": "local" | "world",
//                         "orbital": [0, 90, 0], "orbitalOffset": [0, 0, 0], "radial": 1, "speedModifier": [[0, 1], [1, 0.2]] }
//   "force": { "x": 0, "y": 2, "z": 0, "space": "world", "randomize": false }
//   "speedLimitAxes": { "x": 2, "y": 5, "z": 2, "space": "local" }, "limitDampen": 0.5, "dragBySize": true, "dragBySpeed": true
//   "inheritVelocity": { "mode": "initial" | "current", "amount": 0.5 }
//   "lifeBySpeed": { "speed": [0, 10], "factor": [[0, 1], [1, 0.3]] }
//   "externalForces": 1                         // effect "forceFields" and ParticleSystem::setWind act on it
//   "noise": { "strength": [[0, 1], [1, 0]], "axes": [1, 0.5, 1], "octaves": 3, "octaveMultiplier": 0.5, "octaveScale": 2,
//              "damping": true, "position": 1, "rotation": 90, "size": 0.3, "remap": [[0, -1], [1, 1]] }
//   "row": 2 | "random", "frameOverTime": { "curve": [[0, 0], [1, 1]] }, "framesBySpeed": [0, 10],
//   "flip": [0.5, 0], "sprites": ["fx/a.png", "fx/b.png"]
//   "align": "world" | "local", "pivot": [0, 0.5, 0], "cameraStretch": 0.2, "sort": "distance" | "oldest" | "youngest" | "none",
//   "sortOrder": 1, "minScreenSize": 0, "maxScreenSize": 0.3, "meshes": ["builtin:cube", "models/shard.obj"], "ringBuffer": true
//   "blend": "multiply" | "subtract" | "opaque"
//   "collision": { ..., "ground": true, "planes": [{ "point": [0, 0, 0], "normal": [0, 1, 0] }], "radius": 0.5, "maxKillSpeed": 50 }
//   "trigger": { "volumes": [{ "center": [0, 1, 0], "radius": 2 }], "inside": "kill", "outside": "ignore",
//                "enter": "callback", "exit": "ignore", "radius": 0.5 }        // callback fires "on": "trigger" sub-emitters
//   "subEmitters": [{ "on": "trigger" | "manual", ..., "inherit": ["color", "size", "rotation", "lifetime", "duration"] }]
//   "trail": { ..., "mode": "ribbon", "ribbons": 1, "ratio": 0.5, "lifeColor": [[t, "#.."], ...], "lifeFraction": 0.5,
//              "minDistance": 0.1, "textureMode": "tile", "dieWithParticles": false }
//   "light": { ..., "radiusOverLife": [[0, 1], [1, 0]], "intensityOverLife": [[0, 1], [1, 0]] }
// Effect level (next to "emitters"):
//   "forceFields": [{ "position": [0, 1, 0], "shape": "sphere" | "cylinder", "radius": 5, "startRadius": 0, "length": 4,
//                     "direction": [0, 0, 0], "gravity": 2, "gravityFocus": 0, "rotation": 90, "rotationAttraction": 0.5, "drag": 0 }]
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#include "../data/Json.h"
#include "../render/Math3D.h"

namespace rts::fx {

using render::float2;
using render::float3;
using render::float4;

enum class FxQuality : uint8_t { Off, Low, Medium, High };
enum class FxBlend : uint8_t { Alpha, Additive, Premultiplied, Multiply, Subtract, Opaque };
enum class FxAlign : uint8_t { Camera, Velocity, Ground, Upright, World, Local };
enum class FxShape : uint8_t { Point, Sphere, Disc, Ring, Hemisphere, Cone, Box, Line, Donut, Mesh };
enum class FxSubEvent : uint8_t { Birth, Death, Collision, Trigger, Manual };
enum class FxEmitFrom : uint8_t { Volume, Shell, Edge, Vertex, Triangle };
enum class FxArcMode : uint8_t { Random, Loop, PingPong, Spread };
enum class FxSort : uint8_t { Auto, None, Distance, Oldest, Youngest };
enum class FxTriggerAction : uint8_t { Ignore, Kill, Callback };
enum class FxTextureMode : uint8_t { Stretch, Tile, Distribute };

const char* qualityName(FxQuality q);            // "off" | "low" | "medium" | "high"
bool qualityFromName(const std::string& s, FxQuality& out);

struct FxSizeKey {
    float t = 0; float k = 1;
    bool operator==(const FxSizeKey&) const = default;
};
struct FxColorKey { float t = 0; float4 rgba{1, 1, 1, 1}; };

// A value: constant, random in [min, max], a curve over a normalized time, or random between two curves.
// With curves, min / max hold their overall bounds (culling, editor ranges).
struct FxRange {
    FxRange() = default;
    FxRange(float lo, float hi) : min(lo), max(hi) {}
    float min = 0, max = 0;
    std::vector<FxSizeKey> curve, curveMin;
    bool curved() const { return !curve.empty(); }
    bool constant() const { return !curved() && min == max; }
    float at(float t, float r) const;               // t 0..1 (clamped), r 0..1 (random per particle)
    float average() const;                          // over t and r
    void scale(float k) {                           // every value (bounds and curves) times k
        min *= k; max *= k;
        if (min > max) std::swap(min, max);
        for (auto& c : curve) c.k *= k;
        for (auto& c : curveMin) c.k *= k;
    }
    bool operator==(const FxRange&) const = default;
};
float sampleKeys(const std::vector<FxSizeKey>& keys, float t, float def = 1);   // piecewise linear, clamped
float4 sampleColor(const std::vector<FxColorKey>& keys, float t);

constexpr int kMaxEmitters = 16;
constexpr int kMaxSubEmitters = 8;

struct FxBurst { float time = 0; FxRange count{1, 1}; int cycles = 1; float interval = 0; float chance = 1; };   // cycles 0: forever
struct FxSubEmitter {
    FxSubEvent on = FxSubEvent::Death;
    std::string effect;
    float chance = 1;
    bool inheritColor = false, inheritSize = false, inheritRotation = false, inheritLifetime = false, inheritDuration = false;
};
struct FxTrail {
    bool enabled = false;
    float time = 0.3f;                          // seconds a trail point lives
    float lifeFraction = 0;                     // > 0: time = this x the particle's lifetime (Unity "lifetime" ratio)
    std::vector<FxSizeKey> width{{0, 1}, {1, 0}};  // along the trail: 0 = head, 1 = tail (x particle size)
    std::vector<FxColorKey> color{{0, {1, 1, 1, 1}}, {1, {1, 1, 1, 0}}};
    std::vector<FxColorKey> lifeColor;          // multiplies over the particle's lifetime (empty: white)
    std::string texture = "builtin:soft";
    FxBlend blend = FxBlend::Additive;
    bool inheritColor = true, sizeAffectsWidth = true, sizeAffectsLifetime = false;
    bool ribbon = false;                        // connect the particles of one emitter instead (Unity "ribbon" mode)
    int ribbons = 1;
    float ratio = 1;                            // share of particles that get a trail
    float minDistance = 0;                      // new point only after moving this far (effect units)
    FxTextureMode textureMode = FxTextureMode::Stretch;
    bool dieWithParticles = true;               // false: the trail fades out on its own after the particle died
    FxQuality minQuality = FxQuality::Medium;   // ribbons are the first thing to go on weak GPUs
    bool operator==(const FxTrail&) const;
};
struct FxLight {
    bool enabled = false;
    float4 color{1, 0.8f, 0.6f, 1};
    float radius = 3, intensity = 1, ratio = 1;
    int max = 8;
    bool useParticleColor = true, sizeAffectsRadius = false, alphaAffectsIntensity = true;
    std::vector<FxSizeKey> radiusOverLife, intensityOverLife;   // multipliers over the lifetime (empty: 1)
};
struct FxPlane { float3 point{0, 0, 0}, normal{0, 1, 0}; };
struct FxCollision {
    bool enabled = false;
    float height = 0, bounce = 0.5f, dampen = 0, lifeLoss = 0, minKillSpeed = 0, maxKillSpeed = 0;   // maxKillSpeed 0: none
    bool ground = true;                         // the terrain (setGroundHeight) + "height"
    std::vector<FxPlane> planes;                // effect space
    float radius = 0;                           // particle radius as a share of its size (Unity radius scale)
};
struct FxNoise {
    FxRange strength{0, 0};                     // over the lifetime
    float3 axes{1, 1, 1};                       // per-axis strength multipliers (Unity separate axes)
    float frequency = 0.5f, scroll = 0;
    int octaves = 1;                            // capped per tier: low 1, medium 2, high 4
    float octaveMultiplier = 0.5f, octaveScale = 2;
    bool damping = false;                       // strength proportional to frequency (Unity "damping")
    float position = 1, rotation = 0, size = 0; // how much the field moves / turns (deg/s) / scales particles
    std::vector<FxSizeKey> remap;               // noise -1..1 mapped through [[t 0..1, v -1..1], ...]
    bool enabled() const { return strength.max > 0 || strength.curved(); }
};
struct FxVelocityOverLife {
    bool enabled = false;
    FxRange x, y, z;                            // linear (effect or world axes)
    bool world = false;
    FxRange orbitalX, orbitalY, orbitalZ;       // degrees per second around the effect centre (+ offset)
    float3 orbitalOffset{0, 0, 0};
    FxRange radial;                             // away from the effect centre (units / s)
    FxRange speedModifier{1, 1};                // multiplies the whole velocity
};
struct FxForce { bool enabled = false; FxRange x, y, z; bool world = false, randomize = false; };
struct FxLimitAxes { bool enabled = false; FxRange x, y, z; bool world = false; };
struct FxInherit { bool enabled = false, current = false; FxRange amount{1, 1}; };
struct FxBySpeed { bool enabled = false; FxRange speed{0, 1}; std::vector<FxSizeKey> keys; };
struct FxSphere { float3 center{0, 0, 0}; float radius = 1; };
struct FxTriggerDef {
    bool enabled = false;
    std::vector<FxSphere> volumes;              // effect space
    FxTriggerAction inside = FxTriggerAction::Ignore, outside = FxTriggerAction::Ignore,
                    enter = FxTriggerAction::Ignore, exit = FxTriggerAction::Ignore;
    float radius = 0;                           // particle radius as a share of its size
};
struct FxForceField {
    float3 position{0, 0, 0};
    bool cylinder = false;                      // infinite along the effect Y axis (else a sphere)
    float radius = 5, startRadius = 0, length = 0;
    float3 direction{0, 0, 0};                  // constant force inside (effect space)
    float gravity = 0, gravityFocus = 0;        // pull towards the centre (negative pushes), focus 0..1
    float rotation = 0, rotationAttraction = 0; // vortex around Y (deg/s), pull into the orbit 0..1
    float drag = 0;
};

struct EmitterDef {
    std::string texture = "builtin:soft";
    int frameCols = 1, frameRows = 1;
    float fps = 0;
    FxBlend blend = FxBlend::Additive;
    FxAlign align = FxAlign::Camera;
    int burst = 0;
    FxRange rate{0, 0}, perMeter{0, 0};
    FxRange delay{0, 0};
    float duration = 0;
    bool loop = false;
    FxRange life{0.5f, 1.0f}, speed{1, 2}, size{0.3f, 0.5f}, rotation{0, 0}, spin{0, 0};
    float3 direction{0, 1, 0};
    float spread = 180;
    FxShape shape = FxShape::Point;
    float radius = 0;
    float3 gravity{0, 0, 0};
    FxRange drag{0, 0};
    float stretch = 0;
    std::vector<FxSizeKey> sizeOverLife{{0, 1}, {1, 1}};
    std::vector<FxColorKey> color{{0, {1, 1, 1, 1}}, {1, {1, 1, 1, 0}}};
    FxQuality minQuality = FxQuality::Low;

    // optional modules (see the top of this file)
    FxRange aspect{1, 1};
    float3 offset{0, 0, 0};
    std::vector<FxBurst> bursts;
    std::vector<FxColorKey> startColor;       // empty: white; 1 key: constant; 2 keys at t 0/1 from a pair: lerp
    bool startColorPair = false;               // ["#a", "#b"]: random between two colours (else: random point of the gradient)
    std::vector<FxColorKey> startColorMin;    // gradient form: random between the two gradients
    bool startColorOverTime = false;           // gradient form: evaluated at the emitter time (not a random point)
    bool localSpace = false;
    int maxParticles = 0;                      // 0 = unlimited (only the global budget)
    float3 velocity{0, 0, 0};
    FxRange speedLimit{0, 0};
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

    // the rest of Unity's modules
    bool prewarm = false;
    float donutRadius = 0.2f, length = 0;
    FxEmitFrom emitFrom = FxEmitFrom::Volume;
    std::string mesh;                          // shape "mesh"
    float3 shapeRotation{0, 0, 0};             // degrees
    float randomDirection = 0, sphericalDirection = 0, randomPosition = 0;
    bool alignToDirection = false;
    float flipRotation = 0;
    FxArcMode arcMode = FxArcMode::Random;
    float arcSpeed = 1, arcSpread = 0;
    bool size3D = false;
    FxRange sizeY{1, 1}, sizeZ{1, 1};
    bool rotation3D = false;
    FxRange rotationX{0, 0}, rotationY{0, 0};  // degrees (meshes)
    FxRange gravityScale{1, 1};
    std::vector<FxColorKey> colorMin;
    std::vector<FxSizeKey> sizeOverLifeMin, sizeOverLifeY, sizeOverLifeZ;
    bool spin3D = false;
    FxRange spinX{0, 0}, spinY{0, 0};          // degrees / s (meshes)
    FxBySpeed spinBySpeed;                     // keys: degrees / s
    FxVelocityOverLife velocityOverLife;
    FxForce force;
    FxLimitAxes speedLimitAxes;
    float limitDampen = 1;                     // share of the excess speed removed (1 = hard limit)
    bool dragBySize = false, dragBySpeed = false;
    FxInherit inheritVelocity;
    FxBySpeed lifeBySpeed;                     // keys: lifetime multiplier by the emitter's speed
    float externalForces = 0;
    int row = -1;                              // flipbook single row: -1 whole sheet, -2 random row
    FxRange frameOverTime;                     // curve 0..1 over the lifetime (empty: linear / fps)
    FxRange framesBySpeed{0, 0};               // max > 0: frame from the speed in this range
    float2 flip{0, 0};                         // chance to mirror U / V
    std::vector<std::string> sprites;          // flipbook of separate sprites (frame i = sprite i)
    float3 pivot{0, 0, 0};                     // in particle sizes
    float cameraStretch = 0;
    FxSort sort = FxSort::Auto;
    int sortOrder = 0;                         // higher draws later (on top)
    float minScreenSize = 0, maxScreenSize = 0;// share of the screen height (0: no limit beyond the tier's)
    std::vector<std::string> meshes;           // draw particles as these meshes (random per particle)
    bool ringBuffer = false;                   // with maxParticles: old particles stay until replaced
    FxTriggerDef trigger;

    bool continuous() const { return rate.max > 0 || rate.curved() || perMeter.max > 0 || perMeter.curved(); }
    float endTime() const;  // when this emitter stops creating particles (loop: infinity)
};

struct EffectDef {
    std::vector<EmitterDef> emitters;
    std::vector<FxForceField> forceFields;
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
