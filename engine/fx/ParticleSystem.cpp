#include "ParticleSystem.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <type_traits>
#include "FxNoise.h"
#include "ParticleShaders.h"

namespace rts::fx {

using render::mat4;
using render::RTSCamera;

using namespace detail;

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kDeg = kPi / 180.0f;
constexpr int kLut = 32;                     // colour / size curve samples
constexpr uint32_t kQuadsPerDraw = 16384;    // 16-bit indices: 65536 vertices
constexpr int kMaxInstances = 2048;

const std::vector<std::string> kBuiltins = {"soft", "spark", "ring", "smoke"};

struct Vertex { float3 pos; float uv[2]; uint32_t rgba; };
static_assert(sizeof(Vertex) == 24, "particle vertex must stay 24 bytes");

uint32_t packColor(float4 c) {
    auto q = [](float v) { return uint32_t(v <= 0 ? 0 : v >= 1 ? 255 : v * 255.0f + 0.5f); };
    return q(c.x) | (q(c.y) << 8) | (q(c.z) << 16) | (q(c.w) << 24);
}

float aces(float x) { return std::clamp((x * (2.51f * x + 0.03f)) / (x * (2.43f * x + 0.59f) + 0.14f), 0.0f, 1.0f); }

// Small, fast, deterministic per system (visual only).
struct Rng {
    uint32_t s = 0x9E3779B9u;
    uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
    float unit() { return float(next() >> 8) * (1.0f / 16777216.0f); }      // [0, 1)
    float range(FxRange r) { return r.min + (r.max - r.min) * unit(); }
    int roundCount(float x) { int n = int(x); return n + (unit() < x - float(n) ? 1 : 0); }
};

constexpr int kTrailPts = 9;              // stored ribbon points per particle (high tier draws all, medium every other)
constexpr int kMaxSubDepth = 3;           // sub-emitters of sub-emitters of ...: recursion stops here
constexpr size_t kMaxPendingSubs = 256;   // sub-effects started per frame at most

inline float4 operator+(float4 a, float4 b) { return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w}; }
inline float4 operator-(float4 a, float4 b) { return {a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w}; }
inline float4 operator*(float4 a, float s) { return {a.x * s, a.y * s, a.z * s, a.w * s}; }
inline float4 mul(float4 a, float4 b) { return {a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w}; }

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


template <class T>
T speedLut(const std::array<T, kLut>& lut, FxRange range, float speed) {
    float f = range.max > range.min ? std::clamp((speed - range.min) / (range.max - range.min), 0.0f, 1.0f) : 0.0f;
    return lut[size_t(std::min(kLut - 1, int(f * float(kLut - 1) + 0.5f)))];
}

void basisFrom(float3 n, float3& t, float3& b) {
    float3 ref = std::fabs(n.y) < 0.99f ? float3{0, 1, 0} : float3{1, 0, 0};
    t = render::normalize(render::cross(ref, n));
    b = render::cross(n, t);
}
// v given with +Y as up, rotated by the shortest rotation that takes +Y onto `up` (curved worlds)
float3 upRotate(float3 up, float3 v) {
    const float c = up.y;
    if (c > 0.99999f) return v;
    if (c < -0.99999f) return {v.x, -v.y, -v.z};
    const float3 k = render::normalize(float3{up.z, 0, -up.x});   // axis = Y x up
    const float sn = std::sqrt(std::max(0.0f, 1 - c * c));
    return v * c + render::cross(k, v) * sn + k * (render::dot(k, v) * (1 - c));
}

} // namespace

QualityLimits limitsFor(FxQuality q) {
    switch (q) {
    case FxQuality::Off: return {0, 0, 0, 0, false};
    case FxQuality::Low: return {600, 0.4f, 0.12f, 60, false};
    case FxQuality::Medium: return {2500, 0.7f, 0.25f, 120, true};
    case FxQuality::High: break;
    }
    return {12000, 1.0f, 0.45f, 250, true};
}

FxQuality defaultQualityFor(const std::string& n, bool mobile) {
    if (const char* q = std::getenv("RTS_PARTICLES")) {
        FxQuality v;
        if (qualityFromName(q, v)) return v;
    }
    if (n.find("llvmpipe") != std::string::npos || n.find("SwiftShader") != std::string::npos) return FxQuality::Low;
    if (mobile) {
        for (const char* weak : {"Mali-4", "Mali-T", "Mali-G31", "Mali-G51", "Mali-G52", "Adreno (TM) 3", "Adreno (TM) 4",
                                 "Adreno (TM) 50", "Adreno (TM) 51", "PowerVR SGX", "PowerVR Rogue GE", "PowerVR Rogue GX6"})
            if (n.find(weak) != std::string::npos) return FxQuality::Low;
        return FxQuality::Medium;
    }
    return FxQuality::High;
}

FxQuality defaultQualityFor(const gfx::Device& dev, bool mobile) { return defaultQualityFor(dev.info().name, mobile); }

const std::vector<std::string>& ParticleSystem::builtinTextures() { return kBuiltins; }

// ------------------------------------------------------------------------------------------------

struct ParticleSystem::Impl {
    struct Batch { uint32_t texId; FxBlend blend; gfx::TextureHandle tex; };
    struct EmitterRT {
        FxTexture tex;
        uint16_t batch = 0;
        std::array<float4, kLut> color;
        std::array<float, kLut> size;
        int frames = 1;
        float du = 1, dv = 1;          // one flipbook cell in UV
        // optional modules (FxEffect.h); `modules` false keeps the original per-particle path
        bool modules = false;
        std::array<float4, kLut> startLut, colorSpeed, trailColor;
        std::array<float, kLut> sizeSpeed, trailWidth;
        FxTexture trailTex;
        uint16_t trailBatch = 0;
        std::array<int, 4> subIds{-2, -2, -2, -2};   // -2: not looked up yet, -1: missing
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
        int effect = -1;
        float3 pos, prev, dir{0, 1, 0};
        float scale = 1;
        float4 tint{1, 1, 1, 1};
        float time = 0;
        std::array<float, kMaxEmitters> accum{};
        std::array<bool, kMaxEmitters> burstDone{};
        std::array<uint16_t, kMaxEmitters> alive{};   // live particles per emitter ("maxParticles")
        float3 delta{0, 0, 0};                         // moved this frame ("space": "local")
        uint8_t depth = 0;                             // sub-emitter nesting
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
    };
    struct Trail {
        std::array<float3, kTrailPts> p;
        std::array<float, kTrailPts> t;
        int head = 0, count = 0;
        const float3& at(int j) const { return p[size_t((head - j + kTrailPts) % kTrailPts)]; }   // 0 = newest
        float time(int j) const { return t[size_t((head - j + kTrailPts) % kTrailPts)]; }
    };
    struct SubSpawn { uint16_t effect, emitter; uint8_t sub, depth; float3 pos, dir; float scale; float4 tint; uint32_t uid; };
    struct Rec { uint32_t idx; float w, size; uint16_t batch; int16_t seg = -1; };   // seg >= 0: trail segment

