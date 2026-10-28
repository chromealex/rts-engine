// Geometry of the overlay shapes (world pass) and the screen pass (ImGui draw list).
#include "RtsOverlays.h"
#include "RtsView.h"
#include "../RtsModule.h"
#include "RtsOverlayShaders.h"
#include "imgui.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace rts::game {

using namespace render;

namespace {
constexpr float kPi = 3.14159265358979f;   // same constant as Renderer3D::circleXZ (built-in rings match it exactly)
float3 toF(const Vec3& v) { return {float(v.x.toDouble()), float(v.y.toDouble()), float(v.z.toDouble())}; }
uint32_t scaleAlpha(uint32_t c, float k) {
    const float a = float(c >> 24) * std::clamp(k, 0.0f, 1.0f);
    return (c & 0x00ffffffu) | (uint32_t(a + 0.5f) << 24);
}
RtsModule* moduleOf(Simulation* sim) { return sim ? dynamic_cast<RtsModule*>(&sim->module()) : nullptr; }
ImU32 imCol(float4 c) { return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, c.w)); }
} // namespace

float RtsOverlays::radiusFor(const OverlayDef& d, const Inst& in) const {
    const UnitType* t = in.type < rules_->types.size() ? &rules_->types[in.type] : nullptr;
    Entity e = in.obj >= 0 && scene_ ? scene_->objects[size_t(in.obj)].entity : kNullEntity;
    RtsModule* m = moduleOf(ctx_.sim);
    const bool live = m && e.valid() && ctx_.sim->world().has<Unit>(e) && ctx_.sim->world().has<Owner>(e);
    float r = d.radius;
    if (t) switch (d.radiusFrom) {
    case OvRadius::Fixed: break;
    case OvRadius::Range: case OvRadius::MinRange:
        if (size_t(d.index) < t->weapons.size()) {
            const WeaponDef& wd = t->weapons[size_t(d.index)];
            r = float((d.radiusFrom == OvRadius::Range ? wd.range : wd.minRange).toDouble()) + float(t->radius.toDouble());
            if (live && d.radiusFrom == OvRadius::Range) r += float(m->stat(*ctx_.sim, e, Stat::Range).toDouble());
        } else r = 0;
        break;
    case OvRadius::Sight: r = live ? float(m->stat(*ctx_.sim, e, Stat::Sight).toDouble()) : float(t->sight.toDouble()); break;
    case OvRadius::Aura: {
        r = 0;
        if (d.index > 0 && size_t(d.index) < t->abilities.size()) r = float(t->abilities[size_t(d.index)].auraRadius.toDouble());
        else for (auto& ab : t->abilities) if (ab.aura) { r = float(ab.auraRadius.toDouble()); break; }
        break;
    }
    case OvRadius::BuildRadius: r = float(rules_->buildRadius) + float(t->radius.toDouble()); break;
    case OvRadius::Selection:
        r = view_ ? view_->selectionRadius(in.type) : t->structure ? std::max(t->footprintW, t->footprintH) * 0.62f : float(t->radius.toDouble()) + 0.15f;
        break;
    case OvRadius::Capture: r = float(t->captureRadius.toDouble()); break;
    case OvRadius::Claim: r = float(t->claimRadius.toDouble()); break;
    case OvRadius::Footprint: r = std::max(t->footprintW, t->footprintH) * 0.5f; break;
    case OvRadius::Ability: r = size_t(d.index) < t->abilities.size() ? float(t->abilities[size_t(d.index)].range.toDouble()) : 0; break;
    case OvRadius::Leash: r = float(t->leash.toDouble()); break;
    }
    else if (d.radiusFrom == OvRadius::BuildRadius) r = float(rules_->buildRadius);
    else if (d.radiusFrom != OvRadius::Fixed) r = 0;
    r = r * d.radiusScale + d.radiusAdd;
    if (in.popup >= 0) r += d.event.grow * in.age;
    return r;
}

// GPU part of init (RtsView calls it): textures of icons, the batch shader and pipelines.
void RtsOverlays::initGpu(gfx::Device& dev) {
    dev_ = &dev;
    images_.assign(set_.defs.size(), {});
    textures_.init(dev);
    for (size_t i = 0; i < set_.defs.size(); ++i)
        if (!set_.defs[i].image.empty()) {
            images_[i] = textures_.resolve(set_.defs[i].image, false, base_);
            if (!images_[i].valid()) set_.warnings.push_back("overlay '" + set_.defs[i].id + "': " + set_.defs[i].image + ": " + textures_.lastError());
        }
    if (!shader_.valid()) {
        gfx::ShaderDesc sd;
        sd.glslVertex = shaders::kOverlayVS; sd.glslFragment = shaders::kOverlayFS; sd.msl = shaders::kOverlayMSL;
        sd.textureSlots = 1; sd.uniformSlots = 1; sd.label = "overlays";
        shader_ = dev.createShader(sd);
        if (shader_.valid())
            for (int p = 0; p < 4; ++p) {
                gfx::PipelineDesc pd;
                pd.shader = shader_;
                pd.attrs[0] = {0, gfx::VertexFormat::Float3, 0};
                pd.attrs[1] = {1, gfx::VertexFormat::Float2, 12};
                pd.attrs[2] = {2, gfx::VertexFormat::UByte4N, 20};
                pd.attrCount = 3; pd.stride = sizeof(BatchVertex);
                pd.indexType = gfx::IndexType::None;
                pd.cull = gfx::CullMode::None;
                pd.blend = (p & 1) ? gfx::BlendMode::Additive : gfx::BlendMode::Alpha;
                pd.depthTest = !(p & 2);
                pd.depthWrite = false;
                pd.label = "overlays";
                pipes_[p] = dev.createPipeline(pd);
            }
        const uint32_t w = 0xffffffffu;
        gfx::TextureDesc td; td.width = 1; td.height = 1; td.data = &w; td.label = "overlays white";
        white_ = dev.createTexture(td);
    }
}

