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

constexpr const char* kBlendNames[] = {"alpha", "additive"};
constexpr const char* kAlignNames[] = {"camera", "velocity", "ground"};
constexpr const char* kShapeNames[] = {"point", "sphere", "disc", "ring"};
constexpr const char* kQualityChoice[] = {"off", "low", "medium", "high"};

constexpr const char* kKnownKeys[] = {
    "texture", "frames", "fps", "blend", "align", "burst", "rate", "perMeter", "delay", "duration", "loop",
    "life", "speed", "size", "rotation", "spin", "direction", "spread", "shape", "radius", "gravity", "drag",
    "stretch", "sizeOverLife", "color", "minQuality", "comment"};

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
    if (loop && continuous()) return std::numeric_limits<float>::infinity();
    return delay + (continuous() ? duration : 0.0f);
}

bool EffectDef::looping() const {
    for (auto& e : emitters) if (e.loop && e.continuous()) return true;
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
        if (d.burst == 0 && !d.continuous()) r.fail("burst", "emitter creates no particles: set burst, rate or perMeter");
        if (d.rate > 0 && !d.loop && d.duration <= 0) r.fail("duration", "rate needs a duration (or \"loop\": true)");
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
        list.push(e);
    }
    root.set("emitters", list);
    return root;
}

} // namespace rts::fx
