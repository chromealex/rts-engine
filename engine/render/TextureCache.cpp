#include "TextureCache.h"
#include "../platform/Platform.h"

// Private copy of stb_image (static functions) so this file links next to the UI's copy.
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_TGA
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wsign-compare"
#endif
#include "stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <cstdio>
#include <cstring>

#include "engine/atlas/AtlasFormat.h"

namespace rts::render {

bool decodeImage(const std::string& bytes, int& w, int& h, std::vector<uint8_t>& rgba) {
    int n = 0;
    unsigned char* px = stbi_load_from_memory(reinterpret_cast<const unsigned char*>(bytes.data()), int(bytes.size()), &w, &h, &n, 4);
    if (!px) return false;
    rgba.assign(px, px + size_t(w) * size_t(h) * 4);
    stbi_image_free(px);
    return true;
}

void TextureCache::shutdown() {
    auto isPage = [&](gfx::TextureHandle h) { for (auto& [k, p] : atlasPages_) if (p == h) return true; return false; };
    if (dev_) for (auto& [k, t] : cache_) if (t.tex.valid() && !isPage(t.tex)) dev_->destroyTexture(t.tex);
    if (dev_) for (auto& [k, t] : atlasPages_) if (t.valid()) dev_->destroyTexture(t);
    cache_.clear();
    atlasPages_.clear();
    atlas_.reset();
}

bool TextureCache::resolveAtlased(const std::string& path, bool nearest, const std::string& base, TextureRef& r) {
    if (!atlas_ || atlasBase_ != base) {
        atlasBase_ = base;
        atlas_ = std::make_shared<atlas::ImageSource>([base](const std::string& p, std::string& o) { return platform::readContentFile(base + p, o); },
                                                       [](const std::string& b, atlas::Image& img) { return decodeImage(b, img.width, img.height, img.rgba); });
    }
    const atlas::Region* reg = atlas_->manifest().find(path);
    if (!reg) return false;
    const atlas::Page& pg = atlas_->manifest().pages()[size_t(reg->page)];
    gfx::TextureDesc td;
    td.filter = nearest ? gfx::Filter::Nearest : gfx::Filter::Linear;
    bool untrimmed = !reg->rotated && reg->offsetX == 0 && reg->offsetY == 0 && reg->w == reg->sourceW && reg->h == reg->sourceH;
    if (!untrimmed) { // callers map uvs linearly over the whole image: give them the original pixels
        atlas::Image img;
        if (!atlas_->load(path, img)) return false;
        td.width = img.width; td.height = img.height; td.data = img.rgba.data(); td.label = path.c_str();
        r.tex = dev_->createTexture(td);
        r.width = img.width; r.height = img.height;
        return true;
    }
    std::string key = base + pg.file + (nearest ? "#n" : "#l");
    gfx::TextureHandle& page = atlasPages_[key];
    if (!page.valid()) {
        std::string bytes;
        atlas::Image img;
        if (!platform::readContentFile(base + pg.file, bytes) || !atlas::loadPageImage(bytes, [](const std::string& b, atlas::Image& i) { return decodeImage(b, i.width, i.height, i.rgba); }, img))
            return false;
        td.width = img.width; td.height = img.height; td.data = img.rgba.data(); td.label = "atlas-page";
        page = dev_->createTexture(td);
    }
    r.tex = page;
    r.u0 = float(reg->x) / float(pg.width); r.v0 = float(reg->y) / float(pg.height);
    r.u1 = float(reg->x + reg->w) / float(pg.width); r.v1 = float(reg->y + reg->h) / float(pg.height);
    r.width = reg->sourceW; r.height = reg->sourceH;
    return true;
}

TextureRef TextureCache::resolve(const std::string& path, bool nearest, const std::string& base) {
    std::string key = base + path + (nearest ? "#n" : "#l");
    if (auto it = cache_.find(key); it != cache_.end()) return it->second;
    TextureRef r;
    std::string bytes;
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    if (dev_ && resolveAtlased(path, nearest, base, r)) {
        // packed into a texture atlas at build time
    } else if (!dev_ || !platform::readContentFile(base + path, bytes)) {
        error_ = "texture not found: " + path;
        std::fprintf(stderr, "[sprites] %s\n", error_.c_str());
    } else if (!decodeImage(bytes, w, h, rgba)) {
        error_ = "texture: cannot decode " + path;
        std::fprintf(stderr, "[sprites] %s\n", error_.c_str());
    } else {
        gfx::TextureDesc td;
        td.width = w; td.height = h;
        td.filter = nearest ? gfx::Filter::Nearest : gfx::Filter::Linear;
        td.data = rgba.data();
        td.label = path.c_str();
        r.tex = dev_->createTexture(td);
        r.width = w; r.height = h;
    }
    cache_[key] = r; // failures are cached too: one message, no retry every frame
    return r;
}

TextureRef TextureCache::addGenerated(const std::string& name, int w, int h, const std::vector<uint32_t>& px, bool nearest) {
    if (auto it = cache_.find(name); it != cache_.end()) return it->second;
    TextureRef r;
    if (!dev_ || px.size() < size_t(w) * size_t(h)) return r;
    gfx::TextureDesc td;
    td.width = w; td.height = h;
    td.filter = nearest ? gfx::Filter::Nearest : gfx::Filter::Linear;
    td.data = px.data();
    td.label = name.c_str();
    r.tex = dev_->createTexture(td);
    r.width = w; r.height = h;
    cache_[name] = r;
    return r;
}

} // namespace rts::render
