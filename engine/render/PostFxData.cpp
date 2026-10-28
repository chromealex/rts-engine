#include "PostFxData.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <stdexcept>

namespace rts::render {

namespace {
using K = PostFxParamKind;
constexpr PostFxQuality kLow = PostFxQuality::Low, kMed = PostFxQuality::Medium, kHigh = PostFxQuality::High;

PostFxParamDef num(const char* n, float d, float lo, float hi, const char* help, const char* label) { return {n, K::Number, {d, 0, 0}, lo, hi, help, label}; }
PostFxParamDef col(const char* n, float r, float g, float b, float lo, float hi, const char* help, const char* label) { return {n, K::Color, {r, g, b}, lo, hi, help, label}; }
PostFxParamDef flag(const char* n, bool d, const char* help, const char* label) { return {n, K::Bool, {d ? 1.0f : 0.0f, 0, 0}, 0, 1, help, label}; }

std::vector<PostFxTypeDef> buildCatalog() {
    using T = PostFxType;
    using C = PostFxCostClass;
    std::vector<PostFxTypeDef> c;
    c.push_back({T::Fxaa, "fxaa", "Сглаживание FXAA", "Anti-aliasing of edges (FXAA) as a separate full-screen pass.", C::Heavy, kMed, {
        num("subpixel", 0.75f, 0, 1, "how much thin details are softened", "Мелкие детали")}});
    c.push_back({T::Pixelate, "pixelate", "Пикселизация", "Big square pixels (retro look).", C::Cheap, kLow, {
        num("size", 4, 1, 32, "pixel size in screen pixels", "Размер пикселя")}});
    c.push_back({T::ChromaticAberration, "chromaticAberration", "Хроматическая аберрация", "Colour fringes towards the screen edges (lens look).", C::Medium, kLow, {
        num("intensity", 0.3f, 0, 1, "fringe width", "Сила")}});
    c.push_back({T::Sharpen, "sharpen", "Резкость", "Sharpens the picture (helps on upscaled / blurry renders).", C::Medium, kMed, {
        num("amount", 0.5f, 0, 2, "sharpening strength", "Сила")}});
    c.push_back({T::TiltShift, "tiltShift", "Тилт-шифт (миниатюра)", "Blurs the top and bottom of the screen, keeps a sharp band: the 'miniature' RTS look.", C::Medium, kMed, {
        num("focus", 0.45f, 0, 1, "height of the sharp band centre (0 bottom, 1 top)", "Центр резкости"),
        num("width", 0.3f, 0, 1, "height of the sharp band (screen share); 0 + falloff 0 = whole screen blurred", "Ширина полосы"),
        num("falloff", 0.25f, 0, 1, "soft transition from sharp to blurred", "Переход"),
        num("blur", 0.8f, 0, 1, "blur radius", "Размытие")}});
    c.push_back({T::Bloom, "bloom", "Свечение (bloom)", "Bright areas glow (fire, lights, sun on water).", C::Heavy, kMed, {
        num("threshold", 0.7f, 0, 1, "brightness where glow starts (0..1)", "Порог"),
        num("knee", 0.5f, 0, 1, "soft start of the threshold", "Мягкость порога"),
        num("intensity", 0.8f, 0, 4, "glow strength", "Сила"),
        num("radius", 0.6f, 0, 1, "glow spread", "Радиус"),
        col("tint", 1, 1, 1, 0, 2, "glow colour multiplier", "Цвет")}});
    c.push_back({T::SunShafts, "sunShafts", "Лучи солнца", "Light shafts from the sun through bright areas (radial blur towards the sun on screen).", C::Heavy, kHigh, {
        num("intensity", 0.6f, 0, 3, "strength", "Сила"),
        num("threshold", 0.75f, 0, 1, "brightness that casts shafts", "Порог"),
        num("length", 0.8f, 0, 1, "shaft length", "Длина"),
        col("color", 1, 0.9f, 0.7f, 0, 2, "shaft colour", "Цвет")}});
    c.push_back({T::ColorGrade, "colorGrade", "Цветокоррекция", "Exposure, contrast, saturation, white balance, lift / gamma / gain, colour filter.", C::Cheap, kLow, {
        num("exposure", 0, -3, 3, "stops (+1 = twice as bright)", "Экспозиция"),
        num("contrast", 1, 0, 2, "1 = unchanged", "Контраст"),
        num("saturation", 1, 0, 2, "0 = black and white, 1 = unchanged", "Насыщенность"),
        num("temperature", 0, -1, 1, "white balance: - cold (blue), + warm (orange)", "Температура"),
        num("tint", 0, -1, 1, "white balance: - green, + magenta", "Оттенок"),
        col("lift", 0, 0, 0, -0.5f, 0.5f, "added to shadows", "Тени (lift)"),
        col("gamma", 1, 1, 1, 0.2f, 3, "midtones (>1 brighter)", "Полутона (gamma)"),
        col("gain", 1, 1, 1, 0, 2, "highlights multiplier", "Света (gain)"),
        col("filter", 1, 1, 1, 0, 2, "colour multiplier", "Цветовой фильтр")}});
    c.push_back({T::Lut, "lut", "LUT (таблица цвета)", "Colour lookup table from an image: a strip of N squares N x N (256x16, 1024x32), red left to right inside a square, green top to bottom, blue square by square (Unreal layout; flipY for Unity). Make one with postfx_lut_neutral and grade it in any image editor.", C::Cheap, kLow, {
        {"texture", K::Text, {0, 0, 0}, 0, 0, "image path in the game folder (PNG)", "Картинка"},
        num("intensity", 1, 0, 1, "blend with the original", "Сила"),
        flag("flipY", false, "green grows upwards (Unity LUT images)", "Зелёный снизу вверх (Unity)")}});
    c.push_back({T::Posterize, "posterize", "Постеризация", "Fewer colour levels (comic / retro look).", C::Cheap, kLow, {
        num("levels", 8, 2, 64, "levels per channel", "Уровней")}});
    c.push_back({T::Vignette, "vignette", "Виньетка", "Darkens (or tints) the screen corners.", C::Cheap, kLow, {
        num("intensity", 0.35f, 0, 1, "strength", "Сила"),
        num("softness", 0.5f, 0, 1, "how far the darkening reaches inwards", "Мягкость"),
        col("color", 0, 0, 0, 0, 1, "corner colour", "Цвет")}});
    c.push_back({T::Grain, "grain", "Зерно плёнки", "Animated film grain.", C::Cheap, kLow, {
        num("intensity", 0.08f, 0, 1, "strength", "Сила"),
        num("size", 1.5f, 1, 4, "grain size in pixels", "Размер"),
        flag("animated", true, "changes every frame", "Анимировано")}});
    c.push_back({T::Fade, "fade", "Затемнение / заливка", "Blends the whole picture towards a colour (fade to black, flash, underwater tint).", C::Cheap, kLow, {
        col("color", 0, 0, 0, 0, 1, "colour", "Цвет"),
        num("amount", 0.5f, 0, 1, "0 = none, 1 = only the colour", "Сила")}});
    c.push_back({T::Letterbox, "letterbox", "Кинорамка", "Bars at the top and bottom (cinematic).", C::Cheap, kLow, {
        num("amount", 0.12f, 0, 0.5f, "share of the screen height covered by both bars", "Высота полос"),
        col("color", 0, 0, 0, 0, 1, "bar colour", "Цвет")}});
    return c;
}

int floatsOf(const PostFxParamDef& p) { return p.kind == K::Color ? 3 : p.kind == K::Text ? 0 : 1; }
int offsetOf(const PostFxTypeDef& t, const char* name) {
    int off = 0;
    for (auto& p : t.params) {
        if (std::string(p.name) == name) return off;
        off += floatsOf(p);
    }
    return -1;
}

Json n3(double v) { return Json::number(v, 3); }
std::string fmt(float v) { char b[32]; std::snprintf(b, sizeof b, v < 10 ? "%.1f" : "%.0f", double(v)); return b; }
} // namespace

const std::vector<PostFxTypeDef>& postFxCatalog() { static const std::vector<PostFxTypeDef> c = buildCatalog(); return c; }
const PostFxTypeDef* postFxType(const std::string& id) {
    for (auto& t : postFxCatalog()) if (id == t.id) return &t;
    return nullptr;
}
const PostFxTypeDef& postFxType(PostFxType t) {
    for (auto& d : postFxCatalog()) if (d.type == t) return d;
    return postFxCatalog().front();
}
const char* postFxQualityName(PostFxQuality q) {
    switch (q) { case PostFxQuality::Off: return "off"; case PostFxQuality::Low: return "low"; case PostFxQuality::Medium: return "medium"; default: return "high"; }
}
bool parsePostFxQuality(const std::string& s, PostFxQuality& out) {
    if (s == "off") out = PostFxQuality::Off;
    else if (s == "low") out = PostFxQuality::Low;
    else if (s == "medium") out = PostFxQuality::Medium;
    else if (s == "high") out = PostFxQuality::High;
    else return false;
    return true;
}
const char* postFxCostName(PostFxCostClass c) { return c == PostFxCostClass::Cheap ? "cheap" : c == PostFxCostClass::Medium ? "medium" : "heavy"; }

float PostFxEffect::get(const char* name) const {
    const int o = offsetOf(postFxType(type), name);
    return o >= 0 && size_t(o) < values.size() ? values[size_t(o)] : 0.0f;
}
const float* PostFxEffect::color(const char* name) const {
    static const float kZero[3] = {0, 0, 0};
    const int o = offsetOf(postFxType(type), name);
    return o >= 0 && size_t(o) + 3 <= values.size() ? &values[size_t(o)] : kZero;
}

PostFxEffect defaultPostFxEffect(PostFxType t) {
    const PostFxTypeDef& d = postFxType(t);
    PostFxEffect e;
    e.type = t;
    e.minQuality = d.defaultMinQuality;
    for (auto& p : d.params) for (int i = 0; i < floatsOf(p); ++i) e.values.push_back(p.def[i]);
    return e;
}

PostFxEffect parsePostFxEffect(const Json& j, bool strict, std::vector<std::string>* issues) {
    auto problem = [&](const std::string& m) {
        if (strict) throw std::runtime_error(m);
        if (issues) issues->push_back(m);
    };
    if (!j.isObject()) { problem("postfx: an effect must be an object {\"type\": ...}"); return {}; }
    const std::string id = j.getStr("type");
    const PostFxTypeDef* d = postFxType(id);
    if (!d) {
        std::string known;
        for (auto& t : postFxCatalog()) known += std::string(known.empty() ? "" : ", ") + t.id;
        throw std::runtime_error("postfx: unknown effect type '" + id + "' (known: " + known + ")");
    }
    PostFxEffect e = defaultPostFxEffect(d->type);
    for (auto& [k, v] : j.members()) {
        if (k == "type") continue;
        if (k == "enabled") { e.enabled = v.boolean(true); continue; }
        if (k == "minQuality") {
            PostFxQuality q;
            if (!parsePostFxQuality(v.str(), q) || q == PostFxQuality::Off) problem("postfx '" + id + "': minQuality must be low, medium or high");
            else e.minQuality = q;
            continue;
        }
        const int off = offsetOf(*d, k.c_str());
        const PostFxParamDef* p = nullptr;
        for (auto& pd : d->params) if (k == pd.name) p = &pd;
        if (!p) {
            std::string known;
            for (auto& pd : d->params) known += std::string(known.empty() ? "" : ", ") + pd.name;
            problem("postfx '" + id + "': unknown parameter '" + k + "' (parameters: " + known + ", enabled, minQuality)");
            continue;
        }
        auto clampIn = [&](double x) {
            const float f = float(x);
            const float c = std::clamp(f, p->lo, p->hi);
            if (c != f && issues) issues->push_back("postfx '" + id + "': " + k + " clamped to " + fmt(c) + " (range " + fmt(p->lo) + ".." + fmt(p->hi) + ")");
            return c;
        };
        switch (p->kind) {
        case K::Text:
            if (!v.isString()) problem("postfx '" + id + "': " + k + " must be a string");
            else e.text = v.str();
            break;
        case K::Bool:
            if (v.type() != Json::Type::Bool) problem("postfx '" + id + "': " + k + " must be true or false");
            else e.values[size_t(off)] = v.boolean() ? 1.0f : 0.0f;
            break;
        case K::Number:
            if (!v.isNumber()) problem("postfx '" + id + "': " + k + " must be a number");
            else e.values[size_t(off)] = clampIn(v.toDouble());
            break;
        case K::Color:
            if (v.isNumber()) { const float c = clampIn(v.toDouble()); for (int i = 0; i < 3; ++i) e.values[size_t(off + i)] = c; }
            else if (!v.isArray() || v.size() < 3) problem("postfx '" + id + "': " + k + " must be [r, g, b]");
            else for (int i = 0; i < 3; ++i) e.values[size_t(off + i)] = clampIn(v[size_t(i)].toDouble());
            break;
        }
    }
    if (d->type == PostFxType::Lut && e.text.empty()) problem("postfx 'lut': needs \"texture\" (an image in the game folder)");
    return e;
}

Json postFxEffectJson(const PostFxEffect& e) {
    const PostFxTypeDef& d = postFxType(e.type);
    Json j = Json::object();
    j.set("type", Json::string(d.id));
    if (!e.enabled) j.set("enabled", Json::fromBool(false));
    if (e.minQuality != d.defaultMinQuality) j.set("minQuality", Json::string(postFxQualityName(e.minQuality)));
    int off = 0;
    for (auto& p : d.params) {
        const int n = floatsOf(p);
        bool same = true;
        for (int i = 0; i < n; ++i) if (std::fabs(e.values[size_t(off + i)] - p.def[i]) > 1e-4f) same = false;
        if (p.kind == K::Text) { if (!e.text.empty()) j.set(p.name, Json::string(e.text)); }
        else if (!same) {
            if (p.kind == K::Bool) j.set(p.name, Json::fromBool(e.values[size_t(off)] > 0.5f));
            else if (p.kind == K::Number) j.set(p.name, n3(e.values[size_t(off)]));
            else { Json a = Json::array(); for (int i = 0; i < 3; ++i) a.push(n3(e.values[size_t(off + i)])); j.set(p.name, a); }
        }
        off += n;
    }
    return j;
}

const PostFxEffect* PostFxSettings::find(PostFxType t) const {
    for (auto& e : effects) if (e.type == t) return &e;
    return nullptr;
}
std::vector<const PostFxEffect*> PostFxSettings::activeAt(PostFxQuality q) const {
    std::vector<const PostFxEffect*> out;
    if (!enabled || q == PostFxQuality::Off) return out;
    for (auto& e : effects) if (e.enabled && int(q) >= int(e.minQuality)) out.push_back(&e);
    return out;
}

PostFxSettings parsePostFx(const Json& game, const Json& map, std::vector<std::string>* issues) {
    PostFxSettings s;
    std::map<std::string, Json> byType;     // merged effect objects
    std::vector<std::string> order;
    auto effectsOf = [](const Json& b) -> const Json& { return b.isArray() ? b : b["effects"]; };
    auto addFrom = [&](const Json& block, bool overlay) {
        const Json& list = effectsOf(block);
        for (auto& e : list.items()) {
            if (!e.isObject()) { if (issues) issues->push_back("postfx: an effect must be an object"); continue; }
            const std::string t = e.getStr("type");
            auto it = byType.find(t);
            if (it == byType.end()) { byType[t] = e; order.push_back(t); continue; }
            if (overlay) { for (auto& [k, v] : e.members()) it->second.set(k, v); }
            else it->second = e;
        }
    };
    if (game.isObject() || game.isArray()) {
        if (game.isObject() && !game.getBool("enabled", true)) s.enabled = false;
        addFrom(game, false);
    }
    if (map.isObject() || map.isArray()) {
        if (map.isObject() && map.has("enabled")) s.enabled = map.getBool("enabled", true);
        if (map.isObject() && map.getBool("replace")) { byType.clear(); order.clear(); }
        addFrom(map, true);
    }
    for (auto& d : postFxCatalog()) {          // catalog order = application order
        auto it = byType.find(d.id);
        if (it == byType.end()) continue;
        try { s.effects.push_back(parsePostFxEffect(it->second, false, issues)); }
        catch (const std::exception& ex) { if (issues) issues->push_back(ex.what()); }
    }
    for (auto& t : order) if (!postFxType(t) && issues) issues->push_back("postfx: unknown effect type '" + t + "'");
    return s;
}

// ---------------------------------------------------------------- presets
const std::vector<PostFxPreset>& postFxPresets() {
    static const std::vector<PostFxPreset> p = {
        {"cinematic", "Кино", "soft glow, slightly desaturated contrast, vignette, letterbox, light grain",
         R"([{"type":"bloom","intensity":0.6,"threshold":0.72},{"type":"colorGrade","contrast":1.12,"saturation":0.9,"temperature":0.06},
             {"type":"vignette","intensity":0.35},{"type":"letterbox","amount":0.1},{"type":"grain","intensity":0.04}])"},
        {"vivid", "Сочный", "saturated and crisp: colour boost, a little glow and sharpening",
         R"([{"type":"colorGrade","saturation":1.25,"contrast":1.08},{"type":"bloom","intensity":0.45,"threshold":0.8},{"type":"sharpen","amount":0.3}])"},
        {"mobile", "Лёгкий для телефонов", "cheap look for weak phones: colour grade + vignette only (one composite pass)",
         R"([{"type":"colorGrade","saturation":1.12,"contrast":1.06},{"type":"vignette","intensity":0.25}])"},
        {"noir", "Нуар", "black and white, hard contrast, heavy vignette, grain",
         R"([{"type":"colorGrade","saturation":0,"contrast":1.3},{"type":"vignette","intensity":0.55,"softness":0.6},{"type":"grain","intensity":0.08}])"},
        {"retro", "Ретро", "big pixels and few colours",
         R"([{"type":"pixelate","size":3},{"type":"posterize","levels":12},{"type":"vignette","intensity":0.2}])"},
        {"miniature", "Миниатюра", "tilt-shift: the battlefield looks like a tabletop model",
         R"([{"type":"tiltShift","focus":0.45,"width":0.25,"falloff":0.3},{"type":"colorGrade","saturation":1.3,"contrast":1.1}])"},
        {"sunset", "Закат", "warm light, sun shafts and glow",
         R"([{"type":"colorGrade","temperature":0.35,"tint":0.05,"contrast":1.05},{"type":"sunShafts","intensity":0.7},{"type":"bloom","intensity":0.6},{"type":"vignette","intensity":0.25}])"},
        {"winter", "Зима", "cold white balance, muted colours, soft glow",
         R"([{"type":"colorGrade","temperature":-0.3,"saturation":0.85,"exposure":0.15},{"type":"bloom","intensity":0.4,"threshold":0.8},{"type":"vignette","intensity":0.2}])"},
        {"nightVision", "Ночное видение", "green monochrome, bright, noisy",
         R"([{"type":"colorGrade","saturation":0,"exposure":0.8,"filter":[0.35,1.0,0.35]},{"type":"grain","intensity":0.22},{"type":"vignette","intensity":0.6,"softness":0.3}])"},
        {"clean", "Чистая картинка", "anti-aliasing and light sharpening",
         R"([{"type":"fxaa"},{"type":"sharpen","amount":0.25}])"},
    };
    return p;
}
const PostFxPreset* postFxPreset(const std::string& id) {
    for (auto& p : postFxPresets()) if (id == p.id) return &p;
    return nullptr;
}