    gfx::Device* dev = nullptr;
    gfx::ShaderHandle shader;
    gfx::PipelineHandle pipe[3];
    gfx::BufferHandle vb, ib;
    gfx::TextureHandle atlas;
    TextureLoader loader;
    std::unordered_map<std::string, FxTexture> textures;
    std::vector<Batch> batches;
    std::vector<EffectRT> effects;
    std::unordered_map<std::string, int> byName;
    std::vector<Instance> instances;
    std::vector<int> freeInstances;
    std::vector<Particle> particles;
    std::vector<Rec> recs;
    std::vector<Vertex> verts;
    std::vector<uint32_t> batchStart, batchCount;
    Rng rng;
    QualityLimits limits = limitsFor(FxQuality::Medium);
    FxQuality quality = FxQuality::Medium;
    std::vector<Trail> trails;
    std::vector<int32_t> freeTrails;
    std::vector<SubSpawn> pending;
    std::unordered_map<uint32_t, FxHandle> children;   // particle uid -> its birth sub-effect
    std::function<float(float, float)> ground;
    std::function<float3(float3)> upFn;        // curved worlds (setUpFunction)
    std::function<float(float3)> altFn;        // curved worlds (setAltitudeFunction)
    bool haveSpace = false;                    // setViewSpace
    mat4 space;
    uint32_t nextUid = 0;
    float clock = 0;

    Instance* inst(uint32_t id) {
        if (!id) return nullptr;
        size_t slot = (id & 0xffff) - 1;
        if (slot >= instances.size()) return nullptr;
        auto& in = instances[slot];
        return in.used && in.gen == uint16_t(id >> 16) ? &in : nullptr;
    }
    // Trail points used for drawing (indices into Trail, newest first): every `step`-th plus the oldest.
    // Returns their count = number of ribbon segments (the head particle is the first point).
    static int trailIndices(const Trail& tr, int step, int* out) {
        int n = 0;
        for (int j = 0; j < tr.count; j += step) out[n++] = j;
        if (n > 0 && out[n - 1] != tr.count - 1) out[n++] = tr.count - 1;
        return std::min(n, kTrailPts);
    }
    // Frees what a particle holds outside the pool (trail, attached sub-effect); no events.
    void release(Particle& p) {
        if (p.trail >= 0) { freeTrails.push_back(p.trail); p.trail = -1; }
        if (p.child) {
            auto it = children.find(p.uid);
            if (it != children.end()) { if (auto* c = inst(it->second.id)) c->stopped = true; children.erase(it); }
            p.child = false;
        }
    }
    float4 colorNow(const Particle& p) const {
        const auto& er = effects[p.effect].em[p.emitter];
        float t = std::min(p.age * p.invLife, 1.0f);
        int li = std::min(kLut - 1, int(t * float(kLut - 1) + 0.5f));
        return mul(er.color[size_t(li)], p.tint);
    }
    void queueSub(const Particle& p, FxSubEvent on, float3 at) {
        const int depthOf = p.depth;
        const EmitterDef& d = effects[p.effect].def.emitters[p.emitter];
        for (size_t k = 0; k < d.subEmitters.size(); ++k) {
            const FxSubEmitter& se = d.subEmitters[k];
            if (se.on != on || pending.size() >= kMaxPendingSubs || depthOf + 1 > kMaxSubDepth) continue;
            if (se.chance < 1 && rng.unit() >= se.chance) continue;
            const float3 dir{0, 1, 0};   // like Unity: a sub-emitter keeps its own orientation, not the particle's velocity
            float4 tint = p.tint;
            if (se.inheritColor) { float4 c = colorNow(p); tint = {c.x, c.y, c.z, p.tint.w}; }
            pending.push_back({p.effect, p.emitter, uint8_t(k), uint8_t(depthOf + 1), at, dir, p.scale, tint, on == FxSubEvent::Birth ? p.uid : 0u});
        }
    }
    // Removes particle i (swap with the last): death events, counters, trail.
    void die(size_t i, bool events) {
        Particle& p = particles[i];
        Instance* in = inst(p.owner);
        if (in && in->alive[p.emitter] > 0) --in->alive[p.emitter];
        if (events && effects[p.effect].em[p.emitter].modules) queueSub(p, FxSubEvent::Death, p.pos);
        release(p);
        p = particles.back();
        particles.pop_back();
    }

    // view
    bool haveView = false;
    mat4 viewProj;
    float3 eye, right{1, 0, 0}, up{0, 1, 0};
    float p11 = 1, aspect = 1;
    int fbW = 1, fbH = 1;

    FxTexture builtin(size_t i) const {
        FxTexture t;
        t.tex = atlas;
        const float W = float(kAtlasCell * kAtlasCells);
        t.u0 = (float(i) * kAtlasCell + 0.5f) / W; t.u1 = (float(i + 1) * kAtlasCell - 0.5f) / W;
        t.v0 = 0.5f / kAtlasCell; t.v1 = 1 - 0.5f / kAtlasCell;
        return t;
    }

    FxTexture texture(const std::string& name, std::vector<std::string>& errors, const std::string& where) {
        if (name.rfind("builtin:", 0) == 0) {
            std::string b = name.substr(8);
            for (size_t i = 0; i < kBuiltins.size(); ++i) if (b == kBuiltins[i]) return builtin(i);
            errors.push_back(where + ": unknown builtin texture '" + b + "' (soft, spark, ring, smoke)");
            return builtin(0);
        }
        auto it = textures.find(name);
        if (it != textures.end()) return it->second;
        FxTexture t;
        if (!dev) t = builtin(0);   // headless: texture names only matter for batching
        else if (!loader || !loader(name, t) || !t.tex.valid()) {
            errors.push_back(where + ": texture '" + name + "' could not be loaded" + (loader ? "" : " (no sprite loader connected)"));
            t = builtin(0);
        }
        textures[name] = t;
        return t;
    }

    uint16_t batchFor(const FxTexture& t, FxBlend blend) {
        for (size_t i = 0; i < batches.size(); ++i)
            if (batches[i].texId == t.tex.id && batches[i].blend == blend) return uint16_t(i);
        batches.push_back({t.tex.id, blend, t.tex});
        return uint16_t(batches.size() - 1);
    }