void RtsOverlays::shutdown(gfx::Device& dev) {
    for (auto& p : pipes_) if (p.valid()) { dev.destroyPipeline(p); p = {}; }
    if (shader_.valid()) dev.destroyShader(shader_);
    if (vb_.valid()) dev.destroyBuffer(vb_);
    if (white_.valid()) dev.destroyTexture(white_);
    shader_ = {}; vb_ = {}; white_ = {}; vbBytes_ = 0;
    textures_.shutdown();
    images_.clear();
    buckets_.clear();
    popups_.clear();
    dev_ = nullptr;
}

size_t RtsOverlays::cellCap(const OverlayDef& d) const {
    if (d.builtin) return SIZE_MAX;
    return quality_ == OverlayQuality::Low ? 600 : quality_ == OverlayQuality::Medium ? 3000 : 12000;
}

float RtsOverlays::groundY(const OverlayDef& d, float x, float z, float anchorGround) const {
    switch (d.style.ground) {
    case OvGround::Follow: return (view_ && grid_ ? view_->heightAt(*grid_, x, z) : 0.0f) + lift_;
    case OvGround::Flat: return anchorGround + lift_;
    case OvGround::Zero: return lift_;
    }
    return lift_;
}

void RtsOverlays::tri(const OverlayDef& d, float3 a, float3 b, float3 c, uint32_t col, gfx::TextureHandle tex, const float* uv) {
    if ((col >> 24) == 0) return;
    const bool batch = d.style.onTop || d.style.additive || tex.valid();
    if (!batch) { if (r_) r_->triangle(a, b, c, col); return; }
    if (!dev_) return;
    Bucket& bk = bucket(tex, uint8_t((d.style.additive ? 1 : 0) | (d.style.onTop ? 2 : 0)));
    static const float kNoUv[6] = {0, 0, 0, 0, 0, 0};
    const float* t = uv ? uv : kNoUv;
    bk.verts.push_back({a, t[0], t[1], col});
    bk.verts.push_back({b, t[2], t[3], col});
    bk.verts.push_back({c, t[4], t[5], col});
}

void RtsOverlays::seg(const OverlayDef& d, float3 a, float3 b, uint32_t col) {
    if ((col >> 24) == 0) return;
    const bool batch = d.style.onTop || d.style.additive;
    float w = d.style.thickness;
    if (w <= 0) {
        if (!batch) { if (r_) r_->line(a, b, col); return; }
        w = 0.035f;   // hairline in the batch: a thin ribbon
    }
    float3 n;
    if (d.style.space == OvSpace::Billboard) n = normalize(cross(b - a, cross(camRight_, camUp_)));
    else {
        float dx = b.x - a.x, dz = b.z - a.z, l = std::sqrt(dx * dx + dz * dz);
        if (l < 1e-5f) return;
        n = {-dz / l, 0, dx / l};
    }
    n = n * (w * 0.5f);
    tri(d, a - n, a + n, b + n, col);
    tri(d, a - n, b + n, b - n, col);
}

// Points in world x/z (y given unless `ground`: then y = groundY); dashes and terrain-following
// subdivision applied along the way.
void RtsOverlays::polyline(const OverlayDef& d, const float3* pts, size_t n, bool closed, uint32_t col, float ag, bool ground) {
    if (n < 2) return;
    const OvStyle& s = d.style;
    const bool dashed = s.dashOn > 0 && s.dashOff > 0;
    const float period = s.dashOn + s.dashOff;
    const bool follow = ground && s.ground == OvGround::Follow;
    float dist = dashed ? -std::fmod(float(time_) * s.scroll, period) : 0;
    if (dist < 0) dist += period;
    auto at = [&](float3 p) { return ground ? float3{p.x, groundY(d, p.x, p.z, ag), p.z} : p; };
    auto emitPiece = [&](float3 p0, float3 p1) { seg(d, at(p0), at(p1), col); };
    const size_t segs = closed ? n : n - 1;
    for (size_t k = 0; k < segs; ++k) {
        const float3 a = pts[k], b = pts[(k + 1) % n];
        const float len = ground ? std::sqrt((b.x - a.x) * (b.x - a.x) + (b.z - a.z) * (b.z - a.z)) : length(b - a);
        if (len < 1e-6f) continue;
        const int steps = follow ? std::clamp(int(std::ceil(len / 0.5f)), 1, 256) : 1;
        for (int st = 0; st < steps; ++st) {
            const float t0 = float(st) / float(steps), t1 = float(st + 1) / float(steps);
            const float3 p0 = lerp(a, b, t0), p1 = lerp(a, b, t1);
            const float l = len * (t1 - t0);
            if (!dashed) { emitPiece(p0, p1); continue; }
            float done = 0;
            while (done < l) {
                const float ph = std::fmod(dist + done, period);
                const float runLeft = ph < s.dashOn ? s.dashOn - ph : period - ph;
                const float take = std::min(runLeft, l - done);
                if (ph < s.dashOn) emitPiece(lerp(p0, p1, done / l), lerp(p0, p1, (done + take) / l));
                done += std::max(take, 1e-4f);
            }
            dist += l;
        }
    }
}

