// Image textures for sprites and tiles (PNG/JPEG/TGA via stb_image, read with
// platform::readContentFile so packs, Android assets and the web preload work).
//
// `resolve(path)` is the single indirection between a *logical* texture path used by game data
// ("sprites/tank.png") and the GPU: it returns the texture plus the uv rect the image occupies.
// Today every image is its own texture (rect = 0..1); a build-time atlas packer can return a
// sub-rect of a shared page instead without touching the renderers, as long as callers map their
// frame uvs through TextureRef::u()/v() and never rely on wrap/repeat.
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "../gfx/Gfx.h"

namespace rts::atlas { class ImageSource; }

namespace rts::render {

struct TextureRef {
    gfx::TextureHandle tex;
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;  // rect of the logical image inside `tex`
    int width = 0, height = 0;             // logical image size in pixels (before any atlas trim)
    bool valid() const { return tex.valid() && width > 0 && height > 0; }
    // logical uv (0..1 over the image) -> texture uv
    float u(float lu) const { return u0 + (u1 - u0) * lu; }
    float v(float lv) const { return v0 + (v1 - v0) * lv; }
};

class TextureCache {
public:
    void init(gfx::Device& dev) { dev_ = &dev; }
    void shutdown();
    // `base` is prefixed to the path when reading (editor: "<project dir>/", game: "").
    TextureRef resolve(const std::string& path, bool nearest = true, const std::string& base = "");
    // Procedural texture registered under a logical name (e.g. "builtin:shadow").
    TextureRef addGenerated(const std::string& name, int w, int h, const std::vector<uint32_t>& rgba, bool nearest);
    const std::string& lastError() const { return error_; }

private:
    // Build-time texture atlases (engine/atlas): images packed into a page resolve to the page +
    // their rect; trimmed/rotated ones (never sprite sheets) are rebuilt as standalone textures.
    bool resolveAtlased(const std::string& path, bool nearest, const std::string& base, TextureRef& out);
    std::shared_ptr<atlas::ImageSource> atlas_;
    std::string atlasBase_;             // base the manifest was loaded for
    std::map<std::string, gfx::TextureHandle> atlasPages_;
    gfx::Device* dev_ = nullptr;
    std::map<std::string, TextureRef> cache_;
    std::string error_;
};

// Decodes an image file in memory to RGBA8 (top row first). Returns false on bad data.
bool decodeImage(const std::string& bytes, int& w, int& h, std::vector<uint8_t>& rgba);

} // namespace rts::render