    void build(EffectRT& e, std::vector<std::string>& errors) {
        e.em.clear();
        e.looping = e.def.looping();
        e.emitEnd = 0;
        e.extent = 0.5f;
        for (size_t i = 0; i < e.def.emitters.size(); ++i) {
            const EmitterDef& d = e.def.emitters[i];
            EmitterRT r;
            r.tex = texture(d.texture, errors, "fx/" + e.name + ".json: emitters[" + std::to_string(i) + "]");
            r.batch = batchFor(r.tex, d.blend);
            std::vector<std::pair<float, float4>> ck;
            for (auto& k : d.color) ck.push_back({k.t, k.rgba});
            sampleCurve(r.color, ck, float4{1, 1, 1, 1});
            std::vector<std::pair<float, float>> sk;
            float maxK = 0;
            for (auto& k : d.sizeOverLife) { sk.push_back({k.t, k.k}); maxK = std::max(maxK, k.k); }
            sampleCurve(r.size, sk, 1.0f);
            r.frames = d.frameCols * d.frameRows;
            r.du = (r.tex.u1 - r.tex.u0) / float(d.frameCols);
            r.dv = (r.tex.v1 - r.tex.v0) / float(d.frameRows);
            const EmitterDef base;
            r.modules = d.localSpace || d.maxParticles > 0 || d.speedLimit > 0 || d.noise.strength > 0 || d.collision.enabled ||
                        !d.subEmitters.empty() || d.trail.enabled || d.light.enabled ||
                        d.velocity.x != 0 || d.velocity.y != 0 || d.velocity.z != 0;
            auto lutC = [](std::array<float4, kLut>& lut, const std::vector<FxColorKey>& keys) {
                std::vector<std::pair<float, float4>> k;
                for (auto& x : keys) k.push_back({x.t, x.rgba});
                sampleCurve(lut, k, float4{1, 1, 1, 1});
            };
            auto lutS = [](std::array<float, kLut>& lut, const std::vector<FxSizeKey>& keys) {
                std::vector<std::pair<float, float>> k;
                for (auto& x : keys) k.push_back({x.t, x.k});
                sampleCurve(lut, k, 1.0f);
            };
            lutC(r.startLut, d.startColor);
            lutC(r.colorSpeed, d.colorBySpeed);
            lutS(r.sizeSpeed, d.sizeBySpeed);
            if (d.trail.enabled) {
                lutC(r.trailColor, d.trail.color);
                lutS(r.trailWidth, d.trail.width);
                r.trailTex = texture(d.trail.texture, errors, "fx/" + e.name + ".json: emitters[" + std::to_string(i) + "].trail");
                r.trailBatch = batchFor(r.trailTex, d.trail.blend);
            }
            e.em.push_back(r);
            if (!d.loop) e.emitEnd = std::max(e.emitEnd, d.endTime());
            float g = render::length(d.gravity);
            float travel = d.drag > 0 ? d.speed.max / d.drag : d.speed.max * d.life.max;
            e.extent = std::max(e.extent, render::length(d.offset) + std::max(d.radius, render::length(d.box) * 0.5f) + std::min(travel, d.speed.max * d.life.max) + 0.5f * g * d.life.max * d.life.max +
                                          d.size.max * std::max(1.0f, maxK));
        }
    }

    bool emit(Instance& in, uint32_t owner, int effect, int emitter, float3 at, int& dropped) {
        if (int(particles.size()) >= limits.maxParticles) { ++dropped; return false; }
        const EmitterDef& d = effects[size_t(effect)].def.emitters[size_t(emitter)];
        const EmitterRT& er = effects[size_t(effect)].em[size_t(emitter)];
        if (d.maxParticles > 0 && in.alive[size_t(emitter)] >= d.maxParticles) return false;
        const float s = in.scale;
        float3 t, b;
        basisFrom(in.dir, t, b);
        auto toWorld = [&](float3 v) { return t * v.x + in.dir * v.y + b * v.z; };
        // angle around the axis: full circle keeps the exact original math (old effects unchanged)
        auto around = [&] { return d.arc >= 360 ? rng.unit() * 2 * kPi : rng.unit() * d.arc * kDeg; };
        // spawn position
        float3 off{0, 0, 0}, rim{0, 0, 0};
        float rimFrac = -1;
        if (d.shape == FxShape::Sphere) {
            float z = rng.unit() * 2 - 1, a = around(), rr = std::sqrt(std::max(0.0f, 1 - z * z));
            float u = rng.unit();
            float k = d.thickness >= 1 ? std::cbrt(u) : std::cbrt((1 - u) * std::pow(1 - d.thickness, 3.0f) + u);
            off = float3{rr * std::cos(a), z, rr * std::sin(a)} * (d.radius * k);
        } else if (d.shape == FxShape::Hemisphere) {
            float z = rng.unit(), a = around(), rr = std::sqrt(std::max(0.0f, 1 - z * z));
            float u = rng.unit();
            float k = d.thickness >= 1 ? std::cbrt(u) : std::cbrt((1 - u) * std::pow(1 - d.thickness, 3.0f) + u);
            off = toWorld(float3{rr * std::cos(a), z, rr * std::sin(a)} * (d.radius * k));
        } else if (d.shape == FxShape::Disc || d.shape == FxShape::Ring || d.shape == FxShape::Cone) {
            float a = around(), rr;
            if (d.shape == FxShape::Ring) rr = d.radius;
            else if (d.thickness >= 1) rr = d.radius * std::sqrt(rng.unit());
            else { float u = rng.unit(), in0 = (1 - d.thickness) * (1 - d.thickness); rr = d.radius * std::sqrt(in0 + (1 - in0) * u); }
            if (d.shape == FxShape::Cone) {   // the cone's base is perpendicular to its own axis (disc/ring: to the play direction)
                float3 ca = render::normalize(toWorld(d.direction)), ct, cb;
                basisFrom(ca, ct, cb);
                rim = ct * std::cos(a) + cb * std::sin(a);
                off = rim * rr;
                if (d.radius > 0) rimFrac = rr / d.radius;
            } else off = toWorld({rr * std::cos(a), 0, rr * std::sin(a)});
        } else if (d.shape == FxShape::Box) {
            float x = rng.unit() - 0.5f, y = rng.unit() - 0.5f, z = rng.unit() - 0.5f;
            off = toWorld({x * d.box.x, y * d.box.y, z * d.box.z});
        } else if (d.shape == FxShape::Line) {
            off = toWorld({(rng.unit() * 2 - 1) * d.radius, 0, 0});
        }
        const float3 local = off;   // shape point (radial directions start from the shape centre)
        if (d.offset.x != 0 || d.offset.y != 0 || d.offset.z != 0) off += toWorld(d.offset);
        // direction: cone around the (instance-relative) axis
        float3 axis = render::normalize(toWorld(d.direction));
        float3 dir;
        if (rimFrac >= 0) {   // Unity-style cone: the direction opens with the distance from the centre
            float ang = d.spread * kDeg * rimFrac;
            dir = render::normalize(axis * std::cos(ang) + rim * std::sin(ang));
        } else {
            float3 at2, ab;
            basisFrom(axis, at2, ab);
            float ct = 1 - rng.unit() * (1 - std::cos(d.spread * kDeg));
            float st = std::sqrt(std::max(0.0f, 1 - ct * ct)), ph = rng.unit() * 2 * kPi;
            dir = axis * ct + at2 * (st * std::cos(ph)) + ab * (st * std::sin(ph));
        }
        if (d.radial != 0 && render::length(local) > 1e-5f)   // outward from (or into) the shape centre
            dir = render::normalize(render::lerp(dir, render::normalize(local) * (d.radial > 0 ? 1.0f : -1.0f), std::fabs(d.radial)));
        Particle p;
        p.pos = at + off * s;
        p.vel = dir * (rng.range(d.speed) * s);
        p.age = 0;
        p.invLife = 1.0f / std::max(0.01f, rng.range(d.life));
        p.size = rng.range(d.size) * s;
        p.rot = rng.range(d.rotation) * kDeg;
        p.spin = rng.range(d.spin) * kDeg;
        p.scale = s;
        p.tint = in.tint;
        p.effect = uint16_t(effect);
        p.emitter = uint16_t(emitter);
        p.batch = er.batch;
        p.owner = owner;
        // optional modules: no RNG draws unless used (old effects keep their exact random sequence)
        if (!d.startColor.empty()) {
            float4 c = d.startColor.size() == 1 ? d.startColor[0].rgba : er.startLut[size_t(std::min(kLut - 1, int(rng.unit() * float(kLut - 1) + 0.5f)))];
            p.tint = mul(p.tint, c);
        }
        if (d.aspect.min != 1 || d.aspect.max != 1) p.aspect = rng.range(d.aspect);
        if (d.startFrame.max > 0) p.frame0 = uint8_t(std::min(255.0f, rng.range(d.startFrame)));
        if (d.velocity.x != 0 || d.velocity.y != 0 || d.velocity.z != 0) p.addVel = toWorld(d.velocity) * s;
        p.uid = ++nextUid == 0 ? ++nextUid : nextUid;
        p.depth = in.depth;
        if (d.trail.enabled && quality >= d.trail.minQuality) {
            if (freeTrails.empty()) { freeTrails.push_back(int32_t(trails.size())); trails.emplace_back(); }
            p.trail = freeTrails.back();
            freeTrails.pop_back();
            Trail& tr = trails[size_t(p.trail)];
            tr.count = 1; tr.head = 0; tr.p[0] = p.pos; tr.t[0] = clock;
        }
        ++in.alive[size_t(emitter)];
        if (er.modules && !d.subEmitters.empty()) {
            const size_t before = pending.size();
            queueSub(p, FxSubEvent::Birth, p.pos);
            p.child = pending.size() > before;
        }
        particles.push_back(p);
        return true;
    }
};

