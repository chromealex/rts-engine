// Pure part of RtsLights (no GPU, no view): data parsing, day/night sky, weather, lightning.
// Linked into rts_tests.
#include "RtsLights.h"
#include "../RtsAbilityData.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>

namespace rts::game {

using namespace render;

namespace {
constexpr float kTau = 6.28318530718f;
float num(const Json& j, const char* key, float def) { const Json& v = j[key]; return v.isNumber() ? float(v.fx().toDouble()) : def; }
float3 color(const Json& j, float3 def) {
    const Json& c = j["color"];
    return c.size() >= 3 ? float3{float(c[0].fx().toDouble()), float(c[1].fx().toDouble()), float(c[2].fx().toDouble())} : def;
}
// "halo": number = size, false = none, missing = keep the default
float halo(const Json& j, float def) {
    const Json& h = j["halo"];
    if (h.isNumber()) return float(h.fx().toDouble());
    if (!h.isNull() && !h.boolean(true)) return 0;
    return def;
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
    def.halo = halo(j, def.halo);
    return def;
}
float3 mix(float3 a, float3 b, float t) { return a + (b - a) * t; }
float smooth(float e0, float e1, float x) { float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f); return t * t * (3 - 2 * t); }
// Environment presets ("lighting": {"preset": ...}): palettes for day and night plus defaults for
// the block's fields (explicit fields win).
struct PresetDef {
    const char* id;
    float time, dayLength, nightLevel, clouds, storm;   // < 0: no default
    RtsLights::Palette day, night;                      // day.sunK < 0: keep the renderer's day
};
const PresetDef kPresets[] = {
    {"desert", -1, -1, 0.85f, 0, 0,
     {{1.0f, 0.9f, 0.72f}, {0.62f, 0.6f, 0.55f}, {0.45f, 0.36f, 0.24f}, {0.85f, 0.76f, 0.6f}, 1.2f},
     {{0.7f, 0.75f, 1.0f}, {0.16f, 0.16f, 0.26f}, {0.08f, 0.07f, 0.07f}, {0.1f, 0.1f, 0.16f}, 0.32f}},
    {"snow", -1, -1, 0.8f, 0.2f, 0,
     {{1.0f, 0.97f, 0.94f}, {0.62f, 0.7f, 0.82f}, {0.55f, 0.58f, 0.62f}, {0.8f, 0.85f, 0.92f}, 1.0f},
     {{0.62f, 0.72f, 1.0f}, {0.15f, 0.19f, 0.32f}, {0.12f, 0.13f, 0.17f}, {0.12f, 0.15f, 0.22f}, 0.34f}},
    {"volcanic", -1, -1, 1, 0.5f, 0,
     {{1.0f, 0.62f, 0.4f}, {0.45f, 0.3f, 0.28f}, {0.3f, 0.14f, 0.08f}, {0.5f, 0.28f, 0.2f}, 0.8f},
     {{0.9f, 0.5f, 0.35f}, {0.12f, 0.06f, 0.06f}, {0.14f, 0.05f, 0.02f}, {0.14f, 0.06f, 0.05f}, 0.22f}},
    {"overcast", -1, -1, 1, 0.85f, 0,
     {{0.85f, 0.87f, 0.9f}, {0.55f, 0.58f, 0.62f}, {0.32f, 0.31f, 0.29f}, {0.66f, 0.68f, 0.72f}, 0.75f},
     {{0.6f, 0.68f, 0.9f}, {0.11f, 0.13f, 0.2f}, {0.05f, 0.05f, 0.07f}, {0.07f, 0.08f, 0.12f}, 0.22f}},
    // underground: permanent night with almost no ambient — only lights show the way
    {"cave", 0, 0, 1, 0, 0,
     {{1, 1, 1}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0}, -1},
     {{0.5f, 0.55f, 0.7f}, {0.05f, 0.05f, 0.07f}, {0.03f, 0.03f, 0.03f}, {0.02f, 0.02f, 0.03f}, 0.06f}},
};
const RtsLights::Palette kDefaultNight{{0.62f, 0.72f, 1.0f}, {0.13f, 0.17f, 0.30f}, {0.06f, 0.06f, 0.08f}, {0.07f, 0.09f, 0.15f}, 0.28f};