// ---------------------------------------------------------------- cost
const std::vector<PostFxDeviceProfile>& postFxProfiles() {
    // Rough fill-rate figures: a full-screen texture read per pixel at 720p takes ~0.8 ms on a
    // Mali-T720 MP2 / Adreno 306 class GPU, ~0.15 ms of overhead per extra render target (tile
    // store + load). Mid phones render ~1.7 Mpix at ~3x the rate; desktops are ~40x faster.
    static const std::vector<PostFxDeviceProfile> p = {
        {"weakPhone", "weak phone (Mali-T720 / Adreno 306 class, 1280x720)", 1280, 720, 0.9f, 0.15f, PostFxQuality::Low},
        {"midPhone", "mid phone (Adreno 610 / Mali-G52 class, 1920x864)", 1920, 864, 0.3f, 0.05f, PostFxQuality::Medium},
        {"desktop", "desktop / laptop GPU (2560x1440)", 2560, 1440, 0.02f, 0.01f, PostFxQuality::High},
    };
    return p;
}

BloomPlan bloomPlan(PostFxQuality t) {
    if (t == PostFxQuality::High) return {2, 5};
    if (t == PostFxQuality::Medium) return {4, 4};
    return {4, 3};
}
BlurPlan tiltBlurPlan(PostFxQuality t) { return t == PostFxQuality::High ? BlurPlan{2, 2} : BlurPlan{4, 2}; }
ShaftsPlan shaftsPlan(PostFxQuality t) {
    if (t == PostFxQuality::High) return {2, 32};
    if (t == PostFxQuality::Medium) return {4, 24};
    return {4, 16};
}