ParticleSystem::ParticleSystem() : impl_(std::make_unique<Impl>()) {}
ParticleSystem::~ParticleSystem() { shutdown(); }

bool ParticleSystem::init(gfx::Device* dev) {
    shutdown();
    auto& I = *impl_;
    I.dev = dev;
    I.limits = limitsFor(quality_);
    I.quality = quality_;
    I.particles.reserve(size_t(I.limits.maxParticles));
    if (!dev) return true;

    gfx::ShaderDesc sd;
    sd.glslVertex = shaders::kParticleVS; sd.glslFragment = shaders::kParticleFS; sd.msl = shaders::kParticleMSL;
    sd.textureSlots = 1; sd.uniformSlots = 1; sd.label = "particles";
    I.shader = dev->createShader(sd);
    if (!I.shader.valid()) { error_ = "particles: shader: " + dev->lastError(); I.dev = nullptr; return false; }
    for (int i = 0; i < 3; ++i) {
        gfx::PipelineDesc pd;
        pd.shader = I.shader;
        pd.attrs[0] = {0, gfx::VertexFormat::Float3, 0};
        pd.attrs[1] = {1, gfx::VertexFormat::Float2, 12};
        pd.attrs[2] = {2, gfx::VertexFormat::UByte4N, 20};
        pd.attrCount = 3;
        pd.stride = sizeof(Vertex);
        pd.indexType = gfx::IndexType::UInt16;
        pd.cull = gfx::CullMode::None;
        pd.blend = i == 0 ? gfx::BlendMode::Alpha : i == 1 ? gfx::BlendMode::Additive : gfx::BlendMode::Premultiplied;
        pd.depthTest = true;
        pd.depthWrite = false;
        pd.label = i == 0 ? "particles-alpha" : i == 1 ? "particles-add" : "particles-premul";
        I.pipe[i] = dev->createPipeline(pd);
        if (!I.pipe[i].valid()) { error_ = "particles: pipeline: " + dev->lastError(); I.dev = nullptr; return false; }
    }
    std::vector<uint16_t> idx(size_t(kQuadsPerDraw) * 6);
    for (uint32_t q = 0; q < kQuadsPerDraw; ++q) {
        uint16_t b = uint16_t(q * 4);
        uint16_t* o = &idx[size_t(q) * 6];
        o[0] = b; o[1] = uint16_t(b + 1); o[2] = uint16_t(b + 2); o[3] = b; o[4] = uint16_t(b + 2); o[5] = uint16_t(b + 3);
    }
    gfx::BufferDesc ibd;
    ibd.type = gfx::BufferType::Index; ibd.size = idx.size() * 2; ibd.data = idx.data(); ibd.label = "particles-ib";
    I.ib = dev->createBuffer(ibd);
    gfx::BufferDesc vbd;
    vbd.type = gfx::BufferType::Vertex; vbd.usage = gfx::Usage::Dynamic;
    vbd.size = size_t(std::min(I.limits.maxParticles, 4096) + 64) * 4 * sizeof(Vertex); vbd.label = "particles-vb";
    I.vb = dev->createBuffer(vbd);
    auto px = makeBuiltinAtlas();
    gfx::TextureDesc td;
    td.width = kAtlasCell * kAtlasCells; td.height = kAtlasCell; td.data = px.data(); td.label = "particles-builtin";
    I.atlas = dev->createTexture(td);
    return I.ib.valid() && I.vb.valid() && I.atlas.valid();
}

void ParticleSystem::shutdown() {
    if (!impl_) return;
    auto& I = *impl_;
    if (I.dev) {
        for (auto& p : I.pipe) if (p.valid()) I.dev->destroyPipeline(p);
        if (I.shader.valid()) I.dev->destroyShader(I.shader);
        if (I.vb.valid()) I.dev->destroyBuffer(I.vb);
        if (I.ib.valid()) I.dev->destroyBuffer(I.ib);
        if (I.atlas.valid()) I.dev->destroyTexture(I.atlas);
    }
    TextureLoader keep = std::move(I.loader);
    auto ground = std::move(I.ground);
    auto upFn = std::move(I.upFn);
    auto altFn = std::move(I.altFn);
    impl_ = std::make_unique<Impl>();
    impl_->loader = std::move(keep);
    impl_->ground = std::move(ground);
    impl_->upFn = std::move(upFn);
    impl_->altFn = std::move(altFn);
    impl_->quality = quality_;
    impl_->limits = limitsFor(quality_);
}

void ParticleSystem::setQuality(FxQuality q) {
    quality_ = q;
    auto& I = *impl_;
    I.quality = q;
    I.limits = limitsFor(q);
    while (int(I.particles.size()) > I.limits.maxParticles) I.die(I.particles.size() - 1, false);
    if (q == FxQuality::Off) clear();
}

void ParticleSystem::setGroundHeight(std::function<float(float, float)> fn) { impl_->ground = std::move(fn); }
void ParticleSystem::setUpFunction(std::function<float3(float3)> fn) { impl_->upFn = std::move(fn); }
void ParticleSystem::setAltitudeFunction(std::function<float(float3)> fn) { impl_->altFn = std::move(fn); }
void ParticleSystem::setViewSpace(const mat4* toView) {
    impl_->haveSpace = toView != nullptr;
    if (toView) impl_->space = *toView;
}

void ParticleSystem::setTextureLoader(TextureLoader l) {
    impl_->loader = std::move(l);
    impl_->textures.clear();
    for (auto& e : impl_->effects) if (e.valid) impl_->build(e, errors_);
}

