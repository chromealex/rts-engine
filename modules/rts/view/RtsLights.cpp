#include "RtsLights.h"
#include "RtsView.h"
#include "../RtsModule.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace rts::game {

using namespace render;

namespace {
float3 toF(const Vec3& v) { return {float(v.x.toDouble()), float(v.y.toDouble()), float(v.z.toDouble())}; }
} // namespace

void RtsLights::consumeEvents(const RtsScene& s, double time, const std::vector<uint8_t>* vision, const MapGrid& grid, const RtsView& view) {
    if (s.tick == lastTick_) return; // the scene is rebuilt every frame, events change once per tick
    lastTick_ = s.tick;
    if (s.events.empty()) return;
    std::unordered_map<uint64_t, uint16_t> typeOf;
    typeOf.reserve(s.objects.size());
    for (const SceneObject& o : s.objects) if (!o.isProjectile) typeOf[o.entity.packed()] = o.type;
    auto type = [&](Entity e) -> int { auto it = typeOf.find(e.packed()); return it == typeOf.end() ? -1 : int(it->second); };
    auto seen = [&](float3 p) {
        if (!vision || vision->empty()) return true;
        int x = int(std::floor(p.x)), y = int(std::floor(p.z));
        return grid.inside(x, y) && (*vision)[size_t(grid.index(x, y))] == 2;
    };
    auto spawn = [&](const Flash& f, float3 p, float height) {
        if (!f.on || f.radius <= 0 || !seen(p)) return;
        p.y = view.heightAt(grid, p.x, p.z) + height;
        active_.push_back({p, f.color, f.radius, f.intensity, float(time), std::max(f.time, 0.03f), std::max(f.halo, 0.0f)});
    };
    for (const SimEvent& e : s.events) {
        const float3 p = toF(e.pos);
        switch (e.type) {
        case kEvAttack: if (int t = type(e.a); t >= 0) spawn(types_[size_t(t)].attack, p, 0.8f); break;
        case kEvHit:    if (int t = type(e.b); t >= 0) spawn(types_[size_t(t)].hit, p, 0.5f); break;
        case kEvDied:
            if (e.value >= 0 && size_t(e.value) < types_.size()) spawn(types_[size_t(e.value)].death, p, 0.9f);
            break;
        case kEvAbility: {
            const int t = type(e.a);
            const auto* list = t >= 0 ? &types_[size_t(t)].abilities : nullptr;
            if (list && e.value >= 0 && size_t(e.value) < list->size()) spawn((*list)[size_t(e.value)], p, 1.0f);
            else spawn({true, {0.55f, 0.75f, 1.0f}, 3.0f, 2.2f, 0.4f, 0.8f}, p, 1.0f);
            break;
        }
        default: break;
        }
    }
    if (active_.size() > 512) active_.erase(active_.begin(), active_.begin() + ptrdiff_t(active_.size() - 512));
}

void RtsLights::emitActive(LightSystem& ls, double time) {
    const float now = float(time);
    size_t w = 0;
    for (size_t i = 0; i < active_.size(); ++i) {
        const Active& a = active_[i];
        float t = (now - a.born) / a.life;
        if (t >= 1.0f || t < -0.5f) continue; // expired (or the clock was reset)
        float k = 1.0f - std::max(t, 0.0f);
        // flashes are short: bright and slightly bigger at the start
        PointLight l{a.pos, a.radius * (0.75f + 0.25f * k), a.color, a.intensity * k * k, 1.5f};
        l.halo = a.halo * (0.6f + 0.4f * k);
        l.haloAlpha = k;
        ls.add(l);
        active_[w++] = a;
    }
    active_.resize(w);
}

void RtsLights::addContactShadows(Renderer3D& r, const RtsScene& s, const MapGrid& grid, const RtsView& view) {
    LightSystem& ls = r.lights;
    if (!ls.contactShadows()) return;
    // Real sun shadows already ground the units on Medium/High; without them (Off/Low) a soft blob
    // does that job. Buildings always get an AO skirt: shadow maps don't give ambient occlusion.
    const ShadowQuality sq = r.shadowQuality();
    const float unitK = sq == ShadowQuality::Off ? 0.5f : sq == ShadowQuality::Low ? 0.38f : 0.2f;
    const float buildK = sq == ShadowQuality::Off ? 0.45f : 0.3f;
    for (const SceneObject& o : s.objects) {
        if (!o.visible || o.isProjectile || o.type >= types_.size()) continue;
        const UnitType& t = rules_->types[o.type];
        if (t.layer & kLayerAir) continue; // flyers have their own height blob
        const float c = types_[o.type].contact;
        if (c == 0) continue;
        const float scale = float(t.view.getFx("scale", Fixed::fromInt(1)).toDouble());
        const float3 g{o.pos.x, view.heightAt(grid, o.pos.x, o.pos.z), o.pos.z};
        if (t.structure) {
            const float k = (c < 0 ? buildK : c) * std::clamp(o.build, 0.3f, 1.0f);
            const bool box = types_[o.type].contactBox;
            const float pad = box ? 0.35f : 0.55f;   // an ellipse needs more room to cover the corners
            ls.addContactShadow(g, float(t.footprintW) * 0.5f * scale + pad, float(t.footprintH) * 0.5f * scale + pad, k, box, 0.45f);
        } else {
            const float rad = std::max(float(t.radius.toDouble()) * 1.3f, 0.22f) * scale;
            ls.addContactShadow(g, rad, rad, c < 0 ? unitK : c);
        }
    }
}

