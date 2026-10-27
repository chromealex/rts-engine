#include "UiInternal.h"

#include <RmlUi/Core/CallbackTexture.h>
#include <RmlUi/Core/FileInterface.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Log.h>
#include <RmlUi/Core/Mesh.h>
#include <RmlUi/Core/StringUtilities.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "stb_truetype.h"

namespace rts::ui {

struct UiFontEngine::File {
    std::string data;
    std::string family; // lower case
    Rml::Style::FontStyle style = Rml::Style::FontStyle::Normal;
    Rml::Style::FontWeight weight = Rml::Style::FontWeight::Normal;
    stbtt_fontinfo info{};
};

struct UiFontEngine::Face {
    struct Glyph { float advance = 0; int x0 = 0, y0 = 0, w = 0, h = 0; int ax = 0, ay = 0; };
    File* file = nullptr;
    int size = 0;
    bool fakeBold = false;  // requested bold, file is regular: glyphs are dilated by one pixel
    float scale = 1;
    Rml::FontMetrics metrics{};
    std::unordered_map<uint32_t, Glyph> glyphs;
    std::vector<uint8_t> atlas;
    int aw = 256, ah = 128, penX = 1, penY = 1, rowH = 0;
    bool atlasDirty = true;
    int version = 0;
    std::unique_ptr<Rml::CallbackTextureSource> texture;