void RtsOverlays::ring(const OverlayDef& d, const Inst& in, float radius, uint32_t col, uint32_t fill) {
    if (radius <= 0) return;
    int seg = d.style.segments;
    if (!d.builtin && quality_ == OverlayQuality::Low) seg = std::max(12, seg / 2);
    const float rot = float(time_) * d.style.rotate;
    const float ag = view_ && grid_ ? view_->heightAt(*grid_, in.pos.x, in.pos.z) : 0.0f;
    const bool bill = d.style.space == OvSpace::Billboard;
    tmpPts_.clear();
    const float3 c = bill ? float3{in.pos.x, ag + lift_, in.pos.z} : in.pos;
    for (int i = 0; i <= seg; ++i) {
        const float a = 2 * kPi * i / seg + rot;
        if (bill) tmpPts_.push_back(c + camRight_ * (std::cos(a) * radius) + camUp_ * (std::sin(a) * radius));
        else tmpPts_.push_back({c.x + std::cos(a) * radius, 0, c.z + std::sin(a) * radius});
    }
    if (fill && (fill >> 24)) {
        const int bands = bill || d.style.ground != OvGround::Follow ? 1 : std::clamp(int(std::ceil(radius / 1.5f)), 1, 8);
        auto P = [&](int i, float k) {
            const float3 p = tmpPts_[size_t(i)];
            if (bill) return c + (p - c) * k;
            float3 q{c.x + (p.x - c.x) * k, 0, c.z + (p.z - c.z) * k};
            q.y = groundY(d, q.x, q.z, ag);
            return q;
        };
        for (int b = 0; b < bands; ++b) {
            const float k0 = float(b) / float(bands), k1 = float(b + 1) / float(bands);
            for (int i = 0; i < seg; ++i) {
                if (b == 0) tri(d, P(i, 0), P(i, k1), P(i + 1, k1), fill);
                else { tri(d, P(i, k0), P(i, k1), P(i + 1, k1), fill); tri(d, P(i, k0), P(i + 1, k1), P(i + 1, k0), fill); }
            }
        }
    }
    if (col >> 24) polyline(d, tmpPts_.data(), tmpPts_.size(), false, col, ag, !bill);
}

void RtsOverlays::rectShape(const OverlayDef& d, const Inst& in, float x0, float z0, float x1, float z1, uint32_t col, uint32_t fill) {
    const float ag = view_ && grid_ ? view_->heightAt(*grid_, in.pos.x, in.pos.z) : 0.0f;
    if (d.style.space == OvSpace::Billboard) {
        const float3 c{in.pos.x, ag + lift_, in.pos.z};
        const float hw = (x1 - x0) * 0.5f, hh = (z1 - z0) * 0.5f;
        const float3 p[4] = {c - camRight_ * hw - camUp_ * hh, c + camRight_ * hw - camUp_ * hh, c + camRight_ * hw + camUp_ * hh, c - camRight_ * hw + camUp_ * hh};
        if (fill >> 24) { tri(d, p[0], p[1], p[2], fill); tri(d, p[0], p[2], p[3], fill); }
        if (col >> 24) polyline(d, p, 4, true, col, ag, false);
        return;
    }
    if (fill >> 24) {
        if (d.style.ground != OvGround::Follow) {   // one quad (the placement footprint has always been drawn this way)
            const float y = groundY(d, x0, z0, ag);
            tri(d, {x0, y, z0}, {x0, y, z1}, {x1, y, z1}, fill);
            tri(d, {x0, y, z0}, {x1, y, z1}, {x1, y, z0}, fill);
        } else {
            const int nx = std::clamp(int(std::ceil(x1 - x0)), 1, 64), nz = std::clamp(int(std::ceil(z1 - z0)), 1, 64);
            for (int j = 0; j < nz; ++j)
                for (int i = 0; i < nx; ++i) {
                    const float ax = x0 + (x1 - x0) * i / nx, bx = x0 + (x1 - x0) * (i + 1) / nx;
                    const float az = z0 + (z1 - z0) * j / nz, bz = z0 + (z1 - z0) * (j + 1) / nz;
                    const float3 A{ax, groundY(d, ax, az, ag), az}, B{ax, groundY(d, ax, bz, ag), bz}, C{bx, groundY(d, bx, bz, ag), bz}, D{bx, groundY(d, bx, az, ag), az};
                    tri(d, A, B, C, fill); tri(d, A, C, D, fill);
                }
        }
    }
    if (col >> 24) {
        const float3 p[4] = {{x0, 0, z0}, {x1, 0, z0}, {x1, 0, z1}, {x0, 0, z1}};
        polyline(d, p, 4, true, col, ag, true);
    }
}