int ParticleSystem::define(const std::string& name, const EffectDef& def) {
    auto& I = *impl_;
    int id;
    auto it = I.byName.find(name);
    if (it != I.byName.end()) {
        id = it->second;
        for (size_t i = I.particles.size(); i-- > 0;) if (I.particles[i].effect == uint16_t(id)) I.die(i, false);
        for (size_t i = 0; i < I.instances.size(); ++i)
            if (I.instances[i].used && I.instances[i].effect == id) { I.instances[i].used = false; I.freeInstances.push_back(int(i)); }
    } else {
        id = int(I.effects.size());
        I.effects.emplace_back();
        I.byName[name] = id;
    }
    auto& e = I.effects[size_t(id)];
    e.name = name;
    e.def = def;
    if (e.def.emitters.size() > size_t(kMaxEmitters)) e.def.emitters.resize(size_t(kMaxEmitters));
    e.valid = !def.emitters.empty();
    I.build(e, errors_);
    return e.valid ? id : -1;
}

int ParticleSystem::effect(const std::string& name) {
    auto& I = *impl_;
    auto it = I.byName.find(name);
    if (it != I.byName.end()) return I.effects[size_t(it->second)].valid ? it->second : -1;
    const std::string path = "fx/" + name + ".json";
    EffectDef def;
    std::string text;
    bool ok = false;
    if (!reader_) errors_.push_back(path + ": no file reader");
    else if (!reader_(path, text)) errors_.push_back(path + ": file not found");
    else {
        try { ok = parseEffect(Json::parse(text), path, def, errors_); }
        catch (const std::exception& ex) { errors_.push_back(path + ": " + ex.what()); }
    }
    if (!ok) {   // remembered as invalid: not retried every event
        I.byName[name] = int(I.effects.size());
        I.effects.emplace_back();
        I.effects.back().name = name;
        return -1;
    }
    return define(name, def);
}

void ParticleSystem::reloadAll() {
    auto& I = *impl_;
    clear();
    I.effects.clear();
    I.byName.clear();
    I.textures.clear();
    I.batches.clear();
}

const EffectDef* ParticleSystem::def(int id) const {
    auto& I = *impl_;
    return id >= 0 && size_t(id) < I.effects.size() && I.effects[size_t(id)].valid ? &I.effects[size_t(id)].def : nullptr;
}

const std::string& ParticleSystem::effectName(int id) const {
    static const std::string none;
    auto& I = *impl_;
    return id >= 0 && size_t(id) < I.effects.size() ? I.effects[size_t(id)].name : none;
}

FxHandle ParticleSystem::play(const std::string& name, float3 pos, float3 dir, float scale, float4 tint) {
    int id = effect(name);
    return id < 0 ? FxHandle{} : play(id, pos, dir, scale, tint);
}

FxHandle ParticleSystem::play(int effect, float3 pos, float3 dir, float scale, float4 tint) {
    auto& I = *impl_;
    if (quality_ == FxQuality::Off || effect < 0 || size_t(effect) >= I.effects.size() || !I.effects[size_t(effect)].valid) return {};
    auto& e = I.effects[size_t(effect)];
    if (!e.looping && I.haveView) {   // one-shots out of sight are not worth a single particle
        float r = e.extent * scale;
        float4 c = I.viewProj * float4{pos.x, pos.y, pos.z, 1};
        float m = r * I.p11 * 1.2f;
        bool off = c.w < -r || c.x - m > c.w || c.x + m < -c.w || c.y - m > c.w || c.y + m < -c.w;
        if (off || render::length(pos - I.eye) > I.limits.cullDistance + r) { ++stats_.culled; return {}; }
    }
    int slot;
    if (!I.freeInstances.empty()) { slot = I.freeInstances.back(); I.freeInstances.pop_back(); }
    else if (I.instances.size() < size_t(kMaxInstances)) { slot = int(I.instances.size()); I.instances.emplace_back(); }
    else { ++stats_.dropped; return {}; }
    auto& in = I.instances[size_t(slot)];
    uint16_t gen = uint16_t(in.gen + 1 == 0 ? 1 : in.gen + 1);
    in = {};
    in.gen = gen;
    in.used = true;
    in.effect = effect;
    in.pos = in.prev = pos;
    in.dir = render::length(dir) > 1e-5f ? render::normalize(dir) : float3{0, 1, 0};
    in.scale = std::max(0.01f, scale);
    in.tint = tint;
    return FxHandle{(uint32_t(gen) << 16) | uint32_t(slot + 1)};
}

namespace {
template <class I> auto* findInstance(I& impl, FxHandle h) {
    using T = std::remove_reference_t<decltype(impl.instances[0])>;
    if (!h.valid()) return static_cast<T*>(nullptr);
    size_t slot = (h.id & 0xffff) - 1;
    if (slot >= impl.instances.size()) return static_cast<T*>(nullptr);
    auto& in = impl.instances[slot];
    return in.used && in.gen == uint16_t(h.id >> 16) ? &in : static_cast<T*>(nullptr);
}
} // namespace

void ParticleSystem::move(FxHandle h, float3 pos) { if (auto* in = findInstance(*impl_, h)) in->pos = pos; }
void ParticleSystem::move(FxHandle h, float3 pos, float3 dir) {
    if (auto* in = findInstance(*impl_, h)) { in->pos = pos; if (render::length(dir) > 1e-5f) in->dir = render::normalize(dir); }
}
void ParticleSystem::stop(FxHandle h) { if (auto* in = findInstance(*impl_, h)) in->stopped = true; }
void ParticleSystem::kill(FxHandle h) {
    auto& I = *impl_;
    if (auto* in = findInstance(I, h)) {
        for (size_t i = I.particles.size(); i-- > 0;) if (I.particles[i].owner == h.id) I.die(i, false);
        in->used = false;
        I.freeInstances.push_back(int((h.id & 0xffff) - 1));
    }
}
bool ParticleSystem::playing(FxHandle h) const { return findInstance(*impl_, h) != nullptr; }

void ParticleSystem::clear() {
    auto& I = *impl_;
    I.particles.clear();
    I.trails.clear();
    I.freeTrails.clear();
    I.children.clear();
    I.pending.clear();
    I.freeInstances.clear();
    for (size_t i = 0; i < I.instances.size(); ++i) { I.instances[i].used = false; I.freeInstances.push_back(int(i)); }
}

void ParticleSystem::setView(const RTSCamera& cam, const mat4& viewProj, int fbW, int fbH) {
    auto& I = *impl_;
    I.haveView = true;
    I.viewProj = viewProj;
    I.eye = cam.eye();
    float3 f = render::normalize(cam.target - I.eye);
    float3 worldUp{0, 1, 0};
    if (I.haveSpace) { // particles live in their own space: camera into it, projection from it
        const mat4 inv = I.space.inverse();
        I.viewProj = viewProj * I.space;
        I.eye = inv.transformPoint(I.eye);
        f = render::normalize(inv.transformDir(f));
        worldUp = render::normalize(inv.transformDir(worldUp));
    }
    I.right = render::normalize(render::cross(f, worldUp));
    I.up = render::cross(I.right, f);
    I.p11 = 1.0f / std::tan(cam.fovY * 0.5f);
    I.fbW = std::max(1, fbW);
    I.fbH = std::max(1, fbH);
    I.aspect = float(I.fbW) / float(I.fbH);
}

