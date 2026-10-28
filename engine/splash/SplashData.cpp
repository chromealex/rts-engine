#include "engine/splash/SplashData.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace rts::splash {

namespace {

constexpr float kPi = 3.14159265358979f;

float num(const Json& j, float def) { return j.isNumber() ? float(j.toDouble(def)) : def; }
[[noreturn]] void fail(const std::string& where, const std::string& what) { throw std::runtime_error("splash" + where + ": " + what); }

int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

float ease(Ease e, float k) {
    k = std::clamp(k, 0.0f, 1.0f);
    switch (e) {
    case Ease::In: return k * k;
    case Ease::Out: return 1 - (1 - k) * (1 - k);
    case Ease::InOut: return k < 0.5f ? 2 * k * k : 1 - 2 * (1 - k) * (1 - k);
    default: return k;
    }
}

Ease parseEase(const std::string& s, const std::string& where) {
    if (s.empty() || s == "linear") return Ease::Linear;
    if (s == "in") return Ease::In;
    if (s == "out") return Ease::Out;
    if (s == "inOut" || s == "in-out" || s == "inout") return Ease::InOut;
    fail(where, "unknown ease '" + s + "' (linear, in, out, inOut)");
}

Fit parseFit(const std::string& s, Fit def, const std::string& where) {
    if (s.empty()) return def;
    if (s == "contain") return Fit::Contain;
    if (s == "cover") return Fit::Cover;
    if (s == "center") return Fit::Center;
    if (s == "stretch") return Fit::Stretch;
    fail(where, "unknown fit '" + s + "' (contain, cover, center, stretch)");
}

void parseKeys(const Json& arr, Layer& l, const std::string& where) {
    static const char* names[kPropCount] = {"opacity", "scale", "x", "y", "rotate"};
    for (size_t i = 0; i < arr.size(); ++i) {
        const Json& k = arr[i];
        std::string w = where + ".animation[" + std::to_string(i) + "]";
        if (!k.isObject()) fail(w, "keyframe must be an object {\"t\": seconds, \"opacity\": ...}");
        Keyframe kf;
        kf.t = num(k["t"], -1);
        if (kf.t < 0) fail(w, "\"t\" (seconds from the layer start, >= 0) is required");
        for (int p = 0; p < kPropCount; ++p)
            if (k[names[p]].isNumber()) { kf.value[p] = num(k[names[p]], 0); kf.has[p] = true; }
        kf.ease = parseEase(k.getStr("ease"), w);
        l.keys.push_back(kf);
    }
    std::stable_sort(l.keys.begin(), l.keys.end(), [](const Keyframe& a, const Keyframe& b) { return a.t < b.t; });
}

void parseAnimation(const Json& a, Layer& l, const std::string& where) {
    if (a.isNull()) return;
    if (a.isArray()) { parseKeys(a, l, where); return; }
    if (!a.isObject()) fail(where + ".animation", "expected an array of keyframes or an object of effects");
    Effects& e = l.fx;
    if (a.has("fade")) e.fadeIn = e.fadeOut = num(a["fade"], 0);
    e.fadeIn = num(a["fadeIn"], e.fadeIn);
    e.fadeOut = num(a["fadeOut"], e.fadeOut);
    if (const Json& p = a["pulse"]; p.isNumber()) e.pulseAmount = num(p, 0);
    else if (p.isObject()) { e.pulseAmount = num(p["amount"], 0.05f); e.pulsePeriod = std::max(0.05f, num(p["period"], 1.0f)); }
    e.rotateSpeed = num(a["rotate"], 0);
    if (const Json& s = a["slide"]; !s.isNull()) {
        std::string from = s.isString() ? s.str() : s.getStr("from", "bottom");
        float dist = s.isObject() ? num(s["distance"], 0.1f) : 0.1f;
        e.slideTime = s.isObject() ? num(s["duration"], 0.6f) : 0.6f;
        if (from == "left") e.slideFromX = -dist;
        else if (from == "right") e.slideFromX = dist;
        else if (from == "top") e.slideFromY = -dist;
        else if (from == "bottom") e.slideFromY = dist;
        else fail(where + ".animation.slide", "unknown side '" + from + "' (left, right, top, bottom)");
    }
    if (const Json& z = a["zoom"]; z.isNumber()) { e.zoomFrom = num(z, 1); e.zoomTime = 0.6f; }
    else if (z.isObject()) { e.zoomFrom = num(z["from"], 0.8f); e.zoomTime = num(z["duration"], 0.6f); }
    if (a.has("keyframes")) parseKeys(a["keyframes"], l, where);
}

Layer parseLayer(const Json& j, Fit defFit, float defScale, const std::string& where, std::vector<std::string>* warnings) {
    Layer l;
    if (j.isString()) { l.images.push_back(j.str()); l.fit = defFit; l.scale = defScale; return l; }
    if (!j.isObject()) fail(where, "expected an image path or an object");
    if (std::string im = j.getStr("image"); !im.empty()) l.images.push_back(im);
    for (auto& f : j["frames"].items()) if (!f.str().empty()) l.images.push_back(f.str());
    l.sheet = j.getStr("sheet");
    if (!l.sheet.empty()) {
        const Json& fr = j["frame"];
        if (fr.isArray() && fr.size() == 2) { l.frameW = int(fr[size_t(0)].integer(0)); l.frameH = int(fr[size_t(1)].integer(0)); }
        else if (fr.isNumber()) l.frameW = l.frameH = int(fr.integer(0));
        if (l.frameW <= 0 || l.frameH <= 0) fail(where, "\"sheet\" needs \"frame\": [width, height] in pixels");
        l.frameCount = int(j.getInt("count", 0));
    }
    if (!l.sheet.empty() && !l.images.empty()) fail(where, "use either \"sheet\" or \"image\"/\"frames\", not both");
    l.fps = std::max(0.1f, num(j["fps"], 12));
    l.loopFrames = j["loopFrames"].boolean(true);
    l.fit = parseFit(j.getStr("fit"), defFit, where);
    l.scale = num(j["scale"], defScale);
    if (l.scale <= 0) fail(where, "\"scale\" must be > 0");
    if (const Json& a = j["anchor"]; a.isArray() && a.size() == 2) { l.anchorX = num(a[size_t(0)], 0.5f); l.anchorY = num(a[size_t(1)], 0.5f); }
    l.start = std::max(0.0f, num(j["start"], 0));
    l.duration = std::max(0.0f, num(j["duration"], 0));
    parseAnimation(j["animation"], l, where);
    if (l.fx.fadeOut > 0 && l.duration <= 0 && warnings)
        warnings->push_back("splash" + where + ": fadeOut needs \"duration\" (endless layers fade with the whole splash)");
    return l;
}

} // namespace