void RtsOverlays::grid(const OverlayDef& d, const Inst& in) {
    if (!grid_) return;
    const MapGrid& g = *grid_;
    const float R = std::max(0.0f, radiusFor(d, in));
    int fw = 1, fh = 1;
    if (in.type < rules_->types.size() && rules_->types[in.type].structure) {
        fw = std::max(1, rules_->types[in.type].footprintW); fh = std::max(1, rules_->types[in.type].footprintH);
    }
    const int m = int(std::round(d.margin));
    fw += 2 * m; fh += 2 * m;
    const int fx0 = int(std::floor(in.pos.x - fw * 0.5f + 0.01f)), fz0 = int(std::floor(in.pos.z - fh * 0.5f + 0.01f));
    const int ri = int(std::ceil(R));
    const float ag = view_ ? view_->heightAt(g, in.pos.x, in.pos.z) : 0.0f;
    auto distTo = [&](int x, int y) {
        const float cx = x + 0.5f, cz = y + 0.5f;
        const float dx = std::max(0.0f, std::max(fx0 - cx, cx - float(fx0 + fw))), dz = std::max(0.0f, std::max(fz0 - cz, cz - float(fz0 + fh)));
        return d.square ? std::max(dx, dz) : std::sqrt(dx * dx + dz * dz);
    };
    auto included = [&](int x, int y) {
        if (x < fx0 - ri || y < fz0 - ri || x >= fx0 + fw + ri || y >= fz0 + fh + ri || !g.inside(x, y)) return false;
        return distTo(x, y) <= R + (d.square ? 0.0f : 0.5f) - 1e-4f || (x >= fx0 && x < fx0 + fw && y >= fz0 && y < fz0 + fh);
    };
    const OvStyle& s = d.style;
    const float baseA = animAlpha(d, in);
    const uint32_t lineCol = s.hasLine ? rgbaf({s.lineColor.x, s.lineColor.y, s.lineColor.z, s.lineColor.w * baseA}) : 0;
    const size_t cap = cellCap(d);
    size_t count = 0;
    const float inset = 0.04f;
    for (int y = fz0 - ri; y < fz0 + fh + ri; ++y)
        for (int x = fx0 - ri; x < fx0 + fw + ri; ++x) {
            if (!included(x, y)) continue;
            if (++count > cap) return;
            const bool inFoot = x >= fx0 && x < fx0 + fw && y >= fz0 && y < fz0 + fh;
            float k = 1;
            if (!(inFoot && d.highlightFootprint && in.type != kNoType)) {
                k = in.type != kNoType ? s.outerAlpha : 1.0f;
                if (s.falloff && R > 0 && !d.square) { const float t = distTo(x, y) / (R + 1); k *= std::max(0.0f, 1 - t * t); }
            }
            const int p = d.pred == OvPred::None ? -1 : d.pred == OvPred::Ok ? (in.ok ? 1 : 0) : (cellTest(d.pred, x, y) ? 1 : 0);
            if (s.fill) {
                float4 c = colorFor(d, in, p);
                const bool has = p < 0 || (s.hasColor & (1u << (p ? kColTrue : kColFalse))) || (p == 1 && !(s.hasColor & 3));
                if (has && c.w > 0) {
                    c.w *= k;
                    const uint32_t col = rgbaf(c);
                    const float ax = x + inset, bx = x + 1 - inset, az = y + inset, bz = y + 1 - inset;
                    const float3 A{ax, groundY(d, ax, az, ag), az}, B{ax, groundY(d, ax, bz, ag), bz}, C{bx, groundY(d, bx, bz, ag), bz}, D{bx, groundY(d, bx, az, ag), az};
                    tri(d, A, B, C, col); tri(d, A, C, D, col);
                }
            }
            if (lineCol >> 24) {
                const uint32_t lc = scaleAlpha(lineCol, k);
                const float3 e[4] = {{float(x), 0, float(y)}, {x + 1.0f, 0, float(y)}, {x + 1.0f, 0, y + 1.0f}, {float(x), 0, y + 1.0f}};
                polyline(d, e, 2, false, lc, ag, true);                                    // top
                const float3 l[2] = {e[3], e[0]};
                polyline(d, l, 2, false, lc, ag, true);                                    // left
                if (!included(x + 1, y)) polyline(d, e + 1, 2, false, lc, ag, true);       // right
                if (!included(x, y + 1)) polyline(d, e + 2, 2, false, lc, ag, true);       // bottom
            }
        }
}

