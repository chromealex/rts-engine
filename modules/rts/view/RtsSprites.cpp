// Sprite-sheet units and buildings (Rts2DView, sprite part): looks from the "sprite" blocks of
// rules.json, facings, per-action animation, team colour, shadow blobs, death animations.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include "Rts2DView.h"
#include "RtsView.h"

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

bool Rts2DView::init(gfx::Device& dev, const Rules& rules, const std::string& base) {
    rules_ = &rules;
    base_ = base;
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
    const Json& sh = s["shadow"];
    bool shadow = sh.isObject() || sh.boolean(!t.structure && !flat);
    if (shadow) {
        L.shadowW = f(sh["size"][0], L.width * 0.8f);
        L.shadowH = f(sh["size"][1], L.shadowW * 0.55f);
        L.shadowAlpha = f(sh["alpha"], 0.35f);
    }
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

void Rts2DView::beginFrame(const RTSCamera& cam, double time) {
    cam_ = cam;
    Basis b = basisOf(cam);
    camRight_ = b.right; camUp_ = b.up; camFwd_ = b.fwd; groundRight_ = b.gRight; groundFront_ = b.gFront;
    time_ = time;
    ++frame_;
    batch_.begin();
}

void Rts2DView::corners(const SpriteLook& L, float3 ground, float3 (&c)[4], float& key) const {
    Basis b{camRight_, camUp_, camFwd_, groundRight_, groundFront_};
    spriteCorners(L, b, ground, c, key);
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
    batch_.quad(L.tex, c, uv, tint, team, layer, key);
    if (L.shadowW > 0 && !corpse) {
        SpriteLook S;
        S.mode = SpriteLook::Ground; S.width = L.shadowW; S.height = L.shadowH; S.anchorX = S.anchorY = 0.5f;
        corners(S, ground, c, key);
        batch_.quad(shadowTex_, c, {0, 0, 1, 1}, rgba(0, 0, 0, uint8_t(std::clamp(L.shadowAlpha, 0.0f, 1.0f) * 255)), 0, 0, key);
    }
}

void Rts2DView::addObject(const SceneObject& o, float groundY) {
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

void Rts2DView::submit(Renderer3D& r, const MapGrid& grid, float lh) {
    if (!ready_) return;
    r.drawCustom([this, &r, &grid, lh](gfx::Device& dev) {
        refreshTiles(dev, grid, lh);
        if (tileMesh_.vb.valid() && atlas_.valid()) batch_.drawMesh(dev, tileMesh_, atlas_.tex, r.viewProj());
        batch_.flush(dev, r.viewProj());
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
