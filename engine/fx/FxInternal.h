// Internal state of ParticleSystem (engine/fx only): shared by ParticleSystem.cpp (API, simulation),
// FxEmit.cpp (spawning, shapes), FxRender.cpp (quads, trails, ribbons, meshes) and FxMeshes.cpp.
#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include "ParticleSystem.h"

namespace rts::fx {

using render::mat4;
using render::RTSCamera;

namespace detail {

constexpr float kPi = 3.14159265358979f;
constexpr float kDeg = kPi / 180.0f;
constexpr int kLut = 32;                     // colour / size curve samples
constexpr uint32_t kQuadsPerDraw = 16384;    // 16-bit indices: 65536 vertices
constexpr int kMaxInstances = 2048;
constexpr int kTrailPts = 9;                 // stored trail points per particle (high tier draws all, medium every other)
constexpr int kMaxSubDepth = 3;              // sub-emitters of sub-emitters of ...: recursion stops here
constexpr size_t kMaxPendingSubs = 256;      // sub-effects started per frame at most

struct Vertex { float3 pos; float uv[2]; uint32_t rgba; };
static_assert(sizeof(Vertex) == 24, "particle vertex must stay 24 bytes");

inline uint32_t packColor(float4 c) {
    auto q = [](float v) { return uint32_t(v <= 0 ? 0 : v >= 1 ? 255 : v * 255.0f + 0.5f); };
    return q(c.x) | (q(c.y) << 8) | (q(c.z) << 16) | (q(c.w) << 24);
}
inline float aces(float x) { return std::clamp((x * (2.51f * x + 0.03f)) / (x * (2.43f * x + 0.59f) + 0.14f), 0.0f, 1.0f); }

// Small, fast, deterministic per system (visual only).
struct Rng {
    uint32_t s = 0x9E3779B9u;
    uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
    float unit() { return float(next() >> 8) * (1.0f / 16777216.0f); }      // [0, 1)
    float range(const FxRange& r) { return r.min + (r.max - r.min) * unit(); }
    // a value at time t (curves) or a random value of the range (one draw, like range())
    float value(const FxRange& r, float t) { return r.curved() ? r.at(t, unit()) : range(r); }
    int roundCount(float x) { int n = int(x); return n + (unit() < x - float(n) ? 1 : 0); }
    float3 unitVector() {
        float z = unit() * 2 - 1, a = unit() * 2 * kPi, rr = std::sqrt(std::max(0.0f, 1 - z * z));
        return {rr * std::cos(a), z, rr * std::sin(a)};
    }
};
// Stable random per particle and purpose (over-lifetime curves: one random for the whole life).
inline float prand(uint32_t uid, uint32_t salt) {
    uint32_t h = uid * 2654435761u ^ (salt * 0x85ebca6bu + 0x9e3779b9u);
    h ^= h >> 15; h *= 0x2c1b3c6du; h ^= h >> 12; h *= 0x297a2d39u; h ^= h >> 15;
    return float(h >> 8) * (1.0f / 16777216.0f);
}

inline float4 operator+(float4 a, float4 b) { return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w}; }
inline float4 operator-(float4 a, float4 b) { return {a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w}; }
inline float4 operator*(float4 a, float s) { return {a.x * s, a.y * s, a.z * s, a.w * s}; }
inline float4 mul(float4 a, float4 b) { return {a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w}; }
inline float4 lerp4(float4 a, float4 b, float t) { return a + (b - a) * t; }
inline float3 mul3(float3 a, float3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }

template <class T, size_t N>
void sampleCurve(std::array<T, N>& lut, const std::vector<std::pair<float, T>>& keys, T def) {
    for (size_t i = 0; i < N; ++i) {
        float t = float(i) / float(N - 1);
        if (keys.empty()) { lut[i] = def; continue; }
        if (t <= keys.front().first) { lut[i] = keys.front().second; continue; }
        if (t >= keys.back().first) { lut[i] = keys.back().second; continue; }
        for (size_t k = 1; k < keys.size(); ++k)
            if (t <= keys[k].first) {
                float span = std::max(1e-6f, keys[k].first - keys[k - 1].first);
                float f = (t - keys[k - 1].first) / span;
                lut[i] = keys[k - 1].second + (keys[k].second - keys[k - 1].second) * f;
                break;
            }
    }
}
using ColorLut = std::array<float4, kLut>;
using ScalarLut = std::array<float, kLut>;
inline void lutColor(ColorLut& lut, const std::vector<FxColorKey>& keys) {
    std::vector<std::pair<float, float4>> k;
    for (auto& x : keys) k.push_back({x.t, x.rgba});
    sampleCurve(lut, k, float4{1, 1, 1, 1});
}
inline void lutScalar(ScalarLut& lut, const std::vector<FxSizeKey>& keys, float def = 1) {
    std::vector<std::pair<float, float>> k;
    for (auto& x : keys) k.push_back({x.t, x.k});
    sampleCurve(lut, k, def);
}
inline size_t lutIndex(float t) { return size_t(std::min(kLut - 1, std::max(0, int(t * float(kLut - 1) + 0.5f)))); }
inline float speedFrac(const FxRange& range, float speed) {
    return range.max > range.min ? std::clamp((speed - range.min) / (range.max - range.min), 0.0f, 1.0f) : 0.0f;
}
template <class T>
T speedLut(const std::array<T, kLut>& lut, const FxRange& range, float speed) { return lut[lutIndex(speedFrac(range, speed))]; }

inline void basisFrom(float3 n, float3& t, float3& b) {
    float3 ref = std::fabs(n.y) < 0.99f ? float3{0, 1, 0} : float3{1, 0, 0};
    t = render::normalize(render::cross(ref, n));
    b = render::cross(n, t);
}
// v given with +Y as up, turned by the shortest rotation that takes +Y onto `up` (curved worlds)
inline float3 upRotate(float3 up, float3 v) {
    const float c = up.y;
    if (c > 0.99999f) return v;
    if (c < -0.99999f) return {v.x, -v.y, -v.z};
    const float3 k = render::normalize(float3{up.z, 0, -up.x});   // axis = Y x up
    const float sn = std::sqrt(std::max(0.0f, 1 - c * c));
    return v * c + render::cross(k, v) * sn + k * (render::dot(k, v) * (1 - c));
}

// 3x3 rotation (columns), Euler degrees applied Z, then X, then Y (R = Ry * Rx * Rz, Unity's order).
struct Mat3 {
    float3 c0{1, 0, 0}, c1{0, 1, 0}, c2{0, 0, 1};
    float3 operator*(float3 v) const { return c0 * v.x + c1 * v.y + c2 * v.z; }
    bool identity = true;
    static Mat3 euler(float3 deg) {
        Mat3 m;
        if (deg.x == 0 && deg.y == 0 && deg.z == 0) return m;
        const float cx = std::cos(deg.x * kDeg), sx = std::sin(deg.x * kDeg), cy = std::cos(deg.y * kDeg), sy = std::sin(deg.y * kDeg),
                    cz = std::cos(deg.z * kDeg), sz = std::sin(deg.z * kDeg);
        auto rz = [&](float3 v) { return float3{v.x * cz - v.y * sz, v.x * sz + v.y * cz, v.z}; };
        auto rx = [&](float3 v) { return float3{v.x, v.y * cx - v.z * sx, v.y * sx + v.z * cx}; };
        auto ry = [&](float3 v) { return float3{v.x * cy + v.z * sy, v.y, -v.x * sy + v.z * cy}; };
        m.c0 = ry(rx(rz({1, 0, 0}))); m.c1 = ry(rx(rz({0, 1, 0}))); m.c2 = ry(rx(rz({0, 0, 1})));
        m.identity = false;
        return m;
    }
};
// Rotate v around a unit axis by angle (radians).
inline float3 rotateAxis(float3 v, float3 axis, float a) {
    const float c = std::cos(a), s = std::sin(a);
    return v * c + render::cross(axis, v) * s + axis * (render::dot(axis, v) * (1 - c));
}

// Built-in meshes (FxMeshes.cpp); false for unknown names.
bool builtinMesh(const std::string& name, FxMesh& out);

} // namespace detail

