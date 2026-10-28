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
    if (dev_) for (auto& [k, t] : cache_) if (t.tex.valid()) dev_->destroyTexture(t.tex);
    cache_.clear();
}

TextureRef TextureCache::resolve(const std::string& path, bool nearest, const std::string& base) {
    std::string key = base + path + (nearest ? "#n" : "#l");
    if (auto it = cache_.find(key); it != cache_.end()) return it->second;
    TextureRef r;
    std::string bytes;
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    if (!dev_ || !platform::readContentFile(base + path, bytes)) {
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