void ParticleSystem::update(float dt) {
    auto& I = *impl_;
    dt = std::clamp(dt, 0.0f, 0.1f);
    const float spawn = I.limits.spawnScale;
    // emitters
    for (size_t slot = 0; slot < I.instances.size(); ++slot) {
        auto& in = I.instances[slot];
        if (!in.used) continue;
        const auto& e = I.effects[size_t(in.effect)];
        const uint32_t owner = (uint32_t(in.gen) << 16) | uint32_t(slot + 1);
        if (!in.stopped) {
            for (size_t k = 0; k < e.def.emitters.size(); ++k) {
                const EmitterDef& d = e.def.emitters[k];
                if (d.minQuality > quality_) continue;
                const float t0 = in.time - d.delay, t1 = in.time + dt - d.delay;
                if (t1 < 0) continue;
                if (!in.burstDone[k]) {
                    in.burstDone[k] = true;
                    int n = d.burst > 0 ? std::max(1, I.rng.roundCount(float(d.burst) * spawn)) : 0;
                    for (int i = 0; i < n; ++i) I.emit(in, owner, in.effect, int(k), in.pos, stats_.dropped);
                }
                for (auto& b : d.bursts) {   // timed bursts in [t0, t1); a looping emitter repeats them every duration
                    const float period = d.loop && d.duration > 0 ? d.duration : 0;
                    for (int c = 0; c < b.cycles; ++c) {
                        const float tb = b.time + float(c) * b.interval;
                        if (period <= 0 && (tb < t0 || tb >= t1)) continue;
                        float k0 = period > 0 ? std::max(0.0f, std::ceil((t0 - tb) / period)) : 0;
                        for (float tk = tb + k0 * period; tk < t1; tk += period) {
                            if (tk >= t0) {
                                int n = I.rng.roundCount(I.rng.range(b.count) * spawn);
                                for (int i = 0; i < n; ++i) I.emit(in, owner, in.effect, int(k), in.pos, stats_.dropped);
                            }
                            if (period <= 0) break;
                        }
                    }
                }
                const bool active = d.loop || t0 < d.duration;
                if (!active) continue;
                float count = 0;
                if (d.rate > 0) {
                    float a = std::max(t0, 0.0f), b = d.loop ? t1 : std::min(t1, d.duration);
                    if (b > a) count += d.rate * spawn * (b - a);
                }
                if (d.perMeter > 0) count += d.perMeter * spawn * render::length(in.pos - in.prev) / in.scale;
                in.accum[k] += count;
                int n = int(in.accum[k]);
                in.accum[k] -= float(n);
                for (int i = 0; i < n; ++i) {   // spread along the path moved this frame (smooth trails)
                    float f = (float(i) + I.rng.unit()) / float(n);
                    I.emit(in, owner, in.effect, int(k), render::lerp(in.prev, in.pos, f), stats_.dropped);
                }
            }
        }
        in.time += dt;
        in.delta = in.pos - in.prev;
        in.prev = in.pos;
        if (in.stopped || (!e.looping && in.time >= e.emitEnd)) { in.used = false; I.freeInstances.push_back(int(slot)); }
    }
    // particles
    I.clock += dt;
    for (size_t i = 0; i < I.particles.size();) {
        auto& p = I.particles[i];
        p.age += dt;
        if (p.age * p.invLife >= 1.0f) { I.die(i, true); continue; }
        const EmitterDef& d = I.effects[p.effect].def.emitters[p.emitter];
        const bool modules = I.effects[p.effect].em[p.emitter].modules;
        if (I.upFn) p.vel += upRotate(I.upFn(p.pos), d.gravity) * (dt * p.scale);   // curved worlds: gravity towards the local down
        else p.vel += d.gravity * (dt * p.scale);
        if (d.drag > 0) p.vel = p.vel * (1.0f / (1.0f + d.drag * dt));
        if (modules) {
            if (d.speedLimit > 0) {
                float sp = render::length(p.vel), lim = d.speedLimit * p.scale;
                if (sp > lim) p.vel = p.vel * (lim / sp);
            }
            if (d.localSpace) if (auto* in = I.inst(p.owner)) p.pos += in->delta;
            p.pos += p.addVel * dt;
            if (d.noise.strength > 0) {
                float f = d.noise.frequency / p.scale;
                p.pos += noise3(p.pos * f + float3{0, I.clock * d.noise.scroll, 0}) * (d.noise.strength * p.scale * dt);
            }
        }
        p.pos += p.vel * dt;
        p.rot += p.spin * dt;
        if (modules) {
            if (d.collision.enabled && I.altFn && I.upFn) { // curved worlds: the ground along the local up
                const float3 n = I.upFn(p.pos);
                const float below = d.collision.height * p.scale - I.altFn(p.pos);
                const float vn = render::dot(p.vel, n);
                if (below > 0 && vn < 0) {
                    p.pos += n * below;
                    const float3 vt = (p.vel - n * vn) * (1 - d.collision.dampen);
                    p.vel = vt - n * (vn * d.collision.bounce);
                    p.age += d.collision.lifeLoss / p.invLife;
                    I.queueSub(p, FxSubEvent::Collision, p.pos);
                    if (render::length(p.vel) < d.collision.minKillSpeed * p.scale || p.age * p.invLife >= 1.0f) { I.die(i, true); continue; }
                }
            } else if (d.collision.enabled) {
                float plane = (I.ground ? I.ground(p.pos.x, p.pos.z) : 0.0f) + d.collision.height * p.scale;
                if (p.pos.y < plane && p.vel.y < 0) {
                    p.pos.y = plane;
                    p.vel.y = -p.vel.y * d.collision.bounce;
                    p.vel.x *= 1 - d.collision.dampen; p.vel.z *= 1 - d.collision.dampen;
                    p.age += d.collision.lifeLoss / p.invLife;
                    I.queueSub(p, FxSubEvent::Collision, p.pos);
                    if (render::length(p.vel) < d.collision.minKillSpeed * p.scale || p.age * p.invLife >= 1.0f) { I.die(i, true); continue; }
                }
            }
            if (p.trail >= 0) {
                auto& tr = I.trails[size_t(p.trail)];
                const float step = d.trail.time / float(kTrailPts - 1);
                if (I.clock - tr.time(0) >= step) {
                    tr.head = (tr.head + 1) % kTrailPts;
                    tr.p[size_t(tr.head)] = p.pos; tr.t[size_t(tr.head)] = I.clock;
                    tr.count = std::min(tr.count + 1, kTrailPts);
                }
                while (tr.count > 1 && I.clock - tr.time(tr.count - 1) > d.trail.time) --tr.count;
            }
            if (p.child) {
                auto it = I.children.find(p.uid);
                if (it == I.children.end()) p.child = false;
                else if (auto* c = I.inst(it->second.id)) c->pos = p.pos;
                else { I.children.erase(it); p.child = false; }
            }
        }
        ++i;
    }
    // sub-emitters queued this frame (birth / death / collision): played as effects of their own
    if (!I.pending.empty()) {
        auto subs = std::move(I.pending);
        I.pending.clear();
        for (auto& sp : subs) {
            int id = I.effects[sp.effect].em[sp.emitter].subIds[sp.sub];
            if (id == -2) {
                const std::string name = I.effects[sp.effect].def.emitters[sp.emitter].subEmitters[sp.sub].effect;
                id = effect(name);   // may grow the effect list: no references kept across this call
                I.effects[sp.effect].em[sp.emitter].subIds[sp.sub] = id;
            }
            if (id < 0) continue;
            FxHandle h = play(id, sp.pos, sp.dir, sp.scale, sp.tint);
            if (auto* in = I.inst(h.id)) in->depth = sp.depth;
            if (sp.uid && h.valid()) I.children[sp.uid] = h;
        }
    }
    stats_.particles = int(I.particles.size());
    int live = 0;
    for (auto& in : I.instances) live += in.used ? 1 : 0;
    stats_.instances = live;
}