void RtsOverlays::cells(const OverlayDef& d, const Inst& in) {
    if (!grid_) return;
    const MapGrid& g = *grid_;
    float R = radiusFor(d, in);
    if (R <= 1) R = 24;
    const int cx = int(std::floor(in.pos.x)), cz = int(std::floor(in.pos.z)), ri = int(std::ceil(R));
    const int x0 = std::max(0, cx - ri), x1 = std::min(g.w - 1, cx + ri), y0 = std::max(0, cz - ri), y1 = std::min(g.h - 1, cz + ri);
    const OvStyle& s = d.style;
    const float ag = view_ ? view_->heightAt(g, in.pos.x, in.pos.z) : 0.0f;
    const size_t cap = cellCap(d);
    size_t count = 0;
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            const float dx = x + 0.5f - in.pos.x, dz = y + 0.5f - in.pos.z, dd = std::sqrt(dx * dx + dz * dz);
            if (dd > R) continue;
            const float k = s.falloff ? std::max(0.0f, 1 - (dd / R) * (dd / R)) : 1.0f;
            const bool p = cellTest(d.pred, x, y);
            if (d.pred == OvPred::TerritoryEdge) {
                if (!p) continue;
                const uint8_t own = g.territory[size_t(g.index(x, y))];
                Inst ci = in;
                ci.owner = own;
                const uint32_t col = scaleAlpha(rgbaf(colorFor(d, ci, -1)), k);
                const int nb[4][2] = {{0, -1}, {1, 0}, {0, 1}, {-1, 0}};
                const float3 corner[4] = {{float(x), 0, float(y)}, {x + 1.0f, 0, float(y)}, {x + 1.0f, 0, y + 1.0f}, {float(x), 0, y + 1.0f}};
                for (int e = 0; e < 4; ++e) {
                    const int nx = x + nb[e][0], ny = y + nb[e][1];
                    if (g.inside(nx, ny) && g.territory[size_t(g.index(nx, ny))] == own) continue;
                    if (++count > cap) return;
                    // inset the edge into the owner's cell so two owners' borders sit side by side
                    float3 a = corner[e], b = corner[(e + 1) % 4];
                    const float in2 = std::max(0.02f, s.thickness * 0.5f);
                    a.x -= nb[e][0] * in2; b.x -= nb[e][0] * in2; a.z -= nb[e][1] * in2; b.z -= nb[e][1] * in2;
                    const float3 pts[2] = {a, b};
                    polyline(d, pts, 2, false, col, ag, true);
                }
                continue;
            }
            const int pr = p ? 1 : 0;
            if (!p && !(s.hasColor & (1u << kColFalse))) continue;
            if (++count > cap) return;
            Inst ci = in;
            if (s.playerColor && g.territory.size() > size_t(g.index(x, y))) ci.owner = g.territory[size_t(g.index(x, y))];
            const uint32_t col = scaleAlpha(rgbaf(colorFor(d, ci, pr)), k);
            const float3 A{float(x), groundY(d, float(x), float(y), ag), float(y)}, B{float(x), groundY(d, float(x), y + 1.0f, ag), y + 1.0f};
            const float3 C{x + 1.0f, groundY(d, x + 1.0f, y + 1.0f, ag), y + 1.0f}, D{x + 1.0f, groundY(d, x + 1.0f, float(y), ag), float(y)};
            if (s.fill) { tri(d, A, B, C, col); tri(d, A, C, D, col); }
            if (s.outline) { const float3 q[4] = {A, D, C, B}; polyline(d, q, 4, true, col, ag, true); }
        }
}

void RtsOverlays::icon(const OverlayDef& d, size_t di, const Inst& in, float size, uint32_t col) {
    if (di >= images_.size() || !images_[di].valid()) return;
    const TextureRef& t = images_[di];
    const float ag = view_ && grid_ ? view_->heightAt(*grid_, in.pos.x, in.pos.z) : 0.0f;
    const float h = size * 0.5f, rot = float(time_) * d.style.rotate;
    float3 ax, ay, c;
    if (d.style.space == OvSpace::Billboard) { c = {in.pos.x, ag + lift_, in.pos.z}; ax = camRight_ * h; ay = camUp_ * h; }
    else {
        c = in.pos;
        ax = {std::cos(rot) * h, 0, std::sin(rot) * h};
        ay = {-std::sin(rot) * h, 0, std::cos(rot) * h};
    }
    const int n = d.style.space == OvSpace::Billboard || d.style.ground != OvGround::Follow ? 1 : std::clamp(int(std::ceil(size / 1.5f)), 1, 4);
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            auto P = [&](int a, int b) {
                const float u = float(a) / n * 2 - 1, v = float(b) / n * 2 - 1;
                float3 p = c + ax * u + ay * v;
                if (d.style.space != OvSpace::Billboard) p.y = groundY(d, p.x, p.z, ag);
                return p;
            };
            // image top row at -v (billboards: screen up)
            auto U = [&](int a) { return t.u(float(a) / n); };
            auto V = [&](int b) { return d.style.space == OvSpace::Billboard ? t.v(1 - float(b) / n) : t.v(float(b) / n); };
            const float uv0[6] = {U(i), V(j), U(i + 1), V(j), U(i + 1), V(j + 1)};
            const float uv1[6] = {U(i), V(j), U(i + 1), V(j + 1), U(i), V(j + 1)};
            tri(d, P(i, j), P(i + 1, j), P(i + 1, j + 1), col, t.tex, uv0);
            tri(d, P(i, j), P(i + 1, j + 1), P(i, j + 1), col, t.tex, uv1);
        }
}

void RtsOverlays::arrowHead(const OverlayDef& d, float3 a, float3 b, uint32_t col, float ag) {
    float dx = b.x - a.x, dz = b.z - a.z, l = std::sqrt(dx * dx + dz * dz);
    if (l < 1e-4f) return;
    dx /= l; dz /= l;
    const float hl = std::min(d.head, l), hw = hl * 0.55f;
    const float3 tip{b.x, 0, b.z}, base{b.x - dx * hl, 0, b.z - dz * hl};
    float3 L{base.x - dz * hw, 0, base.z + dx * hw}, R{base.x + dz * hw, 0, base.z - dx * hw};
    auto at = [&](float3 p) { p.y = groundY(d, p.x, p.z, ag); return p; };
    if (d.style.thickness > 0) tri(d, at(tip), at(L), at(R), col);
    else { seg(d, at(tip), at(L), col); seg(d, at(tip), at(R), col); }
}