using namespace detail;

struct ParticleSystem::Impl {
    struct Batch { uint32_t texId; FxBlend blend; gfx::TextureHandle tex; };
    struct MeshRT {
        std::vector<float3> pos, nrm;
        std::vector<float2> uv;
        std::vector<uint32_t> idx;
        std::vector<float> area;        // cumulative triangle areas (shape: emit from triangles)
        float radius = 0.5f;            // bounding radius (culling, extent)
        bool valid = false;
    };
    struct EmitterRT {
        FxTexture tex;
        uint16_t batch = 0;
        ColorLut color;
        ScalarLut size;
        int frames = 1;
        float du = 1, dv = 1;          // one flipbook cell in UV
        // optional modules (FxEffect.h); `modules` false keeps the original per-particle path
        bool modules = false;
        ColorLut startLut, startMinLut, colorSpeed, trailColor, trailLife, colorMinLut;
        ScalarLut sizeSpeed, trailWidth, sizeMinLut, sizeYLut, sizeZLut, spinSpeed, lightRadius, lightIntensity, noiseRemap;
        FxTexture trailTex;
        uint16_t trailBatch = 0;
        std::array<int, kMaxSubEmitters> subIds;   // -2: not looked up yet, -1: missing
        Mat3 shapeRot;
        int shapeMesh = -1;
        std::vector<int> meshIds;                  // mesh particles (indices into Impl::meshes)
        std::vector<FxTexture> spriteTex;          // "sprites" flipbook
        std::vector<uint16_t> spriteBatch;
        bool anim = false;                         // velocity not integrated (velocityOverLife, orbital, inherit current)
        EmitterRT() { subIds.fill(-2); }
    };
    struct EffectRT {
        std::string name;
        bool valid = false;
        EffectDef def;
        std::vector<EmitterRT> em;
        bool looping = false;
        float emitEnd = 0;             // last time any emitter creates particles
        float extent = 1;              // rough radius reached by its particles (culling)
    };
    struct Instance {
        uint16_t gen = 1;
        bool used = false, stopped = false;
        bool emitting = true;          // false: done emitting; the slot lives on while it has particles
        int effect = -1;
        float3 pos, prev, dir{0, 1, 0};
        float scale = 1;
        float4 tint{1, 1, 1, 1};
        float time = 0;
        std::array<float, kMaxEmitters> accum{};
        std::array<bool, kMaxEmitters> burstDone{};
        std::array<uint16_t, kMaxEmitters> alive{};   // live particles per emitter ("maxParticles")
        std::array<float, kMaxEmitters> delay{};      // start delay chosen for this play
        std::array<float, kMaxEmitters> arcPhase{};   // "arcMode" loop / pingpong
        uint32_t live = 0;                             // live particles of every emitter
        float3 delta{0, 0, 0};                         // moved this frame ("space": "local")
        float3 vel{0, 0, 0};                           // velocity from move() ("inheritVelocity", "lifeBySpeed")
        uint8_t depth = 0;                             // sub-emitter nesting
        // sub-emitter inheritance
        float sizeMul = 1, rotAdd = 0, lifeMul = 1, timeScale = 1;
        // toWorld basis of the play direction (effect space -> world)
        float3 bt{0, 0, 1}, bb{1, 0, 0};
        float3 toWorld(float3 v) const { return bt * v.x + dir * v.y + bb * v.z; }
        void setDir(float3 d) { dir = d; basisFrom(dir, bt, bb); }
    };
    struct Particle {
        float3 pos, vel;
        float age, invLife;
        float size, rot, spin, scale;
        float4 tint;
        uint16_t effect, emitter, batch;
        uint32_t owner;                // handle id (kill)
        float3 addVel{0, 0, 0};        // "velocity" in world space
        float aspect = 1;
        uint32_t uid = 0;
        int32_t trail = -1;            // index into trails
        uint8_t frame0 = 0;
        uint8_t depth = 0;             // sub-emitter nesting of its effect (outlives the instance)
        bool child = false;            // has a birth sub-effect following it
        // the rest of Unity's modules
        float3 vtot{0, 0, 0};          // velocity used for drawing / by-speed modules (base + animated)
        float3 dir0{0, 1, 0};          // initial direction ("alignToDirection")
        float depthK = 1;              // size z / size x (meshes)
        float rotX = 0, rotY = 0;      // radians (meshes)
        float grav = 1;                // "gravityScale"
        float nsize = 1;               // noise size factor
        uint8_t mesh = 0, row = 0;
        uint8_t flags = 0;             // 1 flip U, 2 flip V, 4 inside a trigger volume, 8 inside (previous frame known)
    };
    struct Trail {
        std::array<float3, kTrailPts> p;
        std::array<float, kTrailPts> t;
        int head = 0, count = 0;
        float time = 0.3f;             // seconds a point lives (per particle: "lifeFraction")
        const float3& at(int j) const { return p[size_t((head - j + kTrailPts) % kTrailPts)]; }   // 0 = newest
        float timeAt(int j) const { return t[size_t((head - j + kTrailPts) % kTrailPts)]; }
    };
    // A trail whose particle died ("dieWithParticles": false): fades out on its own.
    struct Orphan { int32_t trail; uint16_t effect, emitter; float scale, headSize; float4 base; };
    struct SubSpawn {
        uint16_t effect, emitter; uint8_t sub, depth; float3 pos, dir; float scale; float4 tint; uint32_t uid;
        float sizeMul, rotAdd, lifeMul, timeScale;
    };
    enum RecKind : uint8_t { Quad, TrailSeg, RibbonSeg, OrphanSeg, MeshPart };
    struct Rec {
        uint32_t idx; float w, size; uint16_t batch; int16_t seg = -1;
        uint8_t kind = Quad; uint8_t group = 1; int8_t order = 0; float key = 0;
        uint32_t idx2 = 0; float f0 = 0, f1 = 0;    // ribbon: second particle, positions along the ribbon
    };