void ParticleSystem::render(const FxFog& fog) {
    auto& I = *impl_;
    stats_.drawn = stats_.batches = stats_.drawCalls = 0;
    stats_.fill = 0;
    if (!I.dev || !I.haveView || I.particles.empty() || quality_ == FxQuality::Off) return;

    // 1) visible particles -> records (clip-space culling, on-screen size cap, fill estimate)
    I.recs.clear();
    const float capK = I.limits.maxScreenSize * 2.0f / I.p11;      // size cap per unit of clip w
    const int trailStep = quality_ >= FxQuality::High ? 1 : 2;   // medium: half the ribbon points
    stats_.trailQuads = 0;
    for (uint32_t i = 0; i < I.particles.size(); ++i) {
        const auto& p = I.particles[i];
        const auto& er = I.effects[p.effect].em[p.emitter];
        const EmitterDef& d = I.effects[p.effect].def.emitters[p.emitter];
        float t = std::min(p.age * p.invLife, 1.0f);
        int li = std::min(kLut - 1, int(t * float(kLut - 1) + 0.5f));
        float4 c = I.viewProj * float4{p.pos.x, p.pos.y, p.pos.z, 1};
        if (c.w <= 0.05f) continue;
        if (p.trail >= 0 && quality_ >= d.trail.minQuality) {   // ribbon segments, independent of the head's visibility
            int idx[kTrailPts];
            const int segs = Impl::trailIndices(I.trails[size_t(p.trail)], trailStep, idx);
            for (int k = 0; k < segs; ++k) { I.recs.push_back({i, c.w, 0, er.trailBatch, int16_t(k)}); ++stats_.trailQuads; }
        }
        float size = std::min(p.size * er.size[size_t(li)], capK * c.w);
        if (!d.sizeBySpeed.empty()) size *= speedLut(er.sizeSpeed, d.sizeSpeed, render::length(p.vel) / p.scale);
        if (size <= 0) continue;
        float ext = size * I.p11 * (std::max(1.0f, p.aspect) + d.stretch * render::length(p.vel));
        if (c.x - ext > c.w || c.x + ext < -c.w || c.y - ext > c.w || c.y + ext < -c.w) continue;
        float4 col = mul(er.color[size_t(li)], p.tint);
        if (!d.colorBySpeed.empty()) col = mul(col, speedLut(er.colorSpeed, d.colorSpeed, render::length(p.vel) / p.scale));
        if (col.w <= 0.004f || d.hidden) continue;
        float fh = size * I.p11 / c.w * 0.5f;                        // fraction of screen height
        stats_.fill += fh * fh * p.aspect / I.aspect;
        I.recs.push_back({i, c.w, size, p.batch});
    }
    if (I.recs.empty()) return;

    // 2) order: alpha batches first (sorted back to front on capable tiers), then additive
    const bool sortAlpha = I.limits.sortAlpha;
    std::sort(I.recs.begin(), I.recs.end(), [&](const Impl::Rec& a, const Impl::Rec& b) {
        const bool aa = I.batches[a.batch].blend == FxBlend::Additive, ba = I.batches[b.batch].blend == FxBlend::Additive;
        if (aa != ba) return !aa;
        if (a.batch != b.batch) return a.batch < b.batch;
        return sortAlpha && !aa ? a.w > b.w : false;
    });

    // 3) expand quads on the CPU
    I.verts.resize(I.recs.size() * 4);
    Vertex* v = I.verts.data();
    auto packFor = [](FxBlend b, float4 c) {   // premultiplied textures want a premultiplied tint
        return packColor(b == FxBlend::Premultiplied ? float4{c.x * c.w, c.y * c.w, c.z * c.w, c.w} : c);
    };
    for (auto& r : I.recs) {
        const auto& p = I.particles[r.idx];
        const auto& er = I.effects[p.effect].em[p.emitter];
        const EmitterDef& d = I.effects[p.effect].def.emitters[p.emitter];
        float t = std::min(p.age * p.invLife, 1.0f);
        int li = std::min(kLut - 1, int(t * float(kLut - 1) + 0.5f));
        if (r.seg >= 0) {   // one ribbon segment: from point k to k+1 (point 0 = the particle itself)
            const auto& tr = I.trails[size_t(p.trail)];
            int idx[kTrailPts];
            const int segs = Impl::trailIndices(tr, trailStep, idx);
            const int k = std::min<int>(r.seg, segs - 1);
            auto point = [&](int q) { return q <= 0 ? p.pos : tr.at(idx[std::min(q - 1, segs - 1)]); };
            auto age = [&](int q) { return q <= 0 ? 0.0f : std::clamp((I.clock - tr.time(idx[std::min(q - 1, segs - 1)])) / d.trail.time, 0.0f, 1.0f); };
            float headSize = p.size * er.size[size_t(li)];
            float4 base = d.trail.inheritColor ? mul(er.color[size_t(li)], p.tint) : p.tint;
            const FxTrail& td = d.trail;
            auto corner = [&](int q, float3& left, float3& right, uint32_t& col, float& u) {
                float3 a = point(q), prev = point(q - 1), next = point(q + 1);
                float3 dir = q == 0 ? a - next : prev - next;
                float3 side = render::cross(dir, I.eye - a);
                float sl = render::length(side);
                side = sl > 1e-5f ? side * (1.0f / sl) : I.right;
                float f = age(q);
                int fi = std::min(kLut - 1, int(f * float(kLut - 1) + 0.5f));
                float w = er.trailWidth[size_t(fi)] * (td.sizeAffectsWidth ? headSize : p.scale) * 0.5f;
                left = a - side * w; right = a + side * w;
                col = packFor(td.blend, mul(er.trailColor[size_t(fi)], base));
                u = f;
            };
            float3 l0, r0, l1, r1;
            uint32_t c0, c1;
            float f0, f1;
            corner(k, l0, r0, c0, f0);
            corner(k + 1, l1, r1, c1, f1);
            const FxTexture& tx = er.trailTex;
            float ua = tx.u0 + (tx.u1 - tx.u0) * f0, ub = tx.u0 + (tx.u1 - tx.u0) * f1;
            v[0] = {l0, {ua, tx.v1}, c0};
            v[1] = {l1, {ub, tx.v1}, c1};
            v[2] = {r1, {ub, tx.v0}, c1};
            v[3] = {r0, {ua, tx.v0}, c0};
            v += 4;
            continue;
        }
        float4 colf = mul(er.color[size_t(li)], p.tint);
        if (!d.colorBySpeed.empty()) colf = mul(colf, speedLut(er.colorSpeed, d.colorSpeed, render::length(p.vel) / p.scale));
        const uint32_t col = packFor(d.blend, colf);
        const float h = r.size * 0.5f, hl = h * p.aspect;   // half width, half length
        float3 ax, ay;
        if (d.align == FxAlign::Velocity && render::length(p.vel) > 1e-3f) {
            float sp = render::length(p.vel);
            float3 axis = p.vel * (1.0f / sp);
            float3 side = render::cross(axis, render::normalize(I.eye - p.pos));
            float sl = render::length(side);
            side = sl > 1e-4f ? side * (1.0f / sl) : I.right;
            ax = side * h;
            ay = axis * (hl + d.stretch * sp * 0.5f);
        } else if (d.align == FxAlign::Ground && I.upFn) { // curved worlds: flat on the local ground
            float3 t, b;
            basisFrom(I.upFn(p.pos), t, b);
            float cs = std::cos(p.rot), sn = std::sin(p.rot);
            ax = (t * cs + b * sn) * h;
            ay = (b * cs - t * sn) * hl;
        } else if (d.align == FxAlign::Upright && I.upFn) {
            const float3 u = I.upFn(p.pos);
            float3 rx = I.right - u * render::dot(I.right, u);
            rx = render::length(rx) > 1e-4f ? render::normalize(rx) : float3{1, 0, 0};
            ax = rx * h;
            ay = u * hl;
        } else if (d.align == FxAlign::Ground) {
            float cs = std::cos(p.rot), sn = std::sin(p.rot);
            ax = float3{cs, 0, sn} * h;
            ay = float3{-sn, 0, cs} * hl;
        } else if (d.align == FxAlign::Upright) {
            float3 rx = render::length(float3{I.right.x, 0, I.right.z}) > 1e-4f ? render::normalize(float3{I.right.x, 0, I.right.z}) : float3{1, 0, 0};
            ax = rx * h;
            ay = float3{0, 1, 0} * hl;
        } else {
            float cs = std::cos(p.rot), sn = std::sin(p.rot);
            ax = (I.right * cs + I.up * sn) * h;
            ay = (I.up * cs - I.right * sn) * hl;
        }
        float3 pos = d.align == FxAlign::Ground ? p.pos + (I.upFn ? I.upFn(p.pos) * 0.03f : float3{0, 0.03f, 0}) : p.pos;
        float u0 = er.tex.u0, v0 = er.tex.v0, u1 = er.tex.u1, v1 = er.tex.v1;
        if (er.frames > 1) {
            int f = d.fps > 0 ? int(p.age * d.fps) % er.frames
                  : d.cycles == 1 ? std::min(er.frames - 1, int(t * float(er.frames)))
                                  : int(t * float(er.frames) * d.cycles) % er.frames;
            if (p.frame0) f = (f + p.frame0) % er.frames;
            int cx = f % d.frameCols, cy = f / d.frameCols;
            u0 = er.tex.u0 + er.du * float(cx); u1 = u0 + er.du;
            v0 = er.tex.v0 + er.dv * float(cy); v1 = v0 + er.dv;
        }
        v[0] = {pos - ax - ay, {u0, v1}, col};
        v[1] = {pos + ax - ay, {u1, v1}, col};
        v[2] = {pos + ax + ay, {u1, v0}, col};
        v[3] = {pos - ax + ay, {u0, v0}, col};
        v += 4;
    }
    I.dev->updateBuffer(I.vb, I.verts.data(), I.verts.size() * sizeof(Vertex));   // one upload per frame

    // 4) one draw per batch run (split at 16k quads)
    struct U { float viewProj[16]; float fog[4]; float cam[4]; float params[4]; } u;
    std::memcpy(u.viewProj, I.viewProj.m, sizeof u.viewProj);
    u.fog[0] = aces(fog.color.x * fog.color.x); u.fog[1] = aces(fog.color.y * fog.color.y); u.fog[2] = aces(fog.color.z * fog.color.z);
    u.fog[3] = fog.density;
    u.cam[0] = I.eye.x; u.cam[1] = I.eye.y; u.cam[2] = I.eye.z; u.cam[3] = 1;
    I.dev->setVertexBuffer(I.vb);
    I.dev->setIndexBuffer(I.ib);
    int lastPipe = -1;
    size_t i = 0;
    while (i < I.recs.size()) {
        const uint16_t b = I.recs[i].batch;
        size_t j = i;
        while (j < I.recs.size() && I.recs[j].batch == b) ++j;
        const auto& batch = I.batches[b];
        const int pi = batch.blend == FxBlend::Additive ? 1 : batch.blend == FxBlend::Premultiplied ? 2 : 0;
        if (pi != lastPipe) { I.dev->setPipeline(I.pipe[pi]); lastPipe = pi; }
        u.params[0] = pi == 1 ? 1.0f : 0.0f; u.params[1] = pi == 2 ? 1.0f : 0.0f; u.params[2] = u.params[3] = 0;
        I.dev->setUniforms(0, &u, sizeof u);
        I.dev->setTexture(0, batch.tex.valid() ? batch.tex : I.atlas);
        for (size_t q = i; q < j; q += kQuadsPerDraw) {
            uint32_t n = uint32_t(std::min<size_t>(kQuadsPerDraw, j - q));
            I.dev->drawIndexed(n * 6, 0, int32_t(q * 4));
            ++stats_.drawCalls;
        }
        ++stats_.batches;
        i = j;
    }
    stats_.drawn = int(I.recs.size());
}

