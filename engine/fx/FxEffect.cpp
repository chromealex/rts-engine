#include "FxEffect.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace rts::fx {

namespace {

constexpr const char* kQualityNames[] = {"off", "low", "medium", "high"};

float num(const Json& j, float def) { return j.isNumber() ? float(j.fx().toDouble()) : def; }

// [[t, v], ...]; t clamped to 0..1, v to lo..hi (curves of values may be negative)
bool keysIn(const Json& s, std::vector<FxSizeKey>& out, float lo, float hi) {
    std::vector<FxSizeKey> keys;
    if (s.isNumber()) { float k = std::clamp(num(s, 1), lo, hi); out = {{0, k}, {1, k}}; return true; }
    if (!s.isArray() || s.size() < 1) return false;
    for (size_t k = 0; k < s.size(); ++k) {
        const Json& kv = s[k];
        if (!kv.isArray() || kv.size() != 2 || !kv[size_t(0)].isNumber() || !kv[size_t(1)].isNumber()) return false;
        keys.push_back({std::clamp(num(kv[size_t(0)], 0), 0.0f, 1.0f), std::clamp(num(kv[size_t(1)], 1), lo, hi)});
    }
    std::stable_sort(keys.begin(), keys.end(), [](auto& a, auto& b) { return a.t < b.t; });
    out = keys;
    return true;
}
bool sizeKeys(const Json& s, std::vector<FxSizeKey>& out) { return keysIn(s, out, 0, 1e6f); }

struct Reader {
    const Json& obj;
    std::string where;
    std::vector<std::string>& errors;
    bool ok = true;