    const Glyph& glyph(uint32_t cp);
    float kern(uint32_t a, uint32_t b) const { return a ? float(stbtt_GetCodepointKernAdvance(&file->info, int(a), int(b))) * scale : 0.0f; }
    void syncTexture();
};

namespace {

constexpr int kMaxAtlas = 4096;

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Family name (name id 1) from the font's name table, UTF-16BE Windows record -> ASCII.
std::string familyName(const stbtt_fontinfo& info) {
    int len = 0;
    const char* n = stbtt_GetFontNameString(&info, &len, STBTT_PLATFORM_ID_MICROSOFT, STBTT_MS_EID_UNICODE_BMP, STBTT_MS_LANG_ENGLISH, 1);
    std::string out;
    for (int i = 0; n && i + 1 < len; i += 2)
        if (n[i] == 0) out += n[i + 1];
    return out;
}

} // namespace

const UiFontEngine::Face::Glyph& UiFontEngine::Face::glyph(uint32_t cp) {
    if (auto it = glyphs.find(cp); it != glyphs.end()) return it->second;
    Glyph g;
    const int gi = stbtt_FindGlyphIndex(&file->info, int(cp));
    int adv = 0, lsb = 0;
    stbtt_GetGlyphHMetrics(&file->info, gi, &adv, &lsb);
    g.advance = float(adv) * scale + (fakeBold ? 1.0f : 0.0f);
    int x0, y0, x1, y1;
    stbtt_GetGlyphBitmapBox(&file->info, gi, scale, scale, &x0, &y0, &x1, &y1);
    const int pad = fakeBold ? 1 : 0;
    const int w = x1 - x0 + pad, h = y1 - y0;
    if (w > 0 && h > 0 && cp > 32) {
        if (penX + w + 1 > aw) { penX = 1; penY += rowH + 1; rowH = 0; }
        while (penY + h + 1 > ah && ah < kMaxAtlas) { // grow downwards, keeping existing glyphs in place
            ah *= 2;
            atlas.resize(size_t(aw) * size_t(ah), 0);
        }
        if (penY + h + 1 <= ah && w + 2 <= aw) {
            uint8_t* dst = atlas.data() + size_t(penY) * size_t(aw) + size_t(penX);
            stbtt_MakeGlyphBitmap(&file->info, dst, w - pad, h, aw, scale, scale, gi);
            if (fakeBold)
                for (int y = 0; y < h; ++y)
                    for (int x = w - 1; x > 0; --x) dst[y * aw + x] = std::max(dst[y * aw + x], dst[y * aw + x - 1]);
            g.x0 = x0; g.y0 = y0; g.w = w; g.h = h; g.ax = penX; g.ay = penY;
            penX += w + 1;
            rowH = std::max(rowH, h);
            atlasDirty = true;
        }
    }
    return glyphs.emplace(cp, g).first->second;
}

void UiFontEngine::Face::syncTexture() {
    if (!atlasDirty && texture) return;
    // The callback may run later (and again after a device reset), so it owns a snapshot.
    auto pixels = std::make_shared<std::vector<Rml::byte>>(size_t(aw) * size_t(ah) * 4);
    for (size_t i = 0; i < atlas.size(); ++i) std::memset(pixels->data() + i * 4, atlas[i], 4); // premultiplied white
    const Rml::Vector2i dims{aw, ah};
    texture = std::make_unique<Rml::CallbackTextureSource>([pixels, dims](const Rml::CallbackTextureInterface& ti) {
        return ti.GenerateTexture({pixels->data(), pixels->size()}, dims);
    });
    atlasDirty = false;
    ++version; // geometry built with the previous texture must be regenerated
}

UiFontEngine::UiFontEngine() = default;
UiFontEngine::~UiFontEngine() = default;

bool UiFontEngine::addFont(std::string data, const std::string& family, Rml::Style::FontStyle style, Rml::Style::FontWeight weight) {
    auto f = std::make_unique<File>();
    f->data = std::move(data);
    const auto* bytes = reinterpret_cast<const unsigned char*>(f->data.data());
    const int offset = stbtt_GetFontOffsetForIndex(bytes, 0);
    if (offset < 0 || !stbtt_InitFont(&f->info, bytes, offset)) {
        Rml::Log::Message(Rml::Log::LT_ERROR, "ui font: '%s' is not a TrueType font", family.c_str());
        return false;
    }
    f->family = lower(family.empty() ? familyName(f->info) : family);
    f->style = style;
    if (weight == Rml::Style::FontWeight::Auto) {
        weight = Rml::Style::FontWeight::Normal;
        if (lower(familyName(f->info)).find("bold") != std::string::npos) weight = Rml::Style::FontWeight::Bold;
    }
    f->weight = weight;
    files_.push_back(std::move(f));
    return true;
}

bool UiFontEngine::LoadFontFace(const Rml::String& file_name, int, bool, Rml::Style::FontWeight weight) {
    std::string data;
    if (!Rml::GetFileInterface()->LoadFile(file_name, data) || data.empty()) {
        Rml::Log::Message(Rml::Log::LT_ERROR, "ui font: cannot read '%s'", file_name.c_str());
        return false;
    }
    return addFont(std::move(data), "", Rml::Style::FontStyle::Normal, weight);
}

bool UiFontEngine::LoadFontFace(Rml::Span<const Rml::byte> data, int, const Rml::String& family, Rml::Style::FontStyle style,
                                Rml::Style::FontWeight weight, bool) {
    return addFont(std::string(reinterpret_cast<const char*>(data.data()), data.size()), family, style, weight);
}

Rml::FontFaceHandle UiFontEngine::GetFontFaceHandle(const Rml::String& family, Rml::Style::FontStyle style, Rml::Style::FontWeight weight, int size) {
    if (files_.empty() || size <= 0) return 0;
    const std::string fam = lower(family);
    const int want = weight == Rml::Style::FontWeight::Auto ? 400 : int(weight);
    File* best = nullptr;
    int bestScore = -1;
    for (auto& f : files_) {
        int score = (f->family == fam ? 4000 : 0) + (f->style == style ? 2000 : 0) + 1000 - std::abs(int(f->weight) - want);
        if (score > bestScore) { bestScore = score; best = f.get(); }
    }
    const bool fakeBold = want >= 600 && int(best->weight) < 600;
    for (auto& fc : faces_)
        if (fc->file == best && fc->size == size && fc->fakeBold == fakeBold) return reinterpret_cast<Rml::FontFaceHandle>(fc.get());

    auto fc = std::make_unique<Face>();
    fc->file = best;
    fc->size = size;
    fc->fakeBold = fakeBold;
    fc->scale = stbtt_ScaleForMappingEmToPixels(&best->info, float(size));
    int asc, desc, gap;
    if (!stbtt_GetFontVMetricsOS2(&best->info, &asc, &desc, &gap)) stbtt_GetFontVMetrics(&best->info, &asc, &desc, &gap);
    Rml::FontMetrics& m = fc->metrics;
    m.size = size;
    m.ascent = float(asc) * fc->scale;
    m.descent = float(-desc) * fc->scale;
    m.line_spacing = float(asc - desc + gap) * fc->scale;
    int x0, y0, x1, y1;
    m.x_height = stbtt_GetCodepointBox(&best->info, 'x', &x0, &y0, &x1, &y1) ? float(y1) * fc->scale : float(size) * 0.5f;
    m.underline_position = std::max(1.0f, float(size) * 0.1f);
    m.underline_thickness = std::max(1.0f, float(size) / 14.0f);
    // Most UI text is Latin + Cyrillic: rasterize those up front so text rarely forces an atlas rebuild.
    const int side = std::clamp(int(std::ceil(std::sqrt(float(size * size) * 230.0f) / 64.0f)) * 64, 128, 2048);
    fc->aw = 1;
    while (fc->aw < side) fc->aw *= 2;
    fc->ah = fc->aw / 2;
    fc->atlas.assign(size_t(fc->aw) * size_t(fc->ah), 0);
    for (uint32_t c = 32; c < 127; ++c) fc->glyph(c);
    for (uint32_t c = 0x410; c <= 0x44F; ++c) fc->glyph(c);
    for (uint32_t c : {0x401u, 0x451u, 0x2014u, 0x2013u, 0xABu, 0xBBu, 0x2116u, 0xB7u, 0xD7u, 0x2026u}) fc->glyph(c);
    faces_.push_back(std::move(fc));
    return reinterpret_cast<Rml::FontFaceHandle>(faces_.back().get());
}

Rml::FontEffectsHandle UiFontEngine::PrepareFontEffects(Rml::FontFaceHandle, const Rml::FontEffectList&) {
    return 0; // font effects (shadow/outline/glow) are not supported yet
}

const Rml::FontMetrics& UiFontEngine::GetFontMetrics(Rml::FontFaceHandle handle) {
    return reinterpret_cast<Face*>(handle)->metrics;
}

int UiFontEngine::GetStringWidth(Rml::FontFaceHandle handle, Rml::StringView string, const Rml::TextShapingContext& ctx, Rml::Character prior) {
    Face& f = *reinterpret_cast<Face*>(handle);
    float w = 0;
    uint32_t prev = uint32_t(prior);
    for (auto it = Rml::StringIteratorU8(string.begin(), string.begin(), string.end()); it; ++it) {
        const uint32_t cp = uint32_t(*it);
        w += f.kern(prev, cp) + f.glyph(cp).advance + ctx.letter_spacing;
        prev = cp;
    }
    return int(std::lround(w));
}

int UiFontEngine::GenerateString(Rml::RenderManager& rm, Rml::FontFaceHandle handle, Rml::FontEffectsHandle, Rml::StringView string,
                                 Rml::Vector2f position, Rml::ColourbPremultiplied colour, float, const Rml::TextShapingContext& ctx,
                                 Rml::TexturedMeshList& meshList) {
    Face& f = *reinterpret_cast<Face*>(handle);
    // Rasterize every glyph first: the atlas texture must be final before it is referenced.
    for (auto it = Rml::StringIteratorU8(string.begin(), string.begin(), string.end()); it; ++it) f.glyph(uint32_t(*it));
    f.syncTexture();

    Rml::TexturedMesh tm;
    tm.texture = f.texture->GetTexture(rm);
    auto& verts = tm.mesh.vertices;
    auto& idx = tm.mesh.indices;
    const float iw = 1.0f / float(f.aw), ih = 1.0f / float(f.ah);
    float pen = 0;
    uint32_t prev = 0;
    for (auto it = Rml::StringIteratorU8(string.begin(), string.begin(), string.end()); it; ++it) {
        const uint32_t cp = uint32_t(*it);
        pen += f.kern(prev, cp);
        const Face::Glyph& g = f.glyph(cp);
        if (g.w > 0) {
            const float x = std::round(position.x + pen) + float(g.x0), y = std::round(position.y) + float(g.y0);
            const int base = int(verts.size());
            const float u0 = float(g.ax) * iw, v0 = float(g.ay) * ih, u1 = float(g.ax + g.w) * iw, v1 = float(g.ay + g.h) * ih;
            verts.push_back({{x, y}, colour, {u0, v0}});
            verts.push_back({{x + float(g.w), y}, colour, {u1, v0}});
            verts.push_back({{x + float(g.w), y + float(g.h)}, colour, {u1, v1}});
            verts.push_back({{x, y + float(g.h)}, colour, {u0, v1}});
            for (int k : {0, 1, 2, 0, 2, 3}) idx.push_back(base + k);
        }
        pen += g.advance + ctx.letter_spacing;
        prev = cp;
    }
    if (!verts.empty()) meshList.push_back(std::move(tm));
    return int(std::lround(pen));
}

int UiFontEngine::GetVersion(Rml::FontFaceHandle handle) {
    Face& f = *reinterpret_cast<Face*>(handle);
    // Glyphs added while measuring text become visible on the next regeneration.
    if (f.atlasDirty && f.texture) f.syncTexture();
    return f.version;
}

void UiFontEngine::ReleaseFontResources() { faces_.clear(); }
void UiFontEngine::Shutdown() { faces_.clear(); }

} // namespace rts::ui