bool RtsOverlays::resolveTo(const OverlayDef& d, const Inst& in, float3& out) const {
    const SceneObject* o = in.obj >= 0 && scene_ ? &scene_->objects[size_t(in.obj)] : nullptr;
    World* w = ctx_.sim ? &ctx_.sim->world() : nullptr;
    switch (d.to) {
    case OvTo::None: return false;
    case OvTo::Target: if (o && o->hasTarget) { out = o->target; return true; } return false;
    case OvTo::Cursor: if (ctx_.hasCursor) { out = ctx_.cursor; return true; } return false;
    case OvTo::Point: out = {d.toPoint[0], 0, d.toPoint[1]}; return true;
    case OvTo::Rally:
        if (w && o && o->entity.valid())
            if (auto* pr = w->tryGet<Production>(o->entity); pr && pr->hasRally) { out = toF(pr->rally); return true; }
        return false;
    case OvTo::Order:
        if (w && o && o->entity.valid())
            if (auto* od = w->tryGet<Orders>(o->entity); od && !od->queue.empty()) {
                const Order& ord = od->queue.front();
                if (ord.target.valid()) if (auto* tt = w->tryGet<Transform>(ord.target)) { out = toF(tt->pos); return true; }
                out = toF(ord.point);
                return ord.point.x.raw != 0 || ord.point.z.raw != 0;
            }
        return false;
    }
    return false;
}

void RtsOverlays::emit(const OverlayDef& d, size_t di, const Inst& in) {
    const OvStyle& s = d.style;
    lift_ = s.lift + (in.popup >= 0 ? d.event.rise * in.age : 0.0f);
    const int pred = d.pred == OvPred::None || d.shape == OvShape::Grid || d.shape == OvShape::Cells ? -1
                   : d.pred == OvPred::Ok ? (in.ok ? 1 : 0)
                   : (cellTest(d.pred, int(std::floor(in.pos.x)), int(std::floor(in.pos.z))) ? 1 : 0);
    const float4 c = colorFor(d, in, pred);
    const uint32_t col = rgbaf(c);
    const float aa = animAlpha(d, in);
    const uint32_t lineCol = s.hasLine ? rgbaf({s.lineColor.x, s.lineColor.y, s.lineColor.z, s.lineColor.w * aa}) : col;
    const uint32_t fillCol = s.hasFill ? rgbaf({s.fillColor.x, s.fillColor.y, s.fillColor.z, s.fillColor.w * aa})
                           : s.outline && d.shape != OvShape::Circle ? scaleAlpha(col, 0.3f) : col;
    if (s.space == OvSpace::Minimap) {
        float r = d.shape == OvShape::Rect ? std::max(d.size[0], d.size[1]) * 0.5f : radiusFor(d, in);
        marks_.push_back({in.pos.x, in.pos.z, r, col, d.shape == OvShape::Ring});
        return;
    }
    const float ag = view_ && grid_ ? view_->heightAt(*grid_, in.pos.x, in.pos.z) : 0.0f;
    switch (d.shape) {
    case OvShape::Ring: ring(d, in, radiusFor(d, in), col, s.fill ? fillCol : 0); break;
    case OvShape::Circle: ring(d, in, radiusFor(d, in), s.outline ? lineCol : 0, s.fill ? fillCol : 0); break;
    case OvShape::Rect: {
        float x0, z0, x1, z1;
        if (d.anchor == OvAnchor::Area) { x0 = in.rect[0]; z0 = in.rect[1]; x1 = x0 + in.rect[2]; z1 = z0 + in.rect[3]; }
        else {
            const float g = in.popup >= 0 ? d.event.grow * in.age : 0.0f;
            x0 = in.pos.x - d.size[0] * 0.5f - g; x1 = in.pos.x + d.size[0] * 0.5f + g;
            z0 = in.pos.z - d.size[1] * 0.5f - g; z1 = in.pos.z + d.size[1] * 0.5f + g;
        }
        rectShape(d, in, x0, z0, x1, z1, s.outline ? (s.fill ? lineCol : col) : 0, s.fill ? fillCol : 0);
        break;
    }
    case OvShape::Footprint: {
        if (in.type >= rules_->types.size()) break;
        const UnitType& t = rules_->types[in.type];
        const float fw = float(std::max(1, t.footprintW)) + 2 * d.margin, fh = float(std::max(1, t.footprintH)) + 2 * d.margin;
        const float x0 = in.pos.x - fw * 0.5f, z0 = in.pos.z - fh * 0.5f;
        rectShape(d, in, x0, z0, x0 + fw, z0 + fh, s.outline ? (s.fill ? lineCol : col) : 0, s.fill ? fillCol : 0);
        break;
    }
    case OvShape::Grid: grid(d, in); break;
    case OvShape::Cells: cells(d, in); break;
    case OvShape::Line: case OvShape::Arrow: case OvShape::Path: {
        if (in.pathCount >= 2) {
            const float3* p = pathPts_.data() + in.pathBegin;
            polyline(d, p, in.pathCount, false, col, ag, true);
            if (d.shape == OvShape::Arrow) arrowHead(d, p[in.pathCount - 2], p[in.pathCount - 1], col, ag);
            break;
        }
        float3 to;
        if (!resolveTo(d, in, to)) break;
        const float3 p[2] = {in.pos, to};
        polyline(d, p, 2, false, col, ag, true);
        if (d.shape == OvShape::Arrow) arrowHead(d, p[0], p[1], col, ag);
        break;
    }
    case OvShape::Icon: icon(d, di, in, d.size[0] + (in.popup >= 0 ? d.event.grow * in.age : 0.0f), col); break;
    case OvShape::Text: case OvShape::Bar: break;   // screen pass
    }
}