    void fail(const std::string& key, const std::string& msg) {
        errors.push_back(where + (key.empty() ? "" : "." + key) + ": " + msg);
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
    void boolean(const char* key, bool& v) {
        const Json& j = obj[key];
        if (j.isNull()) return;
        if (j.type() != Json::Type::Bool) return fail(key, "expected true or false");
        v = j.boolean();
    }
    // number | [min, max] | { "curve": [[t, v], ...], "curveMin": [...] }
    void range(const char* key, FxRange& r, float lo, float hi) {
        const Json& j = obj[key];
        if (j.isNull()) return;
        char m[96];
        std::snprintf(m, sizeof m, "must be in %g..%g", double(lo), double(hi));
        if (j.isNumber()) { float x = num(j, 0); if (x < lo || x > hi) return fail(key, m); r = {x, x}; return; }
        if (j.isArray() && j.size() > 0 && j[size_t(0)].isArray()) {   // [[t, v], ...]: shorthand for { "curve": ... }
            FxRange c;
            if (!keysIn(j, c.curve, -1e9f, 1e9f)) return fail(key, "expected [[t, value], ...]");
            c.min = 1e30f; c.max = -1e30f;
            for (auto& k : c.curve) { c.min = std::min(c.min, k.k); c.max = std::max(c.max, k.k); }
            if (c.min < lo || c.max > hi) return fail(key, m);
            r = c;
            return;
        }
        if (j.isObject()) {
            for (auto& [k, v] : j.members())
                if (k != "curve" && k != "curveMin" && k != "comment") return fail(std::string(key) + "." + k, "unknown field (curve, curveMin)");
            FxRange c;
            if (!keysIn(j["curve"], c.curve, -1e9f, 1e9f) || (j.has("curveMin") && !keysIn(j["curveMin"], c.curveMin, -1e9f, 1e9f)))
                return fail(key, "expected { \"curve\": [[t, value], ...], \"curveMin\": [[t, value], ...] }");
            c.min = 1e30f; c.max = -1e30f;
            for (auto* ks : {&c.curve, &c.curveMin})
                for (auto& k : *ks) { c.min = std::min(c.min, k.k); c.max = std::max(c.max, k.k); }
            if (c.min < lo || c.max > hi) return fail(key, m);
            r = c;
            return;
        }
        if (!j.isArray() || j.size() != 2 || !j[size_t(0)].isNumber() || !j[size_t(1)].isNumber())
            return fail(key, "expected a number, [min, max] or { \"curve\": [[t, value], ...] }");
        float a = num(j[size_t(0)], 0), b = num(j[size_t(1)], 0);
        if (a > b) std::swap(a, b);
        if (a < lo || b > hi) return fail(key, m);
        r = {a, b};
    }
    void vec3(const char* key, float3& v) {
        const Json& j = obj[key];
        if (j.isNull()) return;
        if (!j.isArray() || j.size() != 3) return fail(key, "expected [x, y, z]");
        for (size_t i = 0; i < 3; ++i) if (!j[i].isNumber()) return fail(key, "expected [x, y, z]");
        v = {num(j[size_t(0)], 0), num(j[size_t(1)], 0), num(j[size_t(2)], 0)};
    }
    void vec2(const char* key, float2& v, float lo, float hi) {
        const Json& j = obj[key];
        if (j.isNull()) return;
        if (!j.isArray() || j.size() != 2 || !j[size_t(0)].isNumber() || !j[size_t(1)].isNumber()) return fail(key, "expected [a, b]");
        float a = num(j[size_t(0)], 0), b = num(j[size_t(1)], 0);
        if (a < lo || a > hi || b < lo || b > hi) return fail(key, "out of range");
        v = {a, b};
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
    void keys(const char* key, std::vector<FxSizeKey>& out, float lo, float hi) {
        const Json& j = obj[key];
        if (j.isNull()) return;
        if (!keysIn(j, out, lo, hi)) fail(key, "expected a number or [[t, value], ...]");
    }
    Reader sub(const char* key) const { return Reader{obj[key], where + "." + key, errors}; }
};

constexpr const char* kBlendNames[] = {"alpha", "additive", "premultiplied", "multiply", "subtract", "opaque"};
constexpr const char* kAlignNames[] = {"camera", "velocity", "ground", "upright", "world", "local"};
constexpr const char* kShapeNames[] = {"point", "sphere", "disc", "ring", "hemisphere", "cone", "box", "line", "donut", "mesh"};
constexpr const char* kSubNames[] = {"birth", "death", "collision", "trigger", "manual"};
constexpr const char* kSpaceNames[] = {"world", "local"};
constexpr const char* kQualityChoice[] = {"off", "low", "medium", "high"};
constexpr const char* kEmitFromNames[] = {"volume", "shell", "edge", "vertex", "triangle"};
constexpr const char* kArcNames[] = {"random", "loop", "pingpong", "spread"};
constexpr const char* kSortNames[] = {"auto", "none", "distance", "oldest", "youngest"};
constexpr const char* kTriggerNames[] = {"ignore", "kill", "callback"};
constexpr const char* kTexModeNames[] = {"stretch", "tile", "distribute"};
constexpr const char* kTrailModeNames[] = {"particle", "ribbon"};
constexpr const char* kInheritModeNames[] = {"initial", "current"};

constexpr const char* kKnownKeys[] = {
    "texture", "frames", "fps", "blend", "align", "burst", "rate", "perMeter", "delay", "duration", "loop",
    "life", "speed", "size", "rotation", "spin", "direction", "spread", "shape", "radius", "gravity", "drag",
    "stretch", "sizeOverLife", "color", "minQuality", "comment",
    // optional modules
    "aspect", "offset", "bursts", "startColor", "space", "maxParticles", "velocity", "speedLimit", "colorBySpeed",
    "sizeBySpeed", "noise", "cycles", "startFrame", "arc", "thickness", "radial", "hidden", "box", "collision", "subEmitters", "trail", "light",
    // the rest of Unity's modules
    "prewarm", "donutRadius", "length", "emitFrom", "mesh", "shapeRotation", "randomDirection", "sphericalDirection", "randomPosition",
    "alignToDirection", "arcMode", "arcSpeed", "arcSpread", "sizeY", "sizeZ", "rotation3D", "gravityScale", "colorMin", "sizeOverLifeMin",
    "sizeOverLifeY", "sizeOverLifeZ", "spin3D", "spinBySpeed", "velocityOverLife", "force", "speedLimitAxes", "limitDampen", "dragBySize",
    "dragBySpeed", "inheritVelocity", "lifeBySpeed", "externalForces", "row", "frameOverTime", "framesBySpeed", "flip", "sprites", "pivot",
    "cameraStretch", "sort", "sortOrder", "minScreenSize", "maxScreenSize", "meshes", "ringBuffer", "trigger", "flipRotation"};

bool knownKey(const std::string& k, std::initializer_list<const char*> keys) {
    for (const char* n : keys) if (k == n) return true;
    return false;
}
void unknownKeys(const Json& o, std::initializer_list<const char*> keys, const std::string& where, std::vector<std::string>& errors, bool& ok) {
    for (auto& [k, v] : o.members())
        if (!knownKey(k, keys) && k != "comment") { errors.push_back(where + "." + k + ": unknown field"); ok = false; }
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
Json keysJ(const std::vector<FxSizeKey>& keys) {
    Json a = Json::array();
    for (auto& k : keys) { Json kv = Json::array(); kv.push(numJ(k.t)); kv.push(numJ(k.k)); a.push(kv); }
    return a;
}
Json rangeJ(const FxRange& r) {
    if (r.curved()) {
        Json o = Json::object();
        o.set("curve", keysJ(r.curve));
        if (!r.curveMin.empty()) o.set("curveMin", keysJ(r.curveMin));
        return o;
    }
    if (r.min == r.max) return numJ(r.min);
    Json a = Json::array(); a.push(numJ(r.min)); a.push(numJ(r.max)); return a;
}
Json vecJ(float3 v) { Json a = Json::array(); a.push(numJ(v.x)); a.push(numJ(v.y)); a.push(numJ(v.z)); return a; }
bool nonzero(float3 v) { return v.x != 0 || v.y != 0 || v.z != 0; }
bool isZero(const FxRange& r) { return !r.curved() && r.min == 0 && r.max == 0; }

} // namespace

// ---------------------------------------------------------------------------------------------

float sampleKeys(const std::vector<FxSizeKey>& keys, float t, float def) {
    if (keys.empty()) return def;
    if (t <= keys.front().t) return keys.front().k;
    if (t >= keys.back().t) return keys.back().k;
    for (size_t i = 1; i < keys.size(); ++i)
        if (t <= keys[i].t) {
            const float span = std::max(1e-6f, keys[i].t - keys[i - 1].t);
            return keys[i - 1].k + (keys[i].k - keys[i - 1].k) * ((t - keys[i - 1].t) / span);
        }
    return keys.back().k;
}

float4 sampleColor(const std::vector<FxColorKey>& keys, float t) {
    if (keys.empty()) return {1, 1, 1, 1};
    if (t <= keys.front().t) return keys.front().rgba;
    if (t >= keys.back().t) return keys.back().rgba;
    for (size_t i = 1; i < keys.size(); ++i)
        if (t <= keys[i].t) {
            const float f = (t - keys[i - 1].t) / std::max(1e-6f, keys[i].t - keys[i - 1].t);
            const float4 a = keys[i - 1].rgba, b = keys[i].rgba;
            return {a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f, a.z + (b.z - a.z) * f, a.w + (b.w - a.w) * f};
        }
    return keys.back().rgba;
}

float FxRange::at(float t, float r) const {
    if (curve.empty()) return min + (max - min) * r;
    t = std::clamp(t, 0.0f, 1.0f);
    const float hi = sampleKeys(curve, t, 0);
    const float lo = curveMin.empty() ? hi : sampleKeys(curveMin, t, 0);
    return lo + (hi - lo) * r;
}

float FxRange::average() const {
    if (curve.empty()) return (min + max) * 0.5f;
    float s = 0;
    for (int i = 0; i < 16; ++i) s += at((float(i) + 0.5f) / 16, 0.5f);
    return s / 16;
}

const char* qualityName(FxQuality q) { return kQualityNames[std::min<size_t>(size_t(q), 3)]; }
bool qualityFromName(const std::string& s, FxQuality& out) {
    for (size_t i = 0; i < 4; ++i) if (s == kQualityNames[i]) { out = FxQuality(i); return true; }
    return false;
}

float EmitterDef::endTime() const {
    if (loop && (continuous() || !bursts.empty())) return std::numeric_limits<float>::infinity();
    float t = continuous() ? duration : 0.0f;
    for (auto& b : bursts) t = std::max(t, b.cycles > 0 ? b.time + float(b.cycles - 1) * b.interval : std::max(duration, b.time));
    return delay.max + t;
}

bool EffectDef::looping() const {
    for (auto& e : emitters) if (e.loop && (e.continuous() || !e.bursts.empty())) return true;
    return false;
}

float EffectDef::lifetime() const {
    float t = 0;
    for (auto& e : emitters) {
        float life = e.life.max;
        if (e.lifeBySpeed.enabled) for (auto& k : e.lifeBySpeed.keys) life = std::max(life, e.life.max * k.k);
        float extra = e.trail.enabled && !e.trail.dieWithParticles ? (e.trail.lifeFraction > 0 ? e.trail.lifeFraction * life : e.trail.time) : 0.0f;
        t = std::max(t, e.endTime() + life + extra);
    }
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

Json colorKeysJ(const std::vector<FxColorKey>& keys) {
    Json a = Json::array();
    for (auto& k : keys) { Json kv = Json::array(); kv.push(numJ(k.t)); kv.push(Json::string(colorToHex(k.rgba))); a.push(kv); }
    return a;
}

// An object module: unknown keys checked, failures propagated.
template <class Fn>
void module(const Json& e, Reader& r, const char* key, std::initializer_list<const char*> keys, Fn&& fn) {
    const Json& o = e[key];
    if (o.isNull()) return;
    if (!o.isObject()) return r.fail(key, "expected an object");
    Reader mr = r.sub(key);
    unknownKeys(o, keys, mr.where, r.errors, r.ok);
    fn(o, mr);
    if (!mr.ok) r.ok = false;
}

void parseBySpeed(const Json& e, Reader& r, const char* key, const char* valueKey, FxBySpeed& out, float lo, float hi) {
    module(e, r, key, {"speed", valueKey}, [&](const Json& o, Reader& mr) {
        out.enabled = true;
        mr.range("speed", out.speed, 0, 1e5f);
        if (!keysIn(o[valueKey], out.keys, lo, hi)) mr.fail(valueKey, "expected [[t, value], ...]");
    });
}

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
                unknownKeys(x, {"time", "count", "cycles", "interval", "chance"}, br.where, r.errors, r.ok);
                br.number("time", fb.time, 0, 600);
                br.range("count", fb.count, 0, 2000);
                br.integer("cycles", fb.cycles, 0, 1000);
                br.number("interval", fb.interval, 0, 600);
                br.number("chance", fb.chance, 0, 1);
                if (fb.cycles == 0 && fb.interval <= 0) br.fail("interval", "endless cycles (0) need an interval > 0");
                if (!br.ok) r.ok = false;
            } else good = false;
            if (fb.time < 0 || fb.count.max > 2000) good = false;
            if (good) d.bursts.push_back(fb);
        }
        if (!good) r.fail("bursts", "expected [[time, count], ...] or [{ \"time\", \"count\", \"cycles\", \"interval\", \"chance\" }]");
    }
    if (e.has("startColor")) {
        const Json& c = e["startColor"];
        float4 a, b;
        if (parseColor(c, a)) d.startColor = {{0, a}};
        else if (c.isArray() && c.size() == 2 && parseColor(c[size_t(0)], a) && parseColor(c[size_t(1)], b)) { d.startColor = {{0, a}, {1, b}}; d.startColorPair = true; }
        else if (c.isObject()) {
            module(e, r, "startColor", {"gradient", "gradientMin", "overTime"}, [&](const Json& o, Reader& mr) {
                if (!colorKeys(o["gradient"], d.startColor)) mr.fail("gradient", "expected [[t, \"#rrggbbaa\"], ...]");
                if (o.has("gradientMin") && !colorKeys(o["gradientMin"], d.startColorMin)) mr.fail("gradientMin", "expected [[t, \"#rrggbbaa\"], ...]");
                mr.boolean("overTime", d.startColorOverTime);
            });
        } else if (!colorKeys(c, d.startColor)) r.fail("startColor", "expected \"#rrggbbaa\", [\"#a\", \"#b\"], [[t, \"#rrggbbaa\"], ...] or { \"gradient\" }");
    }
    if (e.has("space")) { int sp = 0; r.choice("space", sp, kSpaceNames); d.localSpace = sp == 1; }
    r.integer("maxParticles", d.maxParticles, 0, 100000);
    r.vec3("velocity", d.velocity);
    r.range("speedLimit", d.speedLimit, 0, 1000);
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
    module(e, r, "noise", {"strength", "frequency", "scroll", "axes", "octaves", "octaveMultiplier", "octaveScale", "damping", "position", "rotation", "size", "remap"},
           [&](const Json&, Reader& nr) {
        FxNoise& n = d.noise;
        nr.range("strength", n.strength, 0, 100);
        nr.number("frequency", n.frequency, 0.001f, 100);
        nr.number("scroll", n.scroll, -100, 100);
        nr.vec3("axes", n.axes);
        nr.integer("octaves", n.octaves, 1, 4);
        nr.number("octaveMultiplier", n.octaveMultiplier, 0, 1);
        nr.number("octaveScale", n.octaveScale, 1, 4);
        nr.boolean("damping", n.damping);
        nr.number("position", n.position, 0, 10);
        nr.number("rotation", n.rotation, -3600, 3600);
        nr.number("size", n.size, 0, 10);
        nr.keys("remap", n.remap, -1, 1);
    });
    r.number("cycles", d.cycles, 0.01f, 1000);
    r.range("startFrame", d.startFrame, 0, 255);
    r.number("arc", d.arc, 0, 360);
    r.number("thickness", d.thickness, 0, 1);
    r.number("radial", d.radial, -1, 1);
    r.boolean("hidden", d.hidden);
    r.vec3("box", d.box);
    if (e.has("collision")) {
        const Json& o = e["collision"];
        if (o.type() == Json::Type::Bool) d.collision.enabled = o.boolean();
        else module(e, r, "collision", {"height", "bounce", "dampen", "lifeLoss", "minKillSpeed", "maxKillSpeed", "ground", "planes", "radius"},
                    [&](const Json& c, Reader& cr) {
            FxCollision& col = d.collision;
            col.enabled = true;
            cr.number("height", col.height, -100, 100);
            cr.number("bounce", col.bounce, 0, 2);
            cr.number("dampen", col.dampen, 0, 1);
            cr.number("lifeLoss", col.lifeLoss, 0, 1);
            cr.number("minKillSpeed", col.minKillSpeed, 0, 1000);
            cr.number("maxKillSpeed", col.maxKillSpeed, 0, 100000);
            cr.boolean("ground", col.ground);
            cr.number("radius", col.radius, 0, 10);
            if (c.has("planes")) {
                const Json& l = c["planes"];
                bool good = l.isArray() && l.size() <= 6;
                for (size_t i = 0; good && i < l.size(); ++i) {
                    Reader pr{l[i], cr.where + ".planes[" + std::to_string(i) + "]", r.errors};
                    if (!l[i].isObject()) { good = false; break; }
                    unknownKeys(l[i], {"point", "normal"}, pr.where, r.errors, r.ok);
                    FxPlane pl;
                    pr.vec3("point", pl.point);
                    pr.vec3("normal", pl.normal);
                    if (render::length(pl.normal) < 1e-4f) pr.fail("normal", "must not be zero");
                    else pl.normal = render::normalize(pl.normal);
                    if (!pr.ok) cr.ok = false;
                    col.planes.push_back(pl);
                }
                if (!good) cr.fail("planes", "expected up to 6 [{ \"point\": [x, y, z], \"normal\": [x, y, z] }]");
            }
        });
    }
    if (e.has("subEmitters")) {
        const Json& l = e["subEmitters"];
        if (!l.isArray()) r.fail("subEmitters", "expected [{ \"on\": \"death\", \"effect\": \"name\" }, ...]");
        for (size_t i = 0; l.isArray() && i < l.size(); ++i) {
            const Json& o = l[i];
            Reader sr{o, r.where + ".subEmitters[" + std::to_string(i) + "]", r.errors};
            if (!o.isObject()) { sr.fail("", "expected an object"); r.ok = false; continue; }
            unknownKeys(o, {"on", "effect", "chance", "inheritColor", "inherit"}, sr.where, r.errors, r.ok);
            FxSubEmitter se;
            sr.choice("on", se.on, kSubNames);
            se.effect = o["effect"].str();
            if (se.effect.empty()) sr.fail("effect", "expected an effect name (fx/<name>.json)");
            sr.number("chance", se.chance, 0, 1);
            se.inheritColor = o.getBool("inheritColor");
            if (o.has("inherit")) {
                const Json& in = o["inherit"];
                if (!in.isArray()) sr.fail("inherit", "expected [\"color\", \"size\", \"rotation\", \"lifetime\", \"duration\"]");
                for (size_t k = 0; in.isArray() && k < in.size(); ++k) {
                    const std::string s = in[k].str();
                    if (s == "color") se.inheritColor = true;
                    else if (s == "size") se.inheritSize = true;
                    else if (s == "rotation") se.inheritRotation = true;
                    else if (s == "lifetime") se.inheritLifetime = true;
                    else if (s == "duration") se.inheritDuration = true;
                    else sr.fail("inherit", "unknown '" + s + "' (color, size, rotation, lifetime, duration)");
                }
            }
            if (!sr.ok) r.ok = false;
            else if (d.subEmitters.size() < size_t(kMaxSubEmitters)) d.subEmitters.push_back(se);
            else { r.fail("subEmitters", "at most " + std::to_string(kMaxSubEmitters) + " sub-emitters per emitter"); break; }
        }
    }
    module(e, r, "trail", {"time", "width", "color", "texture", "blend", "inheritColor", "sizeAffectsWidth", "minQuality", "mode", "ribbons",
                           "ratio", "lifeColor", "lifeFraction", "minDistance", "textureMode", "dieWithParticles", "sizeAffectsLifetime"},
           [&](const Json& o, Reader& tr) {
        FxTrail& t = d.trail;
        t.enabled = true;
        tr.number("time", t.time, 0.01f, 10);
        tr.number("lifeFraction", t.lifeFraction, 0, 1);
        if (o.has("width") && !sizeKeys(o["width"], t.width)) tr.fail("width", "expected a number or [[t, k], ...]");
        if (o.has("color") && !colorKeys(o["color"], t.color)) tr.fail("color", "expected \"#rrggbbaa\" or [[t, \"#rrggbbaa\"], ...]");
        if (o.has("lifeColor") && !colorKeys(o["lifeColor"], t.lifeColor)) tr.fail("lifeColor", "expected \"#rrggbbaa\" or [[t, \"#rrggbbaa\"], ...]");
        if (o.has("texture")) { t.texture = o["texture"].str(); if (t.texture.empty()) tr.fail("texture", "expected a name"); }
        tr.choice("blend", t.blend, kBlendNames);
        tr.boolean("inheritColor", t.inheritColor);
        tr.boolean("sizeAffectsWidth", t.sizeAffectsWidth);
        tr.boolean("sizeAffectsLifetime", t.sizeAffectsLifetime);
        tr.choice("minQuality", t.minQuality, kQualityChoice);
        int mode = 0;
        tr.choice("mode", mode, kTrailModeNames);
        t.ribbon = mode == 1;
        tr.integer("ribbons", t.ribbons, 1, 16);
        tr.number("ratio", t.ratio, 0, 1);
        tr.number("minDistance", t.minDistance, 0, 100);
        tr.choice("textureMode", t.textureMode, kTexModeNames);
        tr.boolean("dieWithParticles", t.dieWithParticles);
    });
    module(e, r, "light", {"color", "radius", "intensity", "ratio", "max", "useParticleColor", "sizeAffectsRadius", "alphaAffectsIntensity",
                           "radiusOverLife", "intensityOverLife"},
           [&](const Json& o, Reader& lr) {
        FxLight& l = d.light;
        l.enabled = true;
        if (o.has("color") && !parseColor(o["color"], l.color)) lr.fail("color", "expected \"#rrggbb\"");
        lr.number("radius", l.radius, 0.01f, 100);
        lr.number("intensity", l.intensity, 0, 20);
        lr.number("ratio", l.ratio, 0, 1);
        lr.integer("max", l.max, 0, 64);
        lr.boolean("useParticleColor", l.useParticleColor);
        lr.boolean("sizeAffectsRadius", l.sizeAffectsRadius);
        lr.boolean("alphaAffectsIntensity", l.alphaAffectsIntensity);
        lr.keys("radiusOverLife", l.radiusOverLife, 0, 100);
        lr.keys("intensityOverLife", l.intensityOverLife, 0, 100);
    });

