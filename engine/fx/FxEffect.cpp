#include "FxEffect.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace rts::fx {

namespace {

constexpr const char* kQualityNames[] = {"off", "low", "medium", "high"};

float num(const Json& j, float def) { return j.isNumber() ? float(j.fx().toDouble()) : def; }

struct Reader {
    const Json& obj;
    std::string where;
    std::vector<std::string>& errors;
    bool ok = true;

    void fail(const std::string& key, const std::string& msg) {
        errors.push_back(where + "." + key + ": " + msg);
        ok = false;
    }
    void number(const char* key, float& v, float lo, float hi) {
        const Json& j = obj[key];
        if (j.isNull()) return;
        if (!j.isNumber()) return fail(key, "expected a number");
        float x = num(j, v);
        if (x < lo || x > hi) { char b[96]; std::snprintf(b, sizeof b, "must be in %g..%g", double(lo), double(hi)); return fail(key, b); }
        v = x;
    }
    void integer(const char* key, int& v, int lo, int hi) {
        float f = float(v);
        number(key, f, float(lo), float(hi));
        v = int(std::lround(f));
    }
    void range(const char* key, FxRange& r, float lo, float hi) {
        const Json& j = obj[key];
        if (j.isNull()) return;
        if (j.isNumber()) { float x = num(j, 0); if (x < lo || x > hi) return fail(key, "out of range"); r = {x, x}; return; }
        if (!j.isArray() || j.size() != 2 || !j[size_t(0)].isNumber() || !j[size_t(1)].isNumber()) return fail(key, "expected a number or [min, max]");
        float a = num(j[size_t(0)], 0), b = num(j[size_t(1)], 0);
        if (a > b) std::swap(a, b);
        if (a < lo || b > hi) { char m[96]; std::snprintf(m, sizeof m, "must be in %g..%g", double(lo), double(hi)); return fail(key, m); }
        r = {a, b};
    }
    void vec3(const char* key, float3& v) {
        const Json& j = obj[key];
        if (j.isNull()) return;
        if (!j.isArray() || j.size() != 3) return fail(key, "expected [x, y, z]");
        for (size_t i = 0; i < 3; ++i) if (!j[i].isNumber()) return fail(key, "expected [x, y, z]");
        v = {num(j[size_t(0)], 0), num(j[size_t(1)], 0), num(j[size_t(2)], 0)};
    }
    template <class E, size_t N>
    void choice(const char* key, E& v, const char* const (&names)[N]) {
        const Json& j = obj[key];
        if (j.isNull()) return;
        std::string s = j.str();
        for (size_t i = 0; i < N; ++i) if (s == names[i]) { v = E(i); return; }
        std::string opts;
        for (size_t i = 0; i < N; ++i) opts += std::string(i ? ", " : "") + "'" + names[i] + "'";
        fail(key, "expected one of " + opts);
    }
};

constexpr const char* kBlendNames[] = {"alpha", "additive", "premultiplied"};
constexpr const char* kAlignNames[] = {"camera", "velocity", "ground", "upright"};
constexpr const char* kShapeNames[] = {"point", "sphere", "disc", "ring", "hemisphere", "cone", "box", "line"};
constexpr const char* kSubNames[] = {"birth", "death", "collision"};
constexpr const char* kSpaceNames[] = {"world", "local"};
constexpr const char* kQualityChoice[] = {"off", "low", "medium", "high"};

constexpr const char* kKnownKeys[] = {
    "texture", "frames", "fps", "blend", "align", "burst", "rate", "perMeter", "delay", "duration", "loop",
    "life", "speed", "size", "rotation", "spin", "direction", "spread", "shape", "radius", "gravity", "drag",
    "stretch", "sizeOverLife", "color", "minQuality", "comment",
    // optional modules
    "aspect", "offset", "bursts", "startColor", "space", "maxParticles", "velocity", "speedLimit", "colorBySpeed",
    "sizeBySpeed", "noise", "cycles", "startFrame", "arc", "thickness", "radial", "hidden", "box", "collision", "subEmitters", "trail", "light"};

bool knownKey(const std::string& k, std::initializer_list<const char*> keys) {
    for (const char* n : keys) if (k == n) return true;
    return false;
}
void unknownKeys(const Json& o, std::initializer_list<const char*> keys, const std::string& where, std::vector<std::string>& errors, bool& ok) {
    for (auto& [k, v] : o.members())
        if (!knownKey(k, keys) && k != "comment") { errors.push_back(where + "." + k + ": unknown field"); ok = false; }
}

// [[t, k], ...] with k >= 0
bool sizeKeys(const Json& s, std::vector<FxSizeKey>& out) {
    std::vector<FxSizeKey> keys;
    if (s.isNumber()) { float k = std::max(0.0f, num(s, 1)); out = {{0, k}, {1, k}}; return true; }
    if (!s.isArray() || s.size() < 1) return false;
    for (size_t k = 0; k < s.size(); ++k) {
        const Json& kv = s[k];
        if (!kv.isArray() || kv.size() != 2 || !kv[size_t(0)].isNumber() || !kv[size_t(1)].isNumber()) return false;
        keys.push_back({std::clamp(num(kv[size_t(0)], 0), 0.0f, 1.0f), std::max(0.0f, num(kv[size_t(1)], 1))});
    }
    std::stable_sort(keys.begin(), keys.end(), [](auto& a, auto& b) { return a.t < b.t; });
    out = keys;
    return true;
}
// "#rrggbbaa" or [[t, colour], ...]
bool colorKeys(const Json& c, std::vector<FxColorKey>& out) {
    std::vector<FxColorKey> keys;
    float4 single;
    if (parseColor(c, single)) { out = {{0, single}, {1, single}}; return true; }
    if (!c.isArray() || c.size() < 1) return false;
    for (size_t k = 0; k < c.size(); ++k) {
        const Json& kv = c[k];
        float4 col;
        if (!kv.isArray() || kv.size() != 2 || !kv[size_t(0)].isNumber() || !parseColor(kv[size_t(1)], col)) return false;
        keys.push_back({std::clamp(num(kv[size_t(0)], 0), 0.0f, 1.0f), col});
    }
    std::stable_sort(keys.begin(), keys.end(), [](auto& a, auto& b) { return a.t < b.t; });
    out = keys;
    return true;
}

int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

Json numJ(float v) { return Json::number(double(v), 3); }
Json rangeJ(FxRange r) {
    if (r.min == r.max) return numJ(r.min);
    Json a = Json::array(); a.push(numJ(r.min)); a.push(numJ(r.max)); return a;
}
Json vecJ(float3 v) { Json a = Json::array(); a.push(numJ(v.x)); a.push(numJ(v.y)); a.push(numJ(v.z)); return a; }

} // namespace