void ParticleSystem::collectLights(std::vector<FxPointLight>& out, int maxLights) const {
    const auto& I = *impl_;
    if (quality_ == FxQuality::Off) return;
    if (quality_ == FxQuality::Low) maxLights = std::min(maxLights, 8);
    struct Count { uint32_t owner; uint16_t emitter; int n; };
    std::vector<Count> counts;
    int added = 0;
    for (const auto& p : I.particles) {
        if (added >= maxLights) break;
        const EmitterDef& d = I.effects[p.effect].def.emitters[p.emitter];
        if (!d.light.enabled) continue;
        const FxLight& l = d.light;
        if (l.ratio < 1 && float((p.uid * 2654435761u) >> 8) * (1.0f / 16777216.0f) >= l.ratio) continue;   // stable choice per particle
        auto it = std::find_if(counts.begin(), counts.end(), [&](const Count& c) { return c.owner == p.owner && c.emitter == p.emitter; });
        if (it == counts.end()) { counts.push_back({p.owner, p.emitter, 0}); it = counts.end() - 1; }
        if (it->n >= l.max) continue;
        ++it->n;
        const auto& er = I.effects[p.effect].em[p.emitter];
        float t = std::min(p.age * p.invLife, 1.0f);
        int li = std::min(kLut - 1, int(t * float(kLut - 1) + 0.5f));
        float4 pc = mul(er.color[size_t(li)], p.tint);
        FxPointLight fl;
        fl.pos = p.pos;
        fl.color = l.useParticleColor ? float3{l.color.x * pc.x, l.color.y * pc.y, l.color.z * pc.z} : float3{l.color.x, l.color.y, l.color.z};
        fl.radius = l.radius * p.scale * (l.sizeAffectsRadius ? p.size * er.size[size_t(li)] / p.scale : 1.0f);
        fl.intensity = l.intensity * (l.alphaAffectsIntensity ? pc.w : 1.0f);
        if (fl.intensity <= 0.01f || fl.radius <= 0.01f) continue;
        out.push_back(fl);
        ++added;
    }
}

} // namespace rts::fx