    // ---- the rest of Unity's modules
    r.boolean("prewarm", d.prewarm);
    r.number("donutRadius", d.donutRadius, 0, 100);
    r.number("length", d.length, 0, 1000);
    r.choice("emitFrom", d.emitFrom, kEmitFromNames);
    if (e.has("mesh")) { d.mesh = e["mesh"].str(); if (d.mesh.empty()) r.fail("mesh", "expected \"builtin:cube\" or a model path"); }
    r.vec3("shapeRotation", d.shapeRotation);
    r.number("randomDirection", d.randomDirection, 0, 1);
    r.number("sphericalDirection", d.sphericalDirection, 0, 1);
    r.number("randomPosition", d.randomPosition, 0, 100);
    r.boolean("alignToDirection", d.alignToDirection);
    r.number("flipRotation", d.flipRotation, 0, 1);
    r.choice("arcMode", d.arcMode, kArcNames);
    r.number("arcSpeed", d.arcSpeed, -100, 100);
    r.number("arcSpread", d.arcSpread, 0, 1);
    if (e.has("sizeY")) { d.size3D = true; r.range("sizeY", d.sizeY, 0.001f, 100); }
    if (e.has("sizeZ")) { d.size3D = true; r.range("sizeZ", d.sizeZ, 0.001f, 100); }
    module(e, r, "rotation3D", {"x", "y"}, [&](const Json&, Reader& mr) {
        d.rotation3D = true;
        mr.range("x", d.rotationX, -3600, 3600);
        mr.range("y", d.rotationY, -3600, 3600);
    });
    r.range("gravityScale", d.gravityScale, -100, 100);
    if (e.has("colorMin") && !colorKeys(e["colorMin"], d.colorMin)) r.fail("colorMin", "expected [[t, \"#rrggbbaa\"], ...]");
    r.keys("sizeOverLifeMin", d.sizeOverLifeMin, 0, 1e6f);
    r.keys("sizeOverLifeY", d.sizeOverLifeY, 0, 1e6f);
    r.keys("sizeOverLifeZ", d.sizeOverLifeZ, 0, 1e6f);
    module(e, r, "spin3D", {"x", "y"}, [&](const Json&, Reader& mr) {
        d.spin3D = true;
        mr.range("x", d.spinX, -36000, 36000);
        mr.range("y", d.spinY, -36000, 36000);
    });
    parseBySpeed(e, r, "spinBySpeed", "spin", d.spinBySpeed, -36000, 36000);
    module(e, r, "velocityOverLife", {"x", "y", "z", "space", "orbital", "orbitalX", "orbitalY", "orbitalZ", "orbitalOffset", "radial", "speedModifier"},
           [&](const Json& o, Reader& mr) {
        FxVelocityOverLife& v = d.velocityOverLife;
        v.enabled = true;
        mr.range("x", v.x, -1000, 1000);
        mr.range("y", v.y, -1000, 1000);
        mr.range("z", v.z, -1000, 1000);
        int sp = 1; mr.choice("space", sp, kSpaceNames); v.world = sp == 0;
        if (o.has("orbital")) { float3 orb; mr.vec3("orbital", orb); v.orbitalX = {orb.x, orb.x}; v.orbitalY = {orb.y, orb.y}; v.orbitalZ = {orb.z, orb.z}; }
        mr.range("orbitalX", v.orbitalX, -36000, 36000);
        mr.range("orbitalY", v.orbitalY, -36000, 36000);
        mr.range("orbitalZ", v.orbitalZ, -36000, 36000);
        mr.vec3("orbitalOffset", v.orbitalOffset);
        mr.range("radial", v.radial, -1000, 1000);
        mr.range("speedModifier", v.speedModifier, -100, 100);
    });
    module(e, r, "force", {"x", "y", "z", "space", "randomize"}, [&](const Json&, Reader& mr) {
        FxForce& f = d.force;
        f.enabled = true;
        mr.range("x", f.x, -1000, 1000);
        mr.range("y", f.y, -1000, 1000);
        mr.range("z", f.z, -1000, 1000);
        int sp = 0; mr.choice("space", sp, kSpaceNames); f.world = sp == 0;
        mr.boolean("randomize", f.randomize);
    });
    module(e, r, "speedLimitAxes", {"x", "y", "z", "space"}, [&](const Json&, Reader& mr) {
        FxLimitAxes& l = d.speedLimitAxes;
        l.enabled = true;
        l.x = l.y = l.z = {1e6f, 1e6f};
        mr.range("x", l.x, 0, 1e6f);
        mr.range("y", l.y, 0, 1e6f);
        mr.range("z", l.z, 0, 1e6f);
        int sp = 1; mr.choice("space", sp, kSpaceNames); l.world = sp == 0;
    });
    r.number("limitDampen", d.limitDampen, 0, 1);
    r.boolean("dragBySize", d.dragBySize);
    r.boolean("dragBySpeed", d.dragBySpeed);
    module(e, r, "inheritVelocity", {"mode", "amount"}, [&](const Json&, Reader& mr) {
        d.inheritVelocity.enabled = true;
        int m = 0; mr.choice("mode", m, kInheritModeNames); d.inheritVelocity.current = m == 1;
        mr.range("amount", d.inheritVelocity.amount, -10, 10);
    });
    parseBySpeed(e, r, "lifeBySpeed", "factor", d.lifeBySpeed, 0, 100);
    r.number("externalForces", d.externalForces, 0, 100);
    if (e.has("row")) {
        const Json& rw = e["row"];
        if (rw.isString() && rw.str() == "random") d.row = -2;
        else if (rw.isNumber()) { d.row = int(rw.integer(0)); if (d.row < 0 || d.row > 255) r.fail("row", "expected 0..255 or \"random\""); }
        else r.fail("row", "expected a row index or \"random\"");
    }
    r.range("frameOverTime", d.frameOverTime, 0, 1);
    r.range("framesBySpeed", d.framesBySpeed, 0, 1e5f);
    r.vec2("flip", d.flip, 0, 1);
    if (e.has("sprites")) {
        const Json& l = e["sprites"];
        if (!l.isArray() || l.size() == 0 || l.size() > 64) r.fail("sprites", "expected 1..64 sprite names");
        for (size_t i = 0; l.isArray() && i < l.size() && i < 64; ++i) {
            if (!l[i].isString() || l[i].str().empty()) { r.fail("sprites", "expected sprite names"); break; }
            d.sprites.push_back(l[i].str());
        }
    }
    r.vec3("pivot", d.pivot);
    r.number("cameraStretch", d.cameraStretch, -100, 100);
    r.choice("sort", d.sort, kSortNames);
    r.integer("sortOrder", d.sortOrder, -100, 100);
    r.number("minScreenSize", d.minScreenSize, 0, 1);
    r.number("maxScreenSize", d.maxScreenSize, 0, 2);
    if (e.has("meshes")) {
        const Json& l = e["meshes"];
        if (!l.isArray() || l.size() == 0 || l.size() > 4) r.fail("meshes", "expected 1..4 meshes (\"builtin:cube\" or model paths)");
        for (size_t i = 0; l.isArray() && i < l.size() && i < 4; ++i) {
            if (!l[i].isString() || l[i].str().empty()) { r.fail("meshes", "expected mesh names"); break; }
            d.meshes.push_back(l[i].str());
        }
    }
    r.boolean("ringBuffer", d.ringBuffer);
    module(e, r, "trigger", {"volumes", "inside", "outside", "enter", "exit", "radius"}, [&](const Json& o, Reader& mr) {
        FxTriggerDef& t = d.trigger;
        t.enabled = true;
        const Json& l = o["volumes"];
        if (!l.isArray() || l.size() == 0 || l.size() > 8) mr.fail("volumes", "expected 1..8 [{ \"center\": [x, y, z], \"radius\": r }]");
        for (size_t i = 0; l.isArray() && i < l.size() && i < 8; ++i) {
            Reader vr{l[i], mr.where + ".volumes[" + std::to_string(i) + "]", r.errors};
            if (!l[i].isObject()) { vr.fail("", "expected an object"); mr.ok = false; continue; }
            unknownKeys(l[i], {"center", "radius"}, vr.where, r.errors, r.ok);
            FxSphere s;
            vr.vec3("center", s.center);
            vr.number("radius", s.radius, 0, 1000);
            if (!vr.ok) mr.ok = false;
            t.volumes.push_back(s);
        }
        mr.choice("inside", t.inside, kTriggerNames);
        mr.choice("outside", t.outside, kTriggerNames);
        mr.choice("enter", t.enter, kTriggerNames);
        mr.choice("exit", t.exit, kTriggerNames);
        mr.number("radius", t.radius, 0, 10);
    });
}