// ------------------------------------------------------------------ world pass

void RtsOverlays::drawWorld(Renderer3D& r, const RtsScene& s, double time, const std::vector<uint8_t>* vision, const MapGrid& grid, const RtsView& view) {
    if (!rules_) return;
    r_ = &r;
    view_ = &view;
    if (!qualityForced_)
        quality_ = r.materialQuality() == MaterialQuality::Low ? OverlayQuality::Low
                 : r.materialQuality() == MaterialQuality::Medium ? OverlayQuality::Medium : OverlayQuality::High;
    const RTSCamera& cam = r.camera();
    camTarget_ = cam.target;
    const float cp = std::cos(cam.pitch), sp = std::sin(cam.pitch);
    const float3 fwd = normalize(float3{-std::sin(cam.yaw) * cp, -sp, -std::cos(cam.yaw) * cp});
    camRight_ = normalize(float3{std::cos(cam.yaw), 0, -std::sin(cam.yaw)});
    camUp_ = normalize(cross(camRight_, fwd));
    prepare(s, time, vision, grid, cam.distance);
    marks_.clear();
    for (auto& b : buckets_) b.verts.clear();
    for (uint16_t di : worldDefs_) {
        const OverlayDef& d = set_.defs[di];
        if (!d.enabled || !gather(d, di, false)) continue;
        for (const Inst& in : insts_) emit(d, di, in);
    }
    flush(r);
    r_ = nullptr;
}

RtsOverlays::Bucket& RtsOverlays::bucket(gfx::TextureHandle tex, uint8_t pipe) {
    if (!tex.valid()) tex = white_;
    for (auto& b : buckets_) if (b.tex == tex && b.pipe == pipe) return b;
    buckets_.push_back({tex, pipe, {}});
    return buckets_.back();
}

void RtsOverlays::flush(Renderer3D& r) {
    if (!dev_ || !shader_.valid()) return;
    size_t total = 0;
    for (auto& b : buckets_) total += b.verts.size();
    if (!total) return;
    upload_.clear();
    for (auto& b : buckets_) upload_.insert(upload_.end(), b.verts.begin(), b.verts.end());
    const size_t bytes = upload_.size() * sizeof(BatchVertex);
    if (!vb_.valid() || bytes > vbBytes_) {
        if (vb_.valid()) dev_->destroyBuffer(vb_);
        vbBytes_ = std::max<size_t>(bytes * 2, 64 * 1024);
        gfx::BufferDesc bd; bd.type = gfx::BufferType::Vertex; bd.usage = gfx::Usage::Dynamic; bd.size = vbBytes_; bd.label = "overlays";
        vb_ = dev_->createBuffer(bd);
    }
    const mat4 vp = r.viewProj();
    r.drawCustom([this, vp, bytes](gfx::Device& dev) {
        dev.updateBuffer(vb_, upload_.data(), bytes);
        uint32_t first = 0;
        for (auto& b : buckets_) {
            const uint32_t n = uint32_t(b.verts.size());
            if (!n) continue;
            if (pipes_[b.pipe].valid()) {
                dev.setPipeline(pipes_[b.pipe]);
                dev.setUniforms(0, vp.m, sizeof vp.m);
                dev.setVertexBuffer(vb_);
                dev.setTexture(0, b.tex);
                dev.draw(n, first);
            }
            first += n;
        }
    });
}

// ------------------------------------------------------------------ screen pass

void RtsOverlays::drawScreen(const RTSCamera& cam, const RtsScene& s, float w, float h, float ox, float oy, void* drawList, const RtsView& view) {
    if (!rules_ || !grid_) return;
    view_ = &view;
    if (scene_ != &s) prepare(s, time_, vision_, *grid_, cam.distance);
    zoom_ = cam.distance;
    ImDrawList* dl = drawList ? static_cast<ImDrawList*>(drawList) : ImGui::GetBackgroundDrawList();
    // entity elements object by object (bars of one object stay together, like they always did)
    if (!screenEntityDefs_.empty())
        for (size_t i = 0; i < s.objects.size(); ++i) {
            const SceneObject& o = s.objects[i];
            if (!o.visible || o.isProjectile || o.type >= rules_->types.size()) continue;
            bool have = false;
            float2 sp;
            for (uint16_t di : screenEntityDefs_) {
                const OverlayDef& d = set_.defs[di];
                if (!d.enabled) continue;
                const bool pv = previewed(d);
                if (d.anchor == OvAnchor::Selected && !o.selected && !(pv && selected_.empty())) continue;
                if (d.anchor == OvAnchor::Hovered && int(i) != hovered_) continue;
                if (!matches(d.anchorFilter, o) || (!pv && !test(d.when, &o))) continue;
                if (!have) {
                    if (!view.overlayTop(cam, o, i, w, h, sp)) break;
                    have = true;
                }
                Inst in;
                in.pos = o.pos; in.obj = int(i); in.type = o.type; in.owner = o.owner;
                drawScreenInst(d, di, in, sp.x + ox, sp.y + oy, dl);
            }
        }
    for (uint16_t di : screenOtherDefs_) {
        const OverlayDef& d = set_.defs[di];
        if (!d.enabled || !gather(d, di, true)) continue;
        for (const Inst& in : insts_) {
            if (d.anchor == OvAnchor::Screen) {
                drawScreenInst(d, di, in, (in.sx < 0 ? w + in.sx : in.sx) + ox, (in.sy < 0 ? h + in.sy : in.sy) + oy, dl);
                continue;
            }
            const float rise = in.popup >= 0 ? d.event.rise * in.age : 0.0f;
            const float3 p{in.pos.x, view.heightAt(*grid_, in.pos.x, in.pos.z) + d.style.lift + rise, in.pos.z};
            float2 sp;
            if (!cam.worldToScreen(p, w, h, sp)) continue;
            drawScreenInst(d, di, in, sp.x + ox, sp.y + oy, dl);
        }
    }
}