const char* qualityName(FxQuality q) { return kQualityNames[std::min<size_t>(size_t(q), 3)]; }
bool qualityFromName(const std::string& s, FxQuality& out) {
    for (size_t i = 0; i < 4; ++i) if (s == kQualityNames[i]) { out = FxQuality(i); return true; }
    return false;
}

float EmitterDef::endTime() const {
    if (loop && (continuous() || !bursts.empty())) return std::numeric_limits<float>::infinity();
    float t = continuous() ? duration : 0.0f;
    for (auto& b : bursts) t = std::max(t, b.time + float(std::max(0, b.cycles - 1)) * b.interval);
    return delay + t;
}

bool EffectDef::looping() const {
    for (auto& e : emitters) if (e.loop && (e.continuous() || !e.bursts.empty())) return true;
    return false;
}

float EffectDef::lifetime() const {
    float t = 0;
    for (auto& e : emitters) t = std::max(t, e.endTime() + e.life.max);
    return t;
}

bool parseColor(const Json& j, float4& out) {
    if (j.isString()) {
        std::string s = j.str();
        if (s.size() != 7 && s.size() != 9) return false;
        if (s[0] != '#') return false;
        float c[4] = {1, 1, 1, 1};
        for (size_t i = 0; i * 2 + 1 < s.size(); ++i) {
            int hi = hexDigit(s[1 + i * 2]), lo = hexDigit(s[2 + i * 2]);
            if (hi < 0 || lo < 0) return false;
            c[i] = float(hi * 16 + lo) / 255.0f;
        }
        out = {c[0], c[1], c[2], c[3]};
        return true;
    }
    if (j.isArray() && (j.size() == 3 || j.size() == 4)) {
        float c[4] = {1, 1, 1, 1};
        for (size_t i = 0; i < j.size(); ++i) {
            if (!j[i].isNumber()) return false;
            c[i] = std::clamp(num(j[i], 1), 0.0f, 1.0f);
        }
        out = {c[0], c[1], c[2], c[3]};
        return true;
    }
    return false;
}

