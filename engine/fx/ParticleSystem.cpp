#include "ParticleSystem.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <type_traits>
#include "ParticleShaders.h"

namespace rts::fx {

using render::mat4;
using render::RTSCamera;

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kDeg = kPi / 180.0f;
constexpr int kLut = 32;                     // colour / size curve samples
constexpr uint32_t kQuadsPerDraw = 16384;    // 16-bit indices: 65536 vertices
constexpr int kMaxInstances = 2048;
constexpr int kAtlasCell = 64, kAtlasCells = 4;

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

float hash2(int x, int y) {
    uint32_t h = uint32_t(x) * 374761393u + uint32_t(y) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return float((h ^ (h >> 16)) & 0xffff) / 65535.0f;
}
float valueNoise(float x, float y) {
    int xi = int(std::floor(x)), yi = int(std::floor(y));
    float fx = x - float(xi), fy = y - float(yi);
    fx = fx * fx * (3 - 2 * fx); fy = fy * fy * (3 - 2 * fy);
    float a = hash2(xi, yi), b = hash2(xi + 1, yi), c = hash2(xi, yi + 1), d = hash2(xi + 1, yi + 1);
    return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fy;
}

// 256x64 RGBA: soft | spark | ring | smoke. White RGB, shape in alpha (tinted by the particle colour).
std::vector<uint8_t> makeBuiltinAtlas() {
    const int W = kAtlasCell * kAtlasCells, H = kAtlasCell;
    std::vector<uint8_t> px(size_t(W) * H * 4, 0);
    for (int cell = 0; cell < kAtlasCells; ++cell)
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < kAtlasCell; ++x) {
                float u = (float(x) + 0.5f) / kAtlasCell * 2 - 1, v = (float(y) + 0.5f) / kAtlasCell * 2 - 1;
                float r = std::sqrt(u * u + v * v);
                float a = 0, l = 1;
                if (cell == 0) { float t = std::clamp(1 - r, 0.0f, 1.0f); a = t * t * (3 - 2 * t); }
                else if (cell == 1) a = std::exp(-r * r * 14) + 0.3f * std::exp(-r * r * 4);
                else if (cell == 2) { float d = (r - 0.72f) / 0.13f; a = std::exp(-d * d); }
                else {
                    float n = 0, amp = 0.5f, f = 3;
                    for (int o = 0; o < 4; ++o, amp *= 0.5f, f *= 2.1f) n += amp * valueNoise(u * f + 11.3f, v * f + 7.1f);
                    float t = std::clamp(1 - r, 0.0f, 1.0f);
                    a = t * t * (3 - 2 * t) * std::clamp(0.35f + n * 1.1f, 0.0f, 1.0f);
                    l = 0.78f + 0.22f * n;
                }
                if (r >= 0.98f) a = 0;   // empty border: no bleeding into the neighbour cell
                uint8_t* p = &px[(size_t(y) * W + size_t(cell * kAtlasCell + x)) * 4];
                p[0] = p[1] = p[2] = uint8_t(std::clamp(l, 0.0f, 1.0f) * 255 + 0.5f);
                p[3] = uint8_t(std::clamp(a, 0.0f, 1.0f) * 255 + 0.5f);
            }
    return px;
}

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


