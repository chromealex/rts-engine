// Drawing: billboards, trails, ribbons, fading trails of dead particles, mesh particles; particle lights.
#include <algorithm>
#include <cmath>
#include <cstring>
#include "FxInternal.h"

namespace rts::fx {

namespace {

int blendGroup(FxBlend b) {   // draw order: opaque, then alpha-like, then additive-like
    switch (b) {
    case FxBlend::Opaque: return 0;
    case FxBlend::Additive: case FxBlend::Subtract: return 2;
    default: return 1;
    }
}

uint32_t packFor(FxBlend b, float4 c) {   // premultiplied textures want a premultiplied tint
    return packColor(b == FxBlend::Premultiplied ? float4{c.x * c.w, c.y * c.w, c.z * c.w, c.w} : c);
}

struct Frame { float u0, v0, u1, v1; int sprite = -1; };

} // namespace

void ParticleSystem::render(const FxFog& fog) {
    auto& I = *impl_;
    stats_.drawn = stats_.batches = stats_.drawCalls = 0;
    stats_.fill = 0;
    stats_.trailQuads = stats_.meshVerts = 0;
    if (!I.dev || !I.haveView || (I.particles.empty() && I.orphans.empty()) || quality_ == FxQuality::Off) return;

    const float capK = I.limits.maxScreenSize * 2.0f / I.p11;      // size cap per unit of clip w
    const int trailStep = quality_ >= FxQuality::High ? 1 : 2;   // medium: half the trail points
    const int meshBudget = quality_ <= FxQuality::Low ? 4000 : quality_ == FxQuality::Medium ? 16000 : 64000;
    const bool sortTier = I.limits.sortAlpha;

    // ---- flipbook frame of a particle
    auto frameOf = [&](const Impl::Particle& p, const EmitterDef& d, const Impl::EmitterRT& er, float t) {
        const bool sprites = !er.spriteTex.empty();
        const FxTexture& tx = sprites ? er.spriteTex[0] : er.tex;
        Frame fr{tx.u0, tx.v0, tx.u1, tx.v1};
        int n = sprites ? int(er.spriteTex.size()) : er.frames;
        const bool row = !sprites && d.row != -1;
        if (row) n = d.frameCols;
        if (n > 1) {
            int f;
            if (d.framesBySpeed.max > 0) f = std::min(n - 1, int(speedFrac(d.framesBySpeed, render::length(p.vtot) / p.scale) * float(n)));
            else if (d.frameOverTime.curved() || d.frameOverTime.max > 0)
                f = std::min(n - 1, int(d.frameOverTime.at(t, prand(p.uid, 61)) * float(n) * d.cycles) % n);
            else f = d.fps > 0 ? int(p.age * d.fps) % n
                   : d.cycles == 1 ? std::min(n - 1, int(t * float(n)))
                                   : int(t * float(n) * d.cycles) % n;
            if (p.frame0) f = (f + p.frame0) % n;
            if (sprites) {
                const FxTexture& s = er.spriteTex[size_t(f)];
                fr = {s.u0, s.v0, s.u1, s.v1, f};
            } else {
                const int cell = row ? int(p.row) * d.frameCols + f : f;
                const int cx = cell % d.frameCols, cy = cell / d.frameCols;
                fr.u0 = er.tex.u0 + er.du * float(cx); fr.u1 = fr.u0 + er.du;
                fr.v0 = er.tex.v0 + er.dv * float(cy); fr.v1 = fr.v0 + er.dv;
            }
        }
        if (p.flags & 1) std::swap(fr.u0, fr.u1);
        if (p.flags & 2) std::swap(fr.v0, fr.v1);
        return fr;
    };
    auto depthKey = [&](const EmitterDef& d, int group, float w, float age) -> float {
        switch (d.sort) {
        case FxSort::None: return 0;
        case FxSort::Distance: return sortTier ? -w : 0;
        case FxSort::Oldest: return age;      // oldest drawn last = in front
        case FxSort::Youngest: return -age;
        case FxSort::Auto: break;
        }
        return group == 1 && sortTier ? -w : 0;
    };

    // ---- 1) visible particles -> records (clip-space culling, on-screen size caps, fill estimate)
    I.recs.clear();
    struct RibbonPt { uint32_t owner; uint16_t emitter; uint32_t uid, idx; };
    std::vector<RibbonPt> ribbonPts;
    for (uint32_t i = 0; i < I.particles.size(); ++i) {
        const auto& p = I.particles[i];
        const auto& er = I.rtOf(p);
        const EmitterDef& d = I.defOf(p);
        const float t = I.lifeT(p);
        float4 c = I.viewProj * float4{p.pos.x, p.pos.y, p.pos.z, 1};
        if (d.trail.enabled && d.trail.ribbon && quality_ >= d.trail.minQuality) ribbonPts.push_back({p.owner, p.emitter, p.uid, i});
        if (c.w <= 0.05f) continue;
        const int group = blendGroup(d.blend);
        if (p.trail >= 0 && quality_ >= d.trail.minQuality) {   // trail segments, independent of the head's visibility
            int idx[kTrailPts];
            const int segs = Impl::trailIndices(I.trails[size_t(p.trail)], trailStep, idx);
            const int tg = blendGroup(d.trail.blend);
            for (int k = 0; k < segs; ++k) {
                Impl::Rec r{i, c.w, 0, er.trailBatch, int16_t(k)};
                r.kind = Impl::TrailSeg; r.group = uint8_t(tg); r.order = int8_t(d.sortOrder); r.key = depthKey(d, tg, c.w, p.age);
                I.recs.push_back(r);
                ++stats_.trailQuads;
            }
        }
        if (d.hidden) continue;
        float size = I.sizeNow(p, t);
        const float maxScreen = d.maxScreenSize > 0 ? std::min(capK, d.maxScreenSize * 2.0f / I.p11) : capK;
        size = std::min(size, maxScreen * c.w);
        if (d.minScreenSize > 0) size = std::max(size, d.minScreenSize * 2.0f / I.p11 * c.w);
        if (size <= 0) continue;
        const bool mesh = !er.meshIds.empty();
        float reach = std::max(1.0f, p.aspect) + d.stretch * render::length(p.vtot);
        if (mesh) reach = std::max(reach, I.meshes[size_t(er.meshIds[p.mesh])].radius * 2 * std::max({1.0f, p.aspect, p.depthK}));
        const float ext = size * I.p11 * reach;
        if (c.x - ext > c.w || c.x + ext < -c.w || c.y - ext > c.w || c.y + ext < -c.w) continue;
        float4 col = I.colorNow(p);
        if (!d.colorBySpeed.empty()) col = mul(col, speedLut(er.colorSpeed, d.colorSpeed, render::length(p.vtot) / p.scale));
        if (col.w <= 0.004f && d.blend != FxBlend::Opaque) continue;
        const float fh = size * I.p11 / c.w * 0.5f;                  // fraction of screen height
        stats_.fill += fh * fh * p.aspect / I.aspect;
        Impl::Rec r{i, c.w, size, p.batch};
        if (!er.spriteBatch.empty()) {
            Frame fr = frameOf(p, d, er, t);
            r.batch = er.spriteBatch[size_t(std::max(0, fr.sprite))];
        }
        r.kind = mesh ? Impl::MeshPart : Impl::Quad;
        r.group = uint8_t(group);
        r.order = int8_t(d.sortOrder);
        r.key = depthKey(d, group, c.w, p.age);
        I.recs.push_back(r);
    }
    // ribbons: the particles of one play and emitter connected in birth order (newest = head)
    if (!ribbonPts.empty()) {
        std::sort(ribbonPts.begin(), ribbonPts.end(), [](const RibbonPt& a, const RibbonPt& b) {
            if (a.owner != b.owner) return a.owner < b.owner;
            if (a.emitter != b.emitter) return a.emitter < b.emitter;
            return a.uid > b.uid;
        });
        std::vector<uint32_t> line;
        for (size_t s = 0; s < ribbonPts.size();) {
            size_t e = s;
            while (e < ribbonPts.size() && ribbonPts[e].owner == ribbonPts[s].owner && ribbonPts[e].emitter == ribbonPts[s].emitter) ++e;
            const auto& p0 = I.particles[ribbonPts[s].idx];
            const EmitterDef& d = I.defOf(p0);
            const auto& er = I.rtOf(p0);
            const int ribbons = std::max(1, d.trail.ribbons);
            const int tg = blendGroup(d.trail.blend);
            for (int rb = 0; rb < ribbons; ++rb) {
                line.clear();
                for (size_t k = s + size_t(rb); k < e; k += size_t(ribbons)) line.push_back(ribbonPts[k].idx);
                if (line.size() < 2) continue;
                const float n1 = float(line.size() - 1);
                for (size_t k = 0; k + 1 < line.size(); k += size_t(trailStep)) {
                    const size_t k2 = std::min(line.size() - 1, k + size_t(trailStep));
                    const auto& pa = I.particles[line[k]];
                    float4 c = I.viewProj * float4{pa.pos.x, pa.pos.y, pa.pos.z, 1};
                    Impl::Rec r{line[k], std::max(c.w, 0.06f), 0, er.trailBatch};
                    r.kind = Impl::RibbonSeg; r.idx2 = line[k2]; r.f0 = float(k) / n1; r.f1 = float(k2) / n1;
                    r.group = uint8_t(tg); r.order = int8_t(d.sortOrder); r.key = depthKey(d, tg, c.w, pa.age);
                    I.recs.push_back(r);
                    ++stats_.trailQuads;
                }
            }
            s = e;
        }
    }
    // trails whose particle died
    for (uint32_t o = 0; o < I.orphans.size(); ++o) {
        const auto& orp = I.orphans[o];
        const EmitterDef& d = I.effects[orp.effect].def.emitters[orp.emitter];
        if (quality_ < d.trail.minQuality) continue;
        const auto& tr = I.trails[size_t(orp.trail)];
        int idx[kTrailPts];
        const int segs = Impl::trailIndices(tr, trailStep, idx) - 1;
        const float3 h = tr.at(0);
        float4 c = I.viewProj * float4{h.x, h.y, h.z, 1};
        const int tg = blendGroup(d.trail.blend);
        for (int k = 0; k < segs; ++k) {
            Impl::Rec r{o, std::max(c.w, 0.06f), 0, I.effects[orp.effect].em[orp.emitter].trailBatch, int16_t(k)};
            r.kind = Impl::OrphanSeg; r.group = uint8_t(tg); r.order = int8_t(d.sortOrder); r.key = tg == 1 && sortTier ? -c.w : 0;
            I.recs.push_back(r);
            ++stats_.trailQuads;
        }
    }
    if (I.recs.empty()) return;

    // ---- 2) order: opaque, alpha-like (sorted back to front on capable tiers), additive-like; then sort order, batch, kind
    std::sort(I.recs.begin(), I.recs.end(), [](const Impl::Rec& a, const Impl::Rec& b) {
        if (a.group != b.group) return a.group < b.group;
        if (a.order != b.order) return a.order < b.order;
        if (a.batch != b.batch) return a.batch < b.batch;
        const bool am = a.kind == Impl::MeshPart, bm = b.kind == Impl::MeshPart;
        if (am != bm) return !am;
        return a.key < b.key;
    });

    // ---- 3) expand on the CPU: quads into verts, mesh triangles into meshVerts
    I.verts.clear();
    I.meshVerts.clear();
    I.verts.reserve(I.recs.size() * 4);
    std::vector<uint32_t> vstart(I.recs.size()), vcount(I.recs.size());
    auto quad = [&](float3 a, float3 b, float3 c, float3 d4, float ua, float va, float ub, float vb, uint32_t ca, uint32_t cb) {
        // a-b along the first edge (u: ua -> ub), d-c the opposite edge; ca at a/d, cb at b/c
        I.verts.push_back({a, {ua, vb}, ca});
        I.verts.push_back({b, {ub, vb}, cb});
        I.verts.push_back({c, {ub, va}, cb});
        I.verts.push_back({d4, {ua, va}, ca});
    };
    int meshVertsUsed = 0;
    for (size_t ri = 0; ri < I.recs.size(); ++ri) {
        const auto& r = I.recs[ri];
        const bool meshRec = r.kind == Impl::MeshPart;
        vstart[ri] = uint32_t(meshRec ? I.meshVerts.size() : I.verts.size());
        if (r.kind == Impl::OrphanSeg) {
            const auto& orp = I.orphans[r.idx];
            const auto& er = I.effects[orp.effect].em[orp.emitter];
            const FxTrail& td = I.effects[orp.effect].def.emitters[orp.emitter].trail;
            const auto& tr = I.trails[size_t(orp.trail)];
            int idx[kTrailPts];
            const int pts = Impl::trailIndices(tr, trailStep, idx);
            auto corner = [&](int q, float3& left, float3& right, uint32_t& col, float& f) {
                q = std::clamp(q, 0, pts - 1);
                float3 a = tr.at(idx[q]), prev = tr.at(idx[std::max(0, q - 1)]), next = tr.at(idx[std::min(pts - 1, q + 1)]);
                float3 dir = prev - next;
                float3 side = render::cross(dir, I.eye - a);
                float sl = render::length(side);
                side = sl > 1e-5f ? side * (1.0f / sl) : I.right;
                f = std::clamp((I.clock - tr.timeAt(idx[q])) / tr.time, 0.0f, 1.0f);
                const size_t fi = lutIndex(f);
                float w = er.trailWidth[fi] * (td.sizeAffectsWidth ? orp.headSize : orp.scale) * 0.5f;
                left = a - side * w; right = a + side * w;
                col = packFor(td.blend, mul(er.trailColor[fi], orp.base));
            };
            float3 l0, r0, l1, r1; uint32_t c0, c1; float f0, f1;
            corner(r.seg, l0, r0, c0, f0);
            corner(r.seg + 1, l1, r1, c1, f1);
            const FxTexture& tx = er.trailTex;
            quad(l0, l1, r1, r0, tx.u0 + (tx.u1 - tx.u0) * f0, tx.v0, tx.u0 + (tx.u1 - tx.u0) * f1, tx.v1, c0, c1);
            vcount[ri] = 4;
            continue;
        }
        const auto& p = I.particles[r.idx];
        const auto& er = I.rtOf(p);
        const EmitterDef& d = I.defOf(p);
        const float t = I.lifeT(p);
        const size_t li = lutIndex(t);
        if (r.kind == Impl::TrailSeg) {   // one trail segment: from point k to k+1 (point 0 = the particle itself)
            const auto& tr = I.trails[size_t(p.trail)];
            int idx[kTrailPts];
            const int segs = Impl::trailIndices(tr, trailStep, idx);
            const int k = std::min<int>(r.seg, segs - 1);
            auto point = [&](int q) { return q <= 0 ? p.pos : tr.at(idx[std::min(q - 1, segs - 1)]); };
            auto age = [&](int q) { return q <= 0 ? 0.0f : std::clamp((I.clock - tr.timeAt(idx[std::min(q - 1, segs - 1)])) / tr.time, 0.0f, 1.0f); };
            const float headSize = p.size * er.size[li];
            float4 base = d.trail.inheritColor ? I.colorNow(p) : p.tint;
            if (!d.trail.lifeColor.empty()) base = mul(base, er.trailLife[li]);
            const FxTrail& td = d.trail;
            auto corner = [&](int q, float3& left, float3& right, uint32_t& col, float& f) {
                float3 a = point(q), prev = point(q - 1), next = point(q + 1);
                float3 dir = q == 0 ? a - next : prev - next;
                float3 side = render::cross(dir, I.eye - a);
                float sl = render::length(side);
                side = sl > 1e-5f ? side * (1.0f / sl) : I.right;
                f = age(q);
                const size_t fi = lutIndex(f);
                float w = er.trailWidth[fi] * (td.sizeAffectsWidth ? headSize : p.scale) * 0.5f;
                left = a - side * w; right = a + side * w;
                col = packFor(td.blend, mul(er.trailColor[fi], base));
            };
            float3 l0, r0, l1, r1; uint32_t c0, c1; float f0, f1;
            corner(k, l0, r0, c0, f0);
            corner(k + 1, l1, r1, c1, f1);
            const FxTexture& tx = er.trailTex;
            const float du = tx.u1 - tx.u0;
            if (td.textureMode == FxTextureMode::Distribute) {   // evenly along the points, whatever their spacing
                f0 = float(k) / float(segs); f1 = float(k + 1) / float(segs);
                quad(l0, l1, r1, r0, tx.u0 + du * f0, tx.v0, tx.u0 + du * f1, tx.v1, c0, c1);
                vcount[ri] = 4;
            } else if (td.textureMode == FxTextureMode::Tile) {  // repeats once per unit of length (split at the repeats)
                float along = 0;
                for (int q = 0; q < k; ++q) along += render::length(point(q + 1) - point(q)) / p.scale;
                const float len = render::length(point(k + 1) - point(k)) / p.scale;
                float a0 = along, a1 = along + len;
                int pieces = 0;
                float s0 = a0;
                while (s0 < a1 - 1e-5f && pieces < 6) {
                    float s1 = std::min(a1, std::floor(s0) + 1.0f);
                    if (pieces == 5) s1 = a1;
                    const float g0 = len > 0 ? (s0 - a0) / len : 0, g1 = len > 0 ? (s1 - a0) / len : 1;
                    auto lerp3 = [](float3 x, float3 y, float g) { return x + (y - x) * g; };
                    const float fu0 = s0 - std::floor(s0), fu1 = s1 - std::floor(s0);
                    quad(lerp3(l0, l1, g0), lerp3(l0, l1, g1), lerp3(r0, r1, g1), lerp3(r0, r1, g0),
                         tx.u0 + du * fu0, tx.v0, tx.u0 + du * std::min(1.0f, fu1), tx.v1, c0, c1);
                    ++pieces;
                    s0 = s1;
                }
                vcount[ri] = uint32_t(pieces * 4);
            } else {
                quad(l0, l1, r1, r0, tx.u0 + du * f0, tx.v0, tx.u0 + du * f1, tx.v1, c0, c1);
                vcount[ri] = 4;
            }
            continue;
        }
        if (r.kind == Impl::RibbonSeg) {
            const auto& pb = I.particles[r.idx2];
            const FxTrail& td = d.trail;
            auto corner = [&](const Impl::Particle& q, float3 other, float f, bool head, float3& left, float3& right, uint32_t& col) {
                float3 dir = head ? q.pos - other : other - q.pos;
                float3 side = render::cross(dir, I.eye - q.pos);
                float sl = render::length(side);
                side = sl > 1e-5f ? side * (1.0f / sl) : I.right;
                const size_t fi = lutIndex(f), qi = lutIndex(I.lifeT(q));
                float w = er.trailWidth[fi] * (td.sizeAffectsWidth ? q.size * er.size[qi] : q.scale) * 0.5f;
                left = q.pos - side * w; right = q.pos + side * w;
                float4 base = td.inheritColor ? I.colorNow(q) : q.tint;
                if (!td.lifeColor.empty()) base = mul(base, er.trailLife[qi]);
                col = packFor(td.blend, mul(er.trailColor[fi], base));
            };
            float3 l0, r0, l1, r1; uint32_t c0, c1;
            corner(p, pb.pos, r.f0, true, l0, r0, c0);
            corner(pb, p.pos, r.f1, false, l1, r1, c1);
            const FxTexture& tx = er.trailTex;
            const float du = tx.u1 - tx.u0;
            quad(l0, l1, r1, r0, tx.u0 + du * r.f0, tx.v0, tx.u0 + du * r.f1, tx.v1, c0, c1);
            vcount[ri] = 4;
            continue;
        }
        float4 colf = I.colorNow(p);
        if (!d.colorBySpeed.empty()) colf = mul(colf, speedLut(er.colorSpeed, d.colorSpeed, render::length(p.vtot) / p.scale));
        const Frame fr = frameOf(p, d, er, t);
        const float sizeK = d.sizeOverLifeY.empty() ? 1.0f : er.sizeYLut[li] / std::max(1e-4f, er.size[li]);
        if (meshRec) {   // mesh particle: triangles with CPU shading (sun lambert), no index buffer
            const auto& m = I.meshes[size_t(er.meshIds[p.mesh])];
            if (!m.valid || meshVertsUsed + int(m.idx.size()) > meshBudget) { vcount[ri] = 0; continue; }
            meshVertsUsed += int(m.idx.size());
            const float zK = d.sizeOverLifeZ.empty() ? 1.0f : er.sizeZLut[li] / std::max(1e-4f, er.size[li]);
            const float3 sc{r.size, r.size * p.aspect * sizeK, r.size * p.depthK * zK};
            Mat3 R = Mat3::euler({p.rotX / kDeg, p.rotY / kDeg, p.rot / kDeg});
            float3 zAxis{0, 0, 0};
            if (d.align == FxAlign::Velocity && render::length(p.vtot) > 1e-3f) zAxis = render::normalize(p.vtot);
            else if (d.alignToDirection) zAxis = p.dir0;
            if (zAxis.x != 0 || zAxis.y != 0 || zAxis.z != 0) {   // +Z along the direction, then the particle's own rotation
                float3 xa, ya;
                basisFrom(zAxis, xa, ya);
                Mat3 A; A.c0 = xa; A.c1 = ya; A.c2 = zAxis; A.identity = false;
                Mat3 C; C.c0 = A * R.c0; C.c1 = A * R.c1; C.c2 = A * R.c2; C.identity = false;
                R = C;
            }
            const bool lit = d.blend != FxBlend::Additive && d.blend != FxBlend::Subtract;
            const float du = fr.u1 - fr.u0, dv = fr.v1 - fr.v0;
            for (uint32_t vi : m.idx) {
                const float3 w = p.pos + R * mul3(m.pos[vi], sc);
                float shade = 1;
                if (lit) shade = 0.55f + 0.45f * std::max(0.0f, render::dot(render::normalize(R * m.nrm[vi]), I.lightDir));
                const float2 uv = m.uv[vi];
                I.meshVerts.push_back({w, {fr.u0 + du * std::clamp(uv.x, 0.0f, 1.0f), fr.v0 + dv * std::clamp(uv.y, 0.0f, 1.0f)},
                                       packFor(d.blend, float4{colf.x * shade, colf.y * shade, colf.z * shade, colf.w})});
            }
            vcount[ri] = uint32_t(m.idx.size());
            stats_.meshVerts += int(m.idx.size());
            continue;
        }
        const uint32_t col = packFor(d.blend, colf);
        const float h = r.size * 0.5f, hl = h * p.aspect * sizeK;   // half width, half length
        float3 ax, ay;
        auto rotated = [&](float3 ex, float3 ey, float a) {
            const float cs = std::cos(a), sn = std::sin(a);
            ax = (ex * cs + ey * sn) * h;
            ay = (ey * cs - ex * sn) * hl;
        };
        if (d.align == FxAlign::Velocity && (render::length(p.vtot) > 1e-3f || d.cameraStretch != 0)) {
            const float3 vv = p.vtot - I.camVel * d.cameraStretch;
            const float sp = render::length(vv);
            if (sp > 1e-3f) {
                float3 axis = vv * (1.0f / sp);
                float3 side = render::cross(axis, render::normalize(I.eye - p.pos));
                float sl = render::length(side);
                side = sl > 1e-4f ? side * (1.0f / sl) : I.right;
                ax = side * h;
                ay = axis * (hl + d.stretch * render::length(p.vtot) * 0.5f + std::fabs(d.cameraStretch) * render::length(I.camVel) * 0.5f);
            } else rotated(I.right, I.up, p.rot);
        } else if (d.align == FxAlign::Ground) {
            float cs = std::cos(p.rot), sn = std::sin(p.rot);
            ax = float3{cs, 0, sn} * h;
            ay = float3{-sn, 0, cs} * hl;
        } else if (d.align == FxAlign::Upright) {
            float3 rx = render::length(float3{I.right.x, 0, I.right.z}) > 1e-4f ? render::normalize(float3{I.right.x, 0, I.right.z}) : float3{1, 0, 0};
            ax = rx * h;
            ay = float3{0, 1, 0} * hl;
        } else if (d.align == FxAlign::World) {
            rotated({1, 0, 0}, {0, 1, 0}, p.rot);
        } else if (d.align == FxAlign::Local) {
            const Impl::Instance* in = I.inst(p.owner);
            rotated(in ? in->toWorld({1, 0, 0}) : float3{1, 0, 0}, in ? in->toWorld({0, 1, 0}) : float3{0, 1, 0}, p.rot);
        } else {
            float a = p.rot;
            if (p.flags & 16) a += std::atan2(render::dot(p.dir0, I.up), render::dot(p.dir0, I.right)) - kPi * 0.5f;   // up axis along the start direction
            rotated(I.right, I.up, a);
        }
        float3 pos = d.align == FxAlign::Ground ? p.pos + float3{0, 0.03f, 0} : p.pos;
        if (d.pivot.x != 0 || d.pivot.y != 0 || d.pivot.z != 0)
            pos += ax * (2 * d.pivot.x) + ay * (2 * d.pivot.y) + render::normalize(I.eye - pos) * (d.pivot.z * r.size);
        quad(pos - ax - ay, pos + ax - ay, pos + ax + ay, pos - ax + ay, fr.u0, fr.v0, fr.u1, fr.v1, col, col);
        vcount[ri] = 4;
    }
    const uint32_t meshBase = uint32_t(I.verts.size());
    if (!I.meshVerts.empty()) I.verts.insert(I.verts.end(), I.meshVerts.begin(), I.meshVerts.end());
    if (I.verts.empty()) return;
    I.dev->updateBuffer(I.vb, I.verts.data(), I.verts.size() * sizeof(Vertex));   // one upload per frame

    // ---- 4) one draw per run of (batch, kind) (quads split at 16k)
    struct U { float viewProj[16]; float fog[4]; float cam[4]; float params[4]; } u;
    std::memcpy(u.viewProj, I.viewProj.m, sizeof u.viewProj);
    u.fog[0] = aces(fog.color.x * fog.color.x); u.fog[1] = aces(fog.color.y * fog.color.y); u.fog[2] = aces(fog.color.z * fog.color.z);
    u.fog[3] = fog.density;
    u.cam[0] = I.eye.x; u.cam[1] = I.eye.y; u.cam[2] = I.eye.z; u.cam[3] = 1;
    I.dev->setVertexBuffer(I.vb);
    I.dev->setIndexBuffer(I.ib);
    gfx::PipelineHandle lastPipe;
    size_t i = 0;
    while (i < I.recs.size()) {
        const uint16_t b = I.recs[i].batch;
        const bool meshRun = I.recs[i].kind == Impl::MeshPart;
        size_t j = i;
        while (j < I.recs.size() && I.recs[j].batch == b && (I.recs[j].kind == Impl::MeshPart) == meshRun) ++j;
        const auto& batch = I.batches[b];
        const int pi = int(batch.blend);
        const gfx::PipelineHandle pipe = meshRun ? I.meshPipe[pi] : I.pipe[pi];
        if (!(pipe == lastPipe)) { I.dev->setPipeline(pipe); lastPipe = pipe; }
        u.params[0] = batch.blend == FxBlend::Additive || batch.blend == FxBlend::Subtract ? 1.0f : 0.0f;
        u.params[1] = batch.blend == FxBlend::Premultiplied ? 1.0f : 0.0f;
        u.params[2] = batch.blend == FxBlend::Multiply ? 1.0f : 0.0f;
        u.params[3] = 0;
        I.dev->setUniforms(0, &u, sizeof u);
        I.dev->setTexture(0, batch.tex.valid() ? batch.tex : I.atlas);
        uint32_t first = vstart[i], count = 0;
        for (size_t k = i; k < j; ++k) count += vcount[k];
        if (count > 0) {
            if (meshRun) {
                I.dev->draw(count, meshBase + first);
                ++stats_.drawCalls;
            } else {
                const uint32_t quads = count / 4;
                for (uint32_t q = 0; q < quads; q += kQuadsPerDraw) {
                    const uint32_t n = std::min(kQuadsPerDraw, quads - q);
                    I.dev->drawIndexed(n * 6, 0, int32_t(first + q * 4));
                    ++stats_.drawCalls;
                }
            }
            ++stats_.batches;
        }
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
        const EmitterDef& d = I.defOf(p);
        if (!d.light.enabled) continue;
        const FxLight& l = d.light;
        if (l.ratio < 1 && float((p.uid * 2654435761u) >> 8) * (1.0f / 16777216.0f) >= l.ratio) continue;   // stable choice per particle
        auto it = std::find_if(counts.begin(), counts.end(), [&](const Count& c) { return c.owner == p.owner && c.emitter == p.emitter; });
        if (it == counts.end()) { counts.push_back({p.owner, p.emitter, 0}); it = counts.end() - 1; }
        if (it->n >= l.max) continue;
        ++it->n;
        const auto& er = I.rtOf(p);
        const size_t li = lutIndex(I.lifeT(p));
        float4 pc = I.colorNow(p);
        FxPointLight fl;
        fl.pos = p.pos;
        fl.color = l.useParticleColor ? float3{l.color.x * pc.x, l.color.y * pc.y, l.color.z * pc.z} : float3{l.color.x, l.color.y, l.color.z};
        fl.radius = l.radius * p.scale * (l.sizeAffectsRadius ? p.size * er.size[li] / p.scale : 1.0f);
        fl.intensity = l.intensity * (l.alphaAffectsIntensity ? pc.w : 1.0f);
        if (!l.radiusOverLife.empty()) fl.radius *= er.lightRadius[li];
        if (!l.intensityOverLife.empty()) fl.intensity *= er.lightIntensity[li];
        if (fl.intensity <= 0.01f || fl.radius <= 0.01f) continue;
        out.push_back(fl);
        ++added;
    }
}

} // namespace rts::fx