    gfx::Device* dev = nullptr;
    gfx::ShaderHandle shader, mulShader;
    gfx::PipelineHandle pipe[6], meshPipe[6];
    gfx::BufferHandle vb, ib;
    gfx::TextureHandle atlas;
    TextureLoader loader;
    MeshLoader meshLoader;
    std::unordered_map<std::string, FxTexture> textures;
    std::vector<MeshRT> meshes;
    std::unordered_map<std::string, int> meshByName;
    std::vector<Batch> batches;
    std::vector<EffectRT> effects;
    std::unordered_map<std::string, int> byName;
    std::vector<Instance> instances;
    std::vector<int> freeInstances;
    std::vector<Particle> particles;
    std::vector<Rec> recs;
    std::vector<Vertex> verts, meshVerts;
    Rng rng;
    QualityLimits limits = limitsFor(FxQuality::Medium);
    FxQuality quality = FxQuality::Medium;
    std::vector<Trail> trails;
    std::vector<int32_t> freeTrails;
    std::vector<Orphan> orphans;
    std::vector<SubSpawn> pending;
    std::unordered_map<uint32_t, FxHandle> children;   // particle uid -> its birth sub-effect
    std::function<float(float, float)> ground;
    std::function<float3(float3)> upFn;        // curved worlds (setUpFunction)
    std::function<float(float3)> altFn;        // curved worlds (setAltitudeFunction)
    bool haveSpace = false;                    // setViewSpace
    mat4 space;
    float3 wind{0, 0, 0};
    float3 lightDir = render::normalize(float3{0.55f, 1.0f, 0.35f});
    uint32_t nextUid = 0;
    float clock = 0;