void RtsLights::draw(Renderer3D& r, const RtsScene& s, double time, const std::vector<uint8_t>* vision, const MapGrid& grid, const RtsView& view) {
    if (types_.empty()) return;
    updateSky(r.lighting, s.seconds);
    LightSystem& ls = r.lights;
    addContactShadows(r, s, grid, view);
    ls.setNightGlow(night_ * 1.5f + dusk_ * 0.4f);
    ls.setWaterReflections(waterRefl_);
    {   // clouds drift with the wind in game time; a storm covers the sky
        const float clouds = preview_ ? previewClouds_ : clouds_, storm = preview_ ? previewStorm_ : storm_;
        const float cover = (cycle_ || preview_) ? std::max(clouds, storm * 0.85f) : 0.0f;
        const float t = float(preview_ && s.seconds == 0 ? time : s.seconds);   // editor preview drifts in real time
        ls.setClouds(cover, std::min(cloudStrength_ + storm * 0.3f, 0.9f), cloudScale_, -windX_ * t, -windZ_ * t);
    }
    if (ls.quality() == LightQuality::Off) { active_.clear(); lastTick_ = s.tick; return; }
    consumeEvents(s, time, vision, grid, view);
    const float tt = float(time);
    // Constant lights matter at night; by day they only warm their surroundings a little.
    const bool cycle = cycle_ || preview_;
    const float glowK = cycle ? 0.55f + 0.45f * std::max(night_, dusk_) : 1.0f;
    const float haloK = cycle ? 0.15f + 0.85f * std::max(night_, dusk_ * 0.7f) : 0.35f;
    for (const SceneObject& o : s.objects) {
        if (!o.visible) continue;
        if (o.isProjectile) {
            if (o.shooterType >= types_.size()) continue;
            const Flash& f = types_[o.shooterType].projectile;
            if (!f.on) continue;
            PointLight l{o.pos, f.radius, f.color, f.intensity, 0.5f};
            l.halo = std::max(f.halo, 0.0f);
            l.haloAlpha = 0.8f;
            ls.add(l);
            continue;
        }
        if (o.type >= types_.size()) continue;
        const TypeLights& L = types_[o.type];
        if (L.glows.empty()) continue;
        const UnitType& t = rules_->types[o.type];
        const float scale = float(t.view.getFx("scale", Fixed::fromInt(1)).toDouble());
        const float3 ground{o.pos.x, view.heightAt(grid, o.pos.x, o.pos.z) + ((t.layer & kLayerAir) ? 2.4f : 0.0f), o.pos.z};
        const float yaw = std::atan2(o.facing.z, o.facing.x), ph = o.pos.x * 1.7f + o.pos.z * 2.3f;
        for (const Glow& g : L.glows) {
            if (o.build < 1.0f && !g.whileBuilding) continue;
            PointLight l = glowLight(g, ground, yaw, scale, tt, ph);
            l.haloAlpha = haloK * std::min(l.intensity / std::max(g.intensity, 0.01f), 1.3f);
            l.beam = g.spot ? g.beam * haloK : 0.0f;   // shafts show at dusk and night
            l.intensity *= glowK;
            ls.add(l);
        }
    }
    for (const MapLight& m : mapLights_) { // decorative lights: once the cell has been explored
        if (vision && !vision->empty()) {
            int x = int(std::floor(m.x)), y = int(std::floor(m.z));
            if (!grid.inside(x, y) || (*vision)[size_t(grid.index(x, y))] == 0) continue;
        }
        PointLight l = glowLight(m, {m.x, view.heightAt(grid, m.x, m.z), m.z}, 0.0f, 1.0f, tt, m.x * 1.7f + m.z * 2.3f);
        l.intensity *= glowK;
        l.priority = 0.3f;
        l.haloAlpha = haloK;
        l.beam = m.spot ? m.beam * haloK : 0.0f;
        ls.add(l);
    }
    emitActive(ls, time);
}

} // namespace rts::game