PostFxWork postFxBaseWork() { return {1.6f, 1}; }

PostFxWork postFxEffectWork(const PostFxEffect& e, PostFxQuality tier, bool ignoreMin) {
    if (tier == PostFxQuality::Off || (!ignoreMin && (int(tier) < int(e.minQuality) || !e.enabled))) return {};
    using T = PostFxType;
    switch (e.type) {
    case T::ColorGrade: return {0.15f, 0};
    case T::Vignette: return {0.05f, 0};
    case T::Grain: return {0.1f, 0};
    case T::Fade: case T::Letterbox: return {0.02f, 0};
    case T::Pixelate: case T::Posterize: return {0.05f, 0};
    case T::Lut: return {1.0f, 0};
    case T::ChromaticAberration: return {2.0f, 0};
    case T::Sharpen: return {2.0f, 0};
    case T::Fxaa: return {6.0f, 1};
    case T::Bloom: {
        const BloomPlan b = bloomPlan(tier);
        float a = 1.0f / float(b.startDiv * b.startDiv), u = 4 * a;   // prefilter: 4 taps per output pixel
        for (int i = 1; i < b.levels; ++i) u += 5 * a / float(1 << (2 * i));
        for (int i = 0; i < b.levels - 1; ++i) u += 9 * a / float(1 << (2 * i));
        return {u + 1.0f, 1 + (b.levels - 1) * 2};
    }
    case T::TiltShift: {
        const BlurPlan b = tiltBlurPlan(tier);
        const float a = 1.0f / float(b.div * b.div);
        return {a * (1 + 5.0f * float(b.passes)) + 1.1f, 1 + b.passes};
    }
    case T::SunShafts: {
        const ShaftsPlan s = shaftsPlan(tier);
        const float a = 1.0f / float(s.div * s.div);
        return {a * (4 + float(s.samples)) + 1.0f, 2};
    }
    default: return {};
    }
}