void RtsOverlays::drawScreenInst(const OverlayDef& d, size_t di, const Inst& in, float x, float y, void* dlp) {
    ImDrawList* dl = static_cast<ImDrawList*>(dlp);
    const OvStyle& s = d.style;
    x += s.offsetX; y += s.offsetY;
    const float4 c = colorFor(d, in, d.pred == OvPred::Ok ? (in.ok ? 1 : 0) : -1);
    const ImU32 col = imCol(c);
    auto bgColor = [&]() {
        float4 b = s.backgroundPlayer ? overlayPlayerColor(in.owner) : s.background;
        if (s.backgroundPlayer) b.w = 1;
        return imCol({b.x * s.backgroundShade, b.y * s.backgroundShade, b.z * s.backgroundShade, b.w * s.backgroundAlpha * in.alpha});
    };
    switch (d.shape) {
    case OvShape::Text: {
        const std::string& txt = format(d, in);
        if (txt.empty()) break;
        ImFont* font = ImGui::GetFont();
        const float size = ImGui::GetFontSize() * d.size[0];
        const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0, txt.c_str());
        const ImVec2 at(x - ts.x * 0.5f, y - ts.y * 0.5f);
        if (s.hasBackground)
            dl->AddRectFilled(ImVec2(at.x - s.padX, at.y - s.padY), ImVec2(at.x + ts.x + s.padX, at.y + ts.y + s.padY), bgColor(), s.rounding);
        if (s.shadow) dl->AddText(font, size, ImVec2(at.x + 1, at.y + 1), imCol({0, 0, 0, c.w * 0.75f}), txt.c_str());
        dl->AddText(font, size, at, col, txt.c_str());
        break;
    }
    case OvShape::Bar: {
        if (in.obj < 0 || !scene_) break;
        const SceneObject& o = scene_->objects[size_t(in.obj)];
        const float bw = rules_->types[o.type].structure ? d.barWStructure : d.barW, bh = d.barH;
        const ImVec2 a(x - bw * 0.5f, y), b(x + bw * 0.5f, y + bh);
        if (s.hasBackground) dl->AddRectFilled(ImVec2(a.x - s.border, a.y - s.border), ImVec2(b.x + s.border, b.y + s.border), bgColor());
        const float f = std::clamp(valueOf(d, o), 0.0f, 1.0f);
        float4 fc = c;
        if (!s.stops.empty()) {
            fc = s.stops.back().second;
            for (auto& [v, sc] : s.stops) if (f <= v) { fc = sc; break; }
            fc.w *= animAlpha(d, in);
        }
        dl->AddRectFilled(a, ImVec2(a.x + bw * f, b.y), imCol(fc));
        break;
    }
    case OvShape::Icon: {
        if (di >= images_.size() || !images_[di].valid()) break;
        const TextureRef& t = images_[di];
        const float hs = (d.size[0] + (in.popup >= 0 ? d.event.grow * in.age : 0.0f)) * 0.5f;
        dl->AddImage(ImTextureRef(ImTextureID(t.tex.id)), ImVec2(x - hs, y - hs), ImVec2(x + hs, y + hs), ImVec2(t.u0, t.v0), ImVec2(t.u1, t.v1), col);
        break;
    }
    case OvShape::Circle: case OvShape::Ring: {
        const float r = radiusFor(d, in);
        if (r <= 0) break;
        if (d.shape == OvShape::Circle || s.fill) dl->AddCircleFilled(ImVec2(x, y), r, s.hasFill ? imCol(s.fillColor) : col, s.segments);
        if (d.shape == OvShape::Ring || s.outline) dl->AddCircle(ImVec2(x, y), r, s.hasLine ? imCol(s.lineColor) : col, s.segments, s.thickness > 0 ? s.thickness : 1.5f);
        break;
    }
    case OvShape::Rect: {
        const float g = in.popup >= 0 ? d.event.grow * in.age : 0.0f;
        const ImVec2 a(x - d.size[0] * 0.5f - g, y - d.size[1] * 0.5f - g), b(x + d.size[0] * 0.5f + g, y + d.size[1] * 0.5f + g);
        if (s.fill) dl->AddRectFilled(a, b, s.hasFill ? imCol(s.fillColor) : col, s.rounding);
        if (s.outline) dl->AddRect(a, b, s.hasLine ? imCol(s.lineColor) : col, s.rounding, 0, s.thickness > 0 ? s.thickness : 1.5f);
        break;
    }
    default: break;
    }
}

} // namespace rts::game