void bySpeedJ(Json& e, const char* key, const char* valueKey, const FxBySpeed& b) {
    if (!b.enabled) return;
    Json o = Json::object();
    o.set("speed", rangeJ(b.speed));
    o.set(valueKey, keysJ(b.keys));
    e.set(key, o);
}

void modulesToJson(const EmitterDef& d, Json& e) {
    const EmitterDef base;
    if (!(d.aspect == base.aspect)) e.set("aspect", rangeJ(d.aspect));
    if (nonzero(d.offset)) e.set("offset", vecJ(d.offset));
    if (!d.bursts.empty()) {
        Json a = Json::array();
        for (auto& b : d.bursts) {
            if (b.cycles == 1 && b.count.constant() && b.chance == 1) { Json kv = Json::array(); kv.push(numJ(b.time)); kv.push(numJ(b.count.min)); a.push(kv); continue; }
            Json o = Json::object();
            o.set("time", numJ(b.time));
            o.set("count", rangeJ(b.count));
            if (b.cycles != 1) { o.set("cycles", Json::number(int64_t(b.cycles))); o.set("interval", numJ(b.interval)); }
            if (b.chance != 1) o.set("chance", numJ(b.chance));
            a.push(o);
        }
        e.set("bursts", a);
    }
    if (!d.startColorMin.empty() || d.startColorOverTime) {
        Json o = Json::object();
        o.set("gradient", colorKeysJ(d.startColor));
        if (!d.startColorMin.empty()) o.set("gradientMin", colorKeysJ(d.startColorMin));
        if (d.startColorOverTime) o.set("overTime", Json::fromBool(true));
        e.set("startColor", o);
    } else if (d.startColor.size() == 1) e.set("startColor", Json::string(colorToHex(d.startColor[0].rgba)));
    else if (d.startColorPair && d.startColor.size() == 2) {
        Json a = Json::array(); a.push(Json::string(colorToHex(d.startColor[0].rgba))); a.push(Json::string(colorToHex(d.startColor[1].rgba)));
        e.set("startColor", a);
    } else if (!d.startColor.empty()) e.set("startColor", colorKeysJ(d.startColor));
    if (d.localSpace) e.set("space", Json::string("local"));
    if (d.maxParticles) e.set("maxParticles", Json::number(int64_t(d.maxParticles)));
    if (nonzero(d.velocity)) e.set("velocity", vecJ(d.velocity));
    if (!isZero(d.speedLimit)) e.set("speedLimit", rangeJ(d.speedLimit));
    if (!d.colorBySpeed.empty()) { Json o = Json::object(); o.set("speed", rangeJ(d.colorSpeed)); o.set("color", colorKeysJ(d.colorBySpeed)); e.set("colorBySpeed", o); }
    if (!d.sizeBySpeed.empty()) { Json o = Json::object(); o.set("speed", rangeJ(d.sizeSpeed)); o.set("size", keysJ(d.sizeBySpeed)); e.set("sizeBySpeed", o); }
    if (d.noise.enabled()) {
        const FxNoise nb;
        const FxNoise& n = d.noise;
        Json o = Json::object();
        o.set("strength", rangeJ(n.strength)); o.set("frequency", numJ(n.frequency));
        if (n.scroll) o.set("scroll", numJ(n.scroll));
        if (n.axes.x != 1 || n.axes.y != 1 || n.axes.z != 1) o.set("axes", vecJ(n.axes));
        if (n.octaves != 1) { o.set("octaves", Json::number(int64_t(n.octaves))); o.set("octaveMultiplier", numJ(n.octaveMultiplier)); o.set("octaveScale", numJ(n.octaveScale)); }
        if (n.damping) o.set("damping", Json::fromBool(true));
        if (n.position != nb.position) o.set("position", numJ(n.position));
        if (n.rotation) o.set("rotation", numJ(n.rotation));
        if (n.size) o.set("size", numJ(n.size));
        if (!n.remap.empty()) o.set("remap", keysJ(n.remap));
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
        const FxCollision& c = d.collision;
        Json o = Json::object();
        if (c.height) o.set("height", numJ(c.height));
        o.set("bounce", numJ(c.bounce));
        if (c.dampen) o.set("dampen", numJ(c.dampen));
        if (c.lifeLoss) o.set("lifeLoss", numJ(c.lifeLoss));
        if (c.minKillSpeed) o.set("minKillSpeed", numJ(c.minKillSpeed));
        if (c.maxKillSpeed) o.set("maxKillSpeed", numJ(c.maxKillSpeed));
        if (!c.ground) o.set("ground", Json::fromBool(false));
        if (c.radius) o.set("radius", numJ(c.radius));
        if (!c.planes.empty()) {
            Json a = Json::array();
            for (auto& p : c.planes) { Json po = Json::object(); po.set("point", vecJ(p.point)); po.set("normal", vecJ(p.normal)); a.push(po); }
            o.set("planes", a);
        }
        e.set("collision", o);
    }
    if (!d.subEmitters.empty()) {
        Json a = Json::array();
        for (auto& se : d.subEmitters) {
            Json o = Json::object();
            o.set("on", Json::string(kSubNames[size_t(se.on)]));
            o.set("effect", Json::string(se.effect));
            if (se.chance != 1) o.set("chance", numJ(se.chance));
            if (se.inheritSize || se.inheritRotation || se.inheritLifetime || se.inheritDuration) {
                Json in = Json::array();
                if (se.inheritColor) in.push(Json::string("color"));
                if (se.inheritSize) in.push(Json::string("size"));
                if (se.inheritRotation) in.push(Json::string("rotation"));
                if (se.inheritLifetime) in.push(Json::string("lifetime"));
                if (se.inheritDuration) in.push(Json::string("duration"));
                o.set("inherit", in);
            } else if (se.inheritColor) o.set("inheritColor", Json::fromBool(true));
            a.push(o);
        }
        e.set("subEmitters", a);
    }
    if (d.trail.enabled) {
        const FxTrail tb;
        const FxTrail& t = d.trail;
        Json o = Json::object();
        if (t.ribbon) { o.set("mode", Json::string("ribbon")); if (t.ribbons != 1) o.set("ribbons", Json::number(int64_t(t.ribbons))); }
        o.set("time", numJ(t.time));
        if (t.lifeFraction > 0) o.set("lifeFraction", numJ(t.lifeFraction));
        o.set("width", keysJ(t.width));
        o.set("color", colorKeysJ(t.color));
        if (!t.lifeColor.empty()) o.set("lifeColor", colorKeysJ(t.lifeColor));
        if (t.texture != tb.texture) o.set("texture", Json::string(t.texture));
        o.set("blend", Json::string(kBlendNames[size_t(t.blend)]));
        if (!t.inheritColor) o.set("inheritColor", Json::fromBool(false));
        if (!t.sizeAffectsWidth) o.set("sizeAffectsWidth", Json::fromBool(false));
        if (t.sizeAffectsLifetime) o.set("sizeAffectsLifetime", Json::fromBool(true));
        if (t.ratio != 1) o.set("ratio", numJ(t.ratio));
        if (t.minDistance) o.set("minDistance", numJ(t.minDistance));
        if (t.textureMode != tb.textureMode) o.set("textureMode", Json::string(kTexModeNames[size_t(t.textureMode)]));
        if (!t.dieWithParticles) o.set("dieWithParticles", Json::fromBool(false));
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
        if (!l.radiusOverLife.empty()) o.set("radiusOverLife", keysJ(l.radiusOverLife));
        if (!l.intensityOverLife.empty()) o.set("intensityOverLife", keysJ(l.intensityOverLife));
        e.set("light", o);
    }

    // ---- the rest of Unity's modules
    if (d.prewarm) e.set("prewarm", Json::fromBool(true));
    if (d.shape == FxShape::Donut || d.donutRadius != base.donutRadius) e.set("donutRadius", numJ(d.donutRadius));
    if (d.length) e.set("length", numJ(d.length));
    if (d.emitFrom != base.emitFrom) e.set("emitFrom", Json::string(kEmitFromNames[size_t(d.emitFrom)]));
    if (!d.mesh.empty()) e.set("mesh", Json::string(d.mesh));
    if (nonzero(d.shapeRotation)) e.set("shapeRotation", vecJ(d.shapeRotation));
    if (d.randomDirection) e.set("randomDirection", numJ(d.randomDirection));
    if (d.sphericalDirection) e.set("sphericalDirection", numJ(d.sphericalDirection));
    if (d.randomPosition) e.set("randomPosition", numJ(d.randomPosition));
    if (d.alignToDirection) e.set("alignToDirection", Json::fromBool(true));
    if (d.flipRotation) e.set("flipRotation", numJ(d.flipRotation));
    if (d.arcMode != base.arcMode) {
        e.set("arcMode", Json::string(kArcNames[size_t(d.arcMode)]));
        e.set("arcSpeed", numJ(d.arcSpeed));
        if (d.arcSpread) e.set("arcSpread", numJ(d.arcSpread));
    }
    if (d.size3D) { e.set("sizeY", rangeJ(d.sizeY)); if (!(d.sizeZ == base.sizeZ)) e.set("sizeZ", rangeJ(d.sizeZ)); }
    if (d.rotation3D) { Json o = Json::object(); o.set("x", rangeJ(d.rotationX)); o.set("y", rangeJ(d.rotationY)); e.set("rotation3D", o); }
    if (!(d.gravityScale == base.gravityScale)) e.set("gravityScale", rangeJ(d.gravityScale));
    if (!d.colorMin.empty()) e.set("colorMin", colorKeysJ(d.colorMin));
    if (!d.sizeOverLifeMin.empty()) e.set("sizeOverLifeMin", keysJ(d.sizeOverLifeMin));
    if (!d.sizeOverLifeY.empty()) e.set("sizeOverLifeY", keysJ(d.sizeOverLifeY));
    if (!d.sizeOverLifeZ.empty()) e.set("sizeOverLifeZ", keysJ(d.sizeOverLifeZ));
    if (d.spin3D) { Json o = Json::object(); o.set("x", rangeJ(d.spinX)); o.set("y", rangeJ(d.spinY)); e.set("spin3D", o); }
    bySpeedJ(e, "spinBySpeed", "spin", d.spinBySpeed);
    if (d.velocityOverLife.enabled) {
        const FxVelocityOverLife& v = d.velocityOverLife;
        Json o = Json::object();
        if (!isZero(v.x)) o.set("x", rangeJ(v.x));
        if (!isZero(v.y)) o.set("y", rangeJ(v.y));
        if (!isZero(v.z)) o.set("z", rangeJ(v.z));
        if (v.world) o.set("space", Json::string("world"));
        if (!isZero(v.orbitalX)) o.set("orbitalX", rangeJ(v.orbitalX));
        if (!isZero(v.orbitalY)) o.set("orbitalY", rangeJ(v.orbitalY));
        if (!isZero(v.orbitalZ)) o.set("orbitalZ", rangeJ(v.orbitalZ));
        if (nonzero(v.orbitalOffset)) o.set("orbitalOffset", vecJ(v.orbitalOffset));
        if (!isZero(v.radial)) o.set("radial", rangeJ(v.radial));
        if (!(v.speedModifier == FxRange{1, 1})) o.set("speedModifier", rangeJ(v.speedModifier));
        e.set("velocityOverLife", o);
    }
    if (d.force.enabled) {
        const FxForce& f = d.force;
        Json o = Json::object();
        if (!isZero(f.x)) o.set("x", rangeJ(f.x));
        if (!isZero(f.y)) o.set("y", rangeJ(f.y));
        if (!isZero(f.z)) o.set("z", rangeJ(f.z));
        if (!f.world) o.set("space", Json::string("local"));
        if (f.randomize) o.set("randomize", Json::fromBool(true));
        e.set("force", o);
    }
    if (d.speedLimitAxes.enabled) {
        const FxLimitAxes& l = d.speedLimitAxes;
        Json o = Json::object();
        o.set("x", rangeJ(l.x)); o.set("y", rangeJ(l.y)); o.set("z", rangeJ(l.z));
        if (l.world) o.set("space", Json::string("world"));
        e.set("speedLimitAxes", o);
    }
    if (d.limitDampen != base.limitDampen) e.set("limitDampen", numJ(d.limitDampen));
    if (d.dragBySize) e.set("dragBySize", Json::fromBool(true));
    if (d.dragBySpeed) e.set("dragBySpeed", Json::fromBool(true));
    if (d.inheritVelocity.enabled) {
        Json o = Json::object();
        o.set("mode", Json::string(d.inheritVelocity.current ? "current" : "initial"));
        o.set("amount", rangeJ(d.inheritVelocity.amount));
        e.set("inheritVelocity", o);
    }
    bySpeedJ(e, "lifeBySpeed", "factor", d.lifeBySpeed);
    if (d.externalForces) e.set("externalForces", numJ(d.externalForces));
    if (d.row == -2) e.set("row", Json::string("random"));
    else if (d.row >= 0) e.set("row", Json::number(int64_t(d.row)));
    if (!isZero(d.frameOverTime)) e.set("frameOverTime", rangeJ(d.frameOverTime));
    if (d.framesBySpeed.max > 0) e.set("framesBySpeed", rangeJ(d.framesBySpeed));
    if (d.flip.x || d.flip.y) { Json a = Json::array(); a.push(numJ(d.flip.x)); a.push(numJ(d.flip.y)); e.set("flip", a); }
    if (!d.sprites.empty()) { Json a = Json::array(); for (auto& s : d.sprites) a.push(Json::string(s)); e.set("sprites", a); }
    if (nonzero(d.pivot)) e.set("pivot", vecJ(d.pivot));
    if (d.cameraStretch) e.set("cameraStretch", numJ(d.cameraStretch));
    if (d.sort != base.sort) e.set("sort", Json::string(kSortNames[size_t(d.sort)]));
    if (d.sortOrder) e.set("sortOrder", Json::number(int64_t(d.sortOrder)));
    if (d.minScreenSize) e.set("minScreenSize", numJ(d.minScreenSize));
    if (d.maxScreenSize) e.set("maxScreenSize", numJ(d.maxScreenSize));
    if (!d.meshes.empty()) { Json a = Json::array(); for (auto& s : d.meshes) a.push(Json::string(s)); e.set("meshes", a); }
    if (d.ringBuffer) e.set("ringBuffer", Json::fromBool(true));
    if (d.trigger.enabled) {
        const FxTriggerDef& t = d.trigger;
        Json o = Json::object(), vols = Json::array();
        for (auto& s : t.volumes) { Json so = Json::object(); so.set("center", vecJ(s.center)); so.set("radius", numJ(s.radius)); vols.push(so); }
        o.set("volumes", vols);
        if (t.inside != FxTriggerAction::Ignore) o.set("inside", Json::string(kTriggerNames[size_t(t.inside)]));
        if (t.outside != FxTriggerAction::Ignore) o.set("outside", Json::string(kTriggerNames[size_t(t.outside)]));
        if (t.enter != FxTriggerAction::Ignore) o.set("enter", Json::string(kTriggerNames[size_t(t.enter)]));
        if (t.exit != FxTriggerAction::Ignore) o.set("exit", Json::string(kTriggerNames[size_t(t.exit)]));
        if (t.radius) o.set("radius", numJ(t.radius));
        e.set("trigger", o);
    }
}