float postFxMs(const PostFxWork& w, const PostFxDeviceProfile& p) {
    const float mpix = float(p.width) * float(p.height) / 1e6f;
    return w.units * mpix * p.msPerMpixUnit + float(w.passes) * p.msPerPass;
}

namespace {
float round2(float v) { return std::round(v * 100.0f) / 100.0f; }
const PostFxDeviceProfile& profile(const char* id) {
    for (auto& p : postFxProfiles()) if (std::string(p.id) == id) return p;
    return postFxProfiles().front();
}
} // namespace

std::vector<std::string> postFxEffectWarnings(const PostFxEffect& e) {
    std::vector<std::string> w;
    const PostFxTypeDef& d = postFxType(e.type);
    const auto& weak = profile("weakPhone");
    const auto& mid = profile("midPhone");
    const float weakForced = postFxMs(postFxEffectWork(e, PostFxQuality::Low, true), weak);
    const float midMs = postFxMs(postFxEffectWork(e, PostFxQuality::Medium, true), mid);
    const float hiMs = postFxMs(postFxEffectWork(e, PostFxQuality::High, true), mid);
    if (d.cost == PostFxCostClass::Heavy) {
        std::string s = std::string("HEAVY effect '") + d.id + "': about " + fmt(midMs) + " ms per frame on a mid phone (" +
                        fmt(hiMs) + " ms at quality high there), " + fmt(weakForced) + " ms on a weak phone.";
        if (e.minQuality == PostFxQuality::Low)
            s += " minQuality 'low' forces it onto weak phones too: at 60 fps that is " + fmt(weakForced / 16.7f * 100.0f) +
                 "% of the frame there. Prefer minQuality 'medium' (weak phones skip it) or 'high' (strong devices only).";
        else
            s += std::string(" It runs from quality '") + postFxQualityName(e.minQuality) + "' up, weak phones skip it.";
        w.push_back(s);
    } else if (d.cost == PostFxCostClass::Medium && e.minQuality == PostFxQuality::Low && weakForced >= 1.0f) {
        w.push_back(std::string("'") + d.id + "' costs about " + fmt(weakForced) + " ms per frame on a weak phone; minQuality 'medium' skips it there.");
    }
    return w;
}