void basisFrom(float3 n, float3& t, float3& b) {
    float3 ref = std::fabs(n.y) < 0.99f ? float3{0, 1, 0} : float3{1, 0, 0};
    t = render::normalize(render::cross(ref, n));
    b = render::cross(n, t);
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
    };
    struct Particle {
        float3 pos, vel;
        float age, invLife;
        float size, rot, spin, scale;
        float4 tint;
        uint16_t effect, emitter, batch;
        uint32_t owner;                // handle id (kill)
    };
    struct Rec { uint32_t idx; float w, size; uint16_t batch; };

    gfx::Device* dev = nullptr;
    gfx::ShaderHandle shader;
    gfx::PipelineHandle pipe[2];
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
            e.em.push_back(r);
            if (!d.loop) e.emitEnd = std::max(e.emitEnd, d.endTime());
            float g = render::length(d.gravity);
            float travel = d.drag > 0 ? d.speed.max / d.drag : d.speed.max * d.life.max;
            e.extent = std::max(e.extent, d.radius + std::min(travel, d.speed.max * d.life.max) + 0.5f * g * d.life.max * d.life.max +
                                          d.size.max * std::max(1.0f, maxK));
        }
    }

    bool emit(const Instance& in, uint32_t owner, int effect, int emitter, float3 at, int& dropped) {
        if (int(particles.size()) >= limits.maxParticles) { ++dropped; return false; }
        const EmitterDef& d = effects[size_t(effect)].def.emitters[size_t(emitter)];
        const float s = in.scale;
        float3 t, b;
        basisFrom(in.dir, t, b);
        auto toWorld = [&](float3 v) { return t * v.x + in.dir * v.y + b * v.z; };
        // spawn position
        float3 off{0, 0, 0};
        if (d.shape == FxShape::Sphere) {
            float z = rng.unit() * 2 - 1, a = rng.unit() * 2 * kPi, rr = std::sqrt(std::max(0.0f, 1 - z * z));
            off = float3{rr * std::cos(a), z, rr * std::sin(a)} * (d.radius * std::cbrt(rng.unit()));
        } else if (d.shape == FxShape::Disc || d.shape == FxShape::Ring) {
            float a = rng.unit() * 2 * kPi, rr = d.radius * (d.shape == FxShape::Disc ? std::sqrt(rng.unit()) : 1.0f);
            off = toWorld({rr * std::cos(a), 0, rr * std::sin(a)});
        }
        // direction: cone around the (instance-relative) axis
        float3 axis = render::normalize(toWorld(d.direction));
        float3 at2, ab;
        basisFrom(axis, at2, ab);
        float ct = 1 - rng.unit() * (1 - std::cos(d.spread * kDeg));
        float st = std::sqrt(std::max(0.0f, 1 - ct * ct)), ph = rng.unit() * 2 * kPi;
        float3 dir = axis * ct + at2 * (st * std::cos(ph)) + ab * (st * std::sin(ph));
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
        p.batch = effects[size_t(effect)].em[size_t(emitter)].batch;
        p.owner = owner;
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
    for (int i = 0; i < 2; ++i) {
        gfx::PipelineDesc pd;
        pd.shader = I.shader;
        pd.attrs[0] = {0, gfx::VertexFormat::Float3, 0};
        pd.attrs[1] = {1, gfx::VertexFormat::Float2, 12};
        pd.attrs[2] = {2, gfx::VertexFormat::UByte4N, 20};
        pd.attrCount = 3;
        pd.stride = sizeof(Vertex);
        pd.indexType = gfx::IndexType::UInt16;
        pd.cull = gfx::CullMode::None;
        pd.blend = i == 0 ? gfx::BlendMode::Alpha : gfx::BlendMode::Additive;
        pd.depthTest = true;
        pd.depthWrite = false;
        pd.label = i == 0 ? "particles-alpha" : "particles-add";
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
    impl_ = std::make_unique<Impl>();
    impl_->loader = std::move(keep);
    impl_->quality = quality_;
    impl_->limits = limitsFor(quality_);
}

void ParticleSystem::setQuality(FxQuality q) {
    quality_ = q;
    auto& I = *impl_;
    I.quality = q;
    I.limits = limitsFor(q);
    if (int(I.particles.size()) > I.limits.maxParticles) I.particles.resize(size_t(I.limits.maxParticles));
    if (q == FxQuality::Off) clear();
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
        I.particles.erase(std::remove_if(I.particles.begin(), I.particles.end(), [&](auto& p) { return p.effect == uint16_t(id); }), I.particles.end());
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
        in->used = false;
        I.freeInstances.push_back(int((h.id & 0xffff) - 1));
        I.particles.erase(std::remove_if(I.particles.begin(), I.particles.end(), [&](auto& p) { return p.owner == h.id; }), I.particles.end());
    }
}
bool ParticleSystem::playing(FxHandle h) const { return findInstance(*impl_, h) != nullptr; }

void ParticleSystem::clear() {
    auto& I = *impl_;
    I.particles.clear();
    I.freeInstances.clear();
    for (size_t i = 0; i < I.instances.size(); ++i) { I.instances[i].used = false; I.freeInstances.push_back(int(i)); }
}