// Default light of an ability from what it does (first effect that isn't a delivery wrapper).
RtsLights::Flash abilityDefault(const AbilityDef& a) {
    RtsLights::Flash f{true, {0.55f, 0.75f, 1.0f}, 3.0f, 2.2f, 0.4f, 0.8f};
    float radius = 0;
    bool kindSet = false;
    for (const EffectDef& e : a.effects) {
        radius = std::max(radius, float(e.radius.toDouble()));
        if (kindSet || e.kind == EffectKind::Area || e.kind == EffectKind::Projectile || e.kind == EffectKind::Chain || e.kind == EffectKind::Cast) continue;
        kindSet = true;
        switch (e.kind) {
        case EffectKind::Damage: f.color = {1.0f, 0.55f, 0.25f}; break;
        case EffectKind::Heal: f.color = {0.45f, 1.0f, 0.5f}; break;
        case EffectKind::Stun: f.color = {1.0f, 0.95f, 0.6f}; break;
        case EffectKind::Buff: case EffectKind::Status: f.color = {1.0f, 0.85f, 0.4f}; break;
        case EffectKind::Spawn: f.color = {0.75f, 0.5f, 1.0f}; break;
        case EffectKind::Blink: case EffectKind::Teleport: f.color = {0.4f, 0.8f, 1.0f}; break;
        case EffectKind::Kill: case EffectKind::EnergyBurn: f.color = {0.7f, 0.4f, 1.0f}; break;
        default: break;
        }
    }
    if (radius > 0) { f.radius = std::clamp(radius * 1.4f, 2.0f, 9.0f); f.halo = f.radius * 0.3f; f.intensity = 2.6f; f.time = 0.55f; }
    return f;
}
} // namespace

void RtsLights::init(const Rules& rules) {
    rules_ = &rules;
    types_.clear();
    active_.clear();
    for (const UnitType& t : rules.types) {
        TypeLights L;
        const Json& v = t.view;
        if (const Json& g = v["light"]; g.isObject()) {
            if (g.getBool("enabled", true)) L.glows.push_back(parseGlow(g));
        } else if (g.isArray()) {
            for (const Json& e : g.items()) if (e.isObject() && e.getBool("enabled", true)) L.glows.push_back(parseGlow(e));
        }
        if (const Json& c = v["contactShadow"]; c.isNumber()) L.contact = std::clamp(float(c.fx().toDouble()), 0.0f, 1.0f);
        else if (!c.isNull() && !c.boolean(true)) L.contact = 0;
        L.contactBox = t.structure && t.resourceNode < 0 && t.footprintW * t.footprintH > 1;
        if (v.has("contactShape")) L.contactBox = v.getStr("contactShape") == "box";
        bool ranged = false, splash = false;
        for (const WeaponDef& w : t.weapons) {
            ranged |= w.projectileSpeed.raw > 0 || w.range > Fixed::fromInt(2);
            splash |= w.splash.raw > 0;
        }
        const float big = t.structure ? float(std::max(t.footprintW, t.footprintH)) : 0.0f;
        L.attack = flash(v["attackLight"], {ranged, {1.0f, 0.78f, 0.42f}, 2.2f, 2.4f, 0.09f, 0.45f});
        L.projectile = flash(v["projectileLight"], {ranged, {1.0f, 0.72f, 0.38f}, 1.7f, 1.6f, 0, 0.3f});
        L.hit = flash(v["hitLight"], {splash, {1.0f, 0.55f, 0.22f}, splash ? 3.2f : 1.4f, 2.6f, 0.3f, splash ? 1.0f : 0.4f});
        L.death = flash(v["deathLight"], {true, {1.0f, 0.5f, 0.2f}, t.structure ? 2.5f + big * 1.3f : 2.2f,
                                          t.structure ? 3.0f : 1.8f, t.structure ? 0.8f : 0.35f, t.structure ? 1.2f + big * 0.4f : 0.6f});
        const Json& al = v["abilityLights"];   // by ability id: flash block or false
        for (const AbilityDef& a : t.abilities) L.abilities.push_back(flash(al[a.id], abilityDefault(a)));
        types_.push_back(L);
    }
    configure(Json{}, Json{});
}

