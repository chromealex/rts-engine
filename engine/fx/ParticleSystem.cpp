#include "ParticleSystem.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <type_traits>
#include "FxInternal.h"
#include "FxNoise.h"
#include "ParticleShaders.h"

namespace rts::fx {

namespace {

const std::vector<std::string> kBuiltins = {"soft", "spark", "ring", "smoke"};
const std::vector<std::string> kBuiltinMeshes = {"cube", "sphere", "capsule", "cylinder", "plane", "quad"};

float emitterTime(const EmitterDef& d, float time) {
    if (d.duration <= 0) return 0;
    float t = std::max(0.0f, time) / d.duration;
    return d.loop ? t - std::floor(t) : std::min(t, 1.0f);
}

gfx::BlendMode gfxBlend(int i) {
    switch (FxBlend(i)) {
    case FxBlend::Alpha: return gfx::BlendMode::Alpha;
    case FxBlend::Additive: return gfx::BlendMode::Additive;
    case FxBlend::Premultiplied: return gfx::BlendMode::Premultiplied;
    case FxBlend::Multiply: return gfx::BlendMode::Multiply;
    case FxBlend::Subtract: return gfx::BlendMode::Subtract;
    case FxBlend::Opaque: break;
    }
    return gfx::BlendMode::Opaque;
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
const std::vector<std::string>& ParticleSystem::builtinMeshes() { return kBuiltinMeshes; }

// ------------------------------------------------------------------------------------------------ Impl

float4 ParticleSystem::Impl::colorNow(const Particle& p) const {
    const auto& er = rtOf(p);
    const EmitterDef& d = defOf(p);
    const size_t li = lutIndex(lifeT(p));
    float4 c = er.color[li];
    if (!d.colorMin.empty()) c = lerp4(er.colorMinLut[li], c, prand(p.uid, 51));
    return mul(c, p.tint);
}

float ParticleSystem::Impl::sizeNow(const Particle& p, float t) const {
    const auto& er = rtOf(p);
    const EmitterDef& d = defOf(p);
    const size_t li = lutIndex(t);
    float k = er.size[li];
    if (!d.sizeOverLifeMin.empty()) k = er.sizeMinLut[li] + (k - er.sizeMinLut[li]) * prand(p.uid, 52);
    float s = p.size * k * p.nsize;
    if (!d.sizeBySpeed.empty()) s *= speedLut(er.sizeSpeed, d.sizeSpeed, render::length(p.vtot) / p.scale);
    return s;
}

FxTexture ParticleSystem::Impl::builtin(size_t i) const {
    FxTexture t;
    t.tex = atlas;
    const float W = float(kAtlasCell * kAtlasCells);
    t.u0 = (float(i) * kAtlasCell + 0.5f) / W; t.u1 = (float(i + 1) * kAtlasCell - 0.5f) / W;
    t.v0 = 0.5f / kAtlasCell; t.v1 = 1 - 0.5f / kAtlasCell;
    return t;
}

FxTexture ParticleSystem::Impl::texture(const std::string& name, std::vector<std::string>& errors, const std::string& where) {
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

int ParticleSystem::Impl::mesh(const std::string& name, std::vector<std::string>& errors, const std::string& where) {
    auto it = meshByName.find(name);
    if (it != meshByName.end()) return it->second;
    FxMesh src;
    bool ok = false;
    if (name.rfind("builtin:", 0) == 0) {
        ok = builtinMesh(name.substr(8), src);
        if (!ok) errors.push_back(where + ": unknown builtin mesh '" + name.substr(8) + "' (cube, sphere, capsule, cylinder, plane, quad)");
    } else {
        ok = meshLoader && meshLoader(name, src);
        if (!ok) errors.push_back(where + ": mesh '" + name + "' could not be loaded" + (meshLoader ? "" : " (no mesh loader connected)"));
    }
    if (!ok) builtinMesh("sphere", src);   // something visible instead
    MeshRT m;
    m.pos = std::move(src.pos);
    m.uv = std::move(src.uv);
    m.idx = std::move(src.idx);
    if (m.idx.size() % 3) m.idx.resize(m.idx.size() / 3 * 3);
    for (auto i : m.idx) if (i >= m.pos.size()) { m.idx.clear(); break; }
    if (src.nrm.size() == m.pos.size()) m.nrm = std::move(src.nrm);
    else {   // area-weighted vertex normals
        m.nrm.assign(m.pos.size(), float3{0, 0, 0});
        for (size_t t = 0; t + 2 < m.idx.size(); t += 3) {
            float3 n = render::cross(m.pos[m.idx[t + 1]] - m.pos[m.idx[t]], m.pos[m.idx[t + 2]] - m.pos[m.idx[t]]);
            for (int k = 0; k < 3; ++k) m.nrm[m.idx[t + size_t(k)]] += n;
        }
        for (auto& n : m.nrm) n = render::length(n) > 1e-8f ? render::normalize(n) : float3{0, 1, 0};
    }
    if (m.uv.size() != m.pos.size()) m.uv.assign(m.pos.size(), float2{0.5f, 0.5f});
    float acc = 0;
    for (size_t t = 0; t + 2 < m.idx.size(); t += 3) {
        acc += 0.5f * render::length(render::cross(m.pos[m.idx[t + 1]] - m.pos[m.idx[t]], m.pos[m.idx[t + 2]] - m.pos[m.idx[t]]));
        m.area.push_back(acc);
    }
    if (m.area.empty() || m.area.back() <= 0) m.area.assign(std::max<size_t>(1, m.idx.size() / 3), 1.0f);
    m.radius = 0;
    for (auto& p : m.pos) m.radius = std::max(m.radius, render::length(p));
    m.valid = !m.pos.empty();
    meshes.push_back(std::move(m));
    meshByName[name] = int(meshes.size() - 1);
    return int(meshes.size() - 1);
}

uint16_t ParticleSystem::Impl::batchFor(const FxTexture& t, FxBlend blend) {
    for (size_t i = 0; i < batches.size(); ++i)
        if (batches[i].texId == t.tex.id && batches[i].blend == blend) return uint16_t(i);
    batches.push_back({t.tex.id, blend, t.tex});
    return uint16_t(batches.size() - 1);
}

void ParticleSystem::Impl::build(EffectRT& e, std::vector<std::string>& errors) {
    e.em.clear();
    e.looping = e.def.looping();
    e.emitEnd = 0;
    e.extent = 0.5f;
    for (size_t i = 0; i < e.def.emitters.size(); ++i) {
        const EmitterDef& d = e.def.emitters[i];
        const std::string where = "fx/" + e.name + ".json: emitters[" + std::to_string(i) + "]";
        EmitterRT r;
        r.tex = texture(d.texture, errors, where);
        r.batch = batchFor(r.tex, d.blend);
        lutColor(r.color, d.color);
        float maxK = 0;
        for (auto& k : d.sizeOverLife) maxK = std::max(maxK, k.k);
        for (auto& k : d.sizeOverLifeMin) maxK = std::max(maxK, k.k);
        lutScalar(r.size, d.sizeOverLife);
        r.frames = d.frameCols * d.frameRows;
        r.du = (r.tex.u1 - r.tex.u0) / float(d.frameCols);
        r.dv = (r.tex.v1 - r.tex.v0) / float(d.frameRows);
        const EmitterDef base;
        r.anim = d.velocityOverLife.enabled || (d.inheritVelocity.enabled && d.inheritVelocity.current);
        r.modules = d.localSpace || d.maxParticles > 0 || d.speedLimit.max > 0 || d.speedLimit.curved() || d.noise.enabled() || d.collision.enabled ||
                    !d.subEmitters.empty() || d.trail.enabled || d.light.enabled || d.velocity.x != 0 || d.velocity.y != 0 || d.velocity.z != 0 ||
                    r.anim || d.force.enabled || d.speedLimitAxes.enabled || d.externalForces > 0 || d.trigger.enabled || d.spin3D ||
                    d.spinBySpeed.enabled || d.ringBuffer;
        lutColor(r.startLut, d.startColor);
        lutColor(r.startMinLut, d.startColorMin);
        lutColor(r.colorSpeed, d.colorBySpeed);
        lutColor(r.colorMinLut, d.colorMin);
        lutScalar(r.sizeSpeed, d.sizeBySpeed);
        lutScalar(r.sizeMinLut, d.sizeOverLifeMin);
        lutScalar(r.sizeYLut, d.sizeOverLifeY);
        lutScalar(r.sizeZLut, d.sizeOverLifeZ);
        lutScalar(r.spinSpeed, d.spinBySpeed.keys, 0);
        lutScalar(r.lightRadius, d.light.radiusOverLife);
        lutScalar(r.lightIntensity, d.light.intensityOverLife);
        lutScalar(r.noiseRemap, d.noise.remap, 0);
        if (d.trail.enabled) {
            lutColor(r.trailColor, d.trail.color);
            lutColor(r.trailLife, d.trail.lifeColor);
            lutScalar(r.trailWidth, d.trail.width);
            r.trailTex = texture(d.trail.texture, errors, where + ".trail");
            r.trailBatch = batchFor(r.trailTex, d.trail.blend);
        }
        r.shapeRot = Mat3::euler(d.shapeRotation);
        if (d.shape == FxShape::Mesh && !d.mesh.empty()) r.shapeMesh = mesh(d.mesh, errors, where + ".mesh");
        float meshRadius = 0;
        for (auto& m : d.meshes) {
            int id = mesh(m, errors, where + ".meshes");
            r.meshIds.push_back(id);
            meshRadius = std::max(meshRadius, meshes[size_t(id)].radius);
        }
        for (auto& sp : d.sprites) {
            FxTexture t = texture(sp, errors, where + ".sprites");
            r.spriteTex.push_back(t);
            r.spriteBatch.push_back(batchFor(t, d.blend));
        }
        e.em.push_back(r);
        if (!d.loop) e.emitEnd = std::max(e.emitEnd, d.endTime());
        // rough reach of its particles (off-screen culling of one-shots)
        const float life = d.life.max * (d.lifeBySpeed.enabled ? 2.0f : 1.0f);
        float g = render::length(d.gravity) * std::max(std::fabs(d.gravityScale.min), std::fabs(d.gravityScale.max));
        float spd = std::max(std::fabs(d.speed.min), std::fabs(d.speed.max));
        float travel = d.drag.max > 0 && !d.drag.curved() ? spd / d.drag.max : spd * life;
        float extra = 0;
        if (d.velocityOverLife.enabled) {
            const auto& v = d.velocityOverLife;
            auto mx = [](const FxRange& x) { return std::max(std::fabs(x.min), std::fabs(x.max)); };
            extra += (mx(v.x) + mx(v.y) + mx(v.z) + mx(v.radial)) * life;
        }
        if (d.force.enabled) {
            auto mx = [](const FxRange& x) { return std::max(std::fabs(x.min), std::fabs(x.max)); };
            g += mx(d.force.x) + mx(d.force.y) + mx(d.force.z);
        }
        if (d.noise.enabled()) extra += d.noise.strength.max * life * d.noise.position;
        e.extent = std::max(e.extent, render::length(d.offset) + std::max({d.radius + d.donutRadius + d.length, render::length(d.box) * 0.5f}) +
                                      std::min(travel, spd * life) + 0.5f * g * life * life + extra + d.randomPosition +
                                      d.size.max * std::max(1.0f, maxK) * std::max(1.0f, meshRadius * 2));
    }
}

void ParticleSystem::Impl::release(Particle& p) {
    if (p.trail >= 0) { freeTrails.push_back(p.trail); p.trail = -1; }
    if (p.child) {
        auto it = children.find(p.uid);
        if (it != children.end()) { if (auto* c = inst(it->second.id)) c->stopped = true; children.erase(it); }
        p.child = false;
    }
}

void ParticleSystem::Impl::queueSub(const Particle& p, FxSubEvent on, float3 at) {
    const int depthOf = p.depth;
    const EmitterDef& d = defOf(p);
    for (size_t k = 0; k < d.subEmitters.size(); ++k) {
        const FxSubEmitter& se = d.subEmitters[k];
        if (se.on != on || pending.size() >= kMaxPendingSubs || depthOf + 1 > kMaxSubDepth) continue;
        if (se.chance < 1 && rng.unit() >= se.chance) continue;
        const float3 dir{0, 1, 0};   // like Unity: a sub-emitter keeps its own orientation, not the particle's velocity
        float4 tint = p.tint;
        if (se.inheritColor) { float4 c = colorNow(p); tint = {c.x, c.y, c.z, p.tint.w}; }
        const float t = lifeT(p);
        SubSpawn sp{p.effect, p.emitter, uint8_t(k), uint8_t(depthOf + 1), at, dir, p.scale, tint, on == FxSubEvent::Birth ? p.uid : 0u,
                    1, 0, 1, 0};
        if (se.inheritSize) sp.sizeMul = sizeNow(p, t) / p.scale;
        if (se.inheritRotation) sp.rotAdd = p.rot;
        if (se.inheritLifetime) sp.lifeMul = std::max(0.05f, 1 - t);
        if (se.inheritDuration) sp.timeScale = std::max(0.02f, (1 - t) / p.invLife);   // remaining seconds (resolved when played)
        pending.push_back(sp);
    }
}

void ParticleSystem::Impl::freeInstance(size_t slot) {
    auto& in = instances[slot];
    if (!in.used) return;
    in.used = false;
    freeInstances.push_back(int(slot));
}

// Removes particle i (swap with the last): death events, counters, trail.
void ParticleSystem::Impl::die(size_t i, bool events) {
    Particle& p = particles[i];
    Instance* in = inst(p.owner);
    if (in) {
        if (in->alive[p.emitter] > 0) --in->alive[p.emitter];
        if (in->live > 0) --in->live;
    }
    const EmitterDef& d = defOf(p);
    if (events && rtOf(p).modules) queueSub(p, FxSubEvent::Death, p.pos);
    if (p.trail >= 0 && !d.trail.dieWithParticles && trails[size_t(p.trail)].count > 1) {   // the trail fades out on its own
        orphans.push_back({p.trail, p.effect, p.emitter, p.scale, sizeNow(p, lifeT(p)), d.trail.inheritColor ? colorNow(p) : p.tint});
        p.trail = -1;
    }
    release(p);
    if (in && !in->emitting && in->live == 0) freeInstance(size_t((p.owner & 0xffff) - 1));
    p = particles.back();
    particles.pop_back();
}

void ParticleSystem::Impl::emitInstance(size_t slot, float dt, float spawn, int& dropped) {
    auto& in = instances[slot];
    if (!in.used) return;
    const auto& e = effects[size_t(in.effect)];
    const uint32_t owner = (uint32_t(in.gen) << 16) | uint32_t(slot + 1);
    const float edt = dt * in.timeScale;
    if (in.emitting && !in.stopped) {
        for (size_t k = 0; k < e.def.emitters.size(); ++k) {
            const EmitterDef& d = e.def.emitters[k];
            if (d.minQuality > quality) continue;
            const float t0 = in.time - in.delay[k], t1 = t0 + edt;
            if (t1 < 0) continue;
            if (!in.burstDone[k]) {
                in.burstDone[k] = true;
                int n = d.burst > 0 ? std::max(1, rng.roundCount(float(d.burst) * spawn)) : 0;
                for (int i = 0; i < n; ++i) emit(in, owner, in.effect, int(k), in.pos, dropped, 0, float(i) / float(n));
            }
            for (auto& b : d.bursts) {   // timed bursts in [t0, t1); a looping emitter repeats them every duration
                const float period = d.loop && d.duration > 0 ? d.duration : 0;
                int cycles = b.cycles;
                if (cycles == 0) {   // forever: every interval until the loop (or the duration) ends
                    const float span = (period > 0 ? period : std::max(d.duration, b.interval)) - b.time;
                    cycles = std::max(1, int(std::ceil(span / std::max(1e-3f, b.interval))));
                    if (period <= 0 && d.duration <= 0) cycles = std::max(cycles, int(std::ceil((t1 - b.time) / std::max(1e-3f, b.interval))) + 1);
                }
                for (int c = 0; c < cycles; ++c) {
                    const float tb = b.time + float(c) * b.interval;
                    if (period <= 0 && (tb < t0 || tb >= t1)) continue;
                    float k0 = period > 0 ? std::max(0.0f, std::ceil((t0 - tb) / period)) : 0;
                    for (float tk = tb + k0 * period; tk < t1; tk += period) {
                        if (tk >= t0 && (b.chance >= 1 || rng.unit() < b.chance)) {
                            const FxRange& cnt = b.count;
                            int n = rng.roundCount((cnt.curved() ? cnt.at(emitterTime(d, tk), rng.unit()) : rng.range(cnt)) * spawn);
                            for (int i = 0; i < n; ++i) emit(in, owner, in.effect, int(k), in.pos, dropped, 0, float(i) / float(std::max(1, n)));
                        }
                        if (period <= 0) break;
                    }
                }
            }
            const bool active = d.loop || t0 < d.duration;
            if (!active) continue;
            float count = 0;
            const float a = std::max(t0, 0.0f), bEnd = d.loop ? t1 : std::min(t1, d.duration);
            if ((d.rate.max > 0 || d.rate.curved()) && bEnd > a) {
                const float rate = d.rate.constant() ? d.rate.min : d.rate.at(emitterTime(d, (a + bEnd) * 0.5f), rng.unit());
                count += rate * spawn * (bEnd - a);
            }
            if (d.perMeter.max > 0 || d.perMeter.curved()) {
                const float pm = d.perMeter.constant() ? d.perMeter.min : d.perMeter.at(emitterTime(d, t0), rng.unit());
                count += pm * spawn * render::length(in.pos - in.prev) / in.scale;
            }
            in.accum[k] += count;
            int n = int(in.accum[k]);
            in.accum[k] -= float(n);
            for (int i = 0; i < n; ++i) {   // spread along the path moved this frame (smooth trails)
                float f = (float(i) + rng.unit()) / float(n);
                emit(in, owner, in.effect, int(k), render::lerp(in.prev, in.pos, f), dropped, f * edt, float(i) / float(n));
            }
            if (d.arcMode == FxArcMode::Loop || d.arcMode == FxArcMode::PingPong) in.arcPhase[k] += d.arcSpeed * edt;
        }
    }
    in.time += edt;
    in.delta = in.pos - in.prev;
    if (dt > 0) in.vel = in.delta * (1.0f / dt);
    in.prev = in.pos;
    if (in.emitting && (in.stopped || (!e.looping && in.time >= e.emitEnd))) in.emitting = false;
    if (!in.emitting && in.live == 0) freeInstance(slot);
}

void ParticleSystem::Impl::stepParticle(size_t& i, float dt, bool& removed) {
    removed = false;
    auto& p = particles[i];
    p.age += dt;
    const EmitterDef& d = defOf(p);
    if (p.age * p.invLife >= 1.0f) {
        if (d.ringBuffer) p.age = 0.999f / p.invLife;   // stays at its last moment until a new particle replaces it
        else { die(i, true); removed = true; return; }
    }
    const EmitterRT& er = rtOf(p);
    const bool modules = er.modules;
    const float t = lifeT(p);
    if (upFn) p.vel += upRotate(upFn(p.pos), d.gravity) * (dt * p.scale * p.grav);   // curved worlds: towards the local down
    else p.vel += d.gravity * (dt * p.scale * p.grav);
    if (d.drag.max > 0 || d.drag.curved()) {
        float k = d.drag.constant() ? d.drag.min : d.drag.at(t, prand(p.uid, 3));
        if (d.dragBySize) { float s = p.size / p.scale; k *= s * s; }
        if (d.dragBySpeed) k *= render::length(p.vel) / p.scale;
        p.vel = p.vel * (1.0f / (1.0f + k * dt));
    }
    float3 vt = p.vel;
    if (modules) {
        Instance* in = inst(p.owner);
        const EffectRT& e = effects[p.effect];
        if (d.force.enabled) {
            const FxForce& f = d.force;
            const float rx = f.randomize ? rng.unit() : prand(p.uid, 11), ry = f.randomize ? rng.unit() : prand(p.uid, 12),
                        rz = f.randomize ? rng.unit() : prand(p.uid, 13);
            float3 fv{f.x.at(t, rx), f.y.at(t, ry), f.z.at(t, rz)};
            if (!f.world && in) fv = in->toWorld(fv);
            p.vel += fv * (p.scale * dt);
        }
        if (d.externalForces > 0) {
            p.vel += (upFn ? upRotate(upFn(p.pos), wind) : wind) * (d.externalForces * dt);
            if (in && !e.def.forceFields.empty()) {
                for (const FxForceField& ff : e.def.forceFields) {
                    const float3 c = in->pos + in->toWorld(ff.position) * in->scale;
                    float3 r = p.pos - c;
                    if (ff.cylinder) {
                        const float along = render::dot(r, in->dir);
                        if (ff.length > 0 && std::fabs(along) > ff.length * 0.5f * in->scale) continue;
                        r = r - in->dir * along;
                    }
                    const float dist = render::length(r);
                    if (dist > ff.radius * in->scale || dist < ff.startRadius * in->scale) continue;
                    const float k = d.externalForces;
                    if (ff.direction.x != 0 || ff.direction.y != 0 || ff.direction.z != 0) p.vel += in->toWorld(ff.direction) * (k * in->scale * dt);
                    if (ff.gravity != 0 && dist > 1e-4f) {
                        const float focus = ff.gravityFocus * ff.radius * in->scale;
                        const float sgn = dist > focus ? -1.0f : 1.0f;   // towards the focus shell
                        p.vel += r * (sgn * ff.gravity * k * in->scale * dt / dist);
                    }
                    if (ff.rotation != 0 && dist > 1e-4f) {   // vortex around the field's Y axis: carried around at constant radius
                        p.pos = c + rotateAxis(p.pos - c, in->dir, ff.rotation * kDeg * k * dt);
                        if (ff.rotationAttraction > 0) {   // pulled into the orbit: radial velocity damped away
                            const float3 rd = r * (1.0f / dist);
                            const float a = std::min(1.0f, ff.rotationAttraction * k * dt * 10);
                            p.vel = p.vel - rd * (render::dot(p.vel, rd) * a);
                        }
                    }
                    if (ff.drag > 0) p.vel = p.vel * (1.0f / (1.0f + ff.drag * k * dt));
                }
            }
        }
        const float damp = d.limitDampen >= 1 ? 1.0f : 1.0f - std::pow(1.0f - d.limitDampen, dt * 30.0f);
        if (d.speedLimit.max > 0 || d.speedLimit.curved()) {
            float sp = render::length(p.vel), lim = (d.speedLimit.constant() ? d.speedLimit.min : d.speedLimit.at(t, prand(p.uid, 5))) * p.scale;
            if (sp > lim && sp > 0) p.vel = p.vel * ((sp - (sp - lim) * damp) / sp);
        }
        if (d.speedLimitAxes.enabled) {
            const FxLimitAxes& l = d.speedLimitAxes;
            const bool local = !l.world && in;
            float3 v = local ? float3{render::dot(p.vel, in->bt), render::dot(p.vel, in->dir), render::dot(p.vel, in->bb)} : p.vel;
            auto clampAxis = [&](float& c, const FxRange& r, uint32_t salt) {
                const float lim = r.at(t, prand(p.uid, salt)) * p.scale;
                const float a = std::fabs(c);
                if (a > lim) c = (c > 0 ? 1.0f : -1.0f) * (a - (a - lim) * damp);
            };
            clampAxis(v.x, l.x, 6); clampAxis(v.y, l.y, 7); clampAxis(v.z, l.z, 8);
            p.vel = local ? in->toWorld(v) : v;
        }
        if (d.localSpace && in) p.pos += in->delta;
        // animated velocity: added each frame, not accumulated (Unity "velocity over lifetime")
        float3 anim = p.addVel;
        float speedMod = 1;
        if (d.velocityOverLife.enabled) {
            const FxVelocityOverLife& v = d.velocityOverLife;
            float3 lin{v.x.at(t, prand(p.uid, 21)), v.y.at(t, prand(p.uid, 22)), v.z.at(t, prand(p.uid, 23))};
            if (!v.world && in) lin = in->toWorld(lin);
            anim += lin * p.scale;
            if (in) {
                const float3 c = in->pos + in->toWorld(v.orbitalOffset) * in->scale;
                const float3 rel = p.pos - c;
                const float3 w{v.orbitalX.at(t, prand(p.uid, 24)), v.orbitalY.at(t, prand(p.uid, 25)), v.orbitalZ.at(t, prand(p.uid, 26))};
                if (w.x != 0 || w.y != 0 || w.z != 0) anim += render::cross(in->toWorld(w) * kDeg, rel);
                const float rad = v.radial.at(t, prand(p.uid, 27));
                if (rad != 0 && render::length(rel) > 1e-5f) anim += render::normalize(rel) * (rad * p.scale);
            }
            speedMod = v.speedModifier.at(t, prand(p.uid, 28));
        }
        if (d.inheritVelocity.enabled && d.inheritVelocity.current && in) anim += in->vel * d.inheritVelocity.amount.at(t, prand(p.uid, 29));
        vt = (p.vel + anim) * speedMod;
        if (d.noise.enabled()) {
            const FxNoise& n = d.noise;
            const int oct = std::min(n.octaves, quality <= FxQuality::Low ? 1 : quality == FxQuality::Medium ? 2 : 4);
            float str = (n.strength.constant() ? n.strength.min : n.strength.at(t, prand(p.uid, 41))) * p.scale;
            if (n.damping) str *= n.frequency;
            const float3 sp = p.pos * (n.frequency / p.scale) + float3{0, clock * n.scroll, 0};
            float3 nv{0, 0, 0};
            if (oct <= 1) nv = noise3(sp);
            else {
                float amp = 1, norm = 0, f = 1;
                for (int o = 0; o < oct; ++o) { nv += noise3(sp * f) * amp; norm += amp; amp *= n.octaveMultiplier; f *= n.octaveScale; }
                nv = nv * (1.0f / std::max(1e-4f, norm));
            }
            if (!n.remap.empty()) {
                auto rm = [&](float c) { return er.noiseRemap[lutIndex((c + 1) * 0.5f)]; };
                nv = {rm(nv.x), rm(nv.y), rm(nv.z)};
            }
            nv = mul3(nv, n.axes);
            p.pos += nv * (str * n.position * dt);
            if (n.rotation != 0) p.rot += nv.z * n.rotation * kDeg * dt;
            if (n.size != 0) p.nsize = std::max(0.0f, 1 + nv.y * n.size);
        }
    }
    p.pos += vt * dt;
    p.vtot = vt;
    if (d.spin.curved()) p.rot += d.spin.at(t, prand(p.uid, 31)) * kDeg * dt;
    else p.rot += p.spin * dt;
    if (modules) {
        if (d.spinBySpeed.enabled) p.rot += speedLut(er.spinSpeed, d.spinBySpeed.speed, render::length(vt) / p.scale) * kDeg * dt;
        if (d.spin3D) { p.rotX += d.spinX.at(t, prand(p.uid, 32)) * kDeg * dt; p.rotY += d.spinY.at(t, prand(p.uid, 33)) * kDeg * dt; }
        Instance* in = inst(p.owner);
        if (d.collision.enabled) {
            const FxCollision& c = d.collision;
            const float rad = c.radius > 0 ? c.radius * 0.5f * p.size : 0.0f;
            const float before = render::length(p.vel);
            bool hit = false;
            if (c.ground && upFn && altFn) { // curved worlds: the ground along the local up
                const float3 n = upFn(p.pos);
                const float below = c.height * p.scale + rad - altFn(p.pos);
                const float vn = render::dot(p.vel, n);
                if (below > 0 && vn < 0) {
                    p.pos += n * below;
                    p.vel = (p.vel - n * vn) * (1 - c.dampen) - n * (vn * c.bounce);
                    hit = true;
                }
            } else if (c.ground) {
                float plane = (ground ? ground(p.pos.x, p.pos.z) : 0.0f) + c.height * p.scale;
                if (p.pos.y - rad < plane && p.vel.y < 0) {
                    p.pos.y = plane + rad;
                    p.vel.y = -p.vel.y * c.bounce;
                    p.vel.x *= 1 - c.dampen; p.vel.z *= 1 - c.dampen;
                    hit = true;
                }
            }
            for (const FxPlane& pl : c.planes) {
                const float3 pp = in ? in->pos + in->toWorld(pl.point) * in->scale : pl.point;
                const float3 nn = in ? render::normalize(in->toWorld(pl.normal)) : pl.normal;
                const float dist = render::dot(p.pos - pp, nn) - rad;
                const float vn = render::dot(p.vel, nn);
                if (dist < 0 && vn < 0) {
                    p.pos = p.pos - nn * dist;
                    const float3 tang = p.vel - nn * vn;
                    p.vel = tang * (1 - c.dampen) - nn * (vn * c.bounce);
                    hit = true;
                }
            }
            if (hit) {
                p.age += c.lifeLoss / p.invLife;
                queueSub(p, FxSubEvent::Collision, p.pos);
                const float sp = render::length(p.vel);
                if (sp < c.minKillSpeed * p.scale || (c.maxKillSpeed > 0 && before > c.maxKillSpeed * p.scale) || p.age * p.invLife >= 1.0f) {
                    die(i, true); removed = true; return;
                }
            }
        }
        if (d.trigger.enabled && in) {
            const FxTriggerDef& tg = d.trigger;
            const float rad = tg.radius * 0.5f * p.size;
            bool inside = false;
            for (const FxSphere& s : tg.volumes) {
                const float3 c = in->pos + in->toWorld(s.center) * in->scale;
                if (render::length(p.pos - c) <= s.radius * in->scale + rad) { inside = true; break; }
            }
            const bool known = p.flags & 8, was = p.flags & 4;
            p.flags = uint8_t((p.flags & ~12) | 8 | (inside ? 4 : 0));
            FxTriggerAction act = inside ? tg.inside : tg.outside;
            if (known && inside != was) {
                FxTriggerAction edge = inside ? tg.enter : tg.exit;
                if (edge == FxTriggerAction::Kill) act = edge;
                else if (edge == FxTriggerAction::Callback) queueSub(p, FxSubEvent::Trigger, p.pos);
            }
            if (act == FxTriggerAction::Callback && (!known || inside != was)) queueSub(p, FxSubEvent::Trigger, p.pos);
            if (act == FxTriggerAction::Kill) { die(i, true); removed = true; return; }
        }
        if (p.trail >= 0) {
            auto& tr = trails[size_t(p.trail)];
            const float step = tr.time / float(kTrailPts - 1);
            if (clock - tr.timeAt(0) >= step && (d.trail.minDistance <= 0 || render::length(p.pos - tr.at(0)) >= d.trail.minDistance * p.scale)) {
                tr.head = (tr.head + 1) % kTrailPts;
                tr.p[size_t(tr.head)] = p.pos; tr.t[size_t(tr.head)] = clock;
                tr.count = std::min(tr.count + 1, kTrailPts);
            }
            while (tr.count > 1 && clock - tr.timeAt(tr.count - 1) > tr.time) --tr.count;
        }
        if (p.child) {
            auto it = children.find(p.uid);
            if (it == children.end()) p.child = false;
            else if (auto* c = inst(it->second.id)) c->pos = p.pos;
            else { children.erase(it); p.child = false; }
        }
    }
}

// ------------------------------------------------------------------------------------------------ API

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
    sd.glslFragment = shaders::kParticleMulFS; sd.msl = shaders::kParticleMulMSL; sd.label = "particles-multiply";
    I.mulShader = dev->createShader(sd);
    if (!I.mulShader.valid()) { error_ = "particles: shader: " + dev->lastError(); I.dev = nullptr; return false; }
    static const char* kLabels[] = {"particles-alpha", "particles-add", "particles-premul", "particles-mul", "particles-sub", "particles-opaque"};
    for (int mesh = 0; mesh < 2; ++mesh)
        for (int i = 0; i < 6; ++i) {
            gfx::PipelineDesc pd;
            pd.shader = FxBlend(i) == FxBlend::Multiply ? I.mulShader : I.shader;
            pd.attrs[0] = {0, gfx::VertexFormat::Float3, 0};
            pd.attrs[1] = {1, gfx::VertexFormat::Float2, 12};
            pd.attrs[2] = {2, gfx::VertexFormat::UByte4N, 20};
            pd.attrCount = 3;
            pd.stride = sizeof(Vertex);
            pd.indexType = mesh ? gfx::IndexType::None : gfx::IndexType::UInt16;
            pd.cull = gfx::CullMode::None;
            pd.blend = gfxBlend(i);
            pd.depthTest = true;
            pd.depthWrite = FxBlend(i) == FxBlend::Opaque;
            pd.label = kLabels[i];
            gfx::PipelineHandle h = dev->createPipeline(pd);
            if (!h.valid()) { error_ = "particles: pipeline: " + dev->lastError(); I.dev = nullptr; return false; }
            (mesh ? I.meshPipe : I.pipe)[i] = h;
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
        for (auto& p : I.meshPipe) if (p.valid()) I.dev->destroyPipeline(p);
        if (I.shader.valid()) I.dev->destroyShader(I.shader);
        if (I.mulShader.valid()) I.dev->destroyShader(I.mulShader);
        if (I.vb.valid()) I.dev->destroyBuffer(I.vb);
        if (I.ib.valid()) I.dev->destroyBuffer(I.ib);
        if (I.atlas.valid()) I.dev->destroyTexture(I.atlas);
    }
    TextureLoader keep = std::move(I.loader);
    MeshLoader keepMesh = std::move(I.meshLoader);
    auto ground = std::move(I.ground);
    auto upFn = std::move(I.upFn);
    auto altFn = std::move(I.altFn);
    const float3 wind = I.wind, light = I.lightDir;
    impl_ = std::make_unique<Impl>();
    impl_->loader = std::move(keep);
    impl_->meshLoader = std::move(keepMesh);
    impl_->ground = std::move(ground);
    impl_->upFn = std::move(upFn);
    impl_->altFn = std::move(altFn);
    impl_->wind = wind;
    impl_->lightDir = light;
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
void ParticleSystem::setWind(float3 wind) { impl_->wind = wind; }
void ParticleSystem::setLightDir(float3 dir) { if (render::length(dir) > 1e-5f) impl_->lightDir = render::normalize(dir); }
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

void ParticleSystem::setMeshLoader(MeshLoader l) {
    impl_->meshLoader = std::move(l);
    impl_->meshes.clear();
    impl_->meshByName.clear();
    for (auto& e : impl_->effects) if (e.valid) impl_->build(e, errors_);
}

int ParticleSystem::define(const std::string& name, const EffectDef& def) {
    auto& I = *impl_;
    int id;
    auto it = I.byName.find(name);
    if (it != I.byName.end()) {
        id = it->second;
        for (size_t i = I.particles.size(); i-- > 0;) if (I.particles[i].effect == uint16_t(id)) I.die(i, false);
        I.orphans.erase(std::remove_if(I.orphans.begin(), I.orphans.end(), [&](const Impl::Orphan& o) {
            if (o.effect != uint16_t(id)) return false;
            I.freeTrails.push_back(o.trail);
            return true;
        }), I.orphans.end());
        for (size_t i = 0; i < I.instances.size(); ++i)
            if (I.instances[i].used && I.instances[i].effect == id) I.freeInstance(i);
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
    I.meshes.clear();
    I.meshByName.clear();
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
    in.setDir(render::length(dir) > 1e-5f ? render::normalize(dir) : float3{0, 1, 0});
    in.scale = std::max(0.01f, scale);
    in.tint = tint;
    bool prewarm = false;
    float warm = 0;
    for (size_t k = 0; k < e.def.emitters.size(); ++k) {
        const EmitterDef& d = e.def.emitters[k];
        in.delay[k] = d.delay.constant() ? d.delay.min : d.delay.at(0, I.rng.unit());
        if (d.prewarm && d.loop && d.duration > 0) { prewarm = true; warm = std::max(warm, d.duration); }
    }
    const FxHandle h{(uint32_t(gen) << 16) | uint32_t(slot + 1)};
    if (prewarm) {   // run one loop up front (coarser steps on weak devices)
        const float step = quality_ <= FxQuality::Low ? 0.125f : 1.0f / 15.0f;
        const float spawn = I.limits.spawnScale;
        for (float t = 0; t < warm; t += step) {
            I.emitInstance(size_t(slot), step, spawn, stats_.dropped);
            for (size_t i = 0; i < I.particles.size();) {
                if (I.particles[i].owner != h.id) { ++i; continue; }
                bool removed;
                I.stepParticle(i, step, removed);
                if (!removed) ++i;
            }
        }
        for (auto& d : I.instances[size_t(slot)].delay) d = 0;   // already running
    }
    return h;
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
    if (auto* in = findInstance(*impl_, h)) { in->pos = pos; if (render::length(dir) > 1e-5f) in->setDir(render::normalize(dir)); }
}
void ParticleSystem::stop(FxHandle h) { if (auto* in = findInstance(*impl_, h)) in->stopped = true; }
void ParticleSystem::kill(FxHandle h) {
    auto& I = *impl_;
    if (auto* in = findInstance(I, h)) {
        in->emitting = false;
        for (size_t i = I.particles.size(); i-- > 0;) if (I.particles[i].owner == h.id) I.die(i, false);
        I.freeInstance((h.id & 0xffff) - 1);
    }
}
bool ParticleSystem::playing(FxHandle h) const {
    const auto* in = findInstance(*impl_, h);
    return in && in->emitting && !in->stopped;
}
void ParticleSystem::emitManual(FxHandle h) {
    auto& I = *impl_;
    if (!findInstance(I, h)) return;
    for (const auto& p : I.particles) if (p.owner == h.id) I.queueSub(p, FxSubEvent::Manual, p.pos);
}

void ParticleSystem::snapshot(std::vector<FxParticleInfo>& out) const {
    const auto& I = *impl_;
    out.clear();
    for (const auto& p : I.particles)
        out.push_back({p.pos, p.vtot, I.sizeNow(p, I.lifeT(p)), p.age, 1.0f / p.invLife, p.rot, I.colorNow(p), int(p.effect), int(p.emitter)});
}

void ParticleSystem::clear() {
    auto& I = *impl_;
    I.particles.clear();
    I.trails.clear();
    I.freeTrails.clear();
    I.orphans.clear();
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
    if (I.haveSpace) { // particles live in their own space: the camera into it, the projection from it
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
    if (I.haveView) {   // camera velocity ("cameraStretch")
        if (I.haveLastEye && dt > 0) I.camVel = (I.eye - I.lastEye) * (1.0f / dt);
        I.lastEye = I.eye;
        I.haveLastEye = true;
    }
    const float spawn = I.limits.spawnScale;
    for (size_t slot = 0; slot < I.instances.size(); ++slot) I.emitInstance(slot, dt, spawn, stats_.dropped);
    I.clock += dt;
    for (size_t i = 0; i < I.particles.size();) {
        bool removed;
        I.stepParticle(i, dt, removed);
        if (!removed) ++i;
    }
    // trails of dead particles fade out on their own
    for (size_t k = 0; k < I.orphans.size();) {
        auto& tr = I.trails[size_t(I.orphans[k].trail)];
        while (tr.count > 0 && I.clock - tr.timeAt(tr.count - 1) > tr.time) --tr.count;
        if (tr.count < 2) { I.freeTrails.push_back(I.orphans[k].trail); I.orphans[k] = I.orphans.back(); I.orphans.pop_back(); }
        else ++k;
    }
    // sub-emitters queued this frame (birth / death / collision / trigger / manual): played as effects of their own
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
            if (auto* in = I.inst(h.id)) {
                in->depth = sp.depth;
                in->sizeMul = sp.sizeMul;
                in->rotAdd = sp.rotAdd;
                in->lifeMul = sp.lifeMul;
                const float end = I.effects[size_t(id)].emitEnd;
                if (sp.timeScale > 0 && end > 0 && std::isfinite(end)) in->timeScale = std::clamp(end / sp.timeScale, 0.1f, 20.0f);
            }
            if (sp.uid && h.valid()) I.children[sp.uid] = h;
        }
    }
    stats_.particles = int(I.particles.size());
    int live = 0;
    for (auto& in : I.instances) live += in.used ? 1 : 0;
    stats_.instances = live;
}

} // namespace rts::fx