void parseForceFields(const Json& j, const std::string& where, EffectDef& out, std::vector<std::string>& errors) {
    const Json& l = j["forceFields"];
    if (l.isNull()) return;
    if (!l.isArray() || l.size() > 8) { errors.push_back(where + ".forceFields: expected up to 8 force fields"); return; }
    for (size_t i = 0; i < l.size(); ++i) {
        Reader r{l[i], where + ".forceFields[" + std::to_string(i) + "]", errors};
        if (!l[i].isObject()) { r.fail("", "expected an object"); continue; }
        unknownKeys(l[i], {"position", "shape", "radius", "startRadius", "length", "direction", "gravity", "gravityFocus", "rotation",
                           "rotationAttraction", "drag"}, r.where, errors, r.ok);
        FxForceField f;
        r.vec3("position", f.position);
        int shape = 0;
        constexpr const char* kFieldShapes[] = {"sphere", "cylinder"};
        r.choice("shape", shape, kFieldShapes);
        f.cylinder = shape == 1;
        r.number("radius", f.radius, 0.01f, 1000);
        r.number("startRadius", f.startRadius, 0, 1000);
        r.number("length", f.length, 0, 1000);
        r.vec3("direction", f.direction);
        r.number("gravity", f.gravity, -1000, 1000);
        r.number("gravityFocus", f.gravityFocus, 0, 1);
        r.number("rotation", f.rotation, -36000, 36000);
        r.number("rotationAttraction", f.rotationAttraction, 0, 1);
        r.number("drag", f.drag, 0, 100);
        out.forceFields.push_back(f);
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
    return enabled == o.enabled && time == o.time && lifeFraction == o.lifeFraction && width == o.width && ck(color, o.color) &&
           ck(lifeColor, o.lifeColor) && texture == o.texture && blend == o.blend && inheritColor == o.inheritColor &&
           sizeAffectsWidth == o.sizeAffectsWidth && sizeAffectsLifetime == o.sizeAffectsLifetime && ribbon == o.ribbon && ribbons == o.ribbons &&
           ratio == o.ratio && minDistance == o.minDistance && textureMode == o.textureMode && dieWithParticles == o.dieWithParticles &&
           minQuality == o.minQuality;
}

bool parseEffect(const Json& j, const std::string& where, EffectDef& out, std::vector<std::string>& errors) {
    out = {};
    const size_t before = errors.size();
    if (!j.isObject()) { errors.push_back(where + ": expected an object with \"emitters\""); return false; }
    for (auto& [k, v] : j.members())
        if (k != "emitters" && k != "comment" && k != "forceFields") errors.push_back(where + ": unknown field '" + k + "'");
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
        r.range("rate", d.rate, 0, 2000);
        r.range("perMeter", d.perMeter, 0, 200);
        r.range("delay", d.delay, 0, 60);
        r.number("duration", d.duration, 0, 600);
        r.boolean("loop", d.loop);
        r.range("life", d.life, 0.01f, 60);
        r.range("speed", d.speed, -500, 500);
        r.range("size", d.size, 0.001f, 100);
        r.range("rotation", d.rotation, -3600, 3600);
        r.range("spin", d.spin, -36000, 36000);
        r.vec3("direction", d.direction);
        r.number("spread", d.spread, 0, 180);
        r.choice("shape", d.shape, kShapeNames);
        r.number("radius", d.radius, 0, 100);
        r.vec3("gravity", d.gravity);
        r.range("drag", d.drag, 0, 50);
        r.number("stretch", d.stretch, 0, 10);
        r.choice("minQuality", d.minQuality, kQualityChoice);

        if (e.has("sizeOverLife") && !keysIn(e["sizeOverLife"], d.sizeOverLife, 0, 1e6f)) r.fail("sizeOverLife", "expected [[t, k], ...]");
        if (e.has("color") && !colorKeys(e["color"], d.color)) r.fail("color", "expected \"#rrggbbaa\" or [[t, \"#rrggbbaa\"], ...]");
        parseModules(e, r, d);
        if (d.burst == 0 && d.bursts.empty() && !d.continuous()) r.fail("burst", "emitter creates no particles: set burst, bursts, rate or perMeter");
        if (d.rate.max > 0 && !d.loop && d.duration <= 0) r.fail("duration", "rate needs a duration (or \"loop\": true)");
        if (d.loop && !d.bursts.empty() && d.duration <= 0) r.fail("duration", "looping bursts repeat every \"duration\": set it");
        if (d.shape == FxShape::Mesh && d.mesh.empty()) r.fail("mesh", "shape \"mesh\" needs \"mesh\": \"builtin:cube\" or a model path");
        if (d.ringBuffer && d.maxParticles <= 0) r.fail("ringBuffer", "needs \"maxParticles\"");
        if (render::length(d.direction) < 1e-4f) d.direction = {0, 1, 0};
        out.emitters.push_back(std::move(d));
    }
    parseForceFields(j, where, out, errors);
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
        if (!isZero(d.rate)) e.set("rate", rangeJ(d.rate));
        if (!isZero(d.perMeter)) e.set("perMeter", rangeJ(d.perMeter));
        if (!isZero(d.delay)) e.set("delay", rangeJ(d.delay));
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
        if (nonzero(d.gravity)) e.set("gravity", vecJ(d.gravity));
        if (!isZero(d.drag)) e.set("drag", rangeJ(d.drag));
        if (d.stretch) e.set("stretch", numJ(d.stretch));
        e.set("sizeOverLife", keysJ(d.sizeOverLife));
        e.set("color", colorKeysJ(d.color));
        if (d.minQuality != base.minQuality) e.set("minQuality", Json::string(kQualityChoice[size_t(d.minQuality)]));
        modulesToJson(d, e);
        list.push(e);
    }
    root.set("emitters", list);
    if (!def.forceFields.empty()) {
        Json a = Json::array();
        const FxForceField fb;
        for (auto& f : def.forceFields) {
            Json o = Json::object();
            if (nonzero(f.position)) o.set("position", vecJ(f.position));
            if (f.cylinder) { o.set("shape", Json::string("cylinder")); if (f.length) o.set("length", numJ(f.length)); }
            o.set("radius", numJ(f.radius));
            if (f.startRadius) o.set("startRadius", numJ(f.startRadius));
            if (nonzero(f.direction)) o.set("direction", vecJ(f.direction));
            if (f.gravity) { o.set("gravity", numJ(f.gravity)); if (f.gravityFocus) o.set("gravityFocus", numJ(f.gravityFocus)); }
            if (f.rotation) { o.set("rotation", numJ(f.rotation)); if (f.rotationAttraction) o.set("rotationAttraction", numJ(f.rotationAttraction)); }
            if (f.drag) o.set("drag", numJ(f.drag));
            a.push(o);
        }
        root.set("forceFields", a);
    }
    return root;
}

} // namespace rts::fx