    // view
    bool haveView = false;
    mat4 viewProj;
    float3 eye, right{1, 0, 0}, up{0, 1, 0};
    float3 lastEye{0, 0, 0}, camVel{0, 0, 0};
    bool haveLastEye = false;
    float p11 = 1, aspect = 1;
    int fbW = 1, fbH = 1;

    // ---- lookups
    Instance* inst(uint32_t id) {
        if (!id) return nullptr;
        size_t slot = (id & 0xffff) - 1;
        if (slot >= instances.size()) return nullptr;
        auto& in = instances[slot];
        return in.used && in.gen == uint16_t(id >> 16) ? &in : nullptr;
    }
    const EmitterDef& defOf(const Particle& p) const { return effects[p.effect].def.emitters[p.emitter]; }
    const EmitterRT& rtOf(const Particle& p) const { return effects[p.effect].em[p.emitter]; }
    float lifeT(const Particle& p) const { return std::min(p.age * p.invLife, 1.0f); }
    float4 colorNow(const Particle& p) const;
    float sizeNow(const Particle& p, float t) const;   // world size (life curves, by speed, noise; no screen caps)

    // ---- effects (ParticleSystem.cpp)
    FxTexture builtin(size_t i) const;
    FxTexture texture(const std::string& name, std::vector<std::string>& errors, const std::string& where);
    int mesh(const std::string& name, std::vector<std::string>& errors, const std::string& where);
    uint16_t batchFor(const FxTexture& t, FxBlend blend);
    void build(EffectRT& e, std::vector<std::string>& errors);

    // ---- particles (ParticleSystem.cpp)
    static int trailIndices(const Trail& tr, int step, int* out) {
        int n = 0;
        for (int j = 0; j < tr.count; j += step) out[n++] = j;
        if (n > 0 && out[n - 1] != tr.count - 1) out[n++] = tr.count - 1;
        return std::min(n, kTrailPts);
    }
    void release(Particle& p);
    void queueSub(const Particle& p, FxSubEvent on, float3 at);
    void die(size_t i, bool events);
    void freeInstance(size_t slot);
    void emitInstance(size_t slot, float dt, float spawn, int& dropped);
    void stepParticle(size_t& i, float dt, bool& removed);   // advances particle i (may remove it)

    // ---- spawning (FxEmit.cpp)
    // `u`: position along this frame's emission (0..1, arc loop / pingpong); burstFrac >= 0: index / count in a burst
    bool emit(Instance& in, uint32_t owner, int effect, int emitter, float3 at, int& dropped, float u = 0, float burstFrac = -1);
};

} // namespace rts::fx