RtsLights::Glow RtsLights::parseGlow(const Json& j) {
    Glow g;
    g.spot = j.getStr("type") == "spot" || j.has("angle");
    g.color = color(j, g.spot ? float3{0.95f, 0.97f, 1.0f} : g.color);
    g.radius = num(j, "radius", g.spot ? 9.0f : 4.0f);
    g.intensity = num(j, "intensity", g.spot ? 2.2f : 1.5f);
    g.height = num(j, "height", g.spot ? 1.5f : 1.0f);
    g.flicker = num(j, "flicker", 0.0f);
    g.halo = halo(j, g.radius * (g.spot ? 0.05f : 0.2f));
    g.whileBuilding = j.getBool("whileBuilding", false);
    g.angle = std::clamp(num(j, "angle", g.angle), 2.0f, 170.0f);
    g.softness = std::clamp(num(j, "softness", g.softness), 0.02f, 1.0f);
    g.pitch = std::clamp(num(j, "pitch", g.pitch), -10.0f, 90.0f);
    g.yaw = num(j, "yaw", 0);
    g.sweep = std::max(num(j, "sweep", 0), 0.0f);
    g.sweepSpeed = num(j, "sweepSpeed", g.sweepSpeed);
    // a shaft shows from searchlights up high (dust, haze); low lamps and headlights don't get one
    g.beam = std::clamp(num(j, "beam", g.height >= 1.5f ? g.beam : 0.0f), 0.0f, 1.0f);
    if (const Json& o = j["offset"]; o.size() >= 2) { g.forward = float(o[0].fx().toDouble()); g.side = float(o[1].fx().toDouble()); }
    const std::string pat = j.getStr("pattern", "steady");
    g.pattern = pat == "pulse" ? Glow::Pattern::Pulse : pat == "strobe" ? Glow::Pattern::Strobe : pat == "beacon" ? Glow::Pattern::Beacon : Glow::Pattern::Steady;
    g.rate = std::clamp(num(j, "rate", g.pattern == Glow::Pattern::Strobe ? 1.5f : g.pattern == Glow::Pattern::Beacon ? 0.5f : 0.6f), 0.01f, 30.0f);
    g.electric = j.getBool("electric", g.spot);
    g.nightOnly = j.getBool("nightOnly", false);
    return g;
}

float RtsLights::glowLevel(const Glow& g, float t, float phase, float hp, bool powered, float night) {
    auto hash = [](float x) { float h = std::sin(x * 12.9898f) * 43758.5453f; return h - std::floor(h); };
    float lvl = 1;
    const float cyc = t * g.rate + phase * 0.159f;
    switch (g.pattern) {
    case Glow::Pattern::Pulse: lvl = 0.55f + 0.45f * std::sin(cyc * kTau); break;
    case Glow::Pattern::Strobe: lvl = cyc - std::floor(cyc) < 0.12f ? 1.0f : 0.0f; break;
    case Glow::Pattern::Beacon: if (!g.spot) { float s = 0.5f + 0.5f * std::sin(cyc * kTau); lvl = s * s * s * s; } break;   // spots rotate instead
    default: break;
    }
    if (g.nightOnly) lvl *= smooth(0.15f, 0.5f, night);
    if (g.electric) {
        if (!powered) lvl *= hash(std::floor(t * 7.0f) + phase) > 0.75f ? 0.7f : 0.2f;   // brown-out: dim, stuttering
        if (hp < 0.4f && hash(std::floor(t * 11.0f) + phase * 3.1f) < (0.4f - hp) * 1.8f) lvl *= 0.1f;   // sparking, failing
    }
    return lvl;
}

render::PointLight RtsLights::glowLight(const Glow& g, float3 ground, float yaw, float scale, float t, float phase) {
    const float cy = std::cos(yaw), sy = std::sin(yaw);
    const float3 p{ground.x + (cy * g.forward - sy * g.side) * scale, ground.y + g.height * scale, ground.z + (sy * g.forward + cy * g.side) * scale};
    float in = g.intensity;
    if (g.flicker > 0) // two incommensurate waves per object read as fire, not as a pulse
        in *= 1.0f + g.flicker * (0.6f * std::sin(t * 9.0f + phase) + 0.4f * std::sin(t * 23.0f + phase * 1.9f));
    PointLight l{p, g.radius * scale, g.color, in, 0};
    l.rank = g.intensity;   // flicker must not reshuffle the per-pixel slots
    l.groundY = ground.y;
    l.halo = std::max(g.halo, 0.0f) * scale;
    if (g.spot) {
        constexpr float kDeg = 0.0174532925f;
        // searchlight swing: amplitude `sweep`, peak angular speed `sweepSpeed`
        float swing = g.sweep > 0 ? g.sweep * std::sin(t * g.sweepSpeed / g.sweep + phase) : 0.0f;
        if (g.pattern == Glow::Pattern::Beacon) swing = std::fmod(t * g.rate * 360.0f + phase * 57.3f, 360.0f);   // rotating beacon
        const float a = yaw + (g.yaw + swing) * kDeg, pitch = g.pitch * kDeg;
        l.makeSpot({std::cos(a) * std::cos(pitch), -std::sin(pitch), std::sin(a) * std::cos(pitch)}, g.angle, g.softness);
    }
    return l;
}

