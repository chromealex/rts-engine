#include "AtlasFormat.h"

#include <cstring>

#include "engine/data/Json.h"

namespace rts::atlas {

namespace {
void put32(std::string& s, uint32_t v) { for (int i = 0; i < 4; ++i) s.push_back(char(uint8_t(v >> (8 * i)))); }
uint32_t get32(const std::string& s, size_t o) {
    uint32_t v = 0;
    for (int i = 3; i >= 0; --i) v = (v << 8) | uint8_t(s[o + size_t(i)]);
    return v;
}
bool fail(std::string* e, const std::string& m) { if (e) *e = m; return false; }
} // namespace

// ------------------------------------------------------------------ manifest
// "textures": { "<path>": [page, x, y, w, h, offsetX, offsetY, sourceW, sourceH, rotated] }
bool Manifest::parse(const std::string& text, std::string* error) {
    pages_.clear();
    regions_.clear();
    Json j;
    try { j = Json::parse(text); } catch (const std::exception& e) { return fail(error, std::string("atlas manifest: ") + e.what()); }
    if (j.getInt("version", 0) != kManifestVersion) return fail(error, "atlas manifest: unsupported version");
    for (const Json& p : j["pages"].items()) {
        Page pg;
        pg.file = p.getStr("file");
        pg.group = p.getStr("group");
        pg.width = int(p.getInt("w"));
        pg.height = int(p.getInt("h"));
        pg.alpha = p.getBool("alpha", true);
        pg.mips = p.getBool("mips", false);
        if (pg.file.empty() || pg.width <= 0 || pg.height <= 0) return fail(error, "atlas manifest: bad page entry");
        pages_.push_back(std::move(pg));
    }
    for (const auto& [path, a] : j["textures"].members()) {
        if (!a.isArray() || a.size() < 10) return fail(error, "atlas manifest: bad entry '" + path + "'");
        Region r;
        r.page = int(a[0].integer());
        r.x = int(a[1].integer()); r.y = int(a[2].integer());
        r.w = int(a[3].integer()); r.h = int(a[4].integer());
        r.offsetX = int(a[5].integer()); r.offsetY = int(a[6].integer());
        r.sourceW = int(a[7].integer()); r.sourceH = int(a[8].integer());
        r.rotated = a[9].integer() != 0;
        if (r.page < 0 || size_t(r.page) >= pages_.size()) return fail(error, "atlas manifest: '" + path + "' refers to a missing page");
        const Page& pg = pages_[size_t(r.page)];
        int pw = r.rotated ? r.h : r.w, ph = r.rotated ? r.w : r.h;
        if (r.x < 0 || r.y < 0 || r.w <= 0 || r.h <= 0 || r.x + pw > pg.width || r.y + ph > pg.height)
            return fail(error, "atlas manifest: '" + path + "' is outside its page");
        regions_.emplace(path, r);
    }
    return true;
}

std::string Manifest::serialize() const {
    Json j = Json::object();
    j.set("version", Json::number(int64_t(kManifestVersion)));
    Json pages = Json::array();
    for (const Page& p : pages_) {
        Json o = Json::object();
        o.set("file", Json::string(p.file));
        o.set("group", Json::string(p.group));
        o.set("w", Json::number(int64_t(p.width)));
        o.set("h", Json::number(int64_t(p.height)));
        o.set("alpha", Json::fromBool(p.alpha));
        o.set("mips", Json::fromBool(p.mips));
        pages.push(std::move(o));
    }
    j.set("pages", std::move(pages));
    Json tex = Json::object();
    for (const auto& [path, r] : regions_) { // std::map: sorted, deterministic
        Json a = Json::array();
        for (int v : {r.page, r.x, r.y, r.w, r.h, r.offsetX, r.offsetY, r.sourceW, r.sourceH, int(r.rotated)}) a.push(Json::number(int64_t(v)));
        tex.set(path, std::move(a));
    }
    j.set("textures", std::move(tex));
    return j.dump(1) + "\n";
}

const Region* Manifest::find(std::string_view path) const {
    auto it = regions_.find(path);
    return it == regions_.end() ? nullptr : &it->second;
}

void regionUVs(const Region& r, int pageW, int pageH, float out[8]) {
    float iw = 1.0f / float(pageW), ih = 1.0f / float(pageH);
    float x0 = float(r.x) * iw, y0 = float(r.y) * ih;
    if (!r.rotated) {
        float x1 = float(r.x + r.w) * iw, y1 = float(r.y + r.h) * ih;
        const float v[8] = {x0, y0, x1, y0, x1, y1, x0, y1};
        std::memcpy(out, v, sizeof(v));
    } else { // stored turned 90 degrees clockwise: occupies h x w page pixels
        float x1 = float(r.x + r.h) * iw, y1 = float(r.y + r.w) * ih;
        const float v[8] = {x1, y0, x1, y1, x0, y1, x0, y0};
        std::memcpy(out, v, sizeof(v));
    }
}

// ------------------------------------------------------------------ .rtex
bool isRtex(const std::string& b) { return b.size() >= 24 && std::memcmp(b.data(), "RTEX", 4) == 0; }

std::string encodeRtex(const Image& img, bool alpha, bool mips, bool deltaFilter) {
    std::string s = "RTEX";
    put32(s, 1);
    put32(s, uint32_t(img.width));
    put32(s, uint32_t(img.height));
    put32(s, (alpha ? 1u : 0u) | (mips ? 2u : 0u) | (deltaFilter ? 4u : 0u));
    put32(s, 0);
    size_t row = size_t(img.width) * 4, base = s.size();
    s.resize(base + row * size_t(img.height));
    for (int y = 0; y < img.height; ++y) {
        const uint8_t* src = img.rgba.data() + row * size_t(y);
        uint8_t* dst = reinterpret_cast<uint8_t*>(s.data() + base + row * size_t(y));
        for (size_t i = 0; i < row; ++i) dst[i] = deltaFilter && i >= 4 ? uint8_t(src[i] - src[i - 4]) : src[i];
    }
    return s;
}

bool decodeRtex(const std::string& b, Image& out, uint32_t* flags) {
    if (!isRtex(b) || get32(b, 4) != 1) return false;
    uint32_t w = get32(b, 8), h = get32(b, 12), f = get32(b, 16);
    if (w == 0 || h == 0 || w > 16384 || h > 16384 || b.size() != 24 + size_t(w) * h * 4) return false;
    out.width = int(w);
    out.height = int(h);
    out.rgba.assign(b.begin() + 24, b.end());
    if (f & 4u) {
        size_t row = size_t(w) * 4;
        for (size_t y = 0; y < h; ++y) {
            uint8_t* p = out.rgba.data() + row * y;
            for (size_t i = 4; i < row; ++i) p[i] = uint8_t(p[i] + p[i - 4]);
        }
    }
    if (flags) *flags = f;
    return true;
}

bool extractRegion(const Image& page, const Region& r, Image& out) {
    int pw = r.rotated ? r.h : r.w, ph = r.rotated ? r.w : r.h;
    if (r.x < 0 || r.y < 0 || r.x + pw > page.width || r.y + ph > page.height) return false;
    if (r.offsetX < 0 || r.offsetY < 0 || r.offsetX + r.w > r.sourceW || r.offsetY + r.h > r.sourceH) return false;
    out.width = r.sourceW;
    out.height = r.sourceH;
    out.rgba.assign(size_t(r.sourceW) * size_t(r.sourceH) * 4, 0);
    for (int sy = 0; sy < r.h; ++sy)
        for (int sx = 0; sx < r.w; ++sx) {
            // rotated clockwise: source (sx, sy) -> page (h-1-sy, sx)
            const uint8_t* s = r.rotated ? page.px(r.x + r.h - 1 - sy, r.y + sx) : page.px(r.x + sx, r.y + sy);
            std::memcpy(out.px(r.offsetX + sx, r.offsetY + sy), s, 4);
        }
    return true;
}

// ------------------------------------------------------------------ CPU image source
bool loadPageImage(const std::string& bytes, const ImageSource::DecodeFn& decodePng, Image& out) {
    if (isRtex(bytes)) return decodeRtex(bytes, out);
    return decodePng && decodePng(bytes, out);
}

ImageSource::ImageSource(ReadFn read, DecodeFn decodePng) : read_(std::move(read)), decodePng_(std::move(decodePng)) {
    std::string text;
    if (read_ && read_(kManifestPath, text)) manifest_.parse(text); // a broken manifest = no atlas
}

bool ImageSource::load(const std::string& path, Image& out) {
    const Region* r = manifest_.find(path);
    if (!r) return false;
    if (cachedPage_ != r->page) {
        std::string bytes;
        cachedPage_ = -1;
        if (!read_(manifest_.pages()[size_t(r->page)].file, bytes) || !loadPageImage(bytes, decodePng_, cache_)) return false;
        cachedPage_ = r->page;
    }
    return extractRegion(cache_, *r, out);
}

} // namespace rts::atlas
