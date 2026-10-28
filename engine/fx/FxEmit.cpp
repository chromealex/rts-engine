// Spawning: shapes, directions and start values (ParticleSystem::Impl::emit).
#include <algorithm>
#include <cmath>
#include "FxInternal.h"

namespace rts::fx {

namespace {

// Emitter time 0..1 for curves of start values / rates.
float emitterT(const EmitterDef& d, float time) {
    if (d.duration <= 0) return 0;
    float t = std::max(0.0f, time) / d.duration;
    return d.loop ? t - std::floor(t) : std::min(t, 1.0f);
}

// Thickness: 1 = whole volume, 0 = surface only (radius fraction r^(1/dim) between inner and 1).
float shellRadius(Rng& rng, float thickness, float dim) {
    const float u = rng.unit();
    if (thickness >= 1) return dim == 3 ? std::cbrt(u) : std::sqrt(u);
    const float in = std::pow(1 - thickness, dim);
    return std::pow(in + (1 - in) * u, 1.0f / dim);
}

} // namespace

bool ParticleSystem::Impl::emit(Instance& in, uint32_t owner, int effect, int emitter, float3 at, int& dropped, float u, float burstFrac) {
    const EmitterDef& d = effects[size_t(effect)].def.emitters[size_t(emitter)];
    const EmitterRT& er = effects[size_t(effect)].em[size_t(emitter)];
    if (d.maxParticles > 0 && in.alive[size_t(emitter)] >= d.maxParticles) {
        if (!d.ringBuffer) return false;
        // ring buffer: the oldest particle of this play and emitter makes room
        size_t oldest = particles.size();
        float age = -1;
        for (size_t i = 0; i < particles.size(); ++i)
            if (particles[i].owner == owner && particles[i].emitter == uint16_t(emitter) && particles[i].age > age) { age = particles[i].age; oldest = i; }
        if (oldest == particles.size()) return false;
        die(oldest, false);
    }
    if (int(particles.size()) >= limits.maxParticles) { ++dropped; return false; }
    const float s = in.scale;
    const float tEm = emitterT(d, in.time - in.delay[size_t(emitter)]);
    auto toWorld = [&](float3 v) { return in.toWorld(v); };
    const bool rotated = !er.shapeRot.identity;
    // angle around the axis: full circle with random mode keeps the exact original math (old effects unchanged)
    auto around = [&]() -> float {
        float f = 0;
        switch (d.arcMode) {
        case FxArcMode::Random:
            if (d.arc >= 360 && d.arcSpread <= 0) return rng.unit() * 2 * kPi;
            f = rng.unit();
            break;
        case FxArcMode::Loop: { float ph = in.arcPhase[size_t(emitter)] + d.arcSpeed * u; f = ph - std::floor(ph); break; }
        case FxArcMode::PingPong: { float ph = in.arcPhase[size_t(emitter)] + d.arcSpeed * u; ph -= 2 * std::floor(ph / 2); f = ph <= 1 ? ph : 2 - ph; break; }
        case FxArcMode::Spread: f = burstFrac >= 0 ? burstFrac : rng.unit(); break;
        }
        if (d.arcSpread > 0) f = std::floor(f / d.arcSpread + 1e-4f) * d.arcSpread;
        return f * std::min(d.arc, 360.0f) * kDeg;
    };

    // ---- spawn position (`off`) and, for shapes with their own directions, `shapeDir` (world)
    float3 off{0, 0, 0}, rim{0, 0, 0}, shapeDir{0, 0, 0};
    float rimFrac = -1;
    bool haveShapeDir = false;
    float3 axis = render::normalize(toWorld(rotated ? er.shapeRot * d.direction : d.direction));
    auto shapeToWorld = [&](float3 v) { return toWorld(rotated ? er.shapeRot * v : v); };
    switch (d.shape) {
    case FxShape::Point: break;
    case FxShape::Sphere: {
        float z = rng.unit() * 2 - 1, a = around(), rr = std::sqrt(std::max(0.0f, 1 - z * z));
        float k = d.emitFrom == FxEmitFrom::Shell ? 1.0f : shellRadius(rng, d.thickness, 3);
        float3 q = float3{rr * std::cos(a), z, rr * std::sin(a)} * (d.radius * k);
        off = rotated ? shapeToWorld(q) : q;   // unrotated spheres keep the original (world-space) math
        break;
    }
    case FxShape::Hemisphere: {
        float z = rng.unit(), a = around(), rr = std::sqrt(std::max(0.0f, 1 - z * z));
        float k = d.emitFrom == FxEmitFrom::Shell ? 1.0f : shellRadius(rng, d.thickness, 3);
        off = shapeToWorld(float3{rr * std::cos(a), z, rr * std::sin(a)} * (d.radius * k));
        break;
    }
    case FxShape::Disc: case FxShape::Ring: case FxShape::Cone: {
        float a = around(), rr;
        if (d.shape == FxShape::Ring || d.emitFrom == FxEmitFrom::Shell) rr = d.radius;
        else if (d.thickness >= 1) rr = d.radius * std::sqrt(rng.unit());
        else { float uu = rng.unit(), in0 = (1 - d.thickness) * (1 - d.thickness); rr = d.radius * std::sqrt(in0 + (1 - in0) * uu); }
        if (d.shape == FxShape::Cone) {   // the cone's base is perpendicular to its own axis (disc/ring: to the play direction)
            float3 ct, cb;
            basisFrom(axis, ct, cb);
            rim = ct * std::cos(a) + cb * std::sin(a);
            off = rim * rr;
            if (d.radius > 0) rimFrac = rr / d.radius;
        } else off = shapeToWorld({rr * std::cos(a), 0, rr * std::sin(a)});
        break;
    }
    case FxShape::Box: {
        float3 q;
        if (d.emitFrom == FxEmitFrom::Shell) {          // a point on one of the 6 faces (by area)
            const float ax = d.box.y * d.box.z, ay = d.box.x * d.box.z, az = d.box.x * d.box.y;
            float pick = rng.unit() * (ax + ay + az);
            float x = rng.unit() - 0.5f, y = rng.unit() - 0.5f, sgn = rng.unit() < 0.5f ? -0.5f : 0.5f;
            if (pick < ax) q = {sgn, x, y};
            else if (pick < ax + ay) q = {x, sgn, y};
            else q = {x, y, sgn};
        } else if (d.emitFrom == FxEmitFrom::Edge) {    // a point on one of the 12 edges (by length)
            const float lx = d.box.x, ly = d.box.y, lz = d.box.z;
            float pick = rng.unit() * 4 * (lx + ly + lz), t = rng.unit() - 0.5f;
            float s1 = rng.unit() < 0.5f ? -0.5f : 0.5f, s2 = rng.unit() < 0.5f ? -0.5f : 0.5f;
            if (pick < 4 * lx) q = {t, s1, s2};
            else if (pick < 4 * (lx + ly)) q = {s1, t, s2};
            else q = {s1, s2, t};
        } else q = {rng.unit() - 0.5f, rng.unit() - 0.5f, rng.unit() - 0.5f};
        off = shapeToWorld(mul3(q, d.box));
        break;
    }
    case FxShape::Line:
        off = shapeToWorld({(rng.unit() * 2 - 1) * d.radius, 0, 0});
        break;
    case FxShape::Donut: {   // torus around the effect Y axis: ring "radius", tube "donutRadius"
        float a = around(), b = rng.unit() * 2 * kPi;
        float rt = d.emitFrom == FxEmitFrom::Shell ? d.donutRadius : d.donutRadius * shellRadius(rng, d.thickness, 2);
        float3 radialDir{std::cos(a), 0, std::sin(a)};
        float3 tube = radialDir * std::cos(b) + float3{0, 1, 0} * std::sin(b);
        float3 q = radialDir * d.radius + tube * rt;
        off = shapeToWorld(q);
        shapeDir = render::normalize(shapeToWorld(tube));
        haveShapeDir = true;
        break;
    }
    case FxShape::Mesh: {
        const MeshRT* m = er.shapeMesh >= 0 ? &meshes[size_t(er.shapeMesh)] : nullptr;
        if (!m || !m->valid || m->pos.empty()) break;
        float3 q, n{0, 1, 0};
        const size_t tris = m->idx.size() / 3;
        if (d.emitFrom == FxEmitFrom::Vertex || tris == 0) {
            size_t v = std::min(m->pos.size() - 1, size_t(rng.unit() * float(m->pos.size())));
            q = m->pos[v];
            if (v < m->nrm.size()) n = m->nrm[v];
        } else {
            size_t tri;
            if (d.emitFrom == FxEmitFrom::Edge) tri = std::min(tris - 1, size_t(rng.unit() * float(tris)));
            else {   // area-weighted triangle
                float pick = rng.unit() * m->area.back();
                tri = size_t(std::lower_bound(m->area.begin(), m->area.end(), pick) - m->area.begin());
                tri = std::min(tri, tris - 1);
            }
            const uint32_t i0 = m->idx[tri * 3], i1 = m->idx[tri * 3 + 1], i2 = m->idx[tri * 3 + 2];
            float w1, w2;
            if (d.emitFrom == FxEmitFrom::Edge) {   // one of the triangle's edges
                int e = int(rng.unit() * 3) % 3; float t = rng.unit();
                w1 = e == 0 ? t : e == 1 ? 1 - t : 0; w2 = e == 0 ? 0 : e == 1 ? t : 1 - t;
            } else {
                w1 = rng.unit(); w2 = rng.unit();
                if (w1 + w2 > 1) { w1 = 1 - w1; w2 = 1 - w2; }
            }
            const float w0 = 1 - w1 - w2;
            q = m->pos[i0] * w0 + m->pos[i1] * w1 + m->pos[i2] * w2;
            if (!m->nrm.empty()) n = render::normalize(m->nrm[i0] * w0 + m->nrm[i1] * w1 + m->nrm[i2] * w2);
        }
        // mesh model space -> effect space (the same swap as the default play direction: model axes stay world axes)
        auto toEffect = [](float3 v) { return float3{v.z, v.y, v.x}; };
        off = shapeToWorld(toEffect(mul3(q, d.box)));
        shapeDir = render::normalize(shapeToWorld(toEffect(n)));
        haveShapeDir = true;
        break;
    }
    }
    const float3 local = off;   // shape point (radial / spherical directions start from the shape centre)
    if (d.randomPosition > 0) off += rng.unitVector() * (d.randomPosition * rng.unit());
    if (d.offset.x != 0 || d.offset.y != 0 || d.offset.z != 0) off += toWorld(d.offset);

    // ---- direction
    float3 dir;
    if (rimFrac >= 0) {   // Unity-style cone: the direction opens with the distance from the centre
        float ang = d.spread * kDeg * rimFrac;
        dir = render::normalize(axis * std::cos(ang) + rim * std::sin(ang));
        if (d.length > 0 && d.emitFrom != FxEmitFrom::Edge) {   // cone volume: start anywhere along the length
            off += dir * (d.length * rng.unit());
        }
    } else if (haveShapeDir && d.spread >= 180) {
        dir = shapeDir;   // donut / mesh normals (Unity: emitted along the surface normal)
    } else {
        float3 at2, ab;
        float3 ax0 = haveShapeDir ? shapeDir : axis;
        basisFrom(ax0, at2, ab);
        float ct = 1 - rng.unit() * (1 - std::cos(d.spread * kDeg));
        float st = std::sqrt(std::max(0.0f, 1 - ct * ct)), ph = rng.unit() * 2 * kPi;
        dir = ax0 * ct + at2 * (st * std::cos(ph)) + ab * (st * std::sin(ph));
    }
    if (d.radial != 0 && render::length(local) > 1e-5f)   // outward from (or into) the shape centre
        dir = render::normalize(render::lerp(dir, render::normalize(local) * (d.radial > 0 ? 1.0f : -1.0f), std::fabs(d.radial)));
    if (d.sphericalDirection > 0 && render::length(local) > 1e-5f)
        dir = render::normalize(render::lerp(dir, render::normalize(local), d.sphericalDirection));
    if (d.randomDirection > 0) dir = render::normalize(render::lerp(dir, rng.unitVector(), d.randomDirection));

    // ---- start values (curves follow the emitter time)
    Particle p;
    p.pos = at + off * s;
    p.vel = dir * (rng.value(d.speed, tEm) * s);
    p.age = 0;
    float life = std::max(0.01f, rng.value(d.life, tEm)) * in.lifeMul;
    if (d.lifeBySpeed.enabled)
        life *= sampleKeys(d.lifeBySpeed.keys, speedFrac(d.lifeBySpeed.speed, render::length(in.vel) / s), 1);
    p.invLife = 1.0f / std::max(0.01f, life);
    p.size = rng.value(d.size, tEm) * s * in.sizeMul;
    p.rot = rng.value(d.rotation, tEm) * kDeg + in.rotAdd;
    p.spin = d.spin.curved() ? 0.0f : rng.range(d.spin) * kDeg;   // curves: evaluated over the lifetime
    p.scale = s;
    p.tint = in.tint;
    p.effect = uint16_t(effect);
    p.emitter = uint16_t(emitter);
    p.batch = er.batch;
    p.owner = owner;
    p.vtot = p.vel;
    p.dir0 = dir;
    // optional modules: no RNG draws unless used (old effects keep their exact random sequence)
    if (!d.startColor.empty()) {
        float4 c;
        if (d.startColor.size() == 1) c = d.startColor[0].rgba;
        else if (d.startColorOverTime) {
            c = er.startLut[lutIndex(tEm)];
            if (!d.startColorMin.empty()) c = lerp4(er.startMinLut[lutIndex(tEm)], c, rng.unit());
        } else if (!d.startColorMin.empty()) {
            const size_t li = lutIndex(rng.unit());
            c = lerp4(er.startMinLut[li], er.startLut[li], rng.unit());
        } else c = er.startLut[lutIndex(rng.unit())];
        p.tint = mul(p.tint, c);
    }
    if (d.size3D) {
        const float sx = std::max(1e-4f, p.size / (s * in.sizeMul));
        p.aspect = rng.value(d.sizeY, tEm) / sx;
        p.depthK = rng.value(d.sizeZ, tEm) / sx;
    } else if (d.aspect.min != 1 || d.aspect.max != 1 || d.aspect.curved()) p.aspect = rng.value(d.aspect, tEm);
    if (d.startFrame.max > 0 || d.startFrame.curved()) p.frame0 = uint8_t(std::clamp(rng.value(d.startFrame, tEm), 0.0f, 255.0f));
    if (d.velocity.x != 0 || d.velocity.y != 0 || d.velocity.z != 0) p.addVel = toWorld(d.velocity) * s;
    if (d.rotation3D) { p.rotX = rng.value(d.rotationX, tEm) * kDeg; p.rotY = rng.value(d.rotationY, tEm) * kDeg; }
    if (!d.gravityScale.constant() || d.gravityScale.min != 1) p.grav = rng.value(d.gravityScale, tEm);
    if (d.inheritVelocity.enabled && !d.inheritVelocity.current) p.vel += in.vel * rng.value(d.inheritVelocity.amount, 0);
    if (d.flip.x > 0 && rng.unit() < d.flip.x) p.flags |= 1;
    if (d.flip.y > 0 && rng.unit() < d.flip.y) p.flags |= 2;
    if (d.row == -2) p.row = uint8_t(std::min(d.frameRows - 1, int(rng.unit() * float(d.frameRows))));
    else if (d.row >= 0) p.row = uint8_t(std::min(d.row, d.frameRows - 1));
    if (er.meshIds.size() > 1) p.mesh = uint8_t(std::min(er.meshIds.size() - 1, size_t(rng.unit() * float(er.meshIds.size()))));
    if (d.alignToDirection && d.meshes.empty()) {   // billboards: the quad's up axis along the start direction (screen space at draw time)
        p.flags |= 16;
    }
    p.uid = ++nextUid == 0 ? ++nextUid : nextUid;
    p.depth = in.depth;
    if (d.trail.enabled && !d.trail.ribbon && quality >= d.trail.minQuality && (d.trail.ratio >= 1 || prand(p.uid, 77) < d.trail.ratio)) {
        if (freeTrails.empty()) { freeTrails.push_back(int32_t(trails.size())); trails.emplace_back(); }
        p.trail = freeTrails.back();
        freeTrails.pop_back();
        Trail& tr = trails[size_t(p.trail)];
        tr.count = 1; tr.head = 0; tr.p[0] = p.pos; tr.t[0] = clock;
        tr.time = d.trail.lifeFraction > 0 ? d.trail.lifeFraction * life : d.trail.time;
        if (d.trail.sizeAffectsLifetime) tr.time *= std::max(0.05f, p.size / s);
    }
    ++in.alive[size_t(emitter)];
    ++in.live;
    if (er.modules && !d.subEmitters.empty()) {
        const size_t before = pending.size();
        queueSub(p, FxSubEvent::Birth, p.pos);
        p.child = pending.size() > before;
    }
    particles.push_back(p);
    return true;
}

} // namespace rts::fx
