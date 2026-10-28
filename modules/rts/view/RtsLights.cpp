#include "RtsLights.h"
#include "RtsView.h"
#include "../RtsModule.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <unordered_map>

namespace rts::game {

using namespace render;

namespace {
float num(const Json& j, const char* key, float def) { const Json& v = j[key]; return v.isNumber() ? float(v.fx().toDouble()) : def; }
float3 color(const Json& j, float3 def) {
    const Json& c = j["color"];
    return c.size() >= 3 ? float3{float(c[0].fx().toDouble()), float(c[1].fx().toDouble()), float(c[2].fx().toDouble())} : def;
}
// `false` switches a default flash off, an object overrides fields of it, anything else keeps it.
RtsLights::Flash flash(const Json& j, RtsLights::Flash def) {
    if (j.isNull()) return def;
    if (!j.isObject()) { if (!j.boolean(true)) def.on = false; return def; }
    def.on = j.getBool("enabled", true);
    def.color = color(j, def.color);
    def.radius = num(j, "radius", def.radius);
    def.intensity = num(j, "intensity", def.intensity);
    def.time = num(j, "time", def.time);
    return def;
}
float3 toF(const Vec3& v) { return {float(v.x.toDouble()), float(v.y.toDouble()), float(v.z.toDouble())}; }
float3 mix(float3 a, float3 b, float t) { return a + (b - a) * t; }
} // namespace

void RtsLights::init(const Rules& rules) {
    rules_ = &rules;
    types_.clear();
    for (const UnitType& t : rules.types) {
        TypeLights L;
        const Json& v = t.view;
        if (const Json& g = v["light"]; g.isObject()) {
            L.glow = g.getBool("enabled", true);
            L.color = color(g, {1.0f, 0.62f, 0.32f});
            L.radius = num(g, "radius", 4.0f);
            L.intensity = num(g, "intensity", 1.5f);
            L.height = num(g, "height", 1.0f);
            L.flicker = num(g, "flicker", 0.0f);
            L.whileBuilding = g.getBool("whileBuilding", false);
        }
        bool ranged = false, splash = false;
        for (const WeaponDef& w : t.weapons) {
            ranged |= w.projectileSpeed.raw > 0 || w.range > Fixed::fromInt(2);
            splash |= w.splash.raw > 0;
        }
        const float big = t.structure ? float(std::max(t.footprintW, t.footprintH)) : 0.0f;
        L.attack = flash(v["attackLight"], {ranged, {1.0f, 0.78f, 0.42f}, 2.2f, 2.4f, 0.09f});
        L.projectile = flash(v["projectileLight"], {ranged, {1.0f, 0.72f, 0.38f}, 1.7f, 1.6f, 0});
        L.hit = flash(v["hitLight"], {splash, {1.0f, 0.55f, 0.22f}, splash ? 3.2f : 1.4f, 2.6f, 0.3f});
        L.death = flash(v["deathLight"], {true, {1.0f, 0.5f, 0.2f}, t.structure ? 2.5f + big * 1.3f : 2.2f,
                                          t.structure ? 3.0f : 1.8f, t.structure ? 0.8f : 0.35f});
        types_.push_back(L);
    }
    if (const char* n = std::getenv("RTS_NIGHT")) night_ = std::clamp(float(std::atof(n)), 0.0f, 1.0f);
}

void RtsLights::applyNight(Lighting& l) {
    if (!haveBase_) { base_ = l; haveBase_ = true; }
    if (night_ == appliedNight_ && night_ == 0) return;
    appliedNight_ = night_;
    // Moonlight: weak bluish sun, dark sky; the fog keeps the horizon readable.
    const float n = night_;
    l.sunIntensity = base_.sunIntensity * (1.0f - 0.72f * n);
    l.sunColor = mix(base_.sunColor, {0.62f, 0.72f, 1.0f}, n);
    l.skyColor = mix(base_.skyColor, {0.13f, 0.17f, 0.30f}, n);
    l.groundColor = mix(base_.groundColor, {0.06f, 0.06f, 0.08f}, n);
    l.fogColor = mix(base_.fogColor, {0.07f, 0.09f, 0.15f}, n);
}

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
        active_.push_back({p, f.color, f.radius, f.intensity, float(time), std::max(f.time, 0.03f)});
    };
    for (const SimEvent& e : s.events) {
        const float3 p = toF(e.pos);
        switch (e.type) {
        case kEvAttack: if (int t = type(e.a); t >= 0) spawn(types_[size_t(t)].attack, p, 0.8f); break;
        case kEvHit:    if (int t = type(e.b); t >= 0) spawn(types_[size_t(t)].hit, p, 0.5f); break;
        case kEvDied:
            if (e.value >= 0 && size_t(e.value) < types_.size()) spawn(types_[size_t(e.value)].death, p, 0.9f);
            break;
        case kEvAbility: spawn({true, {0.55f, 0.75f, 1.0f}, 3.0f, 2.2f, 0.4f}, p, 1.0f); break;
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
        ls.add(PointLight{a.pos, a.radius * (0.75f + 0.25f * k), a.color, a.intensity * k * k, 1.5f});
        active_[w++] = a;
    }
    active_.resize(w);
}

void RtsLights::draw(Renderer3D& r, const RtsScene& s, double time, const std::vector<uint8_t>* vision, const MapGrid& grid, const RtsView& view) {
    if (types_.empty()) return;
    applyNight(r.lighting);
    LightSystem& ls = r.lights;
    if (ls.quality() == LightQuality::Off) { active_.clear(); lastTick_ = s.tick; return; }
    consumeEvents(s, time, vision, grid, view);
    const float tt = float(time);
    for (const SceneObject& o : s.objects) {
        if (!o.visible) continue;
        if (o.isProjectile) {
            if (o.shooterType >= types_.size()) continue;
            const Flash& f = types_[o.shooterType].projectile;
            if (f.on) ls.add(PointLight{o.pos, f.radius, f.color, f.intensity, 0.5f});
            continue;
        }
        if (o.type >= types_.size()) continue;
        const TypeLights& L = types_[o.type];
        if (!L.glow || (o.build < 1.0f && !L.whileBuilding)) continue;
        const UnitType& t = rules_->types[o.type];
        const float scale = float(t.view.getFx("scale", Fixed::fromInt(1)).toDouble());
        float3 p{o.pos.x, view.heightAt(grid, o.pos.x, o.pos.z) + L.height * scale + ((t.layer & kLayerAir) ? 2.4f : 0.0f), o.pos.z};
        float in = L.intensity;
        if (L.flicker > 0) { // two incommensurate waves per object read as fire, not as a pulse
            float ph = o.pos.x * 1.7f + o.pos.z * 2.3f;
            in *= 1.0f + L.flicker * (0.6f * std::sin(tt * 9.0f + ph) + 0.4f * std::sin(tt * 23.0f + ph * 1.9f));
        }
        ls.add(PointLight{p, L.radius * scale, L.color, in, 0});
    }
    emitActive(ls, time);
}

} // namespace rts::game