std::string colorToHex(float4 c) {
    auto b = [](float v) { return int(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
    char s[16];
    std::snprintf(s, sizeof s, "#%02x%02x%02x%02x", b(c.x), b(c.y), b(c.z), b(c.w));
    return s;
}

namespace {

void parseModules(const Json& e, Reader& r, EmitterDef& d) {
    r.range("aspect", d.aspect, 0.01f, 100);
    r.vec3("offset", d.offset);
    if (e.has("bursts")) {
        const Json& b = e["bursts"];
        bool good = b.isArray();
        for (size_t i = 0; good && i < b.size(); ++i) {
            const Json& x = b[i];
            FxBurst fb;
            if (x.isArray() && x.size() == 2 && x[size_t(0)].isNumber() && x[size_t(1)].isNumber()) {
                fb.time = num(x[size_t(0)], 0);
                fb.count = {num(x[size_t(1)], 1), num(x[size_t(1)], 1)};
            } else if (x.isObject()) {
                Reader br{x, r.where + ".bursts[" + std::to_string(i) + "]", r.errors};
                unknownKeys(x, {"time", "count", "cycles", "interval"}, br.where, r.errors, r.ok);
                br.number("time", fb.time, 0, 600);
                br.range("count", fb.count, 0, 2000);
                br.integer("cycles", fb.cycles, 1, 1000);
                br.number("interval", fb.interval, 0, 600);
                if (!br.ok) r.ok = false;
            } else good = false;
            if (fb.time < 0 || fb.count.max > 2000) good = false;
            if (good) d.bursts.push_back(fb);
        }
        if (!good) r.fail("bursts", "expected [[time, count], ...] or [{ \"time\", \"count\", \"cycles\", \"interval\" }]");
    }
    if (e.has("startColor")) {
        const Json& c = e["startColor"];
        float4 a, b;
        if (parseColor(c, a)) d.startColor = {{0, a}};
        else if (c.isArray() && c.size() == 2 && parseColor(c[size_t(0)], a) && parseColor(c[size_t(1)], b)) { d.startColor = {{0, a}, {1, b}}; d.startColorPair = true; }
        else if (!colorKeys(c, d.startColor)) r.fail("startColor", "expected \"#rrggbbaa\", [\"#a\", \"#b\"] or [[t, \"#rrggbbaa\"], ...]");
    }
    if (e.has("space")) { int sp = 0; r.choice("space", sp, kSpaceNames); d.localSpace = sp == 1; }
    r.integer("maxParticles", d.maxParticles, 0, 100000);
    r.vec3("velocity", d.velocity);
    r.number("speedLimit", d.speedLimit, 0, 1000);
    auto bySpeed = [&](const char* key, FxRange& range, auto&& keysFn) {
        const Json& o = e[key];
        if (o.isNull()) return;
        if (!o.isObject()) return r.fail(key, "expected { \"speed\": [min, max], ... }");
        Reader sr{o, r.where + "." + key, r.errors};
        sr.range("speed", range, 0, 1000);
        if (!sr.ok) r.ok = false;
        keysFn(o, sr);
    };
    bySpeed("colorBySpeed", d.colorSpeed, [&](const Json& o, Reader& sr) {
        unknownKeys(o, {"speed", "color"}, sr.where, r.errors, r.ok);
        if (!colorKeys(o["color"], d.colorBySpeed)) r.fail("colorBySpeed.color", "expected [[t, \"#rrggbbaa\"], ...]");
    });
    bySpeed("sizeBySpeed", d.sizeSpeed, [&](const Json& o, Reader& sr) {
        unknownKeys(o, {"speed", "size"}, sr.where, r.errors, r.ok);
        if (!sizeKeys(o["size"], d.sizeBySpeed)) r.fail("sizeBySpeed.size", "expected [[t, k], ...]");
    });
    if (e.has("noise")) {
        const Json& o = e["noise"];
        if (!o.isObject()) r.fail("noise", "expected { \"strength\", \"frequency\", \"scroll\" }");
        else {
            Reader nr{o, r.where + ".noise", r.errors};
            unknownKeys(o, {"strength", "frequency", "scroll"}, nr.where, r.errors, r.ok);
            nr.number("strength", d.noise.strength, 0, 100);
            nr.number("frequency", d.noise.frequency, 0.001f, 100);
            nr.number("scroll", d.noise.scroll, -100, 100);
            if (!nr.ok) r.ok = false;
        }
    }
    r.number("cycles", d.cycles, 0.01f, 1000);
    r.range("startFrame", d.startFrame, 0, 255);
    r.number("arc", d.arc, 0, 360);
    r.number("thickness", d.thickness, 0, 1);
    r.number("radial", d.radial, -1, 1);
    if (e.has("hidden")) d.hidden = e["hidden"].boolean();
    r.vec3("box", d.box);
    if (e.has("collision")) {
        const Json& o = e["collision"];
        if (o.type() == Json::Type::Bool) d.collision.enabled = o.boolean();
        else if (!o.isObject()) r.fail("collision", "expected true or { \"height\", \"bounce\", \"dampen\", \"lifeLoss\", \"minKillSpeed\" }");
        else {
            Reader cr{o, r.where + ".collision", r.errors};
            unknownKeys(o, {"height", "bounce", "dampen", "lifeLoss", "minKillSpeed"}, cr.where, r.errors, r.ok);
            d.collision.enabled = true;
            cr.number("height", d.collision.height, -100, 100);
            cr.number("bounce", d.collision.bounce, 0, 2);
            cr.number("dampen", d.collision.dampen, 0, 1);
            cr.number("lifeLoss", d.collision.lifeLoss, 0, 1);
            cr.number("minKillSpeed", d.collision.minKillSpeed, 0, 1000);
            if (!cr.ok) r.ok = false;
        }
    }
    if (e.has("subEmitters")) {
        const Json& l = e["subEmitters"];
        if (!l.isArray()) r.fail("subEmitters", "expected [{ \"on\": \"death\", \"effect\": \"name\" }, ...]");
        for (size_t i = 0; l.isArray() && i < l.size(); ++i) {
            const Json& o = l[i];
            Reader sr{o, r.where + ".subEmitters[" + std::to_string(i) + "]", r.errors};
            if (!o.isObject()) { sr.fail("", "expected an object"); r.ok = false; continue; }
            unknownKeys(o, {"on", "effect", "chance", "inheritColor"}, sr.where, r.errors, r.ok);
            FxSubEmitter se;
            sr.choice("on", se.on, kSubNames);
            se.effect = o["effect"].str();
            if (se.effect.empty()) sr.fail("effect", "expected an effect name (fx/<name>.json)");
            sr.number("chance", se.chance, 0, 1);
            se.inheritColor = o.getBool("inheritColor");
            if (!sr.ok) r.ok = false;
            else if (d.subEmitters.size() < 4) d.subEmitters.push_back(se);
            else { r.fail("subEmitters", "at most 4 sub-emitters per emitter"); break; }
        }
    }
    if (e.has("trail")) {
        const Json& o = e["trail"];
        if (!o.isObject()) r.fail("trail", "expected { \"time\", \"width\", \"color\", ... }");
        else {
            Reader tr{o, r.where + ".trail", r.errors};
            unknownKeys(o, {"time", "width", "color", "texture", "blend", "inheritColor", "sizeAffectsWidth", "minQuality"}, tr.where, r.errors, r.ok);
            FxTrail& t = d.trail;
            t.enabled = true;
            tr.number("time", t.time, 0.01f, 10);
            if (o.has("width") && !sizeKeys(o["width"], t.width)) tr.fail("width", "expected a number or [[t, k], ...]");
            if (o.has("color") && !colorKeys(o["color"], t.color)) tr.fail("color", "expected \"#rrggbbaa\" or [[t, \"#rrggbbaa\"], ...]");
            if (o.has("texture")) { t.texture = o["texture"].str(); if (t.texture.empty()) tr.fail("texture", "expected a name"); }
            tr.choice("blend", t.blend, kBlendNames);
            if (o.has("inheritColor")) t.inheritColor = o["inheritColor"].boolean();
            if (o.has("sizeAffectsWidth")) t.sizeAffectsWidth = o["sizeAffectsWidth"].boolean();
            tr.choice("minQuality", t.minQuality, kQualityChoice);
            if (!tr.ok) r.ok = false;
        }
    }
    if (e.has("light")) {
        const Json& o = e["light"];
        if (!o.isObject()) r.fail("light", "expected { \"color\", \"radius\", \"intensity\", ... }");
        else {
            Reader lr{o, r.where + ".light", r.errors};
            unknownKeys(o, {"color", "radius", "intensity", "ratio", "max", "useParticleColor", "sizeAffectsRadius", "alphaAffectsIntensity"}, lr.where, r.errors, r.ok);
            FxLight& l = d.light;
            l.enabled = true;
            if (o.has("color") && !parseColor(o["color"], l.color)) lr.fail("color", "expected \"#rrggbb\"");
            lr.number("radius", l.radius, 0.01f, 100);
            lr.number("intensity", l.intensity, 0, 20);
            lr.number("ratio", l.ratio, 0, 1);
            lr.integer("max", l.max, 0, 64);
            if (o.has("useParticleColor")) l.useParticleColor = o["useParticleColor"].boolean();
            if (o.has("sizeAffectsRadius")) l.sizeAffectsRadius = o["sizeAffectsRadius"].boolean();
            if (o.has("alphaAffectsIntensity")) l.alphaAffectsIntensity = o["alphaAffectsIntensity"].boolean();
            if (!lr.ok) r.ok = false;
        }
    }
}

Json sizeKeysJ(const std::vector<FxSizeKey>& keys) {
    Json a = Json::array();
    for (auto& k : keys) { Json kv = Json::array(); kv.push(numJ(k.t)); kv.push(numJ(k.k)); a.push(kv); }
    return a;
}
Json colorKeysJ(const std::vector<FxColorKey>& keys) {
    Json a = Json::array();
    for (auto& k : keys) { Json kv = Json::array(); kv.push(numJ(k.t)); kv.push(Json::string(colorToHex(k.rgba))); a.push(kv); }
    return a;
}

void modulesToJson(const EmitterDef& d, Json& e) {
    const EmitterDef base;
    if (!(d.aspect == base.aspect)) e.set("aspect", rangeJ(d.aspect));
    if (d.offset.x != 0 || d.offset.y != 0 || d.offset.z != 0) e.set("offset", vecJ(d.offset));
    if (!d.bursts.empty()) {
        Json a = Json::array();
        for (auto& b : d.bursts) {
            if (b.cycles == 1 && b.count.min == b.count.max) { Json kv = Json::array(); kv.push(numJ(b.time)); kv.push(numJ(b.count.min)); a.push(kv); continue; }
            Json o = Json::object();
            o.set("time", numJ(b.time));
            o.set("count", rangeJ(b.count));
            if (b.cycles != 1) { o.set("cycles", Json::number(int64_t(b.cycles))); o.set("interval", numJ(b.interval)); }
            a.push(o);
        }
        e.set("bursts", a);
    }
    if (d.startColor.size() == 1) e.set("startColor", Json::string(colorToHex(d.startColor[0].rgba)));
    else if (d.startColorPair && d.startColor.size() == 2) {
        Json a = Json::array(); a.push(Json::string(colorToHex(d.startColor[0].rgba))); a.push(Json::string(colorToHex(d.startColor[1].rgba)));
        e.set("startColor", a);
    } else if (!d.startColor.empty()) e.set("startColor", colorKeysJ(d.startColor));
    if (d.localSpace) e.set("space", Json::string("local"));
    if (d.maxParticles) e.set("maxParticles", Json::number(int64_t(d.maxParticles)));
    if (d.velocity.x != 0 || d.velocity.y != 0 || d.velocity.z != 0) e.set("velocity", vecJ(d.velocity));
    if (d.speedLimit) e.set("speedLimit", numJ(d.speedLimit));
    if (!d.colorBySpeed.empty()) { Json o = Json::object(); o.set("speed", rangeJ(d.colorSpeed)); o.set("color", colorKeysJ(d.colorBySpeed)); e.set("colorBySpeed", o); }
    if (!d.sizeBySpeed.empty()) { Json o = Json::object(); o.set("speed", rangeJ(d.sizeSpeed)); o.set("size", sizeKeysJ(d.sizeBySpeed)); e.set("sizeBySpeed", o); }
    if (d.noise.strength > 0) {
        Json o = Json::object();
        o.set("strength", numJ(d.noise.strength)); o.set("frequency", numJ(d.noise.frequency));
        if (d.noise.scroll) o.set("scroll", numJ(d.noise.scroll));
        e.set("noise", o);
    }
    if (d.cycles != base.cycles) e.set("cycles", numJ(d.cycles));
    if (!(d.startFrame == base.startFrame)) e.set("startFrame", rangeJ(d.startFrame));
    if (d.arc != base.arc) e.set("arc", numJ(d.arc));
    if (d.thickness != base.thickness) e.set("thickness", numJ(d.thickness));
    if (d.radial != base.radial) e.set("radial", numJ(d.radial));
    if (d.hidden) e.set("hidden", Json::fromBool(true));
    if (d.shape == FxShape::Box || d.box.x != 1 || d.box.y != 1 || d.box.z != 1) e.set("box", vecJ(d.box));
    if (d.collision.enabled) {
        Json o = Json::object();
        if (d.collision.height) o.set("height", numJ(d.collision.height));
        o.set("bounce", numJ(d.collision.bounce));
        if (d.collision.dampen) o.set("dampen", numJ(d.collision.dampen));
        if (d.collision.lifeLoss) o.set("lifeLoss", numJ(d.collision.lifeLoss));
        if (d.collision.minKillSpeed) o.set("minKillSpeed", numJ(d.collision.minKillSpeed));
        e.set("collision", o);
    }
    if (!d.subEmitters.empty()) {
        Json a = Json::array();
        for (auto& se : d.subEmitters) {
            Json o = Json::object();
            o.set("on", Json::string(kSubNames[size_t(se.on)]));
            o.set("effect", Json::string(se.effect));
            if (se.chance != 1) o.set("chance", numJ(se.chance));
            if (se.inheritColor) o.set("inheritColor", Json::fromBool(true));
            a.push(o);
        }
        e.set("subEmitters", a);
    }
    if (d.trail.enabled) {
        const FxTrail tb;
        const FxTrail& t = d.trail;
        Json o = Json::object();
        o.set("time", numJ(t.time));
        o.set("width", sizeKeysJ(t.width));
        o.set("color", colorKeysJ(t.color));
        if (t.texture != tb.texture) o.set("texture", Json::string(t.texture));
        o.set("blend", Json::string(kBlendNames[size_t(t.blend)]));
        if (!t.inheritColor) o.set("inheritColor", Json::fromBool(false));
        if (!t.sizeAffectsWidth) o.set("sizeAffectsWidth", Json::fromBool(false));
        if (t.minQuality != tb.minQuality) o.set("minQuality", Json::string(kQualityChoice[size_t(t.minQuality)]));
        e.set("trail", o);
    }
    if (d.light.enabled) {
        const FxLight lb;
        const FxLight& l = d.light;
        Json o = Json::object();
        o.set("color", Json::string(colorToHex(l.color).substr(0, 7)));
        o.set("radius", numJ(l.radius));
        o.set("intensity", numJ(l.intensity));
        if (l.ratio != lb.ratio) o.set("ratio", numJ(l.ratio));
        if (l.max != lb.max) o.set("max", Json::number(int64_t(l.max)));
        if (!l.useParticleColor) o.set("useParticleColor", Json::fromBool(false));
        if (l.sizeAffectsRadius) o.set("sizeAffectsRadius", Json::fromBool(true));
        if (!l.alphaAffectsIntensity) o.set("alphaAffectsIntensity", Json::fromBool(false));
        e.set("light", o);
    }
}

} // namespace

bool FxTrail::operator==(const FxTrail& o) const {
    auto ck = [](const std::vector<FxColorKey>& a, const std::vector<FxColorKey>& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (a[i].t != b[i].t || a[i].rgba.x != b[i].rgba.x || a[i].rgba.y != b[i].rgba.y || a[i].rgba.z != b[i].rgba.z || a[i].rgba.w != b[i].rgba.w) return false;
        return true;
    };
    auto sk = [](const std::vector<FxSizeKey>& a, const std::vector<FxSizeKey>& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) if (a[i].t != b[i].t || a[i].k != b[i].k) return false;
        return true;
    };
    return enabled == o.enabled && time == o.time && sk(width, o.width) && ck(color, o.color) && texture == o.texture && blend == o.blend &&
           inheritColor == o.inheritColor && sizeAffectsWidth == o.sizeAffectsWidth && minQuality == o.minQuality;
}

bool parseEffect(const Json& j, const std::string& where, EffectDef& out, std::vector<std::string>& errors) {
    out = {};
    const size_t before = errors.size();
    if (!j.isObject()) { errors.push_back(where + ": expected an object with \"emitters\""); return false; }
    for (auto& [k, v] : j.members())
        if (k != "emitters" && k != "comment") errors.push_back(where + ": unknown field '" + k + "'");
    const Json& list = j["emitters"];
    if (!list.isArray() || list.size() == 0) { errors.push_back(where + ".emitters: expected a non-empty array"); return false; }
    if (list.size() > size_t(kMaxEmitters)) errors.push_back(where + ".emitters: at most " + std::to_string(kMaxEmitters) + " emitters");

    for (size_t i = 0; i < std::min(list.size(), size_t(kMaxEmitters)); ++i) {
        const Json& e = list[i];
        Reader r{e, where + ".emitters[" + std::to_string(i) + "]", errors};
        EmitterDef d;
        if (!e.isObject()) { r.fail("", "expected an object"); continue; }
        for (auto& [k, v] : e.members())
            if (std::find_if(std::begin(kKnownKeys), std::end(kKnownKeys), [&](const char* n) { return k == n; }) == std::end(kKnownKeys))
                r.fail(k, "unknown field");
        if (e.has("texture")) {
            d.texture = e["texture"].str();
            if (d.texture.empty()) r.fail("texture", "expected a name");
        }
        if (e.has("frames")) {
            const Json& f = e["frames"];
            if (!f.isArray() || f.size() != 2 || !f[size_t(0)].isNumber() || !f[size_t(1)].isNumber()) r.fail("frames", "expected [columns, rows]");
            else {
                d.frameCols = int(f[size_t(0)].integer(1));
                d.frameRows = int(f[size_t(1)].integer(1));
                if (d.frameCols < 1 || d.frameRows < 1 || d.frameCols * d.frameRows > 256) { r.fail("frames", "1..256 frames"); d.frameCols = d.frameRows = 1; }
            }
        }
        r.number("fps", d.fps, 0, 120);
        r.choice("blend", d.blend, kBlendNames);
        r.choice("align", d.align, kAlignNames);
        r.integer("burst", d.burst, 0, 2000);
        r.number("rate", d.rate, 0, 2000);
        r.number("perMeter", d.perMeter, 0, 200);
        r.number("delay", d.delay, 0, 60);
        r.number("duration", d.duration, 0, 600);
        if (e.has("loop")) d.loop = e["loop"].boolean();
        r.range("life", d.life, 0.01f, 60);
        r.range("speed", d.speed, 0, 500);
        r.range("size", d.size, 0.001f, 100);
        r.range("rotation", d.rotation, -3600, 3600);
        r.range("spin", d.spin, -36000, 36000);
        r.vec3("direction", d.direction);
        r.number("spread", d.spread, 0, 180);
        r.choice("shape", d.shape, kShapeNames);
        r.number("radius", d.radius, 0, 100);
        r.vec3("gravity", d.gravity);
        r.number("drag", d.drag, 0, 50);
        r.number("stretch", d.stretch, 0, 10);
        r.choice("minQuality", d.minQuality, kQualityChoice);

        if (e.has("sizeOverLife")) {
            const Json& s = e["sizeOverLife"];
            std::vector<FxSizeKey> keys;
            bool good = s.isArray() && s.size() >= 1;
            for (size_t k = 0; good && k < s.size(); ++k) {
                const Json& kv = s[k];
                if (!kv.isArray() || kv.size() != 2 || !kv[size_t(0)].isNumber() || !kv[size_t(1)].isNumber()) { good = false; break; }
                keys.push_back({std::clamp(num(kv[size_t(0)], 0), 0.0f, 1.0f), std::max(0.0f, num(kv[size_t(1)], 1))});
            }
            if (!good) r.fail("sizeOverLife", "expected [[t, k], ...]");
            else { std::stable_sort(keys.begin(), keys.end(), [](auto& a, auto& b) { return a.t < b.t; }); d.sizeOverLife = keys; }
        }
        if (e.has("color")) {
            const Json& c = e["color"];
            std::vector<FxColorKey> keys;
            float4 single;
            if (parseColor(c, single)) keys = {{0, single}, {1, single}};
            else if (c.isArray() && c.size() >= 1) {
                for (size_t k = 0; k < c.size(); ++k) {
                    const Json& kv = c[k];
                    float4 col;
                    if (!kv.isArray() || kv.size() != 2 || !kv[size_t(0)].isNumber() || !parseColor(kv[size_t(1)], col)) { keys.clear(); break; }
                    keys.push_back({std::clamp(num(kv[size_t(0)], 0), 0.0f, 1.0f), col});
                }
            }
            if (keys.empty()) r.fail("color", "expected \"#rrggbbaa\" or [[t, \"#rrggbbaa\"], ...]");
            else { std::stable_sort(keys.begin(), keys.end(), [](auto& a, auto& b) { return a.t < b.t; }); d.color = keys; }
        }
        parseModules(e, r, d);
        if (d.burst == 0 && d.bursts.empty() && !d.continuous()) r.fail("burst", "emitter creates no particles: set burst, bursts, rate or perMeter");
        if (d.rate > 0 && !d.loop && d.duration <= 0) r.fail("duration", "rate needs a duration (or \"loop\": true)");
        if (d.loop && !d.bursts.empty() && d.duration <= 0) r.fail("duration", "looping bursts repeat every \"duration\": set it");
        if (render::length(d.direction) < 1e-4f) d.direction = {0, 1, 0};
        out.emitters.push_back(std::move(d));
    }
    return errors.size() == before;
}

Json effectToJson(const EffectDef& def) {
    const EmitterDef base;
    Json root = Json::object();
    Json list = Json::array();
    for (auto& d : def.emitters) {
        Json e = Json::object();
        if (d.texture != base.texture) e.set("texture", Json::string(d.texture));
        if (d.frameCols != 1 || d.frameRows != 1) { Json f = Json::array(); f.push(Json::number(int64_t(d.frameCols))); f.push(Json::number(int64_t(d.frameRows))); e.set("frames", f); }
        if (d.fps != base.fps) e.set("fps", numJ(d.fps));
        e.set("blend", Json::string(kBlendNames[size_t(d.blend)]));
        if (d.align != base.align) e.set("align", Json::string(kAlignNames[size_t(d.align)]));
        if (d.burst) e.set("burst", Json::number(int64_t(d.burst)));
        if (d.rate) e.set("rate", numJ(d.rate));
        if (d.perMeter) e.set("perMeter", numJ(d.perMeter));
        if (d.delay) e.set("delay", numJ(d.delay));
        if (d.duration) e.set("duration", numJ(d.duration));
        if (d.loop) e.set("loop", Json::fromBool(true));
        e.set("life", rangeJ(d.life));
        e.set("speed", rangeJ(d.speed));
        e.set("size", rangeJ(d.size));
        if (!(d.rotation == base.rotation)) e.set("rotation", rangeJ(d.rotation));
        if (!(d.spin == base.spin)) e.set("spin", rangeJ(d.spin));
        if (d.direction.x != 0 || d.direction.y != 1 || d.direction.z != 0) e.set("direction", vecJ(d.direction));
        if (d.spread != base.spread) e.set("spread", numJ(d.spread));
        if (d.shape != base.shape) e.set("shape", Json::string(kShapeNames[size_t(d.shape)]));
        if (d.radius) e.set("radius", numJ(d.radius));
        if (d.gravity.x != 0 || d.gravity.y != 0 || d.gravity.z != 0) e.set("gravity", vecJ(d.gravity));
        if (d.drag) e.set("drag", numJ(d.drag));
        if (d.stretch) e.set("stretch", numJ(d.stretch));
        Json sz = Json::array();
        for (auto& k : d.sizeOverLife) { Json kv = Json::array(); kv.push(numJ(k.t)); kv.push(numJ(k.k)); sz.push(kv); }
        e.set("sizeOverLife", sz);
        Json col = Json::array();
        for (auto& k : d.color) { Json kv = Json::array(); kv.push(numJ(k.t)); kv.push(Json::string(colorToHex(k.rgba))); col.push(kv); }
        e.set("color", col);
        if (d.minQuality != base.minQuality) e.set("minQuality", Json::string(kQualityChoice[size_t(d.minQuality)]));
        modulesToJson(d, e);
        list.push(e);
    }
    root.set("emitters", list);
    return root;
}

} // namespace rts::fx