uint32_t Color::rgba8() const {
    auto b = [](float v) { return uint32_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255)); };
    return b(r) | (b(g) << 8) | (b(this->b) << 16) | (b(a) << 24);
}

bool parseColor(const std::string& s, Color& out) {
    if (s.size() < 4 || s[0] != '#') return false;
    std::string h = s.substr(1);
    if (h.size() == 3 || h.size() == 4) { std::string x; for (char c : h) { x += c; x += c; } h = x; }
    if (h.size() != 6 && h.size() != 8) return false;
    float v[4] = {0, 0, 0, 1};
    for (size_t i = 0; i < h.size() / 2; ++i) {
        int hi = hexDigit(h[i * 2]), lo = hexDigit(h[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        v[i] = float(hi * 16 + lo) / 255.0f;
    }
    out = {v[0], v[1], v[2], v[3]};
    return true;
}

bool parseColor(const Json& j, Color& out) {
    if (j.isString()) return parseColor(j.str(), out);
    if (j.isArray() && (j.size() == 3 || j.size() == 4)) {
        out = {num(j[size_t(0)], 0), num(j[size_t(1)], 0), num(j[size_t(2)], 0), j.size() == 4 ? num(j[size_t(3)], 1) : 1.0f};
        return true;
    }
    return false;
}

std::string colorHex(const Color& c, bool withAlpha) {
    auto b = [](float v) { return int(std::lround(std::clamp(v, 0.0f, 1.0f) * 255)); };
    char buf[16];
    if (withAlpha) std::snprintf(buf, sizeof buf, "#%02x%02x%02x%02x", b(c.r), b(c.g), b(c.b), b(c.a));
    else std::snprintf(buf, sizeof buf, "#%02x%02x%02x", b(c.r), b(c.g), b(c.b));
    return buf;
}

int Layer::frames() const {
    if (!sheet.empty()) return std::max(1, frameCount);
    return std::max<int>(1, int(images.size()));
}

float Spec::sequenceLength() const {
    float end = 0;
    for (auto& l : layers) if (l.duration > 0) end = std::max(end, l.start + l.duration);
    return end;
}

float Spec::minimumTime() const {
    float m = minSeconds >= 0 ? minSeconds : sequenceLength();
    if (maxSeconds > 0) m = std::min(m, maxSeconds);
    return m;
}

std::vector<std::string> Spec::images() const {
    std::vector<std::string> v;
    for (auto& l : layers) {
        for (auto& i : l.images) v.push_back(i);
        if (!l.sheet.empty()) v.push_back(l.sheet);
    }
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    return v;
}

Spec parseSpec(const Json& j, std::vector<std::string>* warnings) {
    Spec s;
    if (j.isNull()) return s;
    if (!j.isObject()) fail("", "expected an object");
    s.enabled = true;
    if (j.has("background") && !parseColor(j["background"], s.background)) fail(".background", "expected a colour like \"#101418\"");
    if (!j["image"].isNull() || !j["frames"].isNull() || !j["sheet"].isNull()) {
        Json base = j;   // the base layer reads image/frames/sheet/fit/scale/animation from the top level
        base.erase("logos"); base.erase("logo");
        s.layers.push_back(parseLayer(base, Fit::Cover, 1.0f, "", warnings));
    }
    auto addLogos = [&](const Json& list, const std::string& key) {
        if (list.isNull()) return;
        if (list.isArray()) {
            for (size_t i = 0; i < list.size(); ++i) s.layers.push_back(parseLayer(list[i], Fit::Contain, 0.5f, "." + key + "[" + std::to_string(i) + "]", warnings));
        } else {
            s.layers.push_back(parseLayer(list, Fit::Contain, 0.5f, "." + key, warnings));
        }
    };
    addLogos(j["logo"], "logo");
    addLogos(j["logos"], "logos");
    for (size_t i = 0; i < s.layers.size(); ++i)
        if (!s.layers[i].hasImage()) fail(" layer " + std::to_string(i), "has no \"image\", \"frames\" or \"sheet\"");
    s.minSeconds = num(j["minSeconds"], -1);
    s.maxSeconds = std::max(0.0f, num(j["maxSeconds"], 0));
    if (s.maxSeconds > 0 && s.minSeconds > s.maxSeconds) fail(".minSeconds", "is larger than maxSeconds");
    s.loop = j.getBool("loop", false);
    s.skippable = j.getBool("skippable", true);
    s.waitForContent = j.getBool("waitForContent", false);
    s.fadeOut = std::max(0.0f, num(j["fadeOut"], 0.4f));
    const Json& p = j["progress"];
    if (p.type() == Json::Type::Bool) s.progress.show = p.boolean(true);
    else if (p.isObject()) {
        s.progress.show = p.getBool("show", true);
        if (p.has("color") && !parseColor(p["color"], s.progress.color)) fail(".progress.color", "expected a colour");
        if (p.has("track") && !parseColor(p["track"], s.progress.track)) fail(".progress.track", "expected a colour");
        s.progress.position = p.getStr("position", "bottom");
        if (s.progress.position != "bottom" && s.progress.position != "top" && s.progress.position != "center")
            fail(".progress.position", "unknown '" + s.progress.position + "' (bottom, top, center)");
        s.progress.width = std::clamp(num(p["width"], 0.4f), 0.02f, 1.0f);
        s.progress.height = std::clamp(num(p["height"], 6), 1.0f, 64.0f);
        s.progress.offset = std::clamp(num(p["offset"], 0.08f), 0.0f, 0.9f);
    } else if (!p.isNull()) fail(".progress", "expected true/false or an object");
    if (warnings && s.layers.empty()) warnings->push_back("splash: no image or logos — only the background colour and the progress bar are shown");
    return s;
}

void layerState(const Layer& l, float local, float out[kPropCount]) {
    static const float defaults[kPropCount] = {1, 1, 0, 0, 0};
    for (int p = 0; p < kPropCount; ++p) {
        const Keyframe* before = nullptr;
        const Keyframe* after = nullptr;
        for (auto& k : l.keys) {
            if (!k.has[p]) continue;
            if (k.t <= local) before = &k;
            else { after = &k; break; }
        }
        if (!before && !after) out[p] = defaults[p];
        else if (!after) out[p] = before->value[p];
        else if (!before) out[p] = after->value[p];
        else {
            float span = after->t - before->t;
            float k = span > 0 ? ease(after->ease, (local - before->t) / span) : 1.0f;
            out[p] = before->value[p] + (after->value[p] - before->value[p]) * k;
        }
    }
    const Effects& e = l.fx;
    if (e.fadeIn > 0) out[kOpacity] *= std::clamp(local / e.fadeIn, 0.0f, 1.0f);
    if (e.fadeOut > 0 && l.duration > 0) out[kOpacity] *= std::clamp((l.duration - local) / e.fadeOut, 0.0f, 1.0f);
    if (e.pulseAmount != 0) out[kScale] *= 1 + e.pulseAmount * std::sin(2 * kPi * local / e.pulsePeriod);
    out[kRotate] += e.rotateSpeed * local;
    if (e.slideTime > 0) {
        float k = 1 - ease(Ease::Out, local / e.slideTime);
        out[kX] += e.slideFromX * k;
        out[kY] += e.slideFromY * k;
    }
    if (e.zoomTime > 0) out[kScale] *= e.zoomFrom + (1 - e.zoomFrom) * ease(Ease::Out, local / e.zoomTime);
}

std::vector<DrawItem> evaluate(const Spec& s, const std::vector<ImageSize>& sizes, float t, float W, float H, float pixelScale) {
    std::vector<DrawItem> out;
    for (size_t i = 0; i < s.layers.size(); ++i) {
        const Layer& l = s.layers[i];
        float local = t - l.start;
        if (local < 0 || (l.duration > 0 && local >= l.duration)) continue;
        if (i >= sizes.size() || sizes[i].w <= 0 || sizes[i].h <= 0) continue;
        float st[kPropCount];
        layerState(l, local, st);
        float op = std::clamp(st[kOpacity], 0.0f, 1.0f);
        if (op <= 0.001f || st[kScale] <= 0) continue;
        float iw = float(sizes[i].w), ih = float(sizes[i].h), w = iw, h = ih;
        switch (l.fit) {
        case Fit::Contain: { float k = std::min(W / iw, H / ih); w = iw * k; h = ih * k; break; }
        case Fit::Cover: { float k = std::max(W / iw, H / ih); w = iw * k; h = ih * k; break; }
        case Fit::Stretch: w = W; h = H; break;
        case Fit::Center: w = iw * pixelScale; h = ih * pixelScale; break;
        }
        DrawItem d;
        d.layer = int(i);
        d.w = w * l.scale * st[kScale];
        d.h = h * l.scale * st[kScale];
        d.cx = (l.anchorX + st[kX]) * W;
        d.cy = (l.anchorY + st[kY]) * H;
        d.rotation = st[kRotate] * kPi / 180.0f;
        d.opacity = op;
        int n = l.frames();
        if (n > 1) {
            int f = int(std::floor(local * l.fps));
            d.frame = l.loopFrames ? f % n : std::min(f, n - 1);
        }
        out.push_back(d);
    }
    return out;
}

float advanceTimeline(const Spec& s, float timeline, float dt, bool stillLoading) {
    timeline += dt;
    float len = s.sequenceLength();
    if (s.loop && stillLoading && len > 0 && timeline >= len) timeline = std::fmod(timeline, len);
    return timeline;
}

bool progressRect(const Spec& s, float W, float H, float pixelScale, float& x, float& y, float& w, float& h) {
    if (!s.progress.show) return false;
    w = std::round(W * s.progress.width);
    h = std::max(1.0f, std::round(s.progress.height * pixelScale));
    x = std::round((W - w) * 0.5f);
    if (s.progress.position == "top") y = std::round(H * s.progress.offset);
    else if (s.progress.position == "center") y = std::round(H * 0.5f - h * 0.5f);
    else y = std::round(H * (1 - s.progress.offset) - h);
    return true;
}

bool readyToLeave(const Spec& s, float elapsed, bool loaded, bool skipped) {
    return loaded && (skipped || elapsed >= s.minimumTime());
}

} // namespace rts::splash
