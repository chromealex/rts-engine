// Sprite-sheet units and buildings (Rts2DView, sprite part): looks from the "sprite" blocks of
// rules.json, facings, per-action animation, team colour, shadows (projected blobs + sun-shadow
// casters, RtsSpriteShadow.h), lighting, death animations.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include "Rts2DView.h"
#include "RtsView.h"
#include "RtsTerrainSurface.h"

namespace rts::game {

using namespace render;

namespace {
constexpr float kPi = 3.14159265358979f;
const char* kActionNames[kActCount] = {"idle", "move", "attack", "work", "build", "die"};
float f(const Json& j, float def) { return j.isNumber() ? float(j.fx().toDouble()) : def; }

struct Basis { float3 right, up, fwd, gRight, gFront; };
Basis basisOf(const RTSCamera& cam) {
    Basis b;
    b.fwd = normalize(cam.target - cam.eye());
    b.right = {std::cos(cam.yaw), 0, -std::sin(cam.yaw)};
    b.up = cross(b.right, b.fwd);
    b.gRight = b.right;
    b.gFront = {std::sin(cam.yaw), 0, std::cos(cam.yaw)};   // on the ground, towards the camera
    return b;
}

void spriteCorners(const SpriteLook& L, const Basis& B, float3 ground, float3 (&c)[4], float& key) {
    float3 R = B.right, U = B.up;
    if (L.mode == SpriteLook::Upright) U = {0, 1, 0};
    if (L.mode == SpriteLook::Ground) { R = B.gRight; U = -B.gFront; ground.y += 0.02f; }
    key = -dot(ground, B.up);   // painter order: higher on screen first
    float3 p = ground + B.gRight * L.offsetRight + B.gFront * L.offsetFront + B.up * L.lift;
    float3 tl = p - R * (L.anchorX * L.width) + U * (L.anchorY * L.height) - B.fwd * L.depthBias;
    c[0] = tl;
    c[1] = tl + R * L.width;
    c[2] = c[1] - U * L.height;
    c[3] = tl - U * L.height;
}
} // namespace

bool Rts2DView::init(gfx::Device& dev, const Rules& rules, const std::string& base, const SpriteDefaults& defaults) {
    rules_ = &rules;
    base_ = base;
    defaults_ = defaults;
    textures_.init(dev);
    if (!batch_.init(dev)) { std::fprintf(stderr, "[sprites] pipeline: %s\n", dev.lastError().c_str()); return false; }
    // soft round shadow blob, generated: no file needed
    const int S = 32;
    std::vector<uint32_t> px(size_t(S * S));
    for (int y = 0; y < S; ++y)
        for (int x = 0; x < S; ++x) {
            float dx = (x + 0.5f) / S * 2 - 1, dy = (y + 0.5f) / S * 2 - 1;
            float a = std::clamp(1.0f - std::sqrt(dx * dx + dy * dy), 0.0f, 1.0f);
            px[size_t(y * S + x)] = rgba(255, 255, 255, uint8_t(std::min(1.0f, a * 1.6f) * 255));
        }
    shadowTex_ = textures_.addGenerated("builtin:shadow", S, S, px, false);
    looks_.assign(rules.types.size(), {});
    for (size_t i = 0; i < rules.types.size(); ++i) {
        const Json& s = rules.types[i].view["sprite"];
        if (s.isObject()) loadLook(looks_[i], rules.types[i], s);
    }
    ready_ = true;
    return true;
}

void Rts2DView::shutdown(gfx::Device& dev) {
    if (!ready_) return;
    batch_.destroyMesh(dev, tileMesh_);
    batch_.shutdown();
    textures_.shutdown();
    looks_.clear(); seen_.clear(); corpses_.clear();
    ready_ = false;
}

void Rts2DView::loadLook(SpriteLook& L, const UnitType& t, const Json& s) {
    L.sheet = s.getStr("sheet");
    if (L.sheet.empty()) return;
    L.nearest = s.getStr("filter", "nearest") != "linear";
    L.tex = textures_.resolve(L.sheet, L.nearest, base_);
    if (!L.tex.valid()) return;
    L.frameW = int(s["frame"][0].integer(L.tex.width));
    L.frameH = int(s["frame"][1].integer(L.frameW));
    L.facings = std::max<int>(1, int(s.getInt("facings", 1)));
    L.mirror = s.getBool("mirror", false) && L.facings % 4 == 0;
    L.team = s.getStr("team", "magenta") != "none";
    std::string mode = s.getStr("mode", "billboard");
    L.mode = mode == "ground" ? SpriteLook::Ground : mode == "upright" ? SpriteLook::Upright : SpriteLook::Billboard;
    const bool flat = L.mode == SpriteLook::Ground;
    L.anchorX = f(s["anchor"][0], 0.5f);
    L.anchorY = f(s["anchor"][1], flat ? 0.5f : 1.0f);
    float aspect = float(L.frameH) / float(std::max(1, L.frameW));
    if (s.has("pixelsPerCell")) {
        float ppc = std::max(1.0f, f(s["pixelsPerCell"], 32));
        L.width = L.frameW / ppc; L.height = L.frameH / ppc;
    } else if (s["size"].isArray()) {
        L.width = f(s["size"][0], 1); L.height = f(s["size"][1], L.width * aspect);
    } else {
        L.width = f(s["size"], t.structure ? float(t.footprintW) : 1.0f);
        L.height = L.width * aspect;
    }
    float scale = f(s["scale"], 1.0f);
    L.width *= scale; L.height *= scale;
    // upright drawings of buildings stand on the front edge of their footprint
    L.offsetRight = f(s["offset"][0], 0);
    L.offsetFront = f(s["offset"][1], t.structure && !flat ? t.footprintH * 0.5f : 0.0f);
    L.depthBias = f(s["depthBias"], flat ? 0.0f : L.height * 0.5f + 0.3f);
    L.lift = f(s["lift"], (t.layer & kLayerAir) ? 1.2f : 0.0f);
    L.corpseTime = f(s["corpseTime"], 4);
    L.structure = t.structure;
    L.shadow = parseSpriteShadow(s, defaults_, t.structure, flat, L.width, L.height, t.footprintW, t.footprintH);
    const Json& acts = s["actions"];
    for (int a = 0; a < kActCount; ++a) {
        const Json& j = acts[kActionNames[a]];
        SpriteLook::Anim& an = L.anims[a];
        if (j.isArray()) { an.start = int(j[0].integer()); an.count = int(j[1].integer(1)); an.fps = f(j[2], 8); an.defined = true; }
        else if (j.isObject()) { an.start = int(j.getInt("start")); an.count = int(j.getInt("count", 1)); an.fps = f(j["fps"], 8); an.loop = j.getBool("loop", a != kActDie); an.defined = true; }
        if (a == kActDie) an.loop = false;
        an.count = std::max(1, an.count);
    }
    if (!L.anims[kActIdle].defined) L.anims[kActIdle] = {0, 1, 0, true, true};
    L.valid = true;
}

int Rts2DView::facingIndex(const SpriteLook& L, float3 facing, bool& flip) const {
    flip = false;
    const int N = L.facings;
    if (N <= 1) return 0;
    float sx = dot(facing, groundRight_), sy = dot(facing, groundFront_);   // screen right / down
    float a = std::atan2(sy, sx);
    int fi = int(std::lround(a / (2 * kPi / N)));
    fi = ((fi % N) + N) % N;
    if (!L.mirror) return fi;
    const int north = 3 * N / 4;
    int k = ((fi - north) % N + N) % N;
    if (k <= N / 2) return k;
    flip = true;
    int m = ((N / 2 - fi) % N + N) % N;
    return ((m - north) % N + N) % N;
}

void Rts2DView::beginFrame(const RTSCamera& cam, double time, const Renderer3D& r, const MapGrid* grid, float levelHeight) {
    cam_ = cam;
    Basis b = basisOf(cam);
    camRight_ = b.right; camUp_ = b.up; camFwd_ = b.fwd; groundRight_ = b.gRight; groundFront_ = b.gFront;
    quality_ = r.shadowQuality();
    casterRight_ = casterRight(b.gRight, b.gFront, r.lighting.sunDir);
    surface_ = {grid, levelHeight, hasTiles()};
    time_ = time;
    ++frame_;
    batch_.begin();
}

void Rts2DView::corners(const SpriteLook& L, float3 ground, float3 (&c)[4], float& key) const {
    Basis b{camRight_, camUp_, camFwd_, groundRight_, groundFront_};
    spriteCorners(L, b, ground, c, key);
}

float Rts2DView::groundAt(float x, float z) const {
    if (!surface_.grid || surface_.grid->w == 0) return 0;
    const MapGrid& g = *surface_.grid;
    if (!surface_.tiles) return terrain::heightAt(g, surface_.levelHeight, x, z);
    const size_t i = size_t(g.index(std::clamp(int(std::floor(x)), 0, g.w - 1), std::clamp(int(std::floor(z)), 0, g.h - 1)));
    return float(g.level[i]) * surface_.levelHeight + float(g.height[i]) / 16.0f;
}

void Rts2DView::decal(const TextureRef& tex, float4 uv, float3 o, float3 aU, float lenU, float3 aV, float lenV,
                      uint32_t color, uint32_t team, int layer, float key, uint32_t flags) {
    if (!tex.valid() || lenU <= 0 || lenV <= 0) return;
    float3 q[4] = {o, o + aU * lenU, o + aU * lenU + aV * lenV, o + aV * lenV};
    if (!surface_.grid) {   // no ground known: flat quad (as before)
        for (auto& p : q) p.y += 0.02f;
        batch_.quad(tex, q, uv, color, team, layer, key);
        return;
    }
    float minX = q[0].x, maxX = q[0].x, minZ = q[0].z, maxZ = q[0].z;
    for (auto& p : q) { minX = std::min(minX, p.x); maxX = std::max(maxX, p.x); minZ = std::min(minZ, p.z); maxZ = std::max(maxZ, p.z); }
    surfTris_.clear();
    surfaceTriangles(surface_, minX, minZ, maxX, maxZ, surfTris_);
    auto vert = [&](float3 p) {
        const float3 d{p.x - o.x, 0, p.z - o.z};
        const float lu = dot(d, aU) / lenU, lv = dot(d, aV) / lenV;
        SpriteVertex v;
        v.pos = p;
        v.uv = {tex.u(uv.x + (uv.z - uv.x) * lu), tex.v(uv.y + (uv.w - uv.y) * lv)};
        v.color = color; v.team = team;
        v.rpos = {p.x, p.y, p.z, 0};
        return v;
    };
    if (surfTris_.size() >= 3) {   // one plane under the whole rect (flat ground, tiles): a single quad
        const float3 p0 = surfTris_[0], n = cross(surfTris_[1] - p0, surfTris_[2] - p0);
        bool planar = std::fabs(n.y) > 1e-6f;
        for (size_t i = 3; planar && i < surfTris_.size(); ++i) planar = std::fabs(dot(surfTris_[i] - p0, n)) <= 1e-5f * std::fabs(n.y);
        if (planar) {
            SpriteVertex v[4];
            for (int k = 0; k < 4; ++k) {
                float3 p = q[k];
                p.y = p0.y - (n.x * (p.x - p0.x) + n.z * (p.z - p0.z)) / n.y;
                v[k] = vert(p);
            }
            batch_.quadEx(tex.tex, v, layer, key, flags | kSpriteDecal);
            return;
        }
    }
    for (size_t i = 0; i + 2 < surfTris_.size(); i += 3) {
        const float3 tri[3] = {surfTris_[i], surfTris_[i + 1], surfTris_[i + 2]};
        float3 poly[8];
        const int n = clipToRect(tri, o, aU, lenU, aV, lenV, poly);
        for (int k = 1; k + 1 < n; ++k) {
            const SpriteVertex v0 = vert(poly[0]), v1 = vert(poly[k]), v2 = vert(poly[k + 1]);
            const SpriteVertex v[4] = {v0, v1, v2, v2};   // triangle = quad with a repeated corner
            batch_.quadEx(tex.tex, v, layer, key, flags | kSpriteDecal);
        }
    }
}

void Rts2DView::emit(const SpriteLook& L, float3 ground, float3 facing, int act, double t0, uint32_t team, uint32_t tint, bool corpse, float fade) {
    const SpriteLook::Anim& an = L.anims[act];
    int k = an.fps > 0 ? int(std::floor((time_ - t0) * an.fps)) : 0;
    k = an.loop ? ((k % an.count) + an.count) % an.count : std::clamp(k, 0, an.count - 1);
    bool flip;
    int row = facingIndex(L, facing, flip);
    const float fw = float(L.frameW) / float(L.tex.width), fh = float(L.frameH) / float(L.tex.height);
    float u0 = float(an.start + k) * fw, v0 = float(row) * fh;
    float4 uv{u0, v0, u0 + fw, v0 + fh};
    if (flip) std::swap(uv.x, uv.z);
    float3 c[4];
    float key;
    corners(L, ground, c, key);
    if (fade < 1) tint = (tint & 0x00ffffffu) | (uint32_t(float(tint >> 24) * std::max(0.0f, fade)) << 24);
    int layer = corpse ? 1 : L.mode == SpriteLook::Ground ? 1 : L.lift > 0 ? 3 : 2;
    const uint32_t litFlag = L.shadow.lit ? uint32_t(kSpriteLit) : 0u;
    const ShadowDraw sd = shadowDrawFor(L.shadow.mode, quality_, L.structure);
    if (L.mode == SpriteLook::Ground) {
        decal(L.tex, uv, c[0], groundRight_, L.width, groundFront_, L.height, tint, team, layer, key, litFlag);
    } else {
        // upright caster quad: foot on the ground, facing the camera yaw (turned to the sun if needed);
        // also the receive points of the drawn sprite (same uv per corner)
        const float3 up{0, 1, 0};
        const float offR = L.shadow.offsetRight + L.offsetRight;
        const float offF = L.shadow.offsetFront + L.offsetFront;
        const float hc = L.shadow.castHeight;
        // the caster stands on the lowest ground under its foot line: the sprite is placed at the
        // height of the object centre, and a foot hovering over lower ground would detach the shadow
        auto casterAt = [&](float front, float3 (&q)[4]) {
            float3 base = ground + groundRight_ * offR + groundFront_ * front;
            const float3 l = base - casterRight_ * (L.anchorX * L.width), r = l + casterRight_ * L.width;
            base.y = std::min({ground.y, groundAt(l.x, l.z), groundAt(base.x, base.z), groundAt(r.x, r.z)}) + L.lift;
            const float3 tl = base - casterRight_ * (L.anchorX * L.width) + up * (L.anchorY * hc);
            q[0] = tl; q[1] = tl + casterRight_ * L.width; q[2] = q[1] - up * hc; q[3] = tl - up * hc;
        };
        // buildings: the soft-shadow filter fades the shadow in over ~its radius from the foot line,
        // which a flat drawing (unlike a 3D box) doesn't hide - set the front caster back a little so
        // the fade lies under the drawing and the shadow meets its foot
        const float lead = L.shadow.castDepth > 0 ? -0.25f : 0.0f;
        float3 cc[4];
        casterAt(offF + lead, cc);
        const float2 st[4] = {{L.tex.u(uv.x), L.tex.v(uv.y)}, {L.tex.u(uv.z), L.tex.v(uv.y)}, {L.tex.u(uv.z), L.tex.v(uv.w)}, {L.tex.u(uv.x), L.tex.v(uv.w)}};
        SpriteVertex v[4];
        for (int i = 0; i < 4; ++i) v[i] = {c[i], st[i], tint, team, {cc[i].x, cc[i].y, cc[i].z, 0.15f}};
        batch_.quadEx(L.tex.tex, v, layer, key, litFlag);
        if (sd.cast && fade >= 1 && quality_ != ShadowQuality::Off) {
            batch_.caster(L.tex.tex, cc, st);
            if (L.shadow.castDepth > 0) {   // back plane: the footprint casts as a box
                float3 cb[4];
                casterAt(offF + lead - L.shadow.castDepth, cb);
                batch_.caster(L.tex.tex, cb, st);
            }
        }
    }
    if (L.shadow.mode != SpriteShadowMode::None && !corpse) {
        // blob + the contact darkening the lighting system gives 3D objects (RtsLights: its flat
        // decal is skipped for sprites), folded into one projected decal
        const float contact = L.structure ? (quality_ == ShadowQuality::Off ? 0.45f : 0.3f)
                                          : (quality_ == ShadowQuality::Off ? 0.5f : quality_ == ShadowQuality::Low ? 0.38f : 0.2f);
        const float w = L.shadow.blobW, h = L.shadow.blobH;
        const float3 o = ground - groundRight_ * (w * 0.5f) - groundFront_ * (h * 0.5f);
        const float a = std::clamp(1.0f - (1.0f - L.shadow.blobAlpha * sd.blobK) * (1.0f - contact), 0.0f, 1.0f);
        decal(shadowTex_, {0, 0, 1, 1}, o, groundRight_, w, groundFront_, h, rgba(0, 0, 0, uint8_t(a * 255)), 0, 0, key, 0);
    }
}

void Rts2DView::addObject(const SceneObject& o, float groundY) {
    struct Acc {   // RTS_SPRITE_STATS
        double& ms; std::chrono::steady_clock::time_point t;
        ~Acc() { ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count(); }
    } acc{cpuMs_, std::chrono::steady_clock::now()};
    if (!hasSprite(o.type)) return;
    const SpriteLook& L = looks_[o.type];
    const float3 ground{o.pos.x, groundY, o.pos.z};
    if (o.entity.valid()) seen_[o.entity.packed()] = {o.type, o.owner, ground, o.facing, o.visible, frame_};
    if (!o.visible) return;
    float4 tc = RtsView::playerColor(o.owner);
    uint32_t team = L.team ? rgbaf({tc.x, tc.y, tc.z, 1}) : 0;
    uint32_t tint = rgba(255, 255, 255);
    int act = o.action < kActCount ? int(o.action) : int(kActIdle);
    if (o.build < 1.0f) {
        act = kActBuild;
        if (!L.anims[kActBuild].defined) tint = rgba(200, 200, 200, uint8_t(120 + 135 * std::clamp(o.build, 0.0f, 1.0f)));
    }
    if (!L.anims[act].defined) act = act == kActWork && L.anims[kActAttack].defined ? kActAttack : kActIdle;
    double t0 = act == kActBuild && L.anims[kActBuild].defined && !L.anims[kActBuild].loop
                    ? time_ - double(std::clamp(o.build, 0.0f, 1.0f)) * L.anims[kActBuild].count / std::max(0.01f, L.anims[kActBuild].fps)
                    : -double(o.entity.index % 97) * 0.37;   // desynchronise identical units
    emit(L, ground, o.facing, act, t0, team, tint, false, 1);
}

void Rts2DView::endFrame(const MapGrid&) {
    for (auto it = seen_.begin(); it != seen_.end();) {
        if (it->second.frame == frame_) { ++it; continue; }
        const Seen& s = it->second;
        if (s.visible && hasSprite(s.type) && looks_[s.type].anims[kActDie].defined) corpses_.push_back({s.type, s.owner, s.pos, s.facing, time_});
        it = seen_.erase(it);
    }
    for (auto it = corpses_.begin(); it != corpses_.end();) {
        const SpriteLook& L = looks_[it->type];
        const auto& an = L.anims[kActDie];
        double len = an.count / std::max(0.01, double(an.fps)), age = time_ - it->start;
        if (age > len + L.corpseTime + 1.0) { it = corpses_.erase(it); continue; }
        float fade = age < len + L.corpseTime ? 1.0f : float(1.0 - (age - len - L.corpseTime));
        float4 tc = RtsView::playerColor(it->owner);
        emit(L, it->pos, it->facing, kActDie, it->start, L.team ? rgbaf({tc.x, tc.y, tc.z, 1}) : 0, rgba(255, 255, 255), true, fade);
        ++it;
    }
    if (corpses_.size() > 256) corpses_.erase(corpses_.begin(), corpses_.begin() + 64);
}

// Light at which a lit sprite shows its painted colours: flat ground under the default day sun
// (Renderer3D's Lighting defaults), so presets / night / dusk tint sprites like the terrain.
static float3 referenceLight() {
    const Lighting d;
    const float3 L = normalize(d.sunDir);
    const float3 sky{d.skyColor.x * d.skyColor.x, d.skyColor.y * d.skyColor.y, d.skyColor.z * d.skyColor.z};
    const float3 sun{d.sunColor.x * d.sunColor.x, d.sunColor.y * d.sunColor.y, d.sunColor.z * d.sunColor.z};
    return sky + sun * (d.sunIntensity * std::max(L.y, 0.0f));
}

void Rts2DView::submit(Renderer3D& r, const MapGrid& grid, float lh) {
    if (!ready_) return;
    // RTS_SPRITE_STATS=1: quads / shadow casters / CPU time of the sprite queueing, every 60 frames
    static const bool printStats = std::getenv("RTS_SPRITE_STATS") != nullptr;
    if (printStats && frame_ % 60 == 0) {
        std::fprintf(stderr, "[sprites] quads %zu casters %zu cpu %.2f ms/frame\n", batch_.size(), batch_.casterCount(), cpuMs_ / 60.0);
        cpuMs_ = 0;
    }
    if (batch_.casterCount() > 0 && r.shadowQuality() != ShadowQuality::Off)
        r.drawCustomShadow([this, &r](gfx::Device& dev) { batch_.flushCasters(dev, r.lightViewProj()); });
    r.drawCustom([this, &r, &grid, lh](gfx::Device& dev) {
        refreshTiles(dev, grid, lh);
        SpriteLighting lighting;
        lighting.bind = [&r] { r.bindLitFrame(); };
        lighting.ref = referenceLight();
        // receiving sun shadows costs a PCF lookup per sprite pixel: Medium and up
        lighting.receiveShadows = r.shadowQuality() >= ShadowQuality::Medium;
        if (tileMesh_.vb.valid() && atlas_.valid()) batch_.drawMesh(dev, tileMesh_, atlas_.tex, r.viewProj(), defaults_.lit ? &lighting : nullptr);
        batch_.flush(dev, r.viewProj(), &lighting);
    });
}

float3 Rts2DView::barPoint(const SceneObject& o, float groundY) const {
    const SpriteLook& L = looks_[o.type];
    float3 c[4];
    float key;
    corners(L, {o.pos.x, groundY, o.pos.z}, c, key);
    return (c[0] + c[1]) * 0.5f;
}

bool Rts2DView::hit(const RTSCamera& cam, const SceneObject& o, float groundY, float sx, float sy, float w, float h) const {
    if (!hasSprite(o.type)) return false;
    const SpriteLook& L = looks_[o.type];
    float3 c[4];
    float key;
    spriteCorners(L, basisOf(cam), {o.pos.x, groundY, o.pos.z}, c, key);
    float2 s[4];
    for (int i = 0; i < 4; ++i) if (!cam.worldToScreen(c[i], w, h, s[i])) return false;
    // the drawn frame has transparent margins: accept the inner 70%
    float2 m{(s[0].x + s[2].x) * 0.5f, (s[0].y + s[2].y) * 0.5f};
    for (auto& p : s) p = {m.x + (p.x - m.x) * 0.7f, m.y + (p.y - m.y) * 0.7f};
    bool pos = false, neg = false;
    for (int i = 0; i < 4; ++i) {
        float2 a = s[i], b = s[(i + 1) % 4];
        float cr = (b.x - a.x) * (sy - a.y) - (b.y - a.y) * (sx - a.x);
        pos |= cr > 0; neg |= cr < 0;
    }
    return !(pos && neg);
}

} // namespace rts::game