void ParticleSystem::setView(const RTSCamera& cam, const mat4& viewProj, int fbW, int fbH) {
    auto& I = *impl_;
    I.haveView = true;
    I.viewProj = viewProj;
    I.eye = cam.eye();
    float3 f = render::normalize(cam.target - I.eye);
    I.right = render::normalize(render::cross(f, {0, 1, 0}));
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
        in.prev = in.pos;
        if (in.stopped || (!e.looping && in.time >= e.emitEnd)) { in.used = false; I.freeInstances.push_back(int(slot)); }
    }
    // particles
    for (size_t i = 0; i < I.particles.size();) {
        auto& p = I.particles[i];
        p.age += dt;
        if (p.age * p.invLife >= 1.0f) { p = I.particles.back(); I.particles.pop_back(); continue; }
        const EmitterDef& d = I.effects[p.effect].def.emitters[p.emitter];
        p.vel += d.gravity * (dt * p.scale);
        if (d.drag > 0) p.vel = p.vel * (1.0f / (1.0f + d.drag * dt));
        p.pos += p.vel * dt;
        p.rot += p.spin * dt;
        ++i;
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
    for (uint32_t i = 0; i < I.particles.size(); ++i) {
        const auto& p = I.particles[i];
        const auto& er = I.effects[p.effect].em[p.emitter];
        const EmitterDef& d = I.effects[p.effect].def.emitters[p.emitter];
        float t = std::min(p.age * p.invLife, 1.0f);
        int li = std::min(kLut - 1, int(t * float(kLut - 1) + 0.5f));
        float4 c = I.viewProj * float4{p.pos.x, p.pos.y, p.pos.z, 1};
        if (c.w <= 0.05f) continue;
        float size = std::min(p.size * er.size[size_t(li)], capK * c.w);
        if (size <= 0) continue;
        float ext = size * I.p11 * (1.0f + d.stretch * render::length(p.vel));
        if (c.x - ext > c.w || c.x + ext < -c.w || c.y - ext > c.w || c.y + ext < -c.w) continue;
        if (mul(er.color[size_t(li)], p.tint).w <= 0.004f) continue;
        float fh = size * I.p11 / c.w * 0.5f;                        // fraction of screen height
        stats_.fill += fh * fh / I.aspect;
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
    for (auto& r : I.recs) {
        const auto& p = I.particles[r.idx];
        const auto& er = I.effects[p.effect].em[p.emitter];
        const EmitterDef& d = I.effects[p.effect].def.emitters[p.emitter];
        float t = std::min(p.age * p.invLife, 1.0f);
        int li = std::min(kLut - 1, int(t * float(kLut - 1) + 0.5f));
        const uint32_t col = packColor(mul(er.color[size_t(li)], p.tint));
        const float h = r.size * 0.5f;
        float3 ax, ay;
        if (d.align == FxAlign::Velocity && render::length(p.vel) > 1e-3f) {
            float sp = render::length(p.vel);
            float3 axis = p.vel * (1.0f / sp);
            float3 side = render::cross(axis, render::normalize(I.eye - p.pos));
            float sl = render::length(side);
            side = sl > 1e-4f ? side * (1.0f / sl) : I.right;
            ax = side * h;
            ay = axis * (h + d.stretch * sp * 0.5f);
        } else if (d.align == FxAlign::Ground) {
            float cs = std::cos(p.rot), sn = std::sin(p.rot);
            ax = float3{cs, 0, sn} * h;
            ay = float3{-sn, 0, cs} * h;
        } else {
            float cs = std::cos(p.rot), sn = std::sin(p.rot);
            ax = (I.right * cs + I.up * sn) * h;
            ay = (I.up * cs - I.right * sn) * h;
        }
        float3 pos = d.align == FxAlign::Ground ? p.pos + float3{0, 0.03f, 0} : p.pos;
        float u0 = er.tex.u0, v0 = er.tex.v0, u1 = er.tex.u1, v1 = er.tex.v1;
        if (er.frames > 1) {
            int f = d.fps > 0 ? int(p.age * d.fps) % er.frames : std::min(er.frames - 1, int(t * float(er.frames)));
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
        const int pi = batch.blend == FxBlend::Additive ? 1 : 0;
        if (pi != lastPipe) { I.dev->setPipeline(I.pipe[pi]); lastPipe = pi; }
        u.params[0] = float(pi); u.params[1] = u.params[2] = u.params[3] = 0;
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

} // namespace rts::fx