Json postFxCostReport(const PostFxSettings& s) {
    Json r = Json::object(), effects = Json::array(), totals = Json::object(), warnings = Json::array();
    for (auto& e : s.effects) {
        const PostFxTypeDef& d = postFxType(e.type);
        Json j = Json::object();
        j.set("type", Json::string(d.id));
        j.set("class", Json::string(postFxCostName(d.cost)));
        j.set("enabled", Json::fromBool(e.enabled));
        j.set("minQuality", Json::string(postFxQualityName(e.minQuality)));
        Json per = Json::object();
        for (auto& p : postFxProfiles()) {
            const PostFxWork w = postFxEffectWork(e, p.tier);
            Json pj = Json::object();
            pj.set("tier", Json::string(postFxQualityName(p.tier)));
            pj.set("runs", Json::fromBool(w.units > 0));
            pj.set("ms", n3(round2(postFxMs(w, p))));
            if (w.units > 0) pj.set("passes", Json::number(int64_t(w.passes)));
            per.set(p.id, pj);
        }
        j.set("cost", per);
        effects.push(j);
        if (e.enabled) for (auto& m : postFxEffectWarnings(e)) warnings.push(Json::string(m));
    }
    for (auto& p : postFxProfiles()) {
        PostFxWork sum;
        const auto active = s.activeAt(p.tier);
        if (!active.empty()) {
            sum = postFxBaseWork();
            for (auto* e : active) { const PostFxWork w = postFxEffectWork(*e, p.tier); sum.units += w.units; sum.passes += w.passes; }
        }
        const float ms = postFxMs(sum, p);
        Json t = Json::object();
        t.set("device", Json::string(p.label));
        t.set("tier", Json::string(postFxQualityName(p.tier)));
        t.set("effects", Json::number(int64_t(active.size())));
        t.set("passes", Json::number(int64_t(sum.passes)));
        t.set("ms", n3(round2(ms)));
        t.set("fpsAt60", n3(round2(1000.0f / (1000.0f / 60.0f + ms))));
        totals.set(p.id, t);
        const float budget = std::string(p.id) == "desktop" ? 1.5f : 3.0f;
        if (ms > budget)
            warnings.push(Json::string("post chain over budget on a " + std::string(p.label) + ": " + fmt(ms) + " ms per frame (budget " +
                                       fmt(budget) + " ms); 60 fps becomes about " + fmt(1000.0f / (1000.0f / 60.0f + ms)) +
                                       " fps. Raise minQuality of heavy effects or remove some."));
    }
    r.set("effects", effects);
    r.set("total", totals);
    r.set("note", Json::string("Estimates from the fill-rate model in engine/render/PostFxData.cpp (scene to texture + composite counted once). "
                               "Tiers: weak phones get 'low', mid phones 'medium', desktops 'high' by default; players can lower it."));
    r.set("warnings", warnings);
    return r;
}

} // namespace rts::render