std::vector<RtsLights::MapLight> RtsLights::parseMapLights(const Json& lights) {
    std::vector<MapLight> out;
    for (const Json& j : lights.items()) {
        if (!j.isObject()) continue;
        MapLight m;
        static_cast<Glow&>(m) = parseGlow(j);
        const Json& p = j["pos"];
        m.x = p.size() >= 2 ? float(p[0].fx().toDouble()) : num(j, "x", 0);
        m.z = p.size() >= 2 ? float(p[1].fx().toDouble()) : num(j, "z", 0);
        out.push_back(m);
    }
    return out;
}

void RtsLights::configure(const Json& game, const Json& mapBlock) {
    configure(game, mapBlock, Json{});
}

void RtsLights::configure(const Json& game, const Json& map, const Json& lights) {
    mapLights_ = parseMapLights(lights);
    const std::string mapPreset = map.getStr("preset", "");
    preset_ = mapPreset.empty() ? game.getStr("preset", "") : mapPreset;
    const PresetDef* pr = nullptr;
    for (const PresetDef& p : kPresets) if (preset_ == p.id) pr = &p;
    if (!preset_.empty() && !pr) preset_.clear();   // unknown preset: plain lighting
    const bool mapPr = pr && !mapPreset.empty();
    // precedence: map field > map preset > game field > game preset > default
    // (a cave map in a daylight game must stay a cave)
    auto pick = [&](const char* key, float presetValue, float def) {
        if (map[key].isNumber()) return num(map, key, def);
        if (mapPr && presetValue >= 0) return presetValue;
        if (game[key].isNumber()) return num(game, key, def);
        if (pr && presetValue >= 0) return presetValue;
        return def;
    };
    cycle_ = game.isObject() || map.isObject();
    waterRefl_ = map["waterReflections"].boolean(game["waterReflections"].boolean(true));
    combatFlashes_ = map["combatFlashes"].boolean(game["combatFlashes"].boolean(true));
    startTime_ = pick("time", pr ? pr->time : -1, 0.5f);
    dayLength_ = std::max(pick("dayLength", pr ? pr->dayLength : -1, 0.0f), 0.0f);
    nightLevel_ = std::clamp(pick("nightLevel", pr ? pr->nightLevel : -1, 1.0f), 0.0f, 1.0f);
    forcedNight_ = -1;
    day_ = pr ? pr->day : Palette{};
    night_p_ = pr ? pr->night : kDefaultNight;
    clouds_ = std::clamp(pick("clouds", pr ? pr->clouds : -1, 0.0f), 0.0f, 1.0f);
    cloudStrength_ = std::clamp(pick("cloudStrength", -1, 0.45f), 0.0f, 1.0f);
    cloudScale_ = std::max(pick("cloudScale", -1, 1.0f), 0.05f);
    storm_ = std::clamp(pick("storm", pr ? pr->storm : -1, 0.0f), 0.0f, 1.0f);
    {
        const Json& w = map["wind"].size() >= 2 ? map["wind"] : game["wind"];
        windX_ = w.size() >= 2 ? float(w[0].fx().toDouble()) : 1.2f;
        windZ_ = w.size() >= 2 ? float(w[1].fx().toDouble()) : 0.5f;
    }
    if (const char* c = std::getenv("RTS_CLOUDS")) { clouds_ = std::clamp(float(std::atof(c)), 0.0f, 1.0f); cycle_ = true; }
    if (const char* st = std::getenv("RTS_STORM")) { storm_ = std::clamp(float(std::atof(st)), 0.0f, 1.0f); cycle_ = true; }
    if (const char* t = std::getenv("RTS_TIME_OF_DAY")) { startTime_ = float(std::atof(t)); cycle_ = true; }
    if (const char* n = std::getenv("RTS_NIGHT")) { forcedNight_ = std::clamp(float(std::atof(n)), 0.0f, 1.0f); cycle_ = true; }
}

float RtsLights::lightningAt(double seconds, float storm) {
    if (storm <= 0) return 0;
    float out = 0;
    const double sec = std::floor(seconds);
    for (int back = 0; back < 2; ++back) { // a flash started in this or the previous second
        uint32_t h = uint32_t(int64_t(sec) - back) * 2654435761u;
        h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
        if (float(h & 0xffff) / 65535.0f > storm * 0.16f) continue;
        const float start = float(sec - back) + float((h >> 16) & 0xff) / 255.0f;
        const float age = float(seconds) - start;
        if (age < 0 || age > 0.45f) continue;
        // double strike: a bright pulse, a short gap, a weaker one
        const float f = std::exp(-age * 30.0f) + (age > 0.12f ? 0.6f * std::exp(-(age - 0.12f) * 22.0f) : 0.0f);
        out = std::max(out, std::min(f, 1.0f));
    }
    return out;
}

void RtsLights::updateSky(Lighting& l, double seconds, float cycle) {
    if (!haveBase_) { base_ = l; haveBase_ = true; }
    const bool active = cycle_ || preview_ || cycle >= 0;
    if (!active) {
        night_ = dusk_ = 0;
        if (touched_) { l = base_; touched_ = false; }
        return;
    }
    touched_ = true;
    float t = preview_ ? previewTime_ : cycle >= 0 ? cycle : startTime_ + (dayLength_ > 0 ? float(seconds) / dayLength_ : 0.0f);
    t -= std::floor(t);
    timeOfDay_ = t;
    // sun elevation: +1 noon, 0 dawn/dusk, -1 midnight
    const float elev = std::sin(kTau * (t - 0.25f));
    night_ = forcedNight_ >= 0 ? forcedNight_ : smooth(0.0f, -0.3f, elev) * nightLevel_;
    dusk_ = forcedNight_ >= 0 ? 0.0f : (1.0f - smooth(0.05f, 0.4f, std::fabs(elev))) * (1.0f - night_ * 0.7f);
    const float n = night_, d = dusk_;

    // Direction: the sun (moon at night) swings around the base azimuth and never goes low enough
    // to stretch shadows across the whole map. Periodic in t, so the day wraps without a jump.
    const float3 b = base_.sunDir;
    const float horiz = std::sqrt(b.x * b.x + b.z * b.z);
    const float az0 = std::atan2(b.z, b.x), az = az0 + 0.75f * std::sin(kTau * (t - 0.5f));
    const float up = 0.45f + 0.55f * std::fabs(elev);
    l.sunDir = forcedNight_ >= 0 ? b : normalize(float3{std::cos(az) * std::max(horiz, 0.3f), up * std::max(b.y, 0.5f), std::sin(az) * std::max(horiz, 0.3f)});

    // Colours: day -> warm dusk -> night (moonlight by default; presets bring their own palettes).
    const bool ownDay = day_.sunK >= 0;
    const float3 daySun = ownDay ? day_.sun : base_.sunColor, daySky = ownDay ? day_.sky : base_.skyColor,
                 dayGround = ownDay ? day_.ground : base_.groundColor, dayFog = ownDay ? day_.fog : base_.fogColor;
    const float dayK = ownDay ? day_.sunK : 1.0f;
    const float3 duskSun{1.0f, 0.5f, 0.24f}, duskSky{0.62f, 0.44f, 0.46f}, duskFog{0.86f, 0.56f, 0.42f};
    l.sunIntensity = base_.sunIntensity * dayK * (1.0f - 0.2f * d) * (1.0f - (1.0f - night_p_.sunK) * n);
    l.sunColor = mix(mix(daySun, duskSun, d), night_p_.sun, n);
    l.skyColor = mix(mix(daySky, duskSky, d), night_p_.sky, n);
    l.groundColor = mix(mix(dayGround, dayGround * 0.8f, d), night_p_.ground, n);
    l.fogColor = mix(mix(dayFog, duskFog, d), night_p_.fog, n);

    // Storm: overcast (weaker, greyer sun) and lightning. A flash may start in each game second
    // with probability ~ storm; a hash of the second index keeps it identical in replays.
    lightning_ = 0;
    const float storm = preview_ ? previewStorm_ : storm_;
    if (storm > 0) {
        l.sunIntensity *= 1.0f - 0.45f * storm;
        l.sunColor = mix(l.sunColor, {0.75f, 0.78f, 0.85f}, 0.5f * storm);
        l.fogColor = mix(l.fogColor, l.fogColor * 0.6f, storm);
        lightning_ = lightningAt(seconds, storm);
        if (lightning_ > 0) {
            const float k = lightning_;
            l.sunIntensity += 1.6f * k;
            l.sunColor = mix(l.sunColor, {0.85f, 0.9f, 1.0f}, k);
            l.skyColor = mix(l.skyColor, {0.7f, 0.76f, 0.95f}, 0.8f * k);
            l.groundColor = mix(l.groundColor, {0.45f, 0.45f, 0.5f}, 0.5f * k);
        }
    }
}


} // namespace rts::game
